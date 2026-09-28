// SPDX-License-Identifier: GPL-3.0-only
// Included in DXBCPatcher.cpp inside namespace SIE. The named buffer layout was
// audited against FO4 Community Shaders v0.1.1 (1a0439a); see THIRD_PARTY_NOTICES.
namespace
{
    bool HasCommunityMarker(const void* data, size_t size)
    {
        // Avoid reflection work for the thousands of unrelated game shaders.
        const auto* first = static_cast<const char*>(data);
        constexpr char marker[] = "PerCall_CB2";
        return data && size >= sizeof(marker) &&
            std::search(first, first + size, std::begin(marker), std::end(marker)) != first + size;
    }

    bool HasCommunitySunLayout(const void* data, size_t size)
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
        if (FAILED(D3DReflect(data, size, IID_PPV_ARGS(&reflection))))
            return false;
        const auto binding = [&](const char* name, D3D_SHADER_INPUT_TYPE type,
                                 UINT slot, D3D_SRV_DIMENSION dimension = D3D_SRV_DIMENSION_UNKNOWN) {
            D3D11_SHADER_INPUT_BIND_DESC desc{};
            return SUCCEEDED(reflection->GetResourceBindingDescByName(name, &desc)) &&
                desc.Type == type && desc.BindPoint == slot && desc.BindCount == 1 &&
                (dimension == D3D_SRV_DIMENSION_UNKNOWN || desc.Dimension == dimension);
        };
        if (!binding("PerCall_CB2", D3D_SIT_CBUFFER, 2) ||
            !binding("PerFrame_CB12", D3D_SIT_CBUFFER, 12) ||
            !binding("g_tGbufferAlbedo", D3D_SIT_TEXTURE, 0, D3D_SRV_DIMENSION_TEXTURE2D) ||
            !binding("g_tGbufferNormal", D3D_SIT_TEXTURE, 1, D3D_SRV_DIMENSION_TEXTURE2D) ||
            !binding("g_tGbufferMaterial", D3D_SIT_TEXTURE, 2, D3D_SRV_DIMENSION_TEXTURE2D) ||
            !binding("g_tMainDepth", D3D_SIT_TEXTURE, 3, D3D_SRV_DIMENSION_TEXTURE2D) ||
            !binding("g_sMainDepth", D3D_SIT_SAMPLER, 3))
            return false;
        const auto vectorAt = [&](const char* buffer, const char* name, UINT offset, bool used = false) {
            auto* variable = reflection->GetConstantBufferByName(buffer)->GetVariableByName(name);
            D3D11_SHADER_VARIABLE_DESC desc{};
            D3D11_SHADER_TYPE_DESC type{};
            return SUCCEEDED(variable->GetDesc(&desc)) && desc.StartOffset == offset &&
                desc.Size == 16 && (!used || (desc.uFlags & D3D_SVF_USED) != 0) &&
                SUCCEEDED(variable->GetType()->GetDesc(&type)) &&
                type.Class == D3D_SVC_VECTOR && type.Type == D3D_SVT_FLOAT &&
                type.Rows == 1 && type.Columns == 4 && type.Elements == 0;
        };
        if (!vectorAt("PerCall_CB2", "ScreenSize", 0) ||
            !vectorAt("PerFrame_CB12", "FarReproj_row0", 320) ||
            !vectorAt("PerFrame_CB12", "FarReproj_row1", 336) ||
            !vectorAt("PerFrame_CB12", "FarReproj_row2", 352) ||
            !vectorAt("PerFrame_CB12", "FarReproj_row3", 368) ||
            !vectorAt("PerFrame_CB12", "NearReproj_row0", 384) ||
            !vectorAt("PerFrame_CB12", "NearReproj_row1", 400) ||
            !vectorAt("PerFrame_CB12", "NearReproj_row2", 416) ||
            !vectorAt("PerFrame_CB12", "NearReproj_row3", 432))
            return false;
        const bool shadowed = vectorAt("PerCall_CB2", "SunColor_HDR", 32, true) &&
            (vectorAt("PerCall_CB2", "SunDirection", 16) ||
             vectorAt("PerCall_CB2", "SunDirection_and_padding", 16));
        D3D11_SHADER_VARIABLE_DESC attenuation{};
        const bool localLight = SUCCEEDED(reflection->GetConstantBufferByName("PerCall_CB2")
            ->GetVariableByName("LightAttenuation")->GetDesc(&attenuation));
        const bool unshadowed = !localLight &&
            vectorAt("PerCall_CB2", "LightVector", 16) &&
            vectorAt("PerCall_CB2", "LightColor_HDR", 32, true);
        return shadowed || unshadowed;
    }

    // Walk operands rather than searching DWORDs: a literal can have the same
    // bits as a CB operand. Reject relative b2 addressing and preserve swizzles
    // and operand modifiers when replacing its colour vector with a new temp.
    bool RewriteCommunitySunOperands(const uint32_t* source, uint32_t length,
        uint32_t sunTemp, std::vector<uint32_t>& output, uint32_t& references)
    {
        if (!length || (source[0] & 0x7ffu) == DXBCOpcodes::DCL_IMMEDIATE_CB)
            return false;
        output.assign(source, source + length);
        uint32_t pos = 1, token = source[0];
        while ((token >> 31) != 0) {
            if (pos >= length) return false;
            token = source[pos++];
        }
        std::vector<uint32_t> rewritten(source, source + pos);
        while (pos < length) {
            const uint32_t start = pos, operand = source[pos];
            if (!SkipOperand(source, length, pos)) return false;
            const uint32_t type = (operand >> 12) & 0xffu;
            if (type != 8) {
                rewritten.insert(rewritten.end(), source + start, source + pos);
                continue;
            }
            uint32_t index = start + 1, extended = operand;
            while ((extended >> 31) != 0) {
                if (index >= pos) return false;
                extended = source[index++];
            }
            if (((operand >> 20) & 3) != 2 || ((operand >> 22) & 7) != 0 || index >= pos)
                return false;
            if (source[index] != 2) {
                rewritten.insert(rewritten.end(), source + start, source + pos);
                continue;
            }
            if (((operand >> 25) & 7) != 0 || index + 2 != pos) return false;
            if (source[index + 1] != 2) {
                rewritten.insert(rewritten.end(), source + start, source + pos);
                continue;
            }
            // TEMP, one immediate index; retain selection and extensions.
            rewritten.push_back((operand & ~((0xffu << 12) | (0x7ffu << 20))) | (1u << 20));
            rewritten.insert(rewritten.end(), source + start + 1, source + index);
            rewritten.push_back(sunTemp);
            ++references;
        }
        if (rewritten.size() > 127) return false;
        rewritten[0] = (rewritten[0] & ~(0x7fu << 24)) |
            (static_cast<uint32_t>(rewritten.size()) << 24);
        output = std::move(rewritten);
        return true;
    }

    bool CommunitySunCandidate(const void* data, size_t size, DXBCPatcher::ParsedDXBC& parsed)
    {
        if (!data || size > UINT32_MAX || !HasCommunityMarker(data, size) ||
            !DXBCPatcher::Parse(static_cast<const uint8_t*>(data), static_cast<uint32_t>(size), parsed) ||
            !HasCommunitySunLayout(data, size))
            return false;
        const auto& s = parsed.shex;
        if (!IsPixelShader50(s.versionToken) || s.usesDynamicLinkage ||
            s.declaredResourceRegisters.contains(47) || s.tempCount >= 4090 ||
            s.svPositionRegister == UINT32_MAX || s.svPositionRegister != parsed.isgnSvPositionRegister ||
            (s.svPositionMask & 3) != 3 || s.outputRegisterMasks.size() != 2 ||
            !s.outputRegisterMasks.contains(0) || !s.outputRegisterMasks.contains(1) ||
            (s.outputRegisterMasks.at(0) & 7) != 7 || (s.outputRegisterMasks.at(1) & 7) != 7)
            return false;
        uint32_t references = 0;
        std::vector<uint32_t> scratch;
        for (uint32_t pos = s.declEndOffset; pos < s.instrStreamDwords;) {
            const uint32_t length = (s.instrStream[pos] >> 24) & 127;
            if (!length || length > s.instrStreamDwords - pos ||
                !RewriteCommunitySunOperands(s.instrStream + pos, length, s.tempCount, scratch, references))
                return false;
            pos += length;
        }
        return references != 0;
    }
}

bool DXBCPatcher::IsCommunitySunCandidate(const void* data, size_t size) noexcept
{
    try { ParsedDXBC parsed; return CommunitySunCandidate(data, size, parsed); }
    catch (...) { return false; }
}

ID3DBlob* DXBCPatcher::PatchCommunitySunShader(ID3DBlob* source, const DXBCPatch& patch,
    uint32_t factorTempRegister, SunShadowPatchInfo* outInfo)
{
    if (outInfo) *outInfo = {};
    ParsedDXBC parsed;
    if (!source || !CommunitySunCandidate(source->GetBufferPointer(), source->GetBufferSize(), parsed) ||
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
        !GetImmediateRegisterIndex(declaration.data(), static_cast<uint32_t>(declaration.size()), 1, 7, resource) || resource != 47)
        return nullptr;

    const auto& s = parsed.shex;
    const uint32_t sunTemp = s.tempCount + patch.additionalTempRegisters;
    std::vector<uint32_t> stream;
    // Keep declaration ordering and all original resources; t47 precedes inputs.
    bool insertedDeclaration = false;
    for (uint32_t pos = 0; pos < s.declEndOffset;) {
        if (pos == s.resourceDeclEndOffset) {
            stream.insert(stream.end(), declaration.begin(), declaration.end());
            insertedDeclaration = true;
        }
        uint32_t length = GetInstrLength(s.instrStream[pos]);
        if (!length && GetOpcodeFromToken(s.instrStream[pos]) == DXBCOpcodes::DCL_IMMEDIATE_CB)
            length = s.instrStream[pos + 1];
        if (!length || length > s.declEndOffset - pos) return nullptr;
        const size_t start = stream.size();
        stream.insert(stream.end(), s.instrStream + pos, s.instrStream + pos + length);
        if (pos == s.tempsDeclOffset) stream[start + 1] = sunTemp + 1;
        pos += length;
    }
    if (!insertedDeclaration || s.tempsDeclOffset == UINT32_MAX) return nullptr;
    for (size_t i = 0; i < patch.preRetInstructions.size(); ++i) {
        auto instruction = patch.preRetInstructions[i];
        if (instruction.empty() || GetInstrLength(instruction[0]) != instruction.size()) return nullptr;
        for (uint32_t p : patch.tempRemapPositions[i]) {
            if (p >= instruction.size() || instruction[p] >= patch.additionalTempRegisters) return nullptr;
            instruction[p] += s.tempCount;
        }
        for (uint32_t p : patch.svPositionFixupPositions[i]) {
            if (p >= instruction.size()) return nullptr;
            instruction[p] = s.svPositionRegister;
        }
        stream.insert(stream.end(), instruction.begin(), instruction.end());
    }
    stream.insert(stream.end(), {
        0x08000038, 0x00100072, sunTemp, 0x00208246, 2, 2,
        0x00100006, s.tempCount + factorTempRegister, // RGB = cb2[2].rgb * mask
        0x06000036, 0x00100082, sunTemp, 0x0020803a, 2, 2 // W unchanged
    });
    uint32_t references = 0;
    std::vector<uint32_t> instruction;
    for (uint32_t pos = s.declEndOffset; pos < s.instrStreamDwords;) {
        const uint32_t length = GetInstrLength(s.instrStream[pos]);
        if (!RewriteCommunitySunOperands(s.instrStream + pos, length, sunTemp, instruction, references))
            return nullptr;
        stream.insert(stream.end(), instruction.begin(), instruction.end());
        pos += length;
    }
    const int64_t newSize = static_cast<int64_t>(stream.size() + 2) * 4;
    const int64_t delta = newSize - s.shexChunkSize;
    const int64_t total = parsed.totalSize + delta;
    if (!references || total <= 0 || total > UINT32_MAX || newSize > UINT32_MAX) return nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> result;
    if (FAILED(D3DCreateBlob(static_cast<size_t>(total), &result))) return nullptr;
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
    ComputeChecksum(dst, static_cast<uint32_t>(total));
    if (outInfo) {
        outInfo->signatureMatched = true;
        outInfo->signatureVariant = 11;
        outInfo->insertionOffset = s.declEndOffset;
        outInfo->scaledTargetCount = 1;
    }
    return result.Detach();
}
