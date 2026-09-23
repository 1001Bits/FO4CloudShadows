// SPDX-License-Identifier: GPL-3.0-only
#include "CloudMotionResolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include <DirectXMath.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

namespace FO4CS::CloudMotionResolver
{
    namespace
    {
        constexpr std::uint32_t kThreadGroupSize = 8;
        constexpr std::uint32_t kResolverSrvCount = 5;
        constexpr std::uint32_t kResolverSamplerCount = 2;
        constexpr std::uint32_t kResolverConstantBufferCount = 3;
        constexpr std::uint32_t kOutputCubeCount = 3;
        constexpr std::uint32_t kSkyConstantBufferSlot = 2;
        constexpr std::uint32_t kCapturedStateStride = 16;
        constexpr std::uint32_t kNoOutput =
            (std::numeric_limits<std::uint32_t>::max)();

        constexpr const char* kResolverShaderSource = R"hlsl(
cbuffer ResolverConstants : register(b0)
{
    uint2 OutputSize;
    uint TechniqueKind;
    uint HasPriorComposite;
    uint ConstantLayout;
    uint3 ResolverPadding;
};

cbuffer LiveSkyVertexConstants : register(b1)
{
    float4 LiveSkyVertex[15];
};

cbuffer LiveSkyPixelConstants : register(b2)
{
    float4 LiveSkyPixel[1];
};

Texture2DArray<float4> UvMapping : register(t0);
Texture2D<float4> CloudTexture0 : register(t1);
Texture2D<float4> CloudTexture1 : register(t2);
Texture2DArray<float> PriorComposite : register(t3);
ByteAddressBuffer CapturedState : register(t4);
SamplerState CloudSampler0 : register(s0);
SamplerState CloudSampler1 : register(s1);
RWTexture2DArray<float> ResolvedComposite : register(u0);

// Portable finite test. Proton/Wine's vkd3d-based d3dcompiler lacks the
// isfinite intrinsic, and fxc may fold NaN comparisons; the exponent bit test
// is exact IEEE-754 on both compilers. A macro, not overloads: vkd3d cannot
// prioritize between compatible overloads (E5017).
#define IS_FINITE(x) ((asuint(x) & 0x7F800000u) != 0x7F800000u)
bool MappingValueValid(float4 value)
{
    return value.w > 0.5 && all(IS_FINITE(value));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    if (dispatchThreadID.x >= OutputSize.x ||
        dispatchThreadID.y >= OutputSize.y || dispatchThreadID.z >= 6)
        return;

    int3 pixel = int3(dispatchThreadID);
    float prior = HasPriorComposite != 0
        ? PriorComposite.Load(int4(pixel, 0)) : 0.0;
    float alpha = 0.0;
    float4 mapping = UvMapping.Load(int4(pixel, 0));
    float4 captured = asfloat(CapturedState.Load4(dispatchThreadID.z * 16u));

    uint offsetIndex = ConstantLayout == 1 ? 14 : 10;
    uint blendIndex = ConstantLayout == 1 ? 11 : 7;
    float2 liveOffset = LiveSkyVertex[offsetIndex].xy;
    float liveBlend = LiveSkyVertex[blendIndex].w;
    float techniqueParameter = LiveSkyPixel[0].x;

    bool parameterValid = TechniqueKind == 5 ||
        IS_FINITE(techniqueParameter);
    bool metadataValid = MappingValueValid(mapping) && captured.w > 0.5 &&
        all(IS_FINITE(captured)) && all(IS_FINITE(liveOffset)) &&
        IS_FINITE(liveBlend) && parameterValid;
    if (metadataValid && captured.z > 1.0e-6) {
        // TexCoordOff is constant over a draw. Neighbor differences therefore
        // remain valid after rebasing each face from its natural capture time.
        // They are exact inside one linear primitive; seams and primitive
        // boundaries remain the explicitly reported approximation.
        uint2 quadBase = (dispatchThreadID.xy / 2u) * 2u;
        uint leftX = min(quadBase.x, OutputSize.x - 1u);
        uint rightX = min(quadBase.x + 1u, OutputSize.x - 1u);
        uint topY = min(quadBase.y, OutputSize.y - 1u);
        uint bottomY = min(quadBase.y + 1u, OutputSize.y - 1u);
        float4 left = UvMapping.Load(int4(
            int3(leftX, dispatchThreadID.y, dispatchThreadID.z), 0));
        float4 right = UvMapping.Load(int4(
            int3(rightX, dispatchThreadID.y, dispatchThreadID.z), 0));
        float4 top = UvMapping.Load(int4(
            int3(dispatchThreadID.x, topY, dispatchThreadID.z), 0));
        float4 bottom = UvMapping.Load(int4(
            int3(dispatchThreadID.x, bottomY, dispatchThreadID.z), 0));
        float2 gradientX = MappingValueValid(left) && MappingValueValid(right)
            ? right.xy - left.xy : 0.0;
        float2 gradientY = MappingValueValid(top) && MappingValueValid(bottom)
            ? bottom.xy - top.xy : 0.0;
        float2 liveUv = mapping.xy + liveOffset - captured.xy;

        if (all(IS_FINITE(liveUv)) && all(IS_FINITE(gradientX)) &&
            all(IS_FINITE(gradientY))) {
            float4 source = CloudTexture0.SampleGrad(
                CloudSampler0, liveUv, gradientX, gradientY);
            if (TechniqueKind == 6) {
                float4 second = CloudTexture1.SampleGrad(
                    CloudSampler1, liveUv, gradientX, gradientY);
                source = lerp(source, second, techniqueParameter);
            }
            float fade = TechniqueKind == 7
                ? saturate(techniqueParameter - 0.4) * (5.0 / 3.0)
                : 1.0;
            float liveVertexAlpha = mapping.z * (liveBlend / captured.z);
            if (all(IS_FINITE(source)) && IS_FINITE(fade) &&
                IS_FINITE(liveVertexAlpha)) {
                alpha = saturate(source.a * liveVertexAlpha * fade);
            }
        }
    }

    // Native MRT data with zero capture BlendW cannot recover mesh alpha.
    // Private geometry mappings preserve raw mesh attributes with baseline
    // BlendW=1, so an initially transparent layer can become visible normally.
    prior = IS_FINITE(prior) ? saturate(prior) : 0.0;
    ResolvedComposite[pixel] = saturate(alpha + prior * (1.0 - alpha));
}
)hlsl";

        struct alignas(16) ResolverConstants
        {
            DirectX::XMUINT2 outputSize{};
            std::uint32_t technique{};
            std::uint32_t hasPriorComposite{};
            std::uint32_t constantLayout{};
            std::uint32_t padding[3]{};
        };
        static_assert(sizeof(ResolverConstants) == 32);

        struct OutputCube
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cubeSrv;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> arraySrv;
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> arrayUav;
        };

        struct CapturedLayerState
        {
            std::uint64_t stableLayerId{};
            Microsoft::WRL::ComPtr<ID3D11Texture2D> mappingTexture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mappingArraySrv;
            std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>,
                kCloudCubeFaceCount> mappingRtvs{};
            Microsoft::WRL::ComPtr<ID3D11Buffer> capturedStateBuffer;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> capturedStateSrv;
            std::array<CloudTechnique, kCloudCubeFaceCount> techniques{};
            std::uint8_t faceMask{};
        };

        struct CaptureStagingState
        {
            CaptureGenerationDesc description{};
            std::vector<CapturedLayerState> layers;
            std::uint8_t begunFaceMask{};
            std::uint8_t completedFaceMask{};
            std::uint32_t pendingFaceIndex{};
            std::uint64_t pendingLayerId{};
            CloudTechnique pendingTechnique{ CloudTechnique::kClouds };
            bool active{};
            bool layerPending{};
            bool failed{};
        };

        struct FrameState
        {
            FrameDesc description{};
            std::array<std::uint32_t, 2> scratch{ kNoOutput, kNoOutput };
            std::array<std::uint64_t, kMaximumResolvedLayers> seenLayerIds{};
            std::uint32_t currentOutput{ kNoOutput };
            std::uint32_t observedLayerCount{};
            bool active{};
            bool failed{};
        };

        struct ResolverState
        {
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader;
            Microsoft::WRL::ComPtr<ID3D11Buffer> resolverConstants;
            std::array<OutputCube, kOutputCubeCount> outputs{};
            CaptureStagingState staging;
            std::vector<CapturedLayerState> capturedLayers;
            // Reuse retired mappings after the prior frame has finished. At
            // most two complete layer sets exist, including this spare pool.
            std::array<CapturedLayerState, kMaximumResolvedLayers> spareLayers{};
            std::uint32_t spareLayerCount{};
            std::uint64_t mappingResourceCreates{};
            std::uint64_t mappingResourceReuses{};
            FrameState frame;
            SkyConstantLayout activeLayout{ SkyConstantLayout::kFlat };
            DXGI_FORMAT outputFormat{ DXGI_FORMAT_UNKNOWN };
            std::uint32_t faceSize{};
            std::uint32_t publishedOutput{ kNoOutput };
            std::uint32_t publishedLayerCount{};
            std::uint64_t captureEpoch{};
            std::uint64_t worldGeneration{};
            std::uint64_t resolvedFrameSerial{};
            std::uint64_t lastAttemptedFrameSerial{};
            bool captureValid{};
            bool resolvedValid{};
            std::uint64_t publishedCaptureSets{};
            std::uint64_t resolvedFrames{};
            std::uint64_t rejectedCaptureOperations{};
            std::uint64_t rejectedFrames{};
        };

        struct NativeConstantBinding
        {
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            std::uint32_t firstConstant{};
            std::uint32_t constantCount{};
        };

        std::mutex s_mutex;
        ResolverState s_state;

        [[nodiscard]] bool SupportedTechnique(CloudTechnique technique) noexcept
        {
            return technique == CloudTechnique::kClouds ||
                technique == CloudTechnique::kCloudsLerp ||
                technique == CloudTechnique::kCloudsFade;
        }

        [[nodiscard]] bool SupportedLayout(SkyConstantLayout layout) noexcept
        {
            return layout == SkyConstantLayout::kFlat ||
                layout == SkyConstantLayout::kVr;
        }

        [[nodiscard]] bool ShouldLogCount(std::uint64_t count) noexcept
        {
            return count <= 4 || (count & (count - 1u)) == 0;
        }

        template <class T>
        [[nodiscard]] bool BelongsToDevice(
            T* child, ID3D11Device* expected) noexcept
        {
            if (!child || !expected)
                return false;
            Microsoft::WRL::ComPtr<ID3D11Device> actual;
            child->GetDevice(actual.GetAddressOf());
            return actual.Get() == expected;
        }

        void WithdrawResolvedUnlocked() noexcept
        {
            s_state.resolvedValid = false;
            // WorldClouds deliberately retains the last Present-published SRV
            // while a replacement mapping generation is handed off. Keep its
            // output index reserved even though it is no longer a snapshot of
            // the new capture epoch; otherwise BeginFrame could select and
            // overwrite a cube that the renderer is still sampling.
            s_state.publishedLayerCount = 0;
        }

        bool RejectCaptureUnlocked(
            const char* reason, bool poison = true) noexcept
        {
            if (poison) {
                // Capture construction is transactional. A malformed refresh
                // poisons only its staging generation; the last fully
                // published mapping and resolved cube remain authoritative.
                s_state.staging.failed = s_state.staging.active;
            }
            const auto count = ++s_state.rejectedCaptureOperations;
            if (ShouldLogCount(count)) {
                SPDLOG_WARN(
                    "[CloudShadows][MotionResolver] Capture operation "
                    "rejected fail-neutral (reason={} count={})",
                    reason, count);
            }
            return false;
        }

        bool RejectFrameUnlocked(const char* reason) noexcept
        {
            s_state.frame.failed = s_state.frame.active;
            WithdrawResolvedUnlocked();
            const auto count = ++s_state.rejectedFrames;
            if (ShouldLogCount(count)) {
                SPDLOG_WARN(
                    "[CloudShadows][MotionResolver] Frame rejected "
                    "fail-neutral (reason={} count={})", reason, count);
            }
            return false;
        }

        void RecycleLayersUnlocked(std::vector<CapturedLayerState>& layers) noexcept
        {
            for (auto& layer : layers) {
                if (s_state.spareLayerCount < kMaximumResolvedLayers)
                    s_state.spareLayers[s_state.spareLayerCount++] = std::move(layer);
            }
            layers.clear();
        }

        void ResetStagingUnlocked() noexcept
        {
            RecycleLayersUnlocked(s_state.staging.layers);
            auto storage = std::move(s_state.staging.layers);
            s_state.staging = {};
            s_state.staging.layers = std::move(storage);
        }

        void InvalidateCaptureUnlocked() noexcept
        {
            ResetStagingUnlocked();
            RecycleLayersUnlocked(s_state.capturedLayers);
            s_state.frame = {};
            s_state.captureEpoch = 0;
            s_state.worldGeneration = 0;
            s_state.resolvedFrameSerial = 0;
            s_state.lastAttemptedFrameSerial = 0;
            s_state.captureValid = false;
            WithdrawResolvedUnlocked();
        }

        void ReleaseResourcesUnlocked() noexcept
        {
            const auto publishedCaptureSets = s_state.publishedCaptureSets;
            const auto resolvedFrames = s_state.resolvedFrames;
            const auto rejectedCaptureOperations =
                s_state.rejectedCaptureOperations;
            const auto rejectedFrames = s_state.rejectedFrames;
            const auto mappingCreates = s_state.mappingResourceCreates;
            const auto mappingReuses = s_state.mappingResourceReuses;
            s_state = {};
            s_state.publishedCaptureSets = publishedCaptureSets;
            s_state.resolvedFrames = resolvedFrames;
            s_state.rejectedCaptureOperations = rejectedCaptureOperations;
            s_state.rejectedFrames = rejectedFrames;
            s_state.mappingResourceCreates = mappingCreates;
            s_state.mappingResourceReuses = mappingReuses;
        }

        [[nodiscard]] bool ContextMatchesDevice(
            ID3D11DeviceContext* context) noexcept
        {
            if (!context || !s_state.device ||
                context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
                return false;
            }
            Microsoft::WRL::ComPtr<ID3D11Device> device;
            context->GetDevice(device.GetAddressOf());
            return device.Get() == s_state.device.Get();
        }

        [[nodiscard]] bool FormatSupportsResolvedCube(
            ID3D11Device* device, DXGI_FORMAT format) noexcept
        {
            UINT support = 0;
            constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
                D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
            return device &&
                SUCCEEDED(device->CheckFormatSupport(format, &support)) &&
                (support & required) == required;
        }

        [[nodiscard]] bool CreateOutputCube(
            ID3D11Device* device, std::uint32_t faceSize,
            DXGI_FORMAT format, OutputCube& destination) noexcept
        {
            D3D11_TEXTURE2D_DESC textureDescription{};
            textureDescription.Width = faceSize;
            textureDescription.Height = faceSize;
            textureDescription.MipLevels = 1;
            textureDescription.ArraySize = kCloudCubeFaceCount;
            textureDescription.Format = format;
            textureDescription.SampleDesc.Count = 1;
            textureDescription.Usage = D3D11_USAGE_DEFAULT;
            textureDescription.BindFlags =
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            textureDescription.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

            OutputCube candidate;
            if (FAILED(device->CreateTexture2D(
                    &textureDescription, nullptr,
                    candidate.texture.GetAddressOf()))) {
                return false;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC cubeDescription{};
            cubeDescription.Format = format;
            cubeDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            cubeDescription.TextureCube.MostDetailedMip = 0;
            cubeDescription.TextureCube.MipLevels = 1;
            if (FAILED(device->CreateShaderResourceView(
                    candidate.texture.Get(), &cubeDescription,
                    candidate.cubeSrv.GetAddressOf()))) {
                return false;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC arrayDescription{};
            arrayDescription.Format = format;
            arrayDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            arrayDescription.Texture2DArray.MostDetailedMip = 0;
            arrayDescription.Texture2DArray.MipLevels = 1;
            arrayDescription.Texture2DArray.FirstArraySlice = 0;
            arrayDescription.Texture2DArray.ArraySize = kCloudCubeFaceCount;
            if (FAILED(device->CreateShaderResourceView(
                    candidate.texture.Get(), &arrayDescription,
                    candidate.arraySrv.GetAddressOf()))) {
                return false;
            }

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescription{};
            uavDescription.Format = format;
            uavDescription.ViewDimension =
                D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            uavDescription.Texture2DArray.MipSlice = 0;
            uavDescription.Texture2DArray.FirstArraySlice = 0;
            uavDescription.Texture2DArray.ArraySize = kCloudCubeFaceCount;
            if (FAILED(device->CreateUnorderedAccessView(
                    candidate.texture.Get(), &uavDescription,
                    candidate.arrayUav.GetAddressOf()))) {
                return false;
            }

            destination = std::move(candidate);
            return true;
        }

        [[nodiscard]] bool EnsureResourcesUnlocked(
            ID3D11Device* device, std::uint32_t faceSize) noexcept
        {
            if (!device || faceSize == 0 ||
                faceSize > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
                return false;
            }
            if (s_state.device.Get() == device &&
                s_state.faceSize == faceSize && s_state.computeShader &&
                s_state.resolverConstants &&
                std::all_of(s_state.outputs.begin(), s_state.outputs.end(),
                    [](const OutputCube& output) {
                        return output.texture && output.cubeSrv &&
                            output.arraySrv && output.arrayUav;
                    })) {
                return true;
            }

            Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> errors;
            const HRESULT compileResult = D3DCompile(
                kResolverShaderSource, std::strlen(kResolverShaderSource),
                "FO4CloudMotionResolver", nullptr, nullptr, "main", "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS |
                    D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, shaderBlob.GetAddressOf(), errors.GetAddressOf());
            if (FAILED(compileResult) || !shaderBlob) {
                if (errors && errors->GetBufferPointer()) {
                    SPDLOG_ERROR(
                        "[CloudShadows][MotionResolver] Compute compile "
                        "failed: {}",
                        static_cast<const char*>(errors->GetBufferPointer()));
                }
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader;
            if (FAILED(device->CreateComputeShader(
                    shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
                    nullptr, computeShader.GetAddressOf()))) {
                return false;
            }

            D3D11_BUFFER_DESC constantsDescription{};
            constantsDescription.ByteWidth = sizeof(ResolverConstants);
            constantsDescription.Usage = D3D11_USAGE_DYNAMIC;
            constantsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            constantsDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            Microsoft::WRL::ComPtr<ID3D11Buffer> resolverConstants;
            if (FAILED(device->CreateBuffer(
                    &constantsDescription, nullptr,
                    resolverConstants.GetAddressOf()))) {
                return false;
            }

            std::array<OutputCube, kOutputCubeCount> outputs{};
            DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;
            for (const DXGI_FORMAT candidateFormat : {
                     DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32_FLOAT }) {
                if (!FormatSupportsResolvedCube(device, candidateFormat))
                    continue;
                std::array<OutputCube, kOutputCubeCount> candidates{};
                bool created = true;
                for (auto& output : candidates) {
                    if (!CreateOutputCube(
                            device, faceSize, candidateFormat, output)) {
                        created = false;
                        break;
                    }
                }
                if (created) {
                    outputs = std::move(candidates);
                    outputFormat = candidateFormat;
                    break;
                }
            }
            if (outputFormat == DXGI_FORMAT_UNKNOWN)
                return false;

            ReleaseResourcesUnlocked();
            s_state.device = device;
            s_state.computeShader = std::move(computeShader);
            s_state.resolverConstants = std::move(resolverConstants);
            s_state.outputs = std::move(outputs);
            s_state.outputFormat = outputFormat;
            s_state.faceSize = faceSize;
            SPDLOG_INFO(
                "[CloudShadows][MotionResolver] {}x{} triple-buffered "
                "animated cloud field ready (format={})",
                faceSize, faceSize, static_cast<std::uint32_t>(outputFormat));
            return true;
        }

        [[nodiscard]] bool CreateCapturedLayerUnlocked(
            std::uint64_t stableLayerId,
            CapturedLayerState& destination) noexcept
        {
            if (!s_state.device || !s_state.faceSize || !stableLayerId)
                return false;

            if (s_state.spareLayerCount != 0) {
                destination = std::move(
                    s_state.spareLayers[--s_state.spareLayerCount]);
                destination.stableLayerId = stableLayerId;
                destination.faceMask = 0;
                destination.techniques = {};
                ++s_state.mappingResourceReuses;
                // Every face and its captured constants are cleared/written
                // by AcquireLayerMappingTarget before they can be published.
                return true;
            }

            CapturedLayerState candidate{};
            candidate.stableLayerId = stableLayerId;

            D3D11_TEXTURE2D_DESC textureDescription{};
            textureDescription.Width = s_state.faceSize;
            textureDescription.Height = s_state.faceSize;
            textureDescription.MipLevels = 1;
            textureDescription.ArraySize = kCloudCubeFaceCount;
            // TexCoordOff is an unbounded float32 accumulator on OG, AE, and
            // VR. Preserve the absolute animated UV losslessly enough for the
            // later float32 captured-offset subtraction; half precision grows
            // into visible multi-texel steps as the offset magnitude rises.
            textureDescription.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            textureDescription.SampleDesc.Count = 1;
            textureDescription.Usage = D3D11_USAGE_DEFAULT;
            textureDescription.BindFlags =
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            if (FAILED(s_state.device->CreateTexture2D(
                    &textureDescription, nullptr,
                    candidate.mappingTexture.GetAddressOf()))) {
                return false;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
            srvDescription.Format = textureDescription.Format;
            srvDescription.ViewDimension =
                D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDescription.Texture2DArray.MostDetailedMip = 0;
            srvDescription.Texture2DArray.MipLevels = 1;
            srvDescription.Texture2DArray.FirstArraySlice = 0;
            srvDescription.Texture2DArray.ArraySize = kCloudCubeFaceCount;
            if (FAILED(s_state.device->CreateShaderResourceView(
                    candidate.mappingTexture.Get(), &srvDescription,
                    candidate.mappingArraySrv.GetAddressOf()))) {
                return false;
            }

            for (std::uint32_t face = 0;
                 face < kCloudCubeFaceCount; ++face) {
                D3D11_RENDER_TARGET_VIEW_DESC rtvDescription{};
                rtvDescription.Format = textureDescription.Format;
                rtvDescription.ViewDimension =
                    D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDescription.Texture2DArray.MipSlice = 0;
                rtvDescription.Texture2DArray.FirstArraySlice = face;
                rtvDescription.Texture2DArray.ArraySize = 1;
                if (FAILED(s_state.device->CreateRenderTargetView(
                        candidate.mappingTexture.Get(), &rtvDescription,
                        candidate.mappingRtvs[face].GetAddressOf()))) {
                    return false;
                }
            }

            std::array<std::array<float, 4>, kCloudCubeFaceCount>
                initialRecords{};
            D3D11_BUFFER_DESC stateDescription{};
            stateDescription.ByteWidth =
                kCloudCubeFaceCount * kCapturedStateStride;
            stateDescription.Usage = D3D11_USAGE_DEFAULT;
            stateDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            // Snapshot copies write an 8-byte UV and a 4-byte alpha. D3D11
            // requires structured-buffer copy regions to span whole strides;
            // a raw buffer permits these DWORD-aligned subranges instead.
            stateDescription.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.pSysMem = initialRecords.data();
            if (FAILED(s_state.device->CreateBuffer(
                    &stateDescription, &initialData,
                    candidate.capturedStateBuffer.GetAddressOf()))) {
                return false;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC stateSrvDescription{};
            stateSrvDescription.Format = DXGI_FORMAT_R32_TYPELESS;
            stateSrvDescription.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            stateSrvDescription.BufferEx.FirstElement = 0;
            stateSrvDescription.BufferEx.NumElements =
                kCloudCubeFaceCount * kCapturedStateStride / sizeof(uint32_t);
            stateSrvDescription.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (FAILED(s_state.device->CreateShaderResourceView(
                    candidate.capturedStateBuffer.Get(), &stateSrvDescription,
                    candidate.capturedStateSrv.GetAddressOf()))) {
                return false;
            }

            destination = std::move(candidate);
            ++s_state.mappingResourceCreates;
            return true;
        }

        enum class ShaderStage
        {
            kVertex,
            kPixel
        };

        [[nodiscard]] bool GetNativeConstantBinding(
            ID3D11DeviceContext* context, ShaderStage stage,
            std::uint32_t slot, std::uint32_t minimumConstants,
            NativeConstantBinding& destination) noexcept
        {
            destination = {};
            if (!context)
                return false;

            Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
            (void)context->QueryInterface(
                __uuidof(ID3D11DeviceContext1),
                reinterpret_cast<void**>(context1.GetAddressOf()));

            ID3D11Buffer* rawBuffer = nullptr;
            UINT firstConstant = 0;
            UINT constantCount = 0;
            if (context1) {
                if (stage == ShaderStage::kVertex) {
                    context1->VSGetConstantBuffers1(
                        slot, 1, &rawBuffer, &firstConstant, &constantCount);
                } else {
                    context1->PSGetConstantBuffers1(
                        slot, 1, &rawBuffer, &firstConstant, &constantCount);
                }
            } else if (stage == ShaderStage::kVertex) {
                context->VSGetConstantBuffers(slot, 1, &rawBuffer);
            } else {
                context->PSGetConstantBuffers(slot, 1, &rawBuffer);
            }
            destination.buffer.Attach(rawBuffer);
            if (!destination.buffer ||
                !BelongsToDevice(destination.buffer.Get(), s_state.device.Get())) {
                destination = {};
                return false;
            }

            D3D11_BUFFER_DESC description{};
            destination.buffer->GetDesc(&description);
            if ((description.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0 ||
                description.ByteWidth < 16 || description.ByteWidth % 16 != 0) {
                destination = {};
                return false;
            }
            const UINT availableConstants = description.ByteWidth / 16;
            if (!context1) {
                firstConstant = 0;
                constantCount = availableConstants;
            } else if ((firstConstant % 16u) != 0 ||
                (constantCount % 16u) != 0) {
                // SetConstantBuffers1 silently drops nonconforming windows.
                // Preserve the engine's exact legal range rather than
                // clamping it to the allocation and producing a bad count.
                destination = {};
                return false;
            }
            if (constantCount < minimumConstants ||
                firstConstant > availableConstants ||
                minimumConstants > availableConstants - firstConstant) {
                destination = {};
                return false;
            }
            destination.firstConstant = firstConstant;
            destination.constantCount = constantCount;
            return true;
        }

        class ScopedComputeSrvSlot
        {
        public:
            ScopedComputeSrvSlot(
                ID3D11DeviceContext* context, std::uint32_t slot) noexcept :
                context_(context), slot_(slot)
            {
                if (!context_)
                    return;
                ID3D11ShaderResourceView* raw = nullptr;
                context_->CSGetShaderResources(slot_, 1, &raw);
                previous_.Attach(raw);
                ID3D11ShaderResourceView* nullView = nullptr;
                context_->CSSetShaderResources(slot_, 1, &nullView);
                active_ = true;
            }

            ~ScopedComputeSrvSlot()
            {
                if (!active_)
                    return;
                ID3D11ShaderResourceView* raw = previous_.Get();
                context_->CSSetShaderResources(slot_, 1, &raw);
            }

            ScopedComputeSrvSlot(const ScopedComputeSrvSlot&) = delete;
            ScopedComputeSrvSlot& operator=(const ScopedComputeSrvSlot&) =
                delete;

        private:
            ID3D11DeviceContext* context_{};
            std::uint32_t slot_{};
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previous_;
            bool active_{};
        };

        [[nodiscard]] bool SnapshotCapturedStateUnlocked(
            ID3D11DeviceContext* context, std::uint32_t faceIndex,
            CapturedLayerState& layer, bool rawMeshAttributes) noexcept
        {
            const bool vr = s_state.staging.description.constantLayout ==
                SkyConstantLayout::kVr;
            constexpr std::uint32_t flatBlendByte = 124;
            constexpr std::uint32_t flatOffsetByte = 160;
            constexpr std::uint32_t vrBlendByte = 188;
            constexpr std::uint32_t vrOffsetByte = 224;
            const std::uint32_t blendByte = vr ? vrBlendByte : flatBlendByte;
            const std::uint32_t offsetByte = vr ? vrOffsetByte : flatOffsetByte;
            const std::uint32_t minimumConstants = vr ? 15u : 11u;

            NativeConstantBinding source;
            if (!GetNativeConstantBinding(
                    context, ShaderStage::kVertex,
                    kSkyConstantBufferSlot, minimumConstants, source)) {
                return false;
            }

            D3D11_BUFFER_DESC sourceDescription{};
            source.buffer->GetDesc(&sourceDescription);
            const std::uint64_t sourceBase =
                static_cast<std::uint64_t>(source.firstConstant) * 16u;
            if (sourceBase + offsetByte + 8u > sourceDescription.ByteWidth ||
                sourceBase + blendByte + 4u > sourceDescription.ByteWidth) {
                return false;
            }

            // Raw geometry keeps UV and alpha independent of capture-time
            // animation. Its neutral baseline needs no GPU copies/readback
            // and remains recoverable even when native BlendW starts at zero.
            // Native MRT mappings retain their exact captured UV/blend state.
            const std::array<float, 4> baseline{
                0.0f, 0.0f, rawMeshAttributes ? 1.0f : 0.0f, 1.0f };
            D3D11_BOX destinationRecord{};
            destinationRecord.left = faceIndex * kCapturedStateStride;
            destinationRecord.right = destinationRecord.left +
                kCapturedStateStride;
            destinationRecord.top = 0;
            destinationRecord.bottom = 1;
            destinationRecord.front = 0;
            destinationRecord.back = 1;

            ScopedComputeSrvSlot unbindState(context, 4);
            context->UpdateSubresource(
                layer.capturedStateBuffer.Get(), 0, &destinationRecord,
                baseline.data(), 0, 0);
            if (rawMeshAttributes)
                return SUCCEEDED(s_state.device->GetDeviceRemovedReason());

            D3D11_BOX sourceOffset{};
            sourceOffset.left = static_cast<UINT>(sourceBase + offsetByte);
            sourceOffset.right = sourceOffset.left + 8;
            sourceOffset.top = 0;
            sourceOffset.bottom = 1;
            sourceOffset.front = 0;
            sourceOffset.back = 1;
            context->CopySubresourceRegion(
                layer.capturedStateBuffer.Get(), 0,
                faceIndex * kCapturedStateStride, 0, 0,
                source.buffer.Get(), 0, &sourceOffset);

            D3D11_BOX sourceBlend{};
            sourceBlend.left = static_cast<UINT>(sourceBase + blendByte);
            sourceBlend.right = sourceBlend.left + 4;
            sourceBlend.top = 0;
            sourceBlend.bottom = 1;
            sourceBlend.front = 0;
            sourceBlend.back = 1;
            context->CopySubresourceRegion(
                layer.capturedStateBuffer.Get(), 0,
                faceIndex * kCapturedStateStride + 8, 0, 0,
                source.buffer.Get(), 0, &sourceBlend);
            return SUCCEEDED(s_state.device->GetDeviceRemovedReason());
        }

        [[nodiscard]] bool IsNativeTexture2DView(
            ID3D11ShaderResourceView* view) noexcept
        {
            if (!view || !BelongsToDevice(view, s_state.device.Get()))
                return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC description{};
            view->GetDesc(&description);
            return description.ViewDimension ==
                D3D11_SRV_DIMENSION_TEXTURE2D;
        }

        [[nodiscard]] bool ViewAliasesAnyOutput(
            ID3D11ShaderResourceView* view) noexcept
        {
            if (!view)
                return false;
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            if (!resource)
                return true;
            return std::any_of(
                s_state.outputs.begin(), s_state.outputs.end(),
                [&](const OutputCube& output) {
                    return output.texture.Get() == resource.Get();
                });
        }

        class ScopedComputeState
        {
        public:
            explicit ScopedComputeState(ID3D11DeviceContext* context) noexcept :
                context_(context)
            {
                if (!context_)
                    return;

                std::array<ID3D11ClassInstance*, 256> rawClasses{};
                UINT classCount = static_cast<UINT>(rawClasses.size());
                ID3D11ComputeShader* rawShader = nullptr;
                context_->CSGetShader(
                    &rawShader, rawClasses.data(), &classCount);
                shader_.Attach(rawShader);
                classInstanceCount_ = (std::min)(
                    classCount, static_cast<UINT>(rawClasses.size()));
                for (UINT index = 0; index < classInstanceCount_; ++index)
                    classInstances_[index].Attach(rawClasses[index]);

                std::array<ID3D11ShaderResourceView*, kResolverSrvCount>
                    rawResources{};
                context_->CSGetShaderResources(
                    0, kResolverSrvCount, rawResources.data());
                for (std::size_t index = 0; index < resources_.size(); ++index)
                    resources_[index].Attach(rawResources[index]);

                std::array<ID3D11SamplerState*, kResolverSamplerCount>
                    rawSamplers{};
                context_->CSGetSamplers(
                    0, kResolverSamplerCount, rawSamplers.data());
                for (std::size_t index = 0; index < samplers_.size(); ++index)
                    samplers_[index].Attach(rawSamplers[index]);

                ID3D11UnorderedAccessView* rawUav = nullptr;
                context_->CSGetUnorderedAccessViews(0, 1, &rawUav);
                uav_.Attach(rawUav);

                (void)context_->QueryInterface(
                    __uuidof(ID3D11DeviceContext1),
                    reinterpret_cast<void**>(context1_.GetAddressOf()));
                std::array<ID3D11Buffer*, kResolverConstantBufferCount>
                    rawBuffers{};
                if (context1_) {
                    context1_->CSGetConstantBuffers1(
                        0, kResolverConstantBufferCount, rawBuffers.data(),
                        firstConstants_.data(), constantCounts_.data());
                    usedConstantRanges_ = true;
                } else {
                    context_->CSGetConstantBuffers(
                        0, kResolverConstantBufferCount, rawBuffers.data());
                }
                for (std::size_t index = 0;
                     index < constantBuffers_.size(); ++index) {
                    constantBuffers_[index].Attach(rawBuffers[index]);
                }

                ID3D11Predicate* rawPredicate = nullptr;
                context_->GetPredication(&rawPredicate, &predicateValue_);
                predicate_.Attach(rawPredicate);
                active_ = true;
            }

            ~ScopedComputeState()
            {
                if (!active_)
                    return;

                std::array<ID3D11ClassInstance*, 256> rawClasses{};
                for (UINT index = 0;
                     index < classInstanceCount_; ++index) {
                    rawClasses[index] = classInstances_[index].Get();
                }
                context_->CSSetShader(
                    shader_.Get(), classInstanceCount_ != 0
                        ? rawClasses.data() : nullptr,
                    classInstanceCount_);

                std::array<ID3D11Buffer*, kResolverConstantBufferCount>
                    rawBuffers{};
                for (std::size_t index = 0;
                     index < constantBuffers_.size(); ++index) {
                    rawBuffers[index] = constantBuffers_[index].Get();
                }
                if (usedConstantRanges_ && context1_) {
                    // D3D11.1 cannot reliably change only the offset/size of
                    // the same buffer object while it remains bound. Clear the
                    // slots first so the exact saved windows are restored.
                    std::array<ID3D11Buffer*,
                        kResolverConstantBufferCount> nullBuffers{};
                    context_->CSSetConstantBuffers(
                        0, kResolverConstantBufferCount,
                        nullBuffers.data());
                    context1_->CSSetConstantBuffers1(
                        0, kResolverConstantBufferCount, rawBuffers.data(),
                        firstConstants_.data(), constantCounts_.data());
                } else {
                    context_->CSSetConstantBuffers(
                        0, kResolverConstantBufferCount, rawBuffers.data());
                }

                std::array<ID3D11SamplerState*, kResolverSamplerCount>
                    rawSamplers{};
                for (std::size_t index = 0; index < samplers_.size(); ++index)
                    rawSamplers[index] = samplers_[index].Get();
                context_->CSSetSamplers(
                    0, kResolverSamplerCount, rawSamplers.data());

                std::array<ID3D11ShaderResourceView*, kResolverSrvCount>
                    rawResources{};
                for (std::size_t index = 0; index < resources_.size(); ++index)
                    rawResources[index] = resources_[index].Get();
                context_->CSSetShaderResources(
                    0, kResolverSrvCount, rawResources.data());

                ID3D11UnorderedAccessView* rawUav = uav_.Get();
                constexpr UINT keepCounter = UINT(-1);
                context_->CSSetUnorderedAccessViews(
                    0, 1, &rawUav, &keepCounter);
                context_->SetPredication(predicate_.Get(), predicateValue_);
            }

            ScopedComputeState(const ScopedComputeState&) = delete;
            ScopedComputeState& operator=(const ScopedComputeState&) = delete;

        private:
            ID3D11DeviceContext* context_{};
            Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
            std::array<Microsoft::WRL::ComPtr<ID3D11ClassInstance>, 256>
                classInstances_{};
            std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,
                kResolverSrvCount> resources_{};
            std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>,
                kResolverSamplerCount> samplers_{};
            std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>,
                kResolverConstantBufferCount> constantBuffers_{};
            std::array<UINT, kResolverConstantBufferCount> firstConstants_{};
            std::array<UINT, kResolverConstantBufferCount> constantCounts_{};
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav_;
            Microsoft::WRL::ComPtr<ID3D11Predicate> predicate_;
            UINT classInstanceCount_{};
            BOOL predicateValue_{ FALSE };
            bool usedConstantRanges_{};
            bool active_{};
        };

        [[nodiscard]] bool UpdateResolverConstants(
            ID3D11DeviceContext* context,
            const ResolverConstants& constants) noexcept
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(
                    s_state.resolverConstants.Get(), 0,
                    D3D11_MAP_WRITE_DISCARD, 0, &mapped)) ||
                !mapped.pData) {
                return false;
            }
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(s_state.resolverConstants.Get(), 0);
            return true;
        }

        void UnbindResolverIo(ID3D11DeviceContext* context) noexcept
        {
            std::array<ID3D11ShaderResourceView*, kResolverSrvCount>
                nullResources{};
            context->CSSetShaderResources(
                0, kResolverSrvCount, nullResources.data());
            ID3D11UnorderedAccessView* nullUav = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        }

        [[nodiscard]] bool BindNativeLiveConstants(
            ID3D11DeviceContext* context,
            const NativeConstantBinding& vertex,
            const NativeConstantBinding& pixel) noexcept
        {
            std::array<ID3D11Buffer*, 2> buffers{
                vertex.buffer.Get(), pixel.buffer.Get() };
            Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
            (void)context->QueryInterface(
                __uuidof(ID3D11DeviceContext1),
                reinterpret_cast<void**>(context1.GetAddressOf()));
            if (context1) {
                const std::array<UINT, 2> firstConstants{
                    vertex.firstConstant, pixel.firstConstant };
                const std::array<UINT, 2> constantCounts{
                    vertex.constantCount, pixel.constantCount };
                // A third-party compute pass may already have these same
                // buffer objects bound through different windows. Explicitly
                // unbind before changing their D3D11.1 ranges.
                std::array<ID3D11Buffer*, 2> nullBuffers{};
                context->CSSetConstantBuffers(
                    1, 2, nullBuffers.data());
                context1->CSSetConstantBuffers1(
                    1, 2, buffers.data(), firstConstants.data(),
                    constantCounts.data());
            } else {
                if (vertex.firstConstant != 0 || pixel.firstConstant != 0)
                    return false;
                context->CSSetConstantBuffers(1, 2, buffers.data());
            }
            return true;
        }

        [[nodiscard]] CapturedLayerState* FindStagingLayerUnlocked(
            std::uint64_t stableLayerId) noexcept
        {
            const auto found = std::find_if(
                s_state.staging.layers.begin(), s_state.staging.layers.end(),
                [&](const CapturedLayerState& candidate) {
                    return candidate.stableLayerId == stableLayerId;
                });
            return found != s_state.staging.layers.end() ? &*found : nullptr;
        }

        [[nodiscard]] const CapturedLayerState* FindCapturedLayerUnlocked(
            std::uint64_t stableLayerId) noexcept
        {
            const auto found = std::find_if(
                s_state.capturedLayers.begin(), s_state.capturedLayers.end(),
                [&](const CapturedLayerState& candidate) {
                    return candidate.stableLayerId == stableLayerId;
                });
            return found != s_state.capturedLayers.end() ? &*found : nullptr;
        }
    }

    bool BeginCaptureGeneration(
        ID3D11Device* device,
        const CaptureGenerationDesc& description) noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            if (!device || description.captureEpoch == 0 ||
                description.worldGeneration == 0 || description.faceSize == 0 ||
                !SupportedLayout(description.constantLayout)) {
                return RejectCaptureUnlocked("invalid-generation-contract");
            }
            if (s_state.staging.active)
                return RejectCaptureUnlocked("generation-already-active");

            if (!EnsureResourcesUnlocked(device, description.faceSize))
                return RejectCaptureUnlocked("resources");

            ResetStagingUnlocked();
            s_state.staging.description = description;
            s_state.staging.layers.reserve(kMaximumResolvedLayers);
            s_state.staging.active = true;
            return true;
        } catch (...) {
            std::lock_guard lock(s_mutex);
            return RejectCaptureUnlocked("generation-exception");
        }
    }

    bool BeginCaptureCubeFace(std::uint32_t faceIndex) noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            if (!s_state.staging.active || s_state.staging.failed ||
                faceIndex >= kCloudCubeFaceCount) {
                return RejectCaptureUnlocked("invalid-face-begin");
            }
            const std::uint8_t faceBit = static_cast<std::uint8_t>(
                1u << faceIndex);
            if ((s_state.staging.begunFaceMask & faceBit) != 0 ||
                (s_state.staging.completedFaceMask & faceBit) != 0) {
                return RejectCaptureUnlocked("duplicate-face-begin");
            }
            // NativeSkyCube may announce several faces before issuing any of
            // their serial draw calls. Track each announced face separately.
            s_state.staging.begunFaceMask = static_cast<std::uint8_t>(
                s_state.staging.begunFaceMask | faceBit);
            return true;
        } catch (...) {
            std::lock_guard lock(s_mutex);
            return RejectCaptureUnlocked("face-begin-exception");
        }
    }

    bool AcquireLayerMappingTarget(
        ID3D11DeviceContext* context,
        const CaptureLayerFace& layer,
        CaptureFaceTarget& destination) noexcept
    {
        destination = {};
        try {
            std::lock_guard lock(s_mutex);
            if (!ContextMatchesDevice(context) || !s_state.staging.active ||
                s_state.staging.failed || s_state.staging.layerPending ||
                layer.stableLayerId == 0 ||
                layer.faceIndex >= kCloudCubeFaceCount ||
                !SupportedTechnique(layer.technique)) {
                return RejectCaptureUnlocked("invalid-layer-acquire");
            }
            const std::uint8_t faceBit = static_cast<std::uint8_t>(
                1u << layer.faceIndex);
            if ((s_state.staging.begunFaceMask & faceBit) == 0 ||
                (s_state.staging.completedFaceMask & faceBit) != 0) {
                return RejectCaptureUnlocked("layer-face-not-open");
            }

            CapturedLayerState* captured =
                FindStagingLayerUnlocked(layer.stableLayerId);
            if (!captured) {
                if (s_state.staging.layers.size() >= kMaximumResolvedLayers)
                    return RejectCaptureUnlocked("layer-capacity");
                CapturedLayerState candidate;
                if (!CreateCapturedLayerUnlocked(
                        layer.stableLayerId, candidate)) {
                    return RejectCaptureUnlocked("layer-resources");
                }
                s_state.staging.layers.push_back(std::move(candidate));
                captured = &s_state.staging.layers.back();
            }
            if ((captured->faceMask & faceBit) != 0)
                return RejectCaptureUnlocked("duplicate-layer-face");

            ScopedComputeState savedState(context);
            context->SetPredication(nullptr, FALSE);
            const float clear[4]{};
            context->ClearRenderTargetView(
                captured->mappingRtvs[layer.faceIndex].Get(), clear);
            if (!SnapshotCapturedStateUnlocked(
                    context, layer.faceIndex, *captured,
                    layer.rawMeshAttributes)) {
                return RejectCaptureUnlocked("capture-state-snapshot");
            }

            s_state.staging.pendingLayerId = layer.stableLayerId;
            s_state.staging.pendingFaceIndex = layer.faceIndex;
            s_state.staging.pendingTechnique = layer.technique;
            s_state.staging.layerPending = true;
            destination.mappingRtv =
                captured->mappingRtvs[layer.faceIndex];
            destination.stableLayerId = layer.stableLayerId;
            destination.faceIndex = layer.faceIndex;
            destination.technique = layer.technique;
            return true;
        } catch (...) {
            destination = {};
            std::lock_guard lock(s_mutex);
            return RejectCaptureUnlocked("layer-acquire-exception");
        }
    }

    bool CompleteLayerMapping(
        const CaptureLayerFace& layer, bool authenticatedDraw) noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            if (!s_state.staging.active || s_state.staging.failed ||
                !s_state.staging.layerPending || !authenticatedDraw ||
                layer.stableLayerId != s_state.staging.pendingLayerId ||
                layer.faceIndex != s_state.staging.pendingFaceIndex ||
                layer.technique != s_state.staging.pendingTechnique) {
                return RejectCaptureUnlocked("invalid-layer-complete");
            }
            CapturedLayerState* captured =
                FindStagingLayerUnlocked(layer.stableLayerId);
            if (!captured)
                return RejectCaptureUnlocked("completed-layer-missing");

            const std::uint8_t faceBit = static_cast<std::uint8_t>(
                1u << layer.faceIndex);
            if ((captured->faceMask & faceBit) != 0)
                return RejectCaptureUnlocked("duplicate-layer-complete");
            captured->techniques[layer.faceIndex] = layer.technique;
            captured->faceMask = static_cast<std::uint8_t>(
                captured->faceMask | faceBit);
            s_state.staging.pendingLayerId = 0;
            s_state.staging.pendingFaceIndex = 0;
            s_state.staging.layerPending = false;
            return true;
        } catch (...) {
            std::lock_guard lock(s_mutex);
            return RejectCaptureUnlocked("layer-complete-exception");
        }
    }

    bool CompleteCaptureCubeFace(
        std::uint32_t faceIndex, bool authenticatedFace) noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            if (!s_state.staging.active || s_state.staging.failed ||
                s_state.staging.layerPending || !authenticatedFace ||
                faceIndex >= kCloudCubeFaceCount) {
                return RejectCaptureUnlocked("invalid-face-complete");
            }
            const std::uint8_t faceBit = static_cast<std::uint8_t>(
                1u << faceIndex);
            if ((s_state.staging.begunFaceMask & faceBit) == 0 ||
                (s_state.staging.completedFaceMask & faceBit) != 0) {
                return RejectCaptureUnlocked("duplicate-face-complete");
            }
            if (std::any_of(
                    s_state.staging.layers.begin(),
                    s_state.staging.layers.end(),
                    [&](const CapturedLayerState& captured) {
                        return (captured.faceMask & faceBit) == 0;
                    })) {
                return RejectCaptureUnlocked("layer-absent-from-face");
            }

            s_state.staging.completedFaceMask = static_cast<std::uint8_t>(
                s_state.staging.completedFaceMask | faceBit);
            return true;
        } catch (...) {
            std::lock_guard lock(s_mutex);
            return RejectCaptureUnlocked("face-complete-exception");
        }
    }

    bool PublishCaptureGeneration() noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            if (s_state.frame.active ||
                !s_state.staging.active || s_state.staging.failed ||
                s_state.staging.layerPending ||
                s_state.staging.begunFaceMask !=
                    kCompleteCloudCubeFaceMask ||
                s_state.staging.completedFaceMask !=
                    kCompleteCloudCubeFaceMask ||
                s_state.staging.layers.size() > kMaximumResolvedLayers ||
                std::any_of(
                    s_state.staging.layers.begin(),
                    s_state.staging.layers.end(),
                    [](const CapturedLayerState& captured) {
                        return captured.faceMask !=
                                kCompleteCloudCubeFaceMask ||
                            !captured.mappingTexture ||
                            !captured.mappingArraySrv ||
                            !captured.capturedStateBuffer ||
                            !captured.capturedStateSrv;
                    })) {
                return RejectCaptureUnlocked("incoherent-generation");
            }

            RecycleLayersUnlocked(s_state.capturedLayers);
            s_state.capturedLayers.swap(s_state.staging.layers);
            s_state.captureEpoch =
                s_state.staging.description.captureEpoch;
            s_state.worldGeneration =
                s_state.staging.description.worldGeneration;
            s_state.activeLayout =
                s_state.staging.description.constantLayout;
            s_state.captureValid = true;
            ResetStagingUnlocked();
            s_state.frame = {};
            s_state.resolvedFrameSerial = 0;
            s_state.lastAttemptedFrameSerial = 0;
            WithdrawResolvedUnlocked();
            ++s_state.publishedCaptureSets;
            SPDLOG_DEBUG(
                "[CloudShadows][MotionResolver] Published coherent mapping "
                "generation epoch={} generation={} layers={}",
                s_state.captureEpoch, s_state.worldGeneration,
                s_state.capturedLayers.size());
            return true;
        } catch (...) {
            std::lock_guard lock(s_mutex);
            return RejectCaptureUnlocked("publish-exception");
        }
    }

    bool HasCapturedLayer(std::uint64_t stableLayerId) noexcept
    {
        std::lock_guard lock(s_mutex);
        return s_state.captureValid && stableLayerId != 0 &&
            FindCapturedLayerUnlocked(stableLayerId) != nullptr;
    }

    void AbortCaptureGeneration() noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            // Discard only the uncommitted refresh. World/load/device/toggle
            // invalidation uses Invalidate() and remains the authority for
            // withdrawing a genuinely stale committed field.
            ResetStagingUnlocked();
        } catch (...) {
        }
    }

    bool BeginFrame(
        ID3D11DeviceContext* context,
        const FrameDesc& description) noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            if (!ContextMatchesDevice(context) || !s_state.captureValid ||
                description.captureEpoch == 0 ||
                description.worldGeneration == 0 ||
                description.frameSerial == 0 ||
                description.captureEpoch != s_state.captureEpoch ||
                description.worldGeneration != s_state.worldGeneration ||
                description.frameSerial <= s_state.lastAttemptedFrameSerial) {
                return RejectFrameUnlocked("invalid-frame-begin");
            }
            if (s_state.frame.active) {
                s_state.frame = {};
                return RejectFrameUnlocked("overlapping-frame");
            }

            FrameState frame{};
            frame.description = description;
            std::size_t scratchCount = 0;
            for (std::uint32_t index = 0;
                 index < kOutputCubeCount && scratchCount < frame.scratch.size();
                 ++index) {
                // publishedOutput remains reserved across capture-set
                // handoffs because WorldClouds may still own and sample that
                // SRV until this frame publishes its atomic replacement.
                if (index != s_state.publishedOutput)
                    frame.scratch[scratchCount++] = index;
            }
            if (scratchCount != frame.scratch.size())
                return RejectFrameUnlocked("scratch-selection");

            frame.active = true;
            s_state.lastAttemptedFrameSerial = description.frameSerial;
            s_state.frame = std::move(frame);
            return true;
        } catch (...) {
            std::lock_guard lock(s_mutex);
            return RejectFrameUnlocked("frame-begin-exception");
        }
    }

    bool AccumulateLiveLayer(
        ID3D11DeviceContext* context,
        const LiveLayerDraw& layer) noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            if (!ContextMatchesDevice(context) || !s_state.captureValid ||
                !s_state.frame.active || s_state.frame.failed ||
                !layer.authenticated || layer.stableLayerId == 0 ||
                !SupportedTechnique(layer.technique) ||
                s_state.frame.observedLayerCount >=
                    kMaximumResolvedLayers) {
                return RejectFrameUnlocked("invalid-live-layer");
            }
            if (std::find(
                    s_state.frame.seenLayerIds.begin(),
                    s_state.frame.seenLayerIds.begin() +
                        s_state.frame.observedLayerCount, layer.stableLayerId) !=
                s_state.frame.seenLayerIds.begin() +
                    s_state.frame.observedLayerCount) {
                return RejectFrameUnlocked("duplicate-live-layer");
            }
            const CapturedLayerState* captured =
                FindCapturedLayerUnlocked(layer.stableLayerId);
            if (!captured)
                return RejectFrameUnlocked("unmapped-live-layer");

            std::array<ID3D11ShaderResourceView*, 2> rawTextures{};
            context->PSGetShaderResources(0, 2, rawTextures.data());
            std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2>
                textures{};
            for (std::size_t index = 0; index < textures.size(); ++index)
                textures[index].Attach(rawTextures[index]);

            std::array<ID3D11SamplerState*, 2> rawSamplers{};
            context->PSGetSamplers(0, 2, rawSamplers.data());
            std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>, 2>
                samplers{};
            for (std::size_t index = 0; index < samplers.size(); ++index)
                samplers[index].Attach(rawSamplers[index]);

            if (!textures[0] || !samplers[0] ||
                !IsNativeTexture2DView(textures[0].Get()) ||
                !BelongsToDevice(samplers[0].Get(), s_state.device.Get()) ||
                ViewAliasesAnyOutput(textures[0].Get())) {
                return RejectFrameUnlocked("primary-source-binding");
            }
            if (layer.technique == CloudTechnique::kCloudsLerp &&
                (!textures[1] || !samplers[1] ||
                    !IsNativeTexture2DView(textures[1].Get()) ||
                    !BelongsToDevice(
                        samplers[1].Get(), s_state.device.Get()) ||
                    ViewAliasesAnyOutput(textures[1].Get()))) {
                return RejectFrameUnlocked("secondary-source-binding");
            }
            if (!textures[1])
                textures[1] = textures[0];
            if (!samplers[1])
                samplers[1] = samplers[0];

            const std::uint32_t vertexConstantCount =
                s_state.activeLayout == SkyConstantLayout::kVr ? 15u : 11u;
            NativeConstantBinding vertexConstants;
            NativeConstantBinding pixelConstants;
            if (!GetNativeConstantBinding(
                    context, ShaderStage::kVertex,
                    kSkyConstantBufferSlot, vertexConstantCount,
                    vertexConstants) ||
                !GetNativeConstantBinding(
                    context, ShaderStage::kPixel,
                    kSkyConstantBufferSlot, 1, pixelConstants)) {
                return RejectFrameUnlocked("native-constant-binding");
            }

            const std::uint32_t layerIndex =
                s_state.frame.observedLayerCount;
            const std::uint32_t outputIndex =
                s_state.frame.scratch[layerIndex % 2u];
            const bool hasPrior = layerIndex != 0;
            const std::uint32_t priorIndex = hasPrior
                ? s_state.frame.scratch[(layerIndex - 1u) % 2u]
                : kNoOutput;
            if (outputIndex >= s_state.outputs.size() ||
                (hasPrior && priorIndex >= s_state.outputs.size()) ||
                (s_state.resolvedValid &&
                    outputIndex == s_state.publishedOutput)) {
                return RejectFrameUnlocked("invalid-output-selection");
            }

            ResolverConstants constants{};
            constants.outputSize = { s_state.faceSize, s_state.faceSize };
            constants.technique =
                static_cast<std::uint32_t>(layer.technique);
            constants.hasPriorComposite = hasPrior ? 1u : 0u;
            constants.constantLayout =
                static_cast<std::uint32_t>(s_state.activeLayout);
            if (!UpdateResolverConstants(context, constants))
                return RejectFrameUnlocked("constant-update");

            ScopedComputeState savedState(context);
            // Dispatch must not inherit an unrelated engine predicate. A
            // skipped accumulation would otherwise be published as valid.
            context->SetPredication(nullptr, FALSE);
            context->CSSetShader(s_state.computeShader.Get(), nullptr, 0);
            ID3D11Buffer* resolverConstants =
                s_state.resolverConstants.Get();
            context->CSSetConstantBuffers(0, 1, &resolverConstants);
            if (!BindNativeLiveConstants(
                    context, vertexConstants, pixelConstants)) {
                return RejectFrameUnlocked("constant-range-bind");
            }

            const std::array<ID3D11ShaderResourceView*, kResolverSrvCount>
                resources{
                    captured->mappingArraySrv.Get(), textures[0].Get(),
                    textures[1].Get(),
                    hasPrior ? s_state.outputs[priorIndex].arraySrv.Get()
                             : nullptr,
                    captured->capturedStateSrv.Get()
                };
            const std::array<ID3D11SamplerState*, kResolverSamplerCount>
                resolverSamplers{ samplers[0].Get(), samplers[1].Get() };
            ID3D11UnorderedAccessView* output =
                s_state.outputs[outputIndex].arrayUav.Get();
            context->CSSetShaderResources(
                0, kResolverSrvCount, resources.data());
            context->CSSetSamplers(
                0, kResolverSamplerCount, resolverSamplers.data());
            context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
            context->Dispatch(
                (s_state.faceSize + kThreadGroupSize - 1) /
                    kThreadGroupSize,
                (s_state.faceSize + kThreadGroupSize - 1) /
                    kThreadGroupSize,
                kCloudCubeFaceCount);
            UnbindResolverIo(context);
            if (FAILED(s_state.device->GetDeviceRemovedReason()))
                return RejectFrameUnlocked("device-removed");

            s_state.frame.seenLayerIds[layerIndex] = layer.stableLayerId;
            ++s_state.frame.observedLayerCount;
            s_state.frame.currentOutput = outputIndex;
            return true;
        } catch (...) {
            std::lock_guard lock(s_mutex);
            return RejectFrameUnlocked("live-layer-exception");
        }
    }

    bool CompleteFrame(
        ID3D11DeviceContext* context,
        bool authenticatedMainSky) noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            if (!s_state.frame.active) {
                return RejectFrameUnlocked("frame-not-active");
            }
            if (s_state.frame.failed) {
                s_state.frame = {};
                return false;
            }
            if (!authenticatedMainSky || !ContextMatchesDevice(context) ||
                !s_state.captureValid ||
                s_state.frame.description.captureEpoch !=
                    s_state.captureEpoch ||
                s_state.frame.description.worldGeneration !=
                    s_state.worldGeneration) {
                const bool result = RejectFrameUnlocked(
                    "invalid-frame-complete");
                s_state.frame = {};
                return result;
            }

            std::uint32_t publishOutput = s_state.frame.currentOutput;
            if (s_state.frame.observedLayerCount == 0) {
                publishOutput = s_state.frame.scratch[0];
                if (publishOutput >= s_state.outputs.size()) {
                    const bool result = RejectFrameUnlocked(
                        "zero-frame-output");
                    s_state.frame = {};
                    return result;
                }
                ScopedComputeState savedState(context);
                context->SetPredication(nullptr, FALSE);
                const float clear[4]{};
                context->ClearUnorderedAccessViewFloat(
                    s_state.outputs[publishOutput].arrayUav.Get(), clear);
            }
            if (publishOutput >= s_state.outputs.size() ||
                FAILED(s_state.device->GetDeviceRemovedReason())) {
                const bool result = RejectFrameUnlocked(
                    "frame-publication");
                s_state.frame = {};
                return result;
            }

            s_state.publishedOutput = publishOutput;
            s_state.publishedLayerCount =
                s_state.frame.observedLayerCount;
            s_state.resolvedFrameSerial =
                s_state.frame.description.frameSerial;
            s_state.resolvedValid = true;
            s_state.frame = {};
            ++s_state.resolvedFrames;
            return true;
        } catch (...) {
            std::lock_guard lock(s_mutex);
            const bool result = RejectFrameUnlocked(
                "frame-complete-exception");
            s_state.frame = {};
            return result;
        }
    }

    bool AcquireResolvedSnapshot(ResolvedSnapshot& destination) noexcept
    {
        destination = {};
        try {
            std::lock_guard lock(s_mutex);
            if (!s_state.captureValid || !s_state.resolvedValid ||
                s_state.publishedOutput >= s_state.outputs.size() ||
                !s_state.outputs[s_state.publishedOutput].cubeSrv) {
                return false;
            }
            destination.opacityCube =
                s_state.outputs[s_state.publishedOutput].cubeSrv;
            destination.captureEpoch = s_state.captureEpoch;
            destination.worldGeneration = s_state.worldGeneration;
            destination.frameSerial = s_state.resolvedFrameSerial;
            destination.layerCount = s_state.publishedLayerCount;
            destination.faceSize = s_state.faceSize;
            destination.format = s_state.outputFormat;
            return true;
        } catch (...) {
            destination = {};
            return false;
        }
    }

    void Invalidate() noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            InvalidateCaptureUnlocked();
        } catch (...) {
        }
    }

    void ReleaseDeviceResources() noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            ReleaseResourcesUnlocked();
        } catch (...) {
        }
    }

    Diagnostics GetDiagnostics() noexcept
    {
        try {
            std::lock_guard lock(s_mutex);
            return Diagnostics{
                .resourcesReady = s_state.device && s_state.computeShader &&
                    s_state.resolverConstants &&
                    std::all_of(
                        s_state.outputs.begin(), s_state.outputs.end(),
                        [](const OutputCube& output) {
                            return output.texture && output.cubeSrv &&
                                output.arraySrv && output.arrayUav;
                        }),
                .captureGenerationActive = s_state.staging.active,
                .captureSetValid = s_state.captureValid,
                .frameActive = s_state.frame.active,
                .resolvedSnapshotValid = s_state.resolvedValid,
                .finiteDifferenceGradients = true,
                .stagingFaceMask = s_state.staging.completedFaceMask,
                .stagingLayerCount = static_cast<std::uint32_t>(
                    s_state.staging.layers.size()),
                .capturedLayerCount = static_cast<std::uint32_t>(
                    s_state.capturedLayers.size()),
                .resolvedLayerCount = s_state.publishedLayerCount,
                .faceSize = s_state.faceSize,
                .captureEpoch = s_state.captureEpoch,
                .resolvedFrameSerial = s_state.resolvedFrameSerial,
                .publishedCaptureSets = s_state.publishedCaptureSets,
                .resolvedFrames = s_state.resolvedFrames,
                .rejectedCaptureOperations =
                    s_state.rejectedCaptureOperations,
                .rejectedFrames = s_state.rejectedFrames,
                .mappingResourceCreates = s_state.mappingResourceCreates,
                .mappingResourceReuses = s_state.mappingResourceReuses,
                .mappingBytes = static_cast<std::uint64_t>(s_state.faceSize) *
                    s_state.faceSize * kCloudCubeFaceCount * 16u *
                    (s_state.capturedLayers.size() + s_state.staging.layers.size() +
                        s_state.spareLayerCount)
            };
        } catch (...) {
            return {};
        }
    }
}
