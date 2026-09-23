// SPDX-License-Identifier: GPL-3.0-only
#include "GodraysIntegration.h"
#include "BuildFeatures.h"

#if FO4CS_EXPERIMENTAL_FEATURES
#include "GodrayGameSettings.h"
#include "GodrayCloudShader.h"
#include "CloudComparison.h"

#include "CloudShadows.h"
#include "EngineAPI.h"
#include "ShaderTools/DXBCPatch.h"
#include "ShaderTools/DXBCPatcher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <TlHelp32.h>
#include <unordered_map>
#include <vector>

#include <d3d11shader.h>
#include <detours.h>

namespace FO4CS::GodraysIntegration
{
    namespace
    {
        enum class DirectionalShader : std::uint32_t
        {
            kUnsupported = 0,
            kVolumeGeometry,
            kScreenIntegral
        };

        struct KnownShader
        {
            std::uint64_t fnv1a;
            std::uint32_t size;
            std::array<std::uint8_t, 16> dxbcChecksum;
            DirectionalShader variant;
            const char* sha256;
        };

        // Extracted from the stock OG 1.10.163 GFSDK_GodraysLib.x64.dll.
        // The AE 1.11.240 GameWorks corpus is byte-identical. The SHA-256
        // values are recorded for reproducible external corpus verification;
        // runtime identity additionally checks size, FNV-1a and the complete
        // 128-bit DXBC checksum before parsing the exact shader structure.
        constexpr std::array kKnownDirectionalShaders{
            KnownShader{
                0xFD3A5CC23561CE9BULL, 4216,
                { 0xB5, 0x6A, 0x54, 0xE1, 0xC9, 0x1F, 0x19, 0x0E,
                  0xA3, 0x40, 0x8B, 0x2B, 0x8A, 0xC0, 0xA3, 0xAF },
                DirectionalShader::kVolumeGeometry,
                "0193513CF23DFB33B9BFA0A4A2200A57C5862BEBA47E6CF598691261485A1E71" },
            KnownShader{
                0x4D5614856DD1D46AULL, 4652,
                { 0x16, 0x21, 0x1F, 0x82, 0x5D, 0x40, 0xB6, 0x15,
                  0x48, 0x72, 0x80, 0x9A, 0x6E, 0x7F, 0x8E, 0x56 },
                DirectionalShader::kScreenIntegral,
                "6611CC70AA221DE5270D3540E33B4D98EEDA05C574E25EC5D67E6FB2609D0021" }
        };

        constexpr GUID kPatchedGodrayShaderMarker{
            0x7a55a33e, 0xcb80, 0x4eca,
            { 0x98, 0xf8, 0xd8, 0x3e, 0x25, 0x39, 0x41, 0xbc }
        };
        constexpr GUID kPatchedSunMaskGodrayShaderMarker{
            0x7a55a33f, 0xcb80, 0x4eca,
            { 0x98, 0xf8, 0xd8, 0x3e, 0x25, 0x39, 0x41, 0xbc }
        };

        constexpr float kPlanetRadiusWorld = 6371000.0f * 70.0f;
        // CameraPosAdjust and GameWorks' g_vEyePosition are sampled in the same
        // RenderVolume call. Sixteen Fallout units (~23 cm) accepts harmless
        // sub-frame camera adjustments but rejects a different origin, scale,
        // or axis ordering before cloud attenuation can affect the result.
        constexpr float kEyeAttestationToleranceWorld = 16.0f;
        constexpr UINT kCloudTextureSlot = 47;
        constexpr UINT kCloudSamplerSlot = 15;
        constexpr UINT kCloudConstantBufferSlot = 13;

        // NVIDIA's public x64 export returns its 32-bit status enum. The three
        // description types are deliberately opaque: no field or layout is
        // read, and shader identity provides the directional-light proof.
        using RenderVolumeFn = std::int32_t(__cdecl*)(
            void*, ID3D11DeviceContext*, const void*,
            ID3D11ShaderResourceView*, const void*, float, float,
            unsigned int, float);

        RenderVolumeFn s_originalRenderVolume = nullptr;
        std::atomic<bool> s_cloudOcclusionEnabled{ false };
        std::atomic<bool> s_nativeSettingsEnabled{ false };
        std::atomic<bool> s_nativeSettingsError{ false };
        std::atomic<bool> s_nativeSettingsRestartRequired{ false };
        std::atomic<bool> s_exportResolved{ false };
        std::atomic<bool> s_hookInstalled{ false };
        std::atomic<bool> s_nativeConsumerSupported{ false };
        std::atomic<bool> s_vrUnsupportedLogged{ false };
        std::atomic<std::uint32_t> s_authenticatedShaderObjects{ 0 };
        std::atomic<std::uint32_t> s_authenticatedDirectionalVariants{ 0 };
        std::atomic<std::uint32_t> s_authenticatedSunMaskVariants{ 0 };
        std::atomic<std::uint32_t> s_patchFailures{ 0 };
        std::atomic<std::uint64_t> s_renderVolumeCalls{ 0 };
        std::atomic<std::uint64_t> s_submittedDraws{ 0 };
        std::atomic<std::uint64_t> s_enableDrawBaseline{ 0 };
        std::atomic<std::uint64_t> s_lastSubmittedEpoch{ 0 };
        std::mutex s_installMutex;

        struct RenderVolumePhase
        {
            std::uint32_t depth{ 0 };
            std::uint64_t cloudEpoch{ 0 };
        };
        thread_local RenderVolumePhase s_renderVolumePhase;

        using GodrayCloudConstants = FO4CS::GodrayCloudShader::Constants;

        std::mutex s_deviceMutex;
        ComPtr<ID3D11Device> s_constantBufferDevice;
        ComPtr<ID3D11Buffer> s_cloudConstantBuffer;

        class DetourThreadEnlistment
        {
        public:
            ~DetourThreadEnlistment()
            {
                for (HANDLE thread : threads_)
                    CloseHandle(thread);
            }

            LONG EnlistProcessThreads() noexcept
            {
                const DWORD processId = GetCurrentProcessId();
                const DWORD currentThreadId = GetCurrentThreadId();
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (snapshot == INVALID_HANDLE_VALUE)
                    return GetLastError();

                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                if (!Thread32First(snapshot, &entry)) {
                    const LONG error = GetLastError();
                    CloseHandle(snapshot);
                    return error;
                }
                LONG result = NO_ERROR;
                do {
                    if (entry.th32OwnerProcessID != processId ||
                        entry.th32ThreadID == currentThreadId) {
                        continue;
                    }
                    HANDLE thread = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, entry.th32ThreadID);
                    if (!thread)
                        continue;
                    const LONG error = DetourUpdateThread(thread);
                    if (error == NO_ERROR)
                        threads_.push_back(thread);
                    else {
                        CloseHandle(thread);
                        result = error;
                        break;
                    }
                } while (Thread32Next(snapshot, &entry));
                CloseHandle(snapshot);
                if (result != NO_ERROR)
                    return result;
                return DetourUpdateThread(GetCurrentThread());
            }

        private:
            std::vector<HANDLE> threads_;
        };

        [[nodiscard]] bool IsAddressInsideModule(
            const void* address, HMODULE module) noexcept
        {
            if (!address || !module)
                return false;
            const auto* base = reinterpret_cast<const std::uint8_t*>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
                return false;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return false;
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            const auto begin = reinterpret_cast<std::uintptr_t>(base);
            const auto imageSize =
                static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
            return value >= begin && value - begin < imageSize;
        }

        [[nodiscard]] std::uint64_t HashDXBC(
            const void* data, std::size_t size) noexcept
        {
            if (!data || size == 0)
                return 0;
            std::uint64_t hash = 0xCBF29CE484222325ULL;
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t index = 0; index < size; ++index) {
                hash ^= bytes[index];
                hash *= 0x100000001B3ULL;
            }
            return hash;
        }

        [[nodiscard]] DirectionalShader IdentifyDirectionalShader(
            const void* data, std::size_t size) noexcept
        {
            if (!data || size < 20 || size >
                    static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
                return DirectionalShader::kUnsupported;
            }
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            if (std::memcmp(bytes, "DXBC", 4) != 0)
                return DirectionalShader::kUnsupported;
            const std::uint64_t hash = HashDXBC(data, size);
            for (const auto& known : kKnownDirectionalShaders) {
                if (known.size == size && known.fnv1a == hash &&
                    std::memcmp(bytes + 4, known.dxbcChecksum.data(),
                        known.dxbcChecksum.size()) == 0) {
                    return known.variant;
                }
            }
            return DirectionalShader::kUnsupported;
        }

        [[nodiscard]] bool HasExpectedStockStructure(
            const void* data, std::size_t size,
            DirectionalShader variant) noexcept
        {
            if (!data || size >
                    static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
                return false;
            }
            SIE::DXBCPatcher::ParsedDXBC parsed{};
            if (!SIE::DXBCPatcher::Parse(
                    static_cast<const std::uint8_t*>(data),
                    static_cast<std::uint32_t>(size), parsed)) {
                return false;
            }
            const auto output = parsed.shex.outputRegisterMasks.find(0);
            if (parsed.shex.usesDynamicLinkage ||
                parsed.shex.retOffsets.size() != 1 ||
                parsed.shex.tempCount != 3 ||
                parsed.shex.outputRegisterMasks.size() != 1 ||
                output == parsed.shex.outputRegisterMasks.end() ||
                output->second != 0xF) {
                return false;
            }
            if (variant == DirectionalShader::kVolumeGeometry) {
                return parsed.shex.declaredResourceRegisters.size() == 1 &&
                    parsed.shex.declaredResourceRegisters.contains(4);
            }
            return variant == DirectionalShader::kScreenIntegral &&
                parsed.shex.declaredResourceRegisters.size() == 2 &&
                parsed.shex.declaredResourceRegisters.contains(2) &&
                parsed.shex.declaredResourceRegisters.contains(4);
        }

        struct InputRemap
        {
            bool toTemp{ false };
            std::uint32_t index{ 0 };
        };

        [[nodiscard]] bool EqualSemantic(
            const char* left, const char* right) noexcept
        {
            return left && right && _stricmp(left, right) == 0;
        }

        [[nodiscard]] bool BuildInputRemaps(
            ID3DBlob* snippet, ID3DBlob* target,
            std::uint32_t baseOutputTemp,
            std::unordered_map<std::uint32_t, InputRemap>& remaps) noexcept
        {
            remaps.clear();
            ComPtr<ID3D11ShaderReflection> snippetReflection;
            ComPtr<ID3D11ShaderReflection> targetReflection;
            if (FAILED(D3DReflect(
                    snippet->GetBufferPointer(), snippet->GetBufferSize(),
                    __uuidof(ID3D11ShaderReflection),
                    reinterpret_cast<void**>(snippetReflection.GetAddressOf()))) ||
                FAILED(D3DReflect(
                    target->GetBufferPointer(), target->GetBufferSize(),
                    __uuidof(ID3D11ShaderReflection),
                    reinterpret_cast<void**>(targetReflection.GetAddressOf()))) ||
                !snippetReflection || !targetReflection) {
                return false;
            }

            D3D11_SHADER_DESC snippetDescription{};
            D3D11_SHADER_DESC targetDescription{};
            if (FAILED(snippetReflection->GetDesc(&snippetDescription)) ||
                FAILED(targetReflection->GetDesc(&targetDescription))) {
                return false;
            }

            for (UINT sourceIndex = 0;
                 sourceIndex < snippetDescription.InputParameters;
                 ++sourceIndex) {
                D3D11_SIGNATURE_PARAMETER_DESC source{};
                if (FAILED(snippetReflection->GetInputParameterDesc(
                        sourceIndex, &source))) {
                    return false;
                }
                if (EqualSemantic(source.SemanticName, "CLOUD_BASE")) {
                    remaps.emplace(source.Register,
                        InputRemap{ true, baseOutputTemp });
                    continue;
                }

                bool matched = false;
                for (UINT targetIndex = 0;
                     targetIndex < targetDescription.InputParameters;
                     ++targetIndex) {
                    D3D11_SIGNATURE_PARAMETER_DESC destination{};
                    if (FAILED(targetReflection->GetInputParameterDesc(
                            targetIndex, &destination))) {
                        return false;
                    }
                    if (!EqualSemantic(
                            source.SemanticName, destination.SemanticName) ||
                        source.SemanticIndex != destination.SemanticIndex ||
                        source.SystemValueType != destination.SystemValueType ||
                        (source.ReadWriteMask & ~destination.Mask) != 0) {
                        continue;
                    }
                    remaps.emplace(source.Register,
                        InputRemap{ false, destination.Register });
                    matched = true;
                    break;
                }
                if (!matched)
                    return false;
            }
            return !remaps.empty();
        }

        [[nodiscard]] bool RewriteOperand(
            std::vector<std::uint32_t>& instruction,
            std::uint32_t& position,
            const std::unordered_map<std::uint32_t, InputRemap>& inputRemaps,
            std::vector<std::uint32_t>& tempPositions,
            std::uint32_t depth = 0) noexcept
        {
            if (position >= instruction.size() || depth > 8)
                return false;
            const std::uint32_t tokenPosition = position;
            std::uint32_t operandToken = instruction[position++];
            const std::uint32_t operandType = (operandToken >> 12) & 0xFF;
            const std::uint32_t indexDimensions = (operandToken >> 20) & 0x3;
            std::uint32_t extendedToken = operandToken;
            while ((extendedToken >> 31) != 0) {
                if (position >= instruction.size())
                    return false;
                extendedToken = instruction[position++];
            }

            const std::uint32_t componentCount = operandToken & 0x3;
            if (operandType == 4 || operandType == 5) {
                std::uint32_t valueCount = 0;
                if (componentCount == 1)
                    valueCount = 1;
                else if (componentCount == 2)
                    valueCount = 4;
                else
                    return false;
                if (operandType == 5)
                    valueCount *= 2;
                if (valueCount > instruction.size() - position)
                    return false;
                position += valueCount;
                return true;
            }
            if ((operandType == 0 || operandType == 1) &&
                indexDimensions != 1) {
                return false;
            }

            for (std::uint32_t dimension = 0;
                 dimension < indexDimensions; ++dimension) {
                const std::uint32_t representation =
                    (operandToken >> (22 + 3 * dimension)) & 0x7;
                switch (representation) {
                case 0: {
                    if (position >= instruction.size())
                        return false;
                    const std::uint32_t indexPosition = position;
                    if (operandType == 0) {
                        tempPositions.push_back(indexPosition);
                    } else if (operandType == 1) {
                        const auto mapping = inputRemaps.find(
                            instruction[indexPosition]);
                        if (mapping == inputRemaps.end())
                            return false;
                        instruction[indexPosition] = mapping->second.index;
                        if (mapping->second.toTemp) {
                            instruction[tokenPosition] =
                                operandToken & ~(0xFFu << 12);
                            tempPositions.push_back(indexPosition);
                        }
                    }
                    ++position;
                    break;
                }
                case 1:
                    if (operandType == 0 || operandType == 1 ||
                        instruction.size() - position < 2) {
                        return false;
                    }
                    position += 2;
                    break;
                case 2:
                    if (operandType == 0 || operandType == 1 ||
                        !RewriteOperand(
                            instruction, position, inputRemaps,
                            tempPositions, depth + 1)) {
                        return false;
                    }
                    break;
                case 3:
                    if (operandType == 0 || operandType == 1 ||
                        position >= instruction.size()) {
                        return false;
                    }
                    ++position;
                    if (!RewriteOperand(
                            instruction, position, inputRemaps,
                            tempPositions, depth + 1)) {
                        return false;
                    }
                    break;
                case 4:
                    if (operandType == 0 || operandType == 1 ||
                        instruction.size() - position < 2) {
                        return false;
                    }
                    position += 2;
                    if (!RewriteOperand(
                            instruction, position, inputRemaps,
                            tempPositions, depth + 1)) {
                        return false;
                    }
                    break;
                default:
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool RewriteInstructionInputs(
            std::vector<std::uint32_t>& instruction,
            const std::unordered_map<std::uint32_t, InputRemap>& inputRemaps,
            std::vector<std::uint32_t>& tempPositions) noexcept
        {
            tempPositions.clear();
            if (instruction.empty())
                return false;
            std::uint32_t position = 1;
            std::uint32_t opcodeToken = instruction[0];
            while ((opcodeToken >> 31) != 0) {
                if (position >= instruction.size())
                    return false;
                opcodeToken = instruction[position++];
            }
            while (position < instruction.size()) {
                if (!RewriteOperand(
                        instruction, position, inputRemaps, tempPositions)) {
                    return false;
                }
            }
            return position == instruction.size();
        }

        [[nodiscard]] bool ImmediateDeclarationRegister(
            const std::vector<std::uint32_t>& declaration,
            std::uint32_t expectedOperandType,
            std::uint32_t& destination) noexcept
        {
            destination = (std::numeric_limits<std::uint32_t>::max)();
            if (declaration.size() < 3)
                return false;
            std::uint32_t position = 1;
            const std::uint32_t operand = declaration[position++];
            if (((operand >> 12) & 0xFF) != expectedOperandType ||
                ((operand >> 20) & 0x3) == 0 ||
                ((operand >> 22) & 0x7) != 0) {
                return false;
            }
            std::uint32_t extended = operand;
            while ((extended >> 31) != 0) {
                if (position >= declaration.size())
                    return false;
                extended = declaration[position++];
            }
            if (position >= declaration.size())
                return false;
            destination = declaration[position];
            return true;
        }

        [[nodiscard]] bool SelectCloudDeclarations(
            ID3DBlob* snippet, SIE::DXBCPatch& patch) noexcept
        {
            const auto declarations = SIE::DXBCPatcher::ExtractDeclarations(
                snippet,
                { SIE::DXBCOpcodes::DCL_RESOURCE,
                  SIE::DXBCOpcodes::DCL_SAMPLER,
                  SIE::DXBCOpcodes::DCL_CONSTANT_BUFFER });
            for (const auto& declaration : declarations) {
                if (declaration.empty())
                    return false;
                const std::uint32_t opcode = declaration.front() & 0x7FF;
                std::uint32_t slot = 0;
                bool selected = false;
                if (opcode == SIE::DXBCOpcodes::DCL_RESOURCE) {
                    selected = ImmediateDeclarationRegister(
                        declaration, 7, slot) && slot == kCloudTextureSlot;
                } else if (opcode == SIE::DXBCOpcodes::DCL_SAMPLER) {
                    selected = ImmediateDeclarationRegister(
                        declaration, 6, slot) && slot == kCloudSamplerSlot;
                } else if (opcode == SIE::DXBCOpcodes::DCL_CONSTANT_BUFFER) {
                    selected = ImmediateDeclarationRegister(
                        declaration, 8, slot) &&
                        slot == kCloudConstantBufferSlot;
                }
                if (selected)
                    patch.newDeclarations.push_back(declaration);
            }
            return patch.newDeclarations.size() == 3;
        }

        [[nodiscard]] ID3DBlob* BuildPatchedShader(
            const void* bytecode, std::size_t bytecodeSize,
            DirectionalShader variant, bool sunMask = false) noexcept try
        {
            const auto source = FO4CS::GodrayCloudShader::PayloadSource(
                variant == DirectionalShader::kScreenIntegral, sunMask);
            if (source.empty())
                return nullptr;

            ComPtr<ID3DBlob> snippet;
            ComPtr<ID3DBlob> errors;
            const HRESULT compileResult = D3DCompile(
                source.data(), source.size(), "FO4CloudShadowsGodrays",
                nullptr, nullptr, "main", "ps_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS |
                    D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, snippet.GetAddressOf(), errors.GetAddressOf());
            if (FAILED(compileResult) || !snippet) {
                if (errors && errors->GetBufferPointer()) {
                    SPDLOG_ERROR(
                        "[CloudShadows][Godrays] Payload compile failed: {}",
                        static_cast<const char*>(errors->GetBufferPointer()));
                }
                return nullptr;
            }

            ComPtr<ID3DBlob> stock;
            if (FAILED(D3DCreateBlob(bytecodeSize, stock.GetAddressOf())) ||
                !stock) {
                return nullptr;
            }
            std::memcpy(stock->GetBufferPointer(), bytecode, bytecodeSize);

            SIE::DXBCPatcher::ParsedDXBC parsedSnippet{};
            if (snippet->GetBufferSize() >
                    static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) ||
                !SIE::DXBCPatcher::Parse(
                    static_cast<const std::uint8_t*>(
                        snippet->GetBufferPointer()),
                    static_cast<std::uint32_t>(snippet->GetBufferSize()),
                    parsedSnippet) ||
                parsedSnippet.shex.usesDynamicLinkage ||
                parsedSnippet.shex.retOffsets.size() != 1) {
                return nullptr;
            }

            SIE::DXBCPatch patch{};
            const std::uint32_t baseOutputTemp = parsedSnippet.shex.tempCount;
            if (baseOutputTemp >= 4095 || !SelectCloudDeclarations(
                    snippet.Get(), patch)) {
                return nullptr;
            }
            patch.additionalTempRegisters = baseOutputTemp + 1;
            patch.redirectedOutputs.emplace(0, baseOutputTemp);

            std::unordered_map<std::uint32_t, InputRemap> inputRemaps;
            if (!BuildInputRemaps(
                    snippet.Get(), stock.Get(), baseOutputTemp,
                    inputRemaps)) {
                return nullptr;
            }

            const auto& shex = parsedSnippet.shex;
            std::uint32_t position = shex.declEndOffset;
            if (position == (std::numeric_limits<std::uint32_t>::max)())
                return nullptr;
            while (position < shex.instrStreamDwords) {
                const std::uint32_t token = shex.instrStream[position];
                const std::uint32_t opcode = token & 0x7FF;
                std::uint32_t length = (token >> 24) & 0x7F;
                if (length == 0 || position + length > shex.instrStreamDwords)
                    return nullptr;
                if (opcode != SIE::DXBCOpcodes::RET) {
                    std::vector<std::uint32_t> instruction(
                        shex.instrStream + position,
                        shex.instrStream + position + length);
                    std::vector<std::uint32_t> tempPositions;
                    if (!RewriteInstructionInputs(
                            instruction, inputRemaps, tempPositions)) {
                        return nullptr;
                    }
                    patch.preRetInstructions.push_back(std::move(instruction));
                    patch.tempRemapPositions.push_back(
                        std::move(tempPositions));
                    patch.svPositionFixupPositions.emplace_back();
                }
                position += length;
            }
            if (patch.preRetInstructions.empty())
                return nullptr;

            ComPtr<ID3DBlob> patched;
            patched.Attach(SIE::DXBCPatcher::PatchShader(stock.Get(), patch));
            if (!patched || patched->GetBufferSize() >
                    static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
                return nullptr;
            }
            SIE::DXBCPatcher::ParsedDXBC parsedPatched{};
            if (!SIE::DXBCPatcher::Parse(
                    static_cast<const std::uint8_t*>(
                        patched->GetBufferPointer()),
                    static_cast<std::uint32_t>(patched->GetBufferSize()),
                    parsedPatched) ||
                parsedPatched.shex.usesDynamicLinkage ||
                parsedPatched.shex.retOffsets.size() != 1 ||
                parsedPatched.shex.tempCount != 3 +
                    patch.additionalTempRegisters) {
                return nullptr;
            }
            return patched.Detach();
        } catch (...) {
            return nullptr;
        }

        [[nodiscard]] bool EnsureConstantBuffer(ID3D11Device* device) noexcept
        {
            if (!device)
                return false;
            std::lock_guard lock(s_deviceMutex);
            if (s_constantBufferDevice.Get() != device) {
                s_cloudConstantBuffer.Reset();
                s_constantBufferDevice = device;
            }
            if (s_cloudConstantBuffer)
                return true;

            D3D11_BUFFER_DESC description{};
            description.ByteWidth = sizeof(GodrayCloudConstants);
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            const HRESULT result = device->CreateBuffer(
                &description, nullptr, s_cloudConstantBuffer.GetAddressOf());
            if (FAILED(result) || !s_cloudConstantBuffer) {
                SPDLOG_ERROR(
                    "[CloudShadows][Godrays] Failed creating b13: 0x{:08X}",
                    static_cast<std::uint32_t>(result));
                return false;
            }
            return true;
        }

        // Keep SEH around only the POD engine reads. Renderer singletons can be
        // transient during loads; absence or an invalid position leaves the
        // complete native GameWorks draw unchanged.
        [[nodiscard]] bool TryReadExpectedEye(
            DirectX::XMFLOAT3& destination) noexcept
        {
            destination = {};
#if defined(_MSC_VER)
            __try {
#endif
                auto* camera = FO4CS::EngineAPI::GetWorldRootCamera();
                auto* graphicsState = FO4CS::EngineAPI::GetGraphicsState();
                if (!camera || !graphicsState)
                    return false;
                destination = FO4CS::EngineAPI::ReadCameraPosAdjust(
                    graphicsState);
                return std::isfinite(destination.x) &&
                    std::isfinite(destination.y) &&
                    std::isfinite(destination.z);
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                destination = {};
                return false;
            }
#endif
        }

        [[nodiscard]] bool PrepareConstants(
            ID3D11DeviceContext* context,
            std::uint64_t expectedEpoch,
            ID3D11Buffer** destination) noexcept
        {
            if (destination)
                *destination = nullptr;
            if (!context || !destination || expectedEpoch == 0 ||
                CloudShadows::g_worldCloudCommittedEpoch.load(
                    std::memory_order_acquire) != expectedEpoch) {
                return false;
            }

            std::array<CloudShadows::WorldCloudLayerState, 1> layers{};
            if (CloudShadows::CopyCommittedWorldCloudLayers(
                    layers.data(), static_cast<std::uint32_t>(layers.size())) != 1 ||
                layers[0].ActiveBlend <= 0.0f) {
                return false;
            }
            DirectX::XMFLOAT3 origin{};
            if (!CloudShadows::GetCommittedWorldCloudOrigin(origin))
                return false;
            DirectX::XMFLOAT3 expectedEye{};
            if (!TryReadExpectedEye(expectedEye))
                return false;

            ComPtr<ID3D11Device> device;
            context->GetDevice(device.GetAddressOf());
            if (!device || device.Get() != CloudShadows::GetD3DDevice() ||
                !EnsureConstantBuffer(device.Get())) {
                return false;
            }

            GodrayCloudConstants constants{};
            constants.geometryAndStrength = {
                CloudShadows::g_settings.CloudHeight,
                kPlanetRadiusWorld,
                CloudShadows::g_settings.Opacity,
                1.0f
            };
            constants.captureOriginAndBlend = {
                origin.x, origin.y, origin.z,
                std::clamp(layers[0].ActiveBlend, 0.0f, 1.0f)
            };
            constants.expectedEyeAndTolerance = {
                expectedEye.x, expectedEye.y, expectedEye.z,
                kEyeAttestationToleranceWorld
            };
            DirectX::XMFLOAT3 visibleSun{};
            const bool sunValid = FO4CS::EngineAPI::ReadVisibleSunDirection(
                FO4CS::EngineAPI::GetSky(), visibleSun);
            constants.visibleSunDirectionAndValidity = {
                visibleSun.x, visibleSun.y, visibleSun.z, sunValid ? 1.0f : -1.0f
            };
            if (FO4CS::CloudComparison::GetMethod() == FO4CS::CloudComparison::Method::SunMask) {
                if (!CloudShadows::GetCommittedSunMaskProjection(constants.sunProjection)) return false;
                constants.geometryAndStrength.x = constants.sunProjection.upAndHeight.w;
            }

            ComPtr<ID3D11Buffer> buffer;
            {
                std::lock_guard lock(s_deviceMutex);
                buffer = s_cloudConstantBuffer;
            }
            if (!buffer)
                return false;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(
                    buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) ||
                !mapped.pData) {
                return false;
            }
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(buffer.Get(), 0);
            if (CloudShadows::g_worldCloudCommittedEpoch.load(
                    std::memory_order_acquire) != expectedEpoch) {
                return false;
            }
            *destination = buffer.Detach();
            return true;
        }

        struct ScopedRenderVolumePhase
        {
            ScopedRenderVolumePhase() noexcept
            {
                if (s_renderVolumePhase.depth++ == 0) {
                    s_renderVolumePhase.cloudEpoch =
                        CloudShadows::g_worldCloudCommittedEpoch.load(
                            std::memory_order_acquire);
                }
            }
            ~ScopedRenderVolumePhase()
            {
                if (--s_renderVolumePhase.depth == 0)
                    s_renderVolumePhase.cloudEpoch = 0;
            }
        };

        std::int32_t __cdecl HookRenderVolume(
            void* context, ID3D11DeviceContext* deviceContext,
            const void* shadowMapDescription,
            ID3D11ShaderResourceView* shadowMap,
            const void* lightDescription, float depthBias,
            float meshResolution, unsigned int sampleMode,
            float maxDistance)
        {
            s_renderVolumeCalls.fetch_add(1, std::memory_order_relaxed);
            // A real native volume call proves the renderer is already active,
            // even if the INIs were changed before its first initialization.
            s_nativeSettingsRestartRequired.store(false, std::memory_order_release);
            ScopedRenderVolumePhase phase;
            return s_originalRenderVolume(
                context, deviceContext, shadowMapDescription, shadowMap,
                lightDescription, depthBias, meshResolution, sampleMode,
                maxDistance);
        }
    }

    bool IsCloudOcclusionEnabled() noexcept
    {
        return s_cloudOcclusionEnabled.load(std::memory_order_acquire);
    }

    void SetCloudOcclusionEnabled(bool enabled) noexcept
    {
        if (IsCloudOcclusionEnabled() == enabled)
            return;
        // Retain lifetime counters, but require fresh submissions after each
        // enable. An old successful draw must not make a new test appear active.
        s_enableDrawBaseline.store(
            s_submittedDraws.load(std::memory_order_acquire),
            std::memory_order_release);
        s_lastSubmittedEpoch.store(0, std::memory_order_release);
        s_cloudOcclusionEnabled.store(enabled, std::memory_order_release);
        if (enabled)
            EnsureNativeGodraysEnabled();
        SPDLOG_INFO("[CloudShadows][Godrays] Cloud occlusion {}",
            enabled ? "ON" : "OFF");
    }

    void EnsureNativeGodraysEnabled() noexcept
    {
        if (!s_nativeConsumerSupported.load(std::memory_order_acquire))
            return;
        try {
            const auto directory = GodrayGameSettings::Directory();
            const auto result = GodrayGameSettings::Enable(directory);
            s_nativeSettingsEnabled.store(result.succeeded, std::memory_order_release);
            s_nativeSettingsError.store(!result.succeeded, std::memory_order_release);
            if (!result.succeeded) {
                SPDLOG_ERROR("[CloudShadows][Godrays] Native settings enable failed in {}: {}",
                    directory.string(), result.error);
            } else if (result.changed) {
                s_nativeSettingsRestartRequired.store(true, std::memory_order_release);
                SPDLOG_INFO("[CloudShadows][Godrays] Enabled Fallout godrays in {}; "
                    "original INIs backed up. Save mod settings and restart Fallout.",
                    directory.string());
            }
        } catch (const std::exception& error) {
            s_nativeSettingsError.store(true, std::memory_order_release);
            SPDLOG_ERROR("[CloudShadows][Godrays] Native settings enable failed: {}",
                error.what());
        }
    }

    bool TryInstall(F4SECompat::RuntimeTarget target) noexcept
    {
        if (target == F4SECompat::RuntimeTarget::kVR) {
            s_nativeConsumerSupported.store(false, std::memory_order_release);
            if (!s_vrUnsupportedLogged.exchange(true)) {
                SPDLOG_WARN(
                    "[CloudShadows][Godrays] VR 1.2.72 has no native "
                    "GFSDK Godrays consumer; cloud godray occlusion is "
                    "explicitly unsupported (no screen-tint substitute)");
            }
            return false;
        }
        if (target != F4SECompat::RuntimeTarget::kLegacy &&
            target != F4SECompat::RuntimeTarget::kAE) {
            return false;
        }
        s_nativeConsumerSupported.store(true, std::memory_order_release);
        if (s_hookInstalled.load(std::memory_order_acquire))
            return true;

        std::lock_guard lock(s_installMutex);
        if (s_hookInstalled.load(std::memory_order_relaxed))
            return true;
        HMODULE module = GetModuleHandleW(L"GFSDK_GodraysLib.x64.dll");
        if (!module)
            return false;
        // The shipped x64 OG/AE library exports the C++ decorated name, not
        // the spelling in the SDK header. Its encoded argument list matches
        // RenderVolumeFn above. Resolve only these exact ABI names; do not
        // guess an ordinal, match a name prefix, or use an executable RVA.
        constexpr const char* exportNames[]{
            "?GFSDK_GodraysLib_RenderVolume@@YA?AW4GFSDK_GodraysLib_Status@@"
            "PEAUGFSDK_GodraysLib_Ctx@@PEAUID3D11DeviceContext@@"
            "PEBUGFSDK_GodraysLib_ShadowMapDesc@@PEAUID3D11ShaderResourceView@@"
            "PEBUGFSDK_GodraysLib_LightDesc@@MMIM@Z",
            "GFSDK_GodraysLib_RenderVolume"
        };
        RenderVolumeFn exported = nullptr;
        for (const char* name : exportNames) {
            const auto candidate = reinterpret_cast<RenderVolumeFn>(
                GetProcAddress(module, name));
            if (candidate && IsAddressInsideModule(
                    reinterpret_cast<const void*>(candidate), module)) {
                exported = candidate;
                break;
            }
        }
        if (!exported || !IsAddressInsideModule(
                reinterpret_cast<const void*>(exported), module)) {
            SPDLOG_WARN(
                "[CloudShadows][Godrays] Named RenderVolume export is "
                "missing or outside GFSDK_GodraysLib.x64.dll; vanilla retained");
            return false;
        }
        s_exportResolved.store(true, std::memory_order_release);
        s_originalRenderVolume = exported;

        DetourThreadEnlistment threads;
        LONG result = DetourTransactionBegin();
        if (result == NO_ERROR)
            result = threads.EnlistProcessThreads();
        if (result == NO_ERROR) {
            result = DetourAttach(
                reinterpret_cast<PVOID*>(&s_originalRenderVolume),
                reinterpret_cast<PVOID>(&HookRenderVolume));
        }
        if (result != NO_ERROR) {
            DetourTransactionAbort();
            s_originalRenderVolume = nullptr;
            SPDLOG_WARN(
                "[CloudShadows][Godrays] RenderVolume detour rejected "
                "({}); vanilla retained", result);
            return false;
        }
        result = DetourTransactionCommit();
        if (result != NO_ERROR) {
            s_originalRenderVolume = nullptr;
            SPDLOG_WARN(
                "[CloudShadows][Godrays] RenderVolume detour commit failed "
                "({}); vanilla retained", result);
            return false;
        }
        s_hookInstalled.store(true, std::memory_order_release);
        SPDLOG_INFO(
            "[CloudShadows][Godrays] Named RenderVolume hook installed at {} "
            "with exact directional PS identities {} and {}",
            reinterpret_cast<const void*>(exported),
            kKnownDirectionalShaders[0].sha256,
            kKnownDirectionalShaders[1].sha256);
        return true;
    }

    void ObservePixelShaderCreated(
        ID3D11Device* device,
        CreatePixelShaderFn createPixelShader,
        const void* stockBytecode,
        std::size_t stockBytecodeLength,
        ID3D11PixelShader* stockShader) noexcept
    {
        const DirectionalShader variant = IdentifyDirectionalShader(
            stockBytecode, stockBytecodeLength);
        if (variant == DirectionalShader::kUnsupported)
            return;
        if (!device || !createPixelShader || !stockShader ||
            !HasExpectedStockStructure(
                stockBytecode, stockBytecodeLength, variant)) {
            s_patchFailures.fetch_add(1, std::memory_order_relaxed);
            SPDLOG_WARN(
                "[CloudShadows][Godrays] Exact directional hash had an "
                "unexpected DXBC structure; vanilla retained");
            return;
        }

        try {
            ComPtr<ID3DBlob> patchedBlob;
            patchedBlob.Attach(BuildPatchedShader(
                stockBytecode, stockBytecodeLength, variant));
            if (!patchedBlob) {
                s_patchFailures.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_ERROR(
                    "[CloudShadows][Godrays] Authenticated directional "
                    "payload patch failed closed for variant {}",
                    static_cast<std::uint32_t>(variant));
                return;
            }

            ComPtr<ID3D11PixelShader> patchedShader;
            const HRESULT createResult = createPixelShader(
                device, patchedBlob->GetBufferPointer(),
                patchedBlob->GetBufferSize(), nullptr,
                patchedShader.GetAddressOf());
            if (FAILED(createResult) || !patchedShader) {
                s_patchFailures.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_ERROR(
                    "[CloudShadows][Godrays] Driver rejected authenticated "
                    "directional payload variant {}: 0x{:08X}",
                    static_cast<std::uint32_t>(variant),
                    static_cast<std::uint32_t>(createResult));
                return;
            }
            const HRESULT attachResult = stockShader->SetPrivateDataInterface(
                kPatchedGodrayShaderMarker, patchedShader.Get());
            if (FAILED(attachResult)) {
                s_patchFailures.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_ERROR(
                    "[CloudShadows][Godrays] Could not attach replacement "
                    "lifetime: 0x{:08X}",
                    static_cast<std::uint32_t>(attachResult));
                return;
            }
            ComPtr<ID3DBlob> sunBlob;
            sunBlob.Attach(BuildPatchedShader(stockBytecode, stockBytecodeLength, variant, true));
            ComPtr<ID3D11PixelShader> sunShader;
            if (!sunBlob || FAILED(createPixelShader(device, sunBlob->GetBufferPointer(),
                    sunBlob->GetBufferSize(), nullptr, &sunShader)) ||
                FAILED(stockShader->SetPrivateDataInterface(kPatchedSunMaskGodrayShaderMarker, sunShader.Get()))) {
                s_patchFailures.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_ERROR("[CloudShadows][Godrays] Sun-mask variant unavailable; its draws retain vanilla rays");
            } else {
                s_authenticatedSunMaskVariants.fetch_or(
                    variant == DirectionalShader::kVolumeGeometry ? 1u : 2u, std::memory_order_release);
            }
            const std::uint32_t count =
                s_authenticatedShaderObjects.fetch_add(
                    1, std::memory_order_release) + 1;
            const std::uint32_t variantBit =
                variant == DirectionalShader::kVolumeGeometry ? 1u : 2u;
            s_authenticatedDirectionalVariants.fetch_or(
                variantBit, std::memory_order_release);
            SPDLOG_INFO(
                "[CloudShadows][Godrays] Authenticated directional PS #{} "
                "variant={} stock={} replacement={}",
                count, static_cast<std::uint32_t>(variant),
                static_cast<void*>(stockShader),
                static_cast<void*>(patchedShader.Get()));
        } catch (...) {
            s_patchFailures.fetch_add(1, std::memory_order_relaxed);
            SPDLOG_ERROR(
                "[CloudShadows][Godrays] Directional shader preparation "
                "threw; vanilla retained");
        }
    }

    DrawSwapState BeginDraw(ID3D11DeviceContext* context) noexcept
    {
        DrawSwapState state{};
        if (!IsCloudOcclusionEnabled() ||
            !context || s_renderVolumePhase.depth == 0 ||
            !s_hookInstalled.load(std::memory_order_acquire) ||
            !CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire) ||
            context != CloudShadows::GetD3DContext()) {
            return state;
        }
        const std::uint64_t epoch = s_renderVolumePhase.cloudEpoch;
        if (epoch == 0 ||
            CloudShadows::g_worldCloudCommittedEpoch.load(
                std::memory_order_acquire) != epoch) {
            return state;
        }

        UINT classCount = 0;
        context->PSGetShader(&state.vanillaPS, nullptr, &classCount);
        if (!state.vanillaPS || classCount != 0) {
            if (state.vanillaPS)
                state.vanillaPS->Release();
            state.vanillaPS = nullptr;
            return state;
        }

        ComPtr<ID3D11PixelShader> replacement;
        UINT replacementSize = sizeof(ID3D11PixelShader*);
        ID3D11PixelShader* rawReplacement = nullptr;
        const HRESULT replacementResult = state.vanillaPS->GetPrivateData(
            FO4CS::CloudComparison::GetMethod() == FO4CS::CloudComparison::Method::SunMask
                ? kPatchedSunMaskGodrayShaderMarker : kPatchedGodrayShaderMarker,
            &replacementSize, &rawReplacement);
        if (FAILED(replacementResult) ||
            replacementSize != sizeof(ID3D11PixelShader*) ||
            !rawReplacement) {
            state.vanillaPS->Release();
            state.vanillaPS = nullptr;
            return state;
        }
        replacement.Attach(rawReplacement);

        ComPtr<ID3D11Buffer> cloudConstants;
        ID3D11Buffer* rawConstants = nullptr;
        if (!PrepareConstants(context, epoch, &rawConstants)) {
            state.vanillaPS->Release();
            state.vanillaPS = nullptr;
            return state;
        }
        cloudConstants.Attach(rawConstants);
        ID3D11ShaderResourceView* cloudCube =
            CloudShadows::GetCommittedWorldCloudTiles();
        ID3D11SamplerState* cloudSampler =
            CloudShadows::GetWorldCloudSampler();
        if (!cloudCube || !cloudSampler) {
            state.vanillaPS->Release();
            state.vanillaPS = nullptr;
            return state;
        }

        context->PSGetShaderResources(
            kCloudTextureSlot, 1, &state.previousT47);
        context->PSGetSamplers(
            kCloudSamplerSlot, 1, &state.previousS15);
        ComPtr<ID3D11DeviceContext1> context1;
        if (SUCCEEDED(context->QueryInterface(
                __uuidof(ID3D11DeviceContext1),
                reinterpret_cast<void**>(context1.GetAddressOf()))) &&
            context1) {
            context1->PSGetConstantBuffers1(
                kCloudConstantBufferSlot, 1, &state.previousB13,
                &state.previousB13FirstConstant,
                &state.previousB13ConstantCount);
            state.context1 = context1.Detach();
            state.previousB13UsedRange = true;
        } else {
            context->PSGetConstantBuffers(
                kCloudConstantBufferSlot, 1, &state.previousB13);
        }
        context->PSSetShader(replacement.Get(), nullptr, 0);
        context->PSSetShaderResources(kCloudTextureSlot, 1, &cloudCube);
        context->PSSetSamplers(kCloudSamplerSlot, 1, &cloudSampler);
        ID3D11Buffer* constantBuffer = cloudConstants.Get();
        context->PSSetConstantBuffers(
            kCloudConstantBufferSlot, 1, &constantBuffer);
        state.swapped = true;
        state.cloudEpoch = epoch;
        return state;
    }

    void EndDraw(
        ID3D11DeviceContext* context,
        DrawSwapState& state,
        bool submitted) noexcept
    {
        if (state.swapped && context) {
            context->PSSetShader(state.vanillaPS, nullptr, 0);
            context->PSSetShaderResources(
                kCloudTextureSlot, 1, &state.previousT47);
            context->PSSetSamplers(
                kCloudSamplerSlot, 1, &state.previousS15);
            if (state.previousB13UsedRange && state.context1) {
                state.context1->PSSetConstantBuffers1(
                    kCloudConstantBufferSlot, 1, &state.previousB13,
                    &state.previousB13FirstConstant,
                    &state.previousB13ConstantCount);
            } else {
                context->PSSetConstantBuffers(
                    kCloudConstantBufferSlot, 1, &state.previousB13);
            }
            if (submitted) {
                const std::uint64_t count = s_submittedDraws.fetch_add(
                    1, std::memory_order_release) + 1;
                s_lastSubmittedEpoch.store(
                    state.cloudEpoch, std::memory_order_release);
                if (count == 1 || count == 100 || count % 5000 == 0) {
                    SPDLOG_INFO(
                        "[CloudShadows][Godrays] Submitted cloud-occluded "
                        "directional volume draw #{} epoch={}",
                        count, state.cloudEpoch);
                }
            }
        }
        if (state.previousB13)
            state.previousB13->Release();
        if (state.previousS15)
            state.previousS15->Release();
        if (state.previousT47)
            state.previousT47->Release();
        if (state.vanillaPS)
            state.vanillaPS->Release();
        if (state.context1)
            state.context1->Release();
        state = {};
    }

    void ReleaseDeviceResources() noexcept
    {
        std::lock_guard lock(s_deviceMutex);
        s_cloudConstantBuffer.Reset();
        s_constantBufferDevice.Reset();
        // Shader objects and submitted draws belong to the renderer device
        // which created them.  Do not let a completed proof from a retired
        // device make the replacement device appear authenticated before its
        // own exact GameWorks objects have been observed and used.
        s_authenticatedShaderObjects.store(0, std::memory_order_release);
        s_authenticatedDirectionalVariants.store(0, std::memory_order_release);
        s_authenticatedSunMaskVariants.store(0, std::memory_order_release);
        s_patchFailures.store(0, std::memory_order_release);
        s_submittedDraws.store(0, std::memory_order_release);
        s_enableDrawBaseline.store(0, std::memory_order_release);
        s_lastSubmittedEpoch.store(0, std::memory_order_release);
    }

    Diagnostics GetDiagnostics() noexcept
    {
        const bool enabled = IsCloudOcclusionEnabled();
        const auto submitted = s_submittedDraws.load(std::memory_order_acquire);
        const auto baseline = s_enableDrawBaseline.load(std::memory_order_acquire);
        return {
            .cloudOcclusionEnabled = enabled,
            .nativeConsumerSupported = s_nativeConsumerSupported.load(
                std::memory_order_acquire),
            .nativeSettingsEnabled = s_nativeSettingsEnabled.load(std::memory_order_acquire),
            .nativeSettingsError = s_nativeSettingsError.load(std::memory_order_acquire),
            .nativeSettingsRestartRequired = s_nativeSettingsRestartRequired.load(
                std::memory_order_acquire),
            .renderVolumeExportResolved = s_exportResolved.load(
                std::memory_order_acquire),
            .renderVolumeHookInstalled = s_hookInstalled.load(
                std::memory_order_acquire),
            .authenticatedShaderObjects =
                s_authenticatedShaderObjects.load(std::memory_order_acquire),
            .authenticatedDirectionalVariants =
                s_authenticatedDirectionalVariants.load(
                    std::memory_order_acquire),
            .authenticatedSunMaskVariants = s_authenticatedSunMaskVariants.load(std::memory_order_acquire),
            .patchFailures = s_patchFailures.load(std::memory_order_acquire),
            .renderVolumeCalls = s_renderVolumeCalls.load(
                std::memory_order_acquire),
            .submittedCloudOcclusionDraws = submitted,
            .submittedDrawsSinceEnable = enabled && submitted >= baseline ? submitted - baseline : 0,
            .lastSubmittedCloudEpoch = s_lastSubmittedEpoch.load(
                std::memory_order_acquire)
        };
    }
}
#else
// The first release does not intercept, patch, or configure native godrays.
// Old JSON preferences and other menu hosts cannot accidentally enable them.
namespace FO4CS::GodraysIntegration
{
    bool IsCloudOcclusionEnabled() noexcept { return false; }
    void SetCloudOcclusionEnabled(bool) noexcept {}
    void EnsureNativeGodraysEnabled() noexcept {}
    bool TryInstall(F4SECompat::RuntimeTarget) noexcept { return false; }
    void ObservePixelShaderCreated(ID3D11Device*, CreatePixelShaderFn,
        const void*, std::size_t, ID3D11PixelShader*) noexcept {}
    DrawSwapState BeginDraw(ID3D11DeviceContext*) noexcept { return {}; }
    void EndDraw(ID3D11DeviceContext*, DrawSwapState&, bool) noexcept {}
    void ReleaseDeviceResources() noexcept {}
    Diagnostics GetDiagnostics() noexcept { return {}; }
}
#endif
