// SPDX-License-Identifier: GPL-3.0-only
// Included in DXBCPatcher.cpp inside namespace SIE, after CommunityShaderAdapter.inl.
// BSWater lights its surface with one colour, cb0's sun colour scaled by its
// strength, which then feeds both the specular highlight and the sunlit surface
// term. Attenuating that one intermediate removes the sun from the surface. Most
// of the water's brightness is its colour, though, from reflections and depth, so
// that colour is also darkened, down to kWaterShadowFloor under full shadow, just
// before the lighting is added to it. The shore blend and fog come later and stay
// exact, so the shoreline still meets the real ground and fog keeps its brightness.
// Stock FO4 has the light row at cb0[2] (1.10.984 and 1.11.x) or cb0[1] (1.10.163
// and VR 1.2.72), in 27 pixel shaders with 10 unique hashes per runtime.
namespace
{
    constexpr uint32_t kWaterSunVariant = 12;

    // A copy of the container with its shader chunk's instructions replaced and
    // every later chunk moved by `delta` bytes. The caller computes the checksum.
    Microsoft::WRL::ComPtr<ID3DBlob> RebuildShaderChunk(const DXBCPatcher::ParsedDXBC& parsed,
        const std::vector<uint32_t>& stream, int64_t& delta)
    {
        const auto& s = parsed.shex;
        const int64_t newSize = static_cast<int64_t>(stream.size() + 2) * 4;
        delta = newSize - s.shexChunkSize;
        const int64_t total = parsed.totalSize + delta;
        Microsoft::WRL::ComPtr<ID3DBlob> result;
        if (total <= 0 || total > UINT32_MAX || newSize > UINT32_MAX ||
            FAILED(D3DCreateBlob(static_cast<size_t>(total), &result)))
            return nullptr;
        auto* dst = static_cast<uint8_t*>(result->GetBufferPointer());
        const auto* src = parsed.data;
        const uint32_t offset = parsed.chunkOffsets[parsed.shexChunkIndex];
        memcpy(dst, src, offset + 8);
        const auto write32 = [&](size_t at, uint32_t value) { memcpy(dst + at, &value, 4); };
        write32(offset + 4, static_cast<uint32_t>(newSize));
        write32(offset + 8, s.versionToken);
        write32(offset + 12, static_cast<uint32_t>(stream.size() + 2));
        memcpy(dst + offset + 16, stream.data(), stream.size() * 4);
        const uint32_t oldEnd = offset + 8 + s.shexChunkSize;
        memcpy(dst + offset + 8 + newSize, src + oldEnd, parsed.totalSize - oldEnd);
        for (uint32_t i = 0; i < parsed.chunkCount; ++i)
            if (parsed.chunkOffsets[i] > offset)
                write32(0x20 + i * 4, static_cast<uint32_t>(parsed.chunkOffsets[i] + delta));
        write32(0x18, static_cast<uint32_t>(total));
        return result;
    }

    struct WaterSites
    {
        uint32_t lightEnd = 0;          // Stream offset just after the light colour.
        uint32_t lightTemp = 0;         // Temp holding the light colour.
        uint32_t lightDestination = 0;  // Its destination token, with its write mask.
        uint32_t colorAdd = 0;          // Stream offset of `add r, L, C`.
        uint32_t colorTemp = 0;         // Temp holding the water's colour.
    };

    // mul rN.xyz, cb0[K].wwww, cb0[K + 1].xyzx
    // mul rN.yzw, cb0[K].wwww, cb0[K + 1].xxyz
    bool MatchesWaterLight(const uint32_t* t) noexcept
    {
        constexpr uint32_t kMul = 0x09000038;
        constexpr uint32_t kXyz = 0x00100072, kYzw = 0x001000E2;
        constexpr uint32_t kCbW = 0x00208FF6, kCbXyzx = 0x00208246, kCbXxyz = 0x00208906;
        return t[0] == kMul && (t[1] == kXyz || t[1] == kYzw) &&
            t[3] == kCbW && t[4] == 0 && (t[5] == 1 || t[5] == 2) &&
            t[6] == (t[1] == kXyz ? kCbXyzx : kCbXxyz) && t[7] == 0 && t[8] == t[5] + 1;
    }

    // mad L.xyz, r.xyzx, cb1[1].wwww, r.xyzx: lighting = specular * strength + surface
    bool MatchesWaterLighting(const uint32_t* t) noexcept
    {
        return t[0] == 0x0A000032 && t[1] == 0x00100072 && t[3] == 0x00100246 &&
            t[5] == 0x00208FF6 && t[6] == 1 && t[7] == 1 && t[8] == 0x00100246;
    }

    // add r.xyz, L.xyzx, C.xyzx in either order. Returns C, or UINT32_MAX.
    uint32_t MatchWaterColorAdd(const uint32_t* t, uint32_t lighting) noexcept
    {
        if (t[0] != 0x07000000 || t[1] != 0x00100072 || t[3] != 0x00100246 || t[5] != 0x00100246 ||
            (t[4] == lighting) == (t[6] == lighting))
            return UINT32_MAX;
        return t[4] == lighting ? t[6] : t[4];
    }

    // Each site must exist exactly once and in straight-line order: more would
    // mean a layout the audit has not seen, where scaling one site could leave
    // another at full sun, and a branch between them could skip the mask read.
    bool FindWaterSites(const DXBCPatcher::ParsedSHEX& s, WaterSites& sites) noexcept
    {
        uint32_t lights = 0, lightings = 0, colors = 0, lighting = UINT32_MAX;
        for (uint32_t pos = s.declEndOffset; pos < s.instrStreamDwords;) {
            const uint32_t length = (s.instrStream[pos] >> 24) & 127;
            if (!length || length > s.instrStreamDwords - pos)
                return false;
            const uint32_t* t = s.instrStream + pos;
            const uint32_t opcode = t[0] & 0x7FF;
            constexpr uint32_t kIf = 0x1F, kElse = 0x12, kEndIf = 0x15, kLoop = 0x30, kEndLoop = 0x16;
            if (lights == 1 && colors == 0 && (opcode == kIf || opcode == kElse ||
                    opcode == kEndIf || opcode == kLoop || opcode == kEndLoop))
                return false;
            if (length == 9 && MatchesWaterLight(t)) {
                ++lights;
                sites.lightEnd = pos + length;
                sites.lightDestination = t[1];
                sites.lightTemp = t[2];
            } else if (length == 10 && lights == 1 && MatchesWaterLighting(t)) {
                ++lightings;
                lighting = t[2];
            } else if (length == 7 && lightings == 1 && colors == 0) {
                if (const uint32_t color = MatchWaterColorAdd(t, lighting); color != UINT32_MAX) {
                    ++colors;
                    sites.colorAdd = pos;
                    sites.colorTemp = color;
                }
            }
            pos += length;
        }
        return lights == 1 && lightings == 1 && colors == 1;
    }

    bool WaterSunCandidate(const void* data, size_t size,
        DXBCPatcher::ParsedDXBC& parsed, WaterSites& sites)
    {
        if (!data || size > UINT32_MAX ||
            !DXBCPatcher::Parse(static_cast<const uint8_t*>(data), static_cast<uint32_t>(size), parsed))
            return false;
        const auto& s = parsed.shex;
        const uint32_t position = parsed.isgnSvPositionRegister;
        if (!IsPixelShader50(s.versionToken) || s.usesDynamicLinkage ||
            s.declaredResourceRegisters.contains(47) || s.tempCount >= 4090 ||
            s.tempsDeclOffset == UINT32_MAX || position == UINT32_MAX)
            return false;
        // The simplest variant never reads SV_Position, though its signature
        // carries it. It can be declared there, unless the register holds
        // something else.
        if (s.svPositionRegister == UINT32_MAX
                ? s.declaredInputRegisters.contains(position)
                : s.svPositionRegister != position || (s.svPositionMask & 3) != 3)
            return false;
        return FindWaterSites(s, sites);
    }

    // Marks xy of the signature's SV_Position as read, matching the declaration
    // added for variants that did not read it.
    void MarkSignaturePositionRead(uint8_t* blob, const DXBCPatcher::ParsedDXBC& parsed, int64_t delta)
    {
        for (uint32_t i = 0; i < parsed.chunkCount; ++i) {
            uint32_t offset = parsed.chunkOffsets[i];
            if (memcmp(parsed.data + offset, "ISGN", 4) != 0)
                continue;
            if (offset > parsed.chunkOffsets[parsed.shexChunkIndex])
                offset = static_cast<uint32_t>(offset + delta);
            uint32_t count = 0;
            memcpy(&count, blob + offset + 8, 4);
            for (uint32_t e = 0; e < count; ++e) {
                uint8_t* element = blob + offset + 16 + e * 24;
                uint32_t systemValue = 0;
                memcpy(&systemValue, element + 8, 4);
                if (systemValue == 1)
                    element[21] |= 3;
            }
        }
    }
}

bool DXBCPatcher::IsWaterSunCandidate(const void* data, size_t size) noexcept
{
    try {
        ParsedDXBC parsed;
        WaterSites sites;
        return WaterSunCandidate(data, size, parsed, sites);
    } catch (...) {
        return false;
    }
}

ID3DBlob* DXBCPatcher::PatchWaterSunShader(ID3DBlob* source, const DXBCPatch& patch,
    uint32_t factorTempRegister, SunShadowPatchInfo* outInfo)
{
    if (outInfo) *outInfo = {};
    ParsedDXBC parsed;
    WaterSites sites;
    if (!source || !WaterSunCandidate(source->GetBufferPointer(), source->GetBufferSize(), parsed, sites) ||
        !patch.redirectedOutputs.empty() || patch.newDeclarations.size() != 1 ||
        patch.preRetInstructions.empty() || factorTempRegister >= patch.additionalTempRegisters ||
        patch.tempRemapPositions.size() != patch.preRetInstructions.size() ||
        patch.svPositionFixupPositions.size() != patch.preRetInstructions.size() ||
        patch.additionalTempRegisters >= 4096 - parsed.shex.tempCount)
        return nullptr;
    uint32_t resource = UINT32_MAX;
    const auto& declaration = patch.newDeclarations.front();
    if (declaration.empty() || GetOpcodeFromToken(declaration[0]) != DXBCOpcodes::DCL_RESOURCE ||
        GetInstrLength(declaration[0]) != declaration.size() ||
        !GetImmediateRegisterIndex(declaration.data(), static_cast<uint32_t>(declaration.size()), 1, 7, resource) ||
        resource != 47)
        return nullptr;

    const auto& s = parsed.shex;
    const uint32_t position = parsed.isgnSvPositionRegister;
    const bool declarePosition = s.svPositionRegister == UINT32_MAX;
    std::vector<uint32_t> stream;
    bool inserted = false;
    for (uint32_t pos = 0; pos < s.declEndOffset;) {
        if (pos == s.resourceDeclEndOffset) {
            stream.insert(stream.end(), declaration.begin(), declaration.end());
            // dcl_input_ps_siv linear noperspective v.xy, position
            if (declarePosition)
                stream.insert(stream.end(), { 0x04002064, 0x00101032, position, 1 });
            inserted = true;
        }
        uint32_t length = GetInstrLength(s.instrStream[pos]);
        if (!length && GetOpcodeFromToken(s.instrStream[pos]) == DXBCOpcodes::DCL_IMMEDIATE_CB)
            length = s.instrStream[pos + 1];
        if (!length || length > s.declEndOffset - pos)
            return nullptr;
        const size_t start = stream.size();
        stream.insert(stream.end(), s.instrStream + pos, s.instrStream + pos + length);
        if (pos == s.tempsDeclOffset)
            stream[start + 1] = s.tempCount + patch.additionalTempRegisters;
        pos += length;
    }
    if (!inserted)
        return nullptr;
    stream.insert(stream.end(), s.instrStream + s.declEndOffset, s.instrStream + sites.lightEnd);
    for (size_t i = 0; i < patch.preRetInstructions.size(); ++i) {
        auto instruction = patch.preRetInstructions[i];
        if (instruction.empty() || GetInstrLength(instruction[0]) != instruction.size())
            return nullptr;
        for (uint32_t p : patch.tempRemapPositions[i]) {
            if (p >= instruction.size() || instruction[p] >= patch.additionalTempRegisters)
                return nullptr;
            instruction[p] += s.tempCount;
        }
        for (uint32_t p : patch.svPositionFixupPositions[i]) {
            if (p >= instruction.size())
                return nullptr;
            instruction[p] = position;
        }
        stream.insert(stream.end(), instruction.begin(), instruction.end());
    }
    const uint32_t factor = s.tempCount + factorTempRegister;
    // mul rN.mask, rN.xyzw, rFactor.xxxx
    stream.insert(stream.end(), {
        0x07000038, sites.lightDestination, sites.lightTemp, 0x00100E46, sites.lightTemp, 0x00100006, factor });
    stream.insert(stream.end(), s.instrStream + sites.lightEnd, s.instrStream + sites.colorAdd);
    // mad rFactor.y, rFactor.xxxx, l(1 - floor), l(floor)
    // mul rC.xyz, rC.xyzw, rFactor.yyyy
    const uint32_t floorBits = std::bit_cast<uint32_t>(kWaterShadowFloor);
    const uint32_t rangeBits = std::bit_cast<uint32_t>(1.0f - kWaterShadowFloor);
    stream.insert(stream.end(), {
        0x0F000032, 0x00100022, factor, 0x00100006, factor,
        0x00004002, rangeBits, rangeBits, rangeBits, rangeBits,
        0x00004002, floorBits, floorBits, floorBits, floorBits,
        0x07000038, 0x00100072, sites.colorTemp, 0x00100E46, sites.colorTemp, 0x00100556, factor });
    stream.insert(stream.end(), s.instrStream + sites.colorAdd, s.instrStream + s.instrStreamDwords);

    int64_t delta = 0;
    auto result = RebuildShaderChunk(parsed, stream, delta);
    if (!result)
        return nullptr;
    auto* bytes = static_cast<uint8_t*>(result->GetBufferPointer());
    if (declarePosition)
        MarkSignaturePositionRead(bytes, parsed, delta);
    ComputeChecksum(bytes, static_cast<uint32_t>(result->GetBufferSize()));
    if (outInfo) {
        outInfo->signatureMatched = true;
        outInfo->signatureVariant = kWaterSunVariant;
        outInfo->insertionOffset = sites.lightEnd;
        outInfo->scaledTargetCount = 2;
    }
    return result.Detach();
}
