// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace FO4CS::CloudMotionResolver
{
    // This is deliberately tied to the authenticated producer capacity. A
    // larger resolver limit would allow an incomplete captured set to look
    // authoritative.
    inline constexpr std::uint32_t kMaximumResolvedLayers = 16;
    inline constexpr std::uint32_t kCloudCubeFaceCount = 6;
    inline constexpr std::uint8_t kCompleteCloudCubeFaceMask = 0x3F;

    enum class CloudTechnique : std::uint32_t
    {
        kClouds = 5,
        kCloudsLerp = 6,
        kCloudsFade = 7
    };

    enum class SkyConstantLayout : std::uint32_t
    {
        // Fallout 4 1.10.163 and flat next-generation runtimes:
        // BlendColor0.w = VS b2 byte 124; TexCoordOff.xy = bytes 160..167.
        kFlat = 0,
        // Fallout 4 VR:
        // BlendColor0.w = VS b2 byte 188; TexCoordOff.xy = bytes 224..231.
        kVr = 1
    };

    struct CaptureGenerationDesc
    {
        std::uint64_t captureEpoch{};
        std::uint64_t worldGeneration{};
        std::uint32_t faceSize{};
        SkyConstantLayout constantLayout{ SkyConstantLayout::kFlat };
    };

    struct CaptureLayerFace
    {
        std::uint64_t stableLayerId{};
        std::uint32_t faceIndex{};
        CloudTechnique technique{ CloudTechnique::kClouds };
        // Private geometry capture stores mesh UV and unblended vertex alpha.
        // Native MRT capture instead stores animated UV and blended alpha.
        bool rawMeshAttributes{};
    };

    // AddRef-owned mapping target. The caller appends mappingRtv to the
    // authenticated native cloud MRTs, performs exactly the one vanilla draw,
    // and then calls CompleteLayerMapping for the same key.
    struct CaptureFaceTarget
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> mappingRtv;
        std::uint64_t stableLayerId{};
        std::uint32_t faceIndex{};
        CloudTechnique technique{ CloudTechnique::kClouds };
    };

    struct FrameDesc
    {
        std::uint64_t captureEpoch{};
        std::uint64_t worldGeneration{};
        std::uint64_t frameSerial{};
    };

    struct LiveLayerDraw
    {
        std::uint64_t stableLayerId{};
        CloudTechnique technique{ CloudTechnique::kClouds };
        // True only for an exact main-Sky draw authenticated by the caller.
        bool authenticated{};
    };

    struct ResolvedSnapshot
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> opacityCube;
        std::uint64_t captureEpoch{};
        std::uint64_t worldGeneration{};
        std::uint64_t frameSerial{};
        std::uint32_t layerCount{};
        std::uint32_t faceSize{};
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
    };

    struct Diagnostics
    {
        bool resourcesReady{};
        bool captureGenerationActive{};
        bool captureSetValid{};
        bool frameActive{};
        bool resolvedSnapshotValid{};
        // SampleGrad derivatives are reconstructed from adjacent mapping
        // texels. They are exact inside a linearly interpolated dome triangle
        // and approximate at primitive, seam, and helper-lane boundaries.
        bool finiteDifferenceGradients{ true };
        std::uint8_t stagingFaceMask{};
        std::uint32_t stagingLayerCount{};
        std::uint32_t capturedLayerCount{};
        std::uint32_t resolvedLayerCount{};
        std::uint32_t faceSize{};
        std::uint64_t captureEpoch{};
        std::uint64_t resolvedFrameSerial{};
        std::uint64_t publishedCaptureSets{};
        std::uint64_t resolvedFrames{};
        std::uint64_t rejectedCaptureOperations{};
        std::uint64_t rejectedFrames{};
        std::uint64_t mappingResourceCreates{};
        std::uint64_t mappingResourceReuses{};
        // Allocation size of the float32 UV mappings, including staging and
        // the bounded spare pool; excludes driver padding and opacity cubes.
        std::uint64_t mappingBytes{};
    };

    // Starts an independently-owned staging generation. The last published
    // capture remains immutable while six natural reflection faces arrive.
    [[nodiscard]] bool BeginCaptureGeneration(
        ID3D11Device* device,
        const CaptureGenerationDesc& description) noexcept;

    [[nodiscard]] bool BeginCaptureCubeFace(
        std::uint32_t faceIndex) noexcept;

    // Clears the owned RGBA32F mapping face, snapshots the exact currently
    // bound VS b2 subrange entirely GPU-side, and returns its RTV. The six
    // captured records are (TexCoordOff.xy, BlendColor0.w, valid), or
    // (0, 0, 1, 1) when the mapping contains raw mesh attributes.
    [[nodiscard]] bool AcquireLayerMappingTarget(
        ID3D11DeviceContext* context,
        const CaptureLayerFace& layer,
        CaptureFaceTarget& destination) noexcept;

    [[nodiscard]] bool CompleteLayerMapping(
        const CaptureLayerFace& layer, bool authenticatedDraw) noexcept;

    [[nodiscard]] bool CompleteCaptureCubeFace(
        std::uint32_t faceIndex, bool authenticatedFace) noexcept;

    // Publishes only a coherent 0x3F generation. Every layer present in the
    // set must have exactly one authenticated mapping draw on every face.
    // The caller authenticates that each face observed a stock cloud draw, so
    // an empty intercepted set is never published as a false clear sky.
    [[nodiscard]] bool PublishCaptureGeneration() noexcept;

    // A weather/mesh change can introduce geometry absent from the mapping.
    // The owner uses this to request one new bootstrap, rather than endlessly
    // rejecting the same otherwise valid live layer.
    [[nodiscard]] bool HasCapturedLayer(std::uint64_t stableLayerId) noexcept;

    // Aborting discards only the uncommitted staging generation. Explicit
    // world/load/device/toggle invalidation withdraws the committed field.
    void AbortCaptureGeneration() noexcept;

    // Starts current-main-Sky collection. The previously Present-published
    // output stays immutable until CompleteFrame succeeds.
    [[nodiscard]] bool BeginFrame(
        ID3D11DeviceContext* context,
        const FrameDesc& description) noexcept;

    // Call immediately after the one authenticated vanilla main-Sky draw,
    // while PS t0/t1, s0/s1, VS b2 and PS b2 remain bound. The resolver reads
    // those exact bindings and performs one compute source-over accumulation;
    // it never replays cloud geometry or adds a graphics draw.
    [[nodiscard]] bool AccumulateLiveLayer(
        ID3D11DeviceContext* context,
        const LiveLayerDraw& layer) noexcept;

    // Called at the publication boundary (normally Present). Captured IDs
    // absent from this main Sky are treated as zero. Duplicate or unmapped
    // live IDs fail neutral. A valid main Sky with zero cloud draws publishes
    // an exact-zero cube.
    [[nodiscard]] bool CompleteFrame(
        ID3D11DeviceContext* context,
        bool authenticatedMainSky) noexcept;

    // Returns an AddRef-owned immutable snapshot. Its cube is excluded from
    // resolver scratch until a later frame has published a replacement.
    [[nodiscard]] bool AcquireResolvedSnapshot(
        ResolvedSnapshot& destination) noexcept;

    void Invalidate() noexcept;
    void ReleaseDeviceResources() noexcept;
    [[nodiscard]] Diagnostics GetDiagnostics() noexcept;
}
