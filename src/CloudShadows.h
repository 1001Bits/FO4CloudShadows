#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include <DirectXMath.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include "EngineAPI.h"
#include "SunMaskProjection.h"
#include "ShaderTools/DFLightPatcher.h"

namespace fs = std::filesystem;
using namespace DirectX;
using Microsoft::WRL::ComPtr;

#define DLLEXPORT __declspec(dllexport)

#ifndef FO4CLOUDSHADOWS_VERSION_STR
#define FO4CLOUDSHADOWS_VERSION_STR "1.0.0"
#endif
#ifndef FO4CS_BUILD_ID
#define FO4CS_BUILD_ID "untracked"
#endif

namespace CloudShadows
{
    // Settings — all hot-reloadable from Data/Shaders/Features/CloudShadows.json
    // (Present re-reads the file when its mtime changes, roughly every 2 seconds).
    struct Settings
    {
        float Opacity = 2.0f;
        // Legacy settings/menu-bridge fields retained for schema compatibility.
        // Production composites every authenticated visible-cloud draw into
        // one field on one physical spherical shell; it does not invent a
        // separate altitude or scale from a renderer draw identity.
        float LayerHeightStep = 6000.0f;
        float LayerScaleMultiplier = 1.18f;
        // Legacy optical-model field. Captured alpha is already the game's
        // visible opacity, so production does not apply a second extinction.
        float VerticalOpticalDepth = 0.85f;
        // Legacy tuning fields retained in the settings/menu ABI. Production
        // traces the exact receiver-to-centre-of-sun ray at cubemap LOD zero.
        float SunAngularRadius = 0.00465f;
        float MaxOpticalSlant = 8.0f;
        float BaseMipBias = 0.0f;
        // Retained only in the settings/menu bridge ABI. Production does not
        // fade a valid cloud hit by sun elevation: the ray result is decisive.
        float SunFadeStart = 0.0f;
        float SunFadeEnd = 0.0f;
        // Physical cloud-shell distance above the stable projection anchor.
        // Matches the current flat visual-test setting. This controls the
        // projected scale; it does not change the game's visible cloud altitude.
        float CloudHeight = 10000.0f;
        // Legacy schema/menu field retained for compatibility. Native
        // directional cubemap sampling has no periodic world-tile scale.
        float WorldTileSize = 20480.0f;
        // 0 = render cloud shadows. 1 = render a world-space debug grid (lines
        // every 1024 units). Walk around: lines sticking to the ground confirms
        // that reconstruction and the captured cloud field share world space.
        float DebugMode = 0.0f;
        // Legacy settings/menu-bridge ABI field. Production capture always
        // composites every authenticated visible Sky draw; this value is inert.
        float MaxLayers = 16.0f;
    };

    // Public ABI capacity retained for constant-buffer compatibility. The
    // runtime producer publishes one source-over-composited cubemap field in
    // slot zero, containing every authenticated visible Sky cloud draw.
    constexpr uint32_t kMaxWorldCloudLayers = 16;
    constexpr uint32_t kWorldCloudCubeSize = 256;

    // Exact BSSky low-byte technique IDs from Fallout 4 1.10.163. Technique 4
    // is the generic Texture permutation and is intentionally not a cloud.
    constexpr uint32_t kSkyTechniqueTexture = 4;
    constexpr uint32_t kSkyTechniqueClouds = 5;
    constexpr uint32_t kSkyTechniqueCloudsLerp = 6;
    constexpr uint32_t kSkyTechniqueCloudsFade = 7;

    constexpr bool IsCloudTechnique(uint32_t skyTechnique) noexcept
    {
        return skyTechnique >= kSkyTechniqueClouds &&
            skyTechnique <= kSkyTechniqueCloudsFade;
    }

    enum class CloudDrawKind : uint32_t
    {
        DrawIndexed = 0,
        Draw = 1,
        DrawIndexedInstanced = 2,
        DrawInstanced = 3
    };

    enum class CloudDrawAuthentication : uint32_t
    {
        kUnauthenticated = 0,
        // Set only after the live VS/PS bytecode pair has matched the exact
        // stock runtime permutation for this BSSky cloud technique.
        kStockVisibleCloudShaders = 0x534B5943u  // "SKYC"
    };

    struct CloudDrawCommand
    {
        CloudDrawKind kind{ CloudDrawKind::DrawIndexed };
        uint32_t a{ 0 };
        uint32_t b{ 0 };
        uint32_t c{ 0 };
        int32_t d{ 0 };
        uint32_t e{ 0 };
        uint32_t skyTechnique{ 0 };
        CloudDrawAuthentication authentication{
            CloudDrawAuthentication::kUnauthenticated };
        uint64_t captureEpoch{ 0 };
        uint64_t stableLayerId{ 0 };
    };

    using CloudDrawReissue = void (*)(ID3D11DeviceContext*, const CloudDrawCommand&);

    // Skyrim-style native cubemap piggyback contract. Fallout's reflection
    // producer continues to render its own colour target and cloud geometry;
    // the hook appends this fixed MRT slot and the authenticated cloud shader
    // writes scalar visible-cloud opacity into it. The authenticated reflection
    // contract owns RT0 only; the hook must fail closed if RT1 is occupied.
    inline constexpr uint32_t kNativeWorldCloudOpacityTargetSlot = 1;
    inline constexpr uint32_t kWorldCloudCubeFaceCount = 6;
    inline constexpr uint32_t kCompleteWorldCloudCubeFaceMask =
        (1u << kWorldCloudCubeFaceCount) - 1u;

    // Starts one or more native cubemap faces. A non-null referenceFaceRTV
    // supplies the actual extent/sample contract for exactly its requested
    // FirstArraySlice on OG, AE, or VR; after that contract is learned,
    // lifecycle begin callbacks may pass null. Every requested staging face is
    // cleared to exact zero before Fallout renders it.
    bool PrepareNativeWorldCloudCaptureFaces(
        ID3D11DeviceContext* context,
        ID3D11RenderTargetView* referenceFaceRTV,
        uint32_t faceMask,
        uint64_t sourceEpoch) noexcept;
    // Borrowed pointers, valid until device-resource release/recreation.
    ID3D11RenderTargetView* GetNativeWorldCloudCaptureFaceRTV(
        uint32_t faceIndex) noexcept;
    // Returns a state which preserves the effective stock blend contract for
    // every existing MRT and source-over composites scalar opacity at slot 1.
    ID3D11BlendState* GetNativeWorldCloudCaptureBlendState(
        ID3D11BlendState* stockBlendState) noexcept;
    // Mark faces only after ClickCubeMap/native face rendering has returned.
    // A complete authenticated 0x3F generation is atomically published; an
    // unauthenticated completion invalidates the field fail-neutral.
    [[nodiscard]] bool CompleteNativeWorldCloudCaptureFaces(
        ID3D11DeviceContext* context,
        uint32_t faceMask,
        uint64_t sourceEpoch,
        bool capturePathAuthenticated) noexcept;
    void AbortNativeWorldCloudCapture(
        ID3D11DeviceContext* context,
        uint64_t sourceEpoch) noexcept;
    uint32_t GetNativeWorldCloudStagingFaceMask() noexcept;

    struct WorldCloudLayerState
    {
        uint64_t stableLayerId{ 0 };
        // Legacy ABI payload retained for external/menu compatibility.
        float PlaneZ{ 0.0f };
        float WorldScale{ 1.0f };
        float VerticalOpticalDepth{ 0.0f };
        float ActiveBlend{ 0.0f };
        float CoverageScale{ 1.0f };
        float MipBias{ 0.0f };
        uint32_t SliceIndex{ 0 };
    };

    // Legacy main-view draw ABI. Native cubemap MRT capture is the only
    // producer; this remains a fail-safe pass-through so Fallout stays solely
    // responsible for the visible player sky.
    bool ProcessWorldCloudDraw(
        ID3D11DeviceContext* context,
        const CloudDrawCommand& command,
        CloudDrawReissue reissue) noexcept;
    // Legacy rejection ABI. It never suppresses Fallout's visible clouds.
    bool HandleUncapturedWorldCloudDraw(
        ID3D11DeviceContext* context, uint64_t captureEpoch) noexcept;

    // Exact kMain render-target/subresource plus contained-viewport gate for
    // DFLight/prepass work. Sky has a separate kMainTemp contract below.
    bool IsMainWorldView(ID3D11DeviceContext* context) noexcept;
    // Exact kMainTemp render-target/subresource plus full-output viewport gate
    // used only for Sky epoch advertisement and cloud capture/replacement.
    bool IsMainSkyWorldView(ID3D11DeviceContext* context) noexcept;
    // Samples the authoritative main-view world/camera identity while Fallout
    // is outside its reflection-camera override. Native cube capture reuses
    // this stable origin instead of the transient zero pos-adjust.
    void ObserveMainWorldCloudCamera(
        ID3D11DeviceContext* context) noexcept;
    // Bounded runtime evidence for a rejected Sky draw; never changes acceptance.
    void LogMainSkyWorldViewRejection(
        ID3D11DeviceContext* context,
        uint32_t skyTechnique,
        uint32_t rejectionOrdinal) noexcept;

    // Promotes the field captured by the preceding qualified main Sky pass.
    // The main-world sunlight path may synchronize render-camera continuity.
    void CommitWorldCloudFrame(ID3D11DeviceContext* context) noexcept;
    // Present fallback publishes an already advertised/validated epoch without
    // sampling transient post-render camera coordinates.
    void CommitWorldCloudFrameAtPresent(ID3D11DeviceContext* context) noexcept;
    void RequestWorldCloudReset() noexcept;
    // Invalidate captured opacity across an enable toggle without moving the
    // stable same-world projection origin.
    void InvalidateWorldCloudCaptureForToggle() noexcept;
    uint32_t CopyCommittedWorldCloudLayers(
        WorldCloudLayerState* destination,
        uint32_t capacity) noexcept;
    ID3D11ShaderResourceView* GetCommittedWorldCloudTiles() noexcept;
    bool GetCommittedSunMaskProjection(FO4CS::SunMaskProjection& destination) noexcept;
    ID3D11SamplerState* GetWorldCloudSampler() noexcept;
    // Called only after authenticating the main DFLight targets/viewport.
    // The first valid gameplay origin is retained until a world/load reset.
    bool ConfirmWorldCloudOrigin(ID3D11DeviceContext* context,
        ID3D11Buffer* enginePerFrame, UINT firstConstant, bool vr) noexcept;
    bool GetCommittedWorldCloudOrigin(XMFLOAT3& destination) noexcept;

    // Compute shader constant buffer — keep in exact sync with
    // FO4CloudShadowScreenCS.hlsl's CloudShadowScreenCB.  DFLight's actual b12
    // is bound separately at CS b1, so the shader consumes the runtime's exact
    // engine reprojection rows without CPU readback.
    struct alignas(16) CloudShadowScreenCBData
    {
        // Flat uses this authoritative Ni camera basis/origin to transform
        // DFLight view XYZ (after its required ZYX conversion). VR uses native
        // inverse ViewProj plus its exact b12 c59/c60 eye positions.
        XMFLOAT4X4 ViewToWorld;               // c0-c3
        XMFLOAT4 OutputSizeAndInvSize;         // c4
        XMFLOAT4 OutputPixelToDepthUV;         // c5: scale.xy, bias.xy
        XMFLOAT4 OutputPixelToNDC;             // c6: scale.xy, bias.xy
        XMFLOAT4 SunDirectionAndAngularRadius; // c7: xyz test selector; w sun radius
        // z is one only for the dispatch satisfying an explicit screen-mask
        // evidence request; it gates the diagnostic ReceiverValidity UAV.
        // w gates acceptance/menu physical telemetry. Production uses only x/y.
        XMFLOAT4 ShadowParams;                 // c8: opacity, source blend, evidence write, telemetry
        XMFLOAT4 ModelParams;                  // c9: layer count, test cap cosines, debug
        XMFLOAT4 LayerGeometry[kMaxWorldCloudLayers]; // c10-c25: shell height, planet radius, tile scale, slice
        XMFLOAT4 LayerOptics[kMaxWorldCloudLayers];   // c26-c41: cube-generation camera xyz, active
        // Camera/player proxy in absolute Ni-world space; w is the radius of
        // the horizontal diagnostic disk around it.
        XMFLOAT4 DiagnosticReceiverAndRadius;         // c42
        XMFLOAT4 VisibleSunDirectionAndValidity;       // c43: world direction; w=1 valid/-1 invalid
        FO4CS::SunMaskProjection SunProjection;       // c44-c47: committed 2D capture transform
    };
    static_assert(sizeof(CloudShadowScreenCBData) == 48 * 16);

    struct alignas(16) WorldCloudTileCBData
    {
        XMFLOAT4 FaceRight;
        XMFLOAT4 FaceUp;
        XMFLOAT4 FaceForward;
        XMFLOAT4 CaptureParams;
    };
    static_assert(sizeof(WorldCloudTileCBData) == 64);

    struct alignas(16) WorldCloudSkyCBData
    {
        // x cloud height, y planet radius, z cube index, w active blend.
        XMFLOAT4 ShellGeometry;
        XMFLOAT4 CaptureOrigin;
        XMFLOAT4 CameraPosition;
        XMFLOAT4 PreviousCameraPosition;
    };
    static_assert(sizeof(WorldCloudSkyCBData) == 64);

    // Draw-hook watchdog (defined in Plugin.cpp): checks that our thunks are
    // still in the context vtable draw slots, logs + re-heals if overwritten.
    void VerifyDrawHookIntegrity() noexcept;
    // Cheap per-Present identity check for all five D3D entry points required
    // by the native opacity-only transaction. A driver-vtable generation is
    // never allowed to capture until both draw suppression and primary-clear
    // preservation are routed through our permanent Detours trampolines.
    [[nodiscard]] bool NativeCaptureRoutesCurrent() noexcept;

    // Captured by the D3D11CreateDeviceAndSwapChain detour when the game creates
    // its D3D device. RuntimeAPI selects the reviewed OG, AE, or VR bindings;
    // their distinct Sky constant layouts are handled at capture/dispatch time.
    inline ID3D11Device*        g_capturedDevice    = nullptr;
    inline ID3D11DeviceContext* g_capturedContext   = nullptr;
    inline IDXGISwapChain*      g_capturedSwapChain = nullptr;

    // D3D11 resources
    inline ID3D11Texture2D* g_cloudShadowTex = nullptr;
    inline ID3D11ShaderResourceView* g_cloudShadowSRV = nullptr;
    inline ID3D11UnorderedAccessView* g_cloudShadowUAV = nullptr;
    // The production entry contains only full-resolution reconstruction and
    // one current-cube ray/shell sample. The general entry is retained for
    // F11 debug views and explicit acceptance/evidence requests.
    inline ID3D11ComputeShader* g_cloudShadowProductionCS = nullptr;
    inline ID3D11ComputeShader* g_cloudShadowCS = nullptr;
    inline ID3D11Buffer* g_cloudShadowCB = nullptr;

    // Live capture-health telemetry shown by the F11 overlay. Sky begins count
    // every observed Sky BeginTechnique call; cloud draws count candidates
    // that also survive the cloud phase and bound-shader identity gates.
    inline std::atomic<uint32_t> g_skyDrawsSeen{ 0 };
    inline std::atomic<uint32_t> g_cloudDrawCount{ 0 };
    inline std::atomic<uint32_t> g_reRenderCount{ 0 };
    inline std::atomic<uint64_t> g_geometryCaptureAttempts{ 0 };
    inline std::atomic<uint64_t> g_geometryCapturePublished{ 0 };
    inline std::atomic<uint64_t> g_geometryCaptureRejected{ 0 };
    inline std::atomic<uint64_t> g_geometryCaptureDraws{ 0 };
    inline std::atomic<uint32_t> g_lastSkyTechnique{ 0 };
    inline std::atomic<float> g_projectionAnchorX{ 0.0f };
    inline std::atomic<float> g_projectionAnchorY{ 0.0f };
    inline std::atomic<float> g_projectionAnchorZ{ 0.0f };

    // Asynchronous physical cloud-field telemetry. The local-area values are
    // samples on a horizontal disk at camera/player-proxy Z, not terrain-area
    // coverage. Fallout uses approximately 0.01428 metres per world unit.
    inline constexpr float kCloudTelemetryRadiusWorldUnits = 4096.0f;
    inline constexpr float kCloudTelemetryOpacityThreshold = 0.01f;
    inline constexpr float kWorldUnitMetres = 0.01428f;
    inline std::atomic<bool> g_cloudTelemetryValid{ false };
    inline std::atomic<float> g_receiverSunRayCloudOpacity{ 0.0f };
    inline std::atomic<float> g_localCloudOpacityMinimum{ 0.0f };
    inline std::atomic<float> g_localCloudOpacityMean{ 0.0f };
    inline std::atomic<float> g_localCloudOpacityMaximum{ 0.0f };
    inline std::atomic<float> g_localCloudShadowedFraction{ 0.0f };
    inline std::atomic<uint32_t> g_localCloudTelemetrySampleCount{ 0 };
    inline std::atomic<uint64_t> g_cloudTelemetryEpoch{ 0 };
    inline std::atomic<uint64_t> g_cloudTelemetryWorldGeneration{ 0 };
    // Exact DFLight prepass which produced the published telemetry. Epoch alone
    // is not sufficient because several screen-mask dispatches can consume one
    // committed cloud cube.
    inline std::atomic<uint32_t> g_cloudTelemetryDispatchOrdinal{ 0 };
    // Completed, generation-current GPU observation of a finite above-horizon
    // main-view sun. Field eligibility is separate so night/no-sun cannot hide
    // a missing or broken committed capture.
    inline std::atomic<bool> g_cloudTelemetrySunEligible{ false };
    inline std::atomic<bool> g_cloudTelemetryFieldCommitted{ false };

    // Non-blocking GPU readback summaries used by the in-game acceptance
    // runner. Counts are exact for the sampled texels; min/max/mean/stddev are
    // over finite samples only. "neutral" means >= 0.999, "strong shadow"
    // means <= 0.9, "clear" means <= 0.001, and "opaque" means >= 0.999.
    struct EvidenceDistributionSummary
    {
        uint64_t sampleCount{ 0 };
        uint64_t finiteCount{ 0 };
        uint64_t nonFiniteCount{ 0 };
        uint64_t outOfRangeCount{ 0 };
        uint64_t exactZeroCount{ 0 };
        uint64_t exactOneCount{ 0 };
        uint64_t neutralCount{ 0 };
        uint64_t strongShadowCount{ 0 };
        uint64_t clearCount{ 0 };
        uint64_t opaqueCount{ 0 };
        float minimum{ 0.0f };
        float maximum{ 0.0f };
        double mean{ 0.0 };
        double standardDeviation{ 0.0 };
    };

    enum class ScreenMaskEvidenceScope : uint32_t
    {
        kAll = 0,
        kLowerHalf,
        kCentreGround,
        kCount
    };
    inline constexpr uint32_t kScreenMaskEvidenceScopeCount =
        static_cast<uint32_t>(ScreenMaskEvidenceScope::kCount);

    struct ValidReceiverEvidenceSummary
    {
        uint64_t validReceiverCount{ 0 };
        uint64_t validReceiverExactOneCount{ 0 };
        uint64_t validReceiverAttenuatedCount{ 0 };
        // Distribution over reconstruction/depth-valid receivers only. Its
        // range and standard deviation provide the valid-pixel variation test.
        EvidenceDistributionSummary distribution{};
    };

    struct ScreenMaskEvidenceSnapshot
    {
        bool valid{ false };
        // Monotonically increases for every publication or invalidation.
        uint64_t generation{ 0 };
        // A nonzero request ID proves which on-demand request this capture
        // satisfies. Requests may be coalesced; the newest ID satisfies all
        // older outstanding IDs.
        uint64_t requestId{ 0 };
        uint64_t epoch{ 0 };
        uint64_t worldGeneration{ 0 };
        uint32_t dispatchOrdinal{ 0 };
        uint32_t width{ 0 };
        uint32_t height{ 0 };
        uint32_t sampleStride{ 0 };
        float debugMode{ 0.0f };
        std::array<EvidenceDistributionSummary,
            kScreenMaskEvidenceScopeCount> scopes{};
        std::array<ValidReceiverEvidenceSummary,
            kScreenMaskEvidenceScopeCount> validReceivers{};
    };

    struct WorldCloudCubeEvidenceSnapshot
    {
        bool valid{ false };
        uint64_t generation{ 0 };
        uint64_t requestId{ 0 };
        // Nonzero when the cube publication armed a screen-mask capture in the
        // same DFLight prepass. This is the exact request token for correlating
        // both readbacks to one committed field epoch.
        uint64_t pairedScreenRequestId{ 0 };
        uint64_t epoch{ 0 };
        uint64_t worldGeneration{ 0 };
        uint32_t width{ 0 };
        uint32_t height{ 0 };
        uint32_t mipLevels{ 0 };
        uint32_t coarseMip{ 0 };
        uint32_t activeLayerCount{ 0 };
        uint32_t sampledLayerCount{ 0 };
        EvidenceDistributionSummary baseAllFaces{};
        std::array<EvidenceDistributionSummary, 6> baseFaces{};
        EvidenceDistributionSummary coarseAllFaces{};
    };

    // Snapshot reads are versioned and lock-free. A false return only means a
    // writer remained active for the bounded retry window; callers may retry.
    bool GetScreenMaskEvidenceSnapshot(
        ScreenMaskEvidenceSnapshot& destination) noexcept;
    bool GetWorldCloudCubeEvidenceSnapshot(
        WorldCloudCubeEvidenceSnapshot& destination) noexcept;
    // Returns a monotonic token. Wait for a valid snapshot whose requestId is
    // at least this value and whose generation is newer than the baseline.
    uint64_t RequestScreenMaskEvidenceCapture() noexcept;
    uint64_t RequestWorldCloudCubeEvidenceCapture() noexcept;
    // Stops outstanding acceptance requests from arming new GPU copies. A copy
    // which is already in flight is still polled to completion so its staging
    // resources are never reused while the GPU owns them.
    void CancelPendingAcceptanceEvidenceRequests() noexcept;

    // V4 world-cloud health/readiness telemetry.
    inline std::atomic<bool> g_worldCloudReady{ false };
    inline std::atomic<uint64_t> g_worldCloudCommittedEpoch{ 0 };
    inline std::atomic<uint64_t> g_worldCloudPendingEpoch{ 0 };
    // Increments only for an actual world/load/fast-travel field reset. An
    // explicitly requested screen diagnostic records it so a readback cannot
    // be accepted after the world that produced it has changed.
    inline std::atomic<uint64_t> g_worldCloudResetGeneration{ 0 };
    inline std::atomic<uint32_t> g_worldCloudActiveLayers{ 0 };
    inline std::atomic<uint32_t> g_worldCloudCaptureOverflow{ 0 };
    inline std::atomic<uint32_t> g_worldCloudReplacementDraws{ 0 };
    inline ID3D11ShaderResourceView* g_worldCloudPreviewSRV = nullptr;

    bool CreateWorldCloudResources();
    void ReleaseWorldCloudResources() noexcept;

    // DFLight PS patcher — fail-closed matching patches only stock directional
    // sunlight variants and modulates their cascade visibility before the final
    // direct-light MADs. Ambient/environment terms remain unchanged.
    inline SIE::DFLightPatcher g_dfLightPatcher;
    inline std::atomic<bool> g_dfLightPatcherInitialized{false};

    // Per-attempt validity consumed immediately by BeginSwap. Every Prepass
    // entry clears it, and only that same validated attempt may set it again.
    inline std::atomic<bool> g_shadowMaskValid{ false };
    // Frame-level OR latch. A nonzero value records the committed epoch + 1 of
    // any successful dispatch since the last authoritative Present. Rejected
    // later Prepass attempts must not erase it; Present may legitimately
    // publish the following Sky epoch before consuming this stamp.
    inline std::atomic<uint64_t> g_shadowMaskSuccessStamp{ 0 };
    // Present reports VALID only when the frame success stamp was nonzero and
    // the renderer device was still healthy at the frame boundary.
    inline std::atomic<bool> g_lastCompletedShadowMaskValid{ false };

    // Resource, device, context, world, and explicit-reset paths use one
    // fail-neutral invalidation so neither attempt nor completed diagnostics
    // can outlive the state that produced the mask.
    void InvalidateShadowMaskState() noexcept;

    // Runtime assets are always resolved relative to the host executable, not
    // the mutable process current directory. Shader source is returned only
    // when its exact SHA-256 matches the corresponding compile-time manifest.
    bool ResolveGameRelativePath(
        const fs::path& relativePath, fs::path& destination) noexcept;
    bool ReadAuthenticatedShaderSource(
        const fs::path& relativePath,
        std::string_view expectedSha256,
        std::string& source) noexcept;

    inline Settings g_settings;
    inline bool g_initialized = false;

    // Shared by the F10 A/B hotkey and the ImGui menu. Kept separate from
    // Opacity so disabling the feature does not discard the configured value.
    extern std::atomic<bool> g_shadowsEnabled;
    // Master switch for the F7/F8/F10/F11 hotkeys and the F11 menu (default off).
    extern std::atomic<bool> g_hotkeysEnabled;

    // Non-persistent diagnostic: an angular aperture around a real visible
    // cloud selected from the current camera direction. It deliberately does
    // not enter the settings schema or the external menu ABI.
    inline std::atomic<bool> g_singleCloudIsolationEnabled{ false };
    inline std::atomic<float> g_singleCloudSelectorX{ 0.0f };
    inline std::atomic<float> g_singleCloudSelectorY{ 0.0f };
    inline std::atomic<float> g_singleCloudSelectorZ{ 1.0f };
    inline std::atomic<float> g_singleCloudRadiusDegrees{ 12.0f };
    bool LockSingleCloudToCurrentView() noexcept;

    // Device/context come from the D3D11CreateDeviceAndSwapChain detour —
    // cross-runtime safe.
    inline ID3D11Device*        GetD3DDevice()  { return g_capturedDevice; }
    inline ID3D11DeviceContext* GetD3DContext() { return g_capturedContext; }

    // Engine singletons are resolved through the project-owned runtime bridge
    // so no CommonLibF4 type or relocation helper reaches rendering code.
    inline FO4CS::EngineAPI::RendererData* GetRendererData()
    {
        return FO4CS::EngineAPI::GetRendererData();
    }

    // State 600795/2704621 and the reviewed VR RVA name the singleton object
    // itself, so the runtime bridge resolves the object address directly.
    inline FO4CS::EngineAPI::State* GetGraphicsState()
    {
        return FO4CS::EngineAPI::GetGraphicsState();
    }

    // Authoritative full-resolution player-view extent. Secondary cubemap,
    // reflection, and preview passes must never resize or consume the mask.
    bool GetOutputDimensions(uint32_t& width, uint32_t& height);
    // True only when the view addresses the renderer's live kMain resource
    // with the same mip/array subresource as its authoritative RTV.
    bool IsMainRenderTargetView(ID3D11RenderTargetView* view) noexcept;
    // Sky renders into the live kMainTemp resource on Fallout 4 1.10.163.
    // Require the same mip/array subresource as its authoritative RTV.
    bool IsMainSkyRenderTargetView(ID3D11RenderTargetView* view) noexcept;

    // Lifecycle
    void Initialize();
    // Poll F10 once per authoritative frame so the disabled draw-hook path can
    // remain a true pass-through while still allowing the feature to turn on.
    void PollShadowToggle() noexcept;
    // Releases every object tied to the current D3D device. Called on the
    // render thread before accepting a recreated renderer device.
    void ReleaseDeviceResources() noexcept;
    void SetupResources();
    void CompileComputeShader();
    void InitializeDFLightPatcher();
    // Returns true only after a validated dispatch for the currently bound
    // main-view sunlight draw produced a mask. It is intentionally called for
    // every exact sunlight draw because DFLight can reuse buffer objects while
    // changing their contents between draws.
    bool Prepass();

    // Settings persistence
	void ValidateSettings(Settings& value) noexcept;
    // A user-initiated reload adopts the file's Enabled value. Internal
    // callers that merely re-read tuning during an initialization retry must
    // pass false, so an unchanged file cannot revert a live F10 or menu toggle.
    bool LoadSettings(bool applyEnabledFromFile = true);
    void SaveSettings();
}
