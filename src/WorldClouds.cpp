#include "CpuStageProfiler.h"
#include "CloudShadows.h"
#include "CloudGeometryCapture.h"
#include "CloudFramePublication.h"
#include "WorldCloudAnchor.h"
#include "MainViewCameraReadback.h"
#include "MainViewViewport.h"
#include "CloudComparison.h"
#include "AcceptanceRunner.h"
#if FO4CS_ENABLE_DEVELOPER_TOOLS
#include "ConstantBufferProbe.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

#include <DirectXPackedVector.h>

namespace CloudShadows
{
    namespace
    {
        constexpr DXGI_FORMAT kWorldCloudFormat = DXGI_FORMAT_R16_FLOAT;
        constexpr uint64_t kCompositeCloudFieldId =
            0x434C4F5544464945ULL;  // "CLOUDFIE"

        struct CloudCubeSet
        {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11ShaderResourceView> srv;
            std::array<ComPtr<ID3D11RenderTargetView>,
                kWorldCloudCubeFaceCount> faceRTVs;
        };

        struct WorldCloudResources
        {
            ComPtr<ID3D11Device> device;
            // Retain the currently published immutable opacity view so existing
            // surface-shadow, Godray, preview, and evidence consumers share
            // exactly one field.
            ComPtr<ID3D11ShaderResourceView> resolvedSrv;
            ComPtr<ID3D11Texture2D> resolvedTexture;
            CloudCubeSet cubeSets[2];
            ComPtr<ID3D11SamplerState> sampler;
            uint32_t faceWidth{ 0 };
            uint32_t faceHeight{ 0 };
            uint32_t mipLevels{ 0 };

            // Acceptance-only asynchronous readback. It never flushes or
            // waits for the GPU on the render thread.
            ComPtr<ID3D11Texture2D> evidenceStaging;
            ComPtr<ID3D11Query> evidenceQuery;
            uint64_t evidenceEpoch{ 0 };
            uint64_t evidenceWorldGeneration{ 0 };
            uint64_t evidenceRequestId{ 0 };
            uint64_t evidenceScreenRequestId{ 0 };
            uint32_t evidenceMapRetryCount{ 0 };
            uint64_t nextEvidenceAllocationAttempt{ 0 };
            bool evidencePending{ false };
        };

        struct CloudSnapshot
        {
            WorldCloudLayerState layer{};
            uint64_t epoch{ 0 };
            XMFLOAT3 captureOrigin{};
            bool captureOriginValid{ false };
            bool active{ false };

            void Clear() noexcept
            {
                layer = {};
                epoch = 0;
                captureOrigin = {};
                captureOriginValid = false;
                active = false;
            }
        };

        class ScopedUnpredicated
        {
        public:
            explicit ScopedUnpredicated(ID3D11DeviceContext* context) noexcept :
                context_(context)
            {
                context_->GetPredication(predicate_.GetAddressOf(), &value_);
                context_->SetPredication(nullptr, FALSE);
            }

            ~ScopedUnpredicated()
            {
                context_->SetPredication(predicate_.Get(), value_);
            }

            ScopedUnpredicated(const ScopedUnpredicated&) = delete;
            ScopedUnpredicated& operator=(const ScopedUnpredicated&) = delete;

        private:
            ID3D11DeviceContext* context_;
            ComPtr<ID3D11Predicate> predicate_;
            BOOL value_{ FALSE };
        };

        std::mutex s_resourceMutex;
        // Device-owned state is never destroyed by a DLL static destructor:
        // at process exit that would release D3D objects under the loader lock.
        std::unique_ptr<WorldCloudResources>& s_resources =
            *new std::unique_ptr<WorldCloudResources>();
        CloudSnapshot s_snapshots[2];
        FO4CS::CloudFramePublication s_mainSkyFrame;

        std::atomic<bool> s_resetRequested{ false };
        std::uintptr_t s_worldIdentity = 0;
        XMFLOAT3 s_lastCameraPosition{};
        bool s_lastCameraPositionValid = false;
        XMFLOAT3 s_projectionOrigin{};
        bool s_projectionOriginValid = false;
        FO4CS::WorldCloudAnchor s_confirmedProjectionOrigin;
        FO4CS::MainViewCameraReadback& s_mainViewCameraReadback =
            *new FO4CS::MainViewCameraReadback();
        XMFLOAT3 s_publishedMappingOrigin{};
        bool s_publishedMappingOriginValid = false;
        constexpr uint32_t kGeometryMappingFaceSize = 256;

        // Re-anchor crossfade (see WorldCloudAnchor.h). While active, the
        // projection also samples the field from the previous origin and
        // blends toward the new one so the ground pattern never jumps.
        XMFLOAT3 s_previousProjectionOrigin{};
        ULONGLONG s_crossfadeStartTick = 0;
        bool s_crossfadeActive = false;
        ULONGLONG s_nextAnchorReadbackTick = 0;

        // Any main-view render (main Sky draw or main DFLight prepass) since
        // the previous Present. A Present without one shows no new scene and
        // keeps the published field on every runtime.
        bool s_mainViewRenderedSincePresent = false;

        // Per-frame capture gate: evaluated at the first main Sky draw of a
        // frame and reused by its remaining cloud layers.
        enum class CaptureGate : uint8_t { kUnknown, kCapture, kSkip };
        CaptureGate s_captureGate = CaptureGate::kUnknown;

        // Deterministic resource failures are retried with backoff and
        // logged once per failure streak instead of on every Sky draw.
        struct FailureLatch
        {
            ULONGLONG retryAfter{ 0 };
            uint32_t failures{ 0 };

            [[nodiscard]] bool Blocked() const noexcept
            {
                return failures != 0 && GetTickCount64() < retryAfter;
            }

            // Returns true when this failure should be logged.
            bool Fail() noexcept
            {
                ++failures;
                const ULONGLONG backoff = (std::min)(
                    300000ull, 1000ull << (std::min)(failures, 8u));
                retryAfter = GetTickCount64() + backoff;
                return failures == 1 || (failures & (failures - 1)) == 0;
            }

            void Succeed() noexcept { *this = {}; }
        };
        FailureLatch s_cubeCreation;

#if FO4CS_ENABLE_DEVELOPER_TOOLS
        // The capture takes dome direction from Sky VS b2 World rows
        // (c4-c6 on flat, c8-c10 on VR). Compare these with view motion.
        void LogSkyDiagnostics(ID3D11DeviceContext* context,
            uint32_t technique) noexcept
        {
            try {
                static auto& probe = *new FO4CS::ConstantBufferProbe();
                ComPtr<ID3D11Buffer> buffer;
                UINT first = 0;
                UINT count = 0;
                ComPtr<ID3D11DeviceContext1> context1;
                if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1))) && context1)
                    context1->VSGetConstantBuffers1(2, 1, buffer.GetAddressOf(), &first, &count);
                else
                    context->VSGetConstantBuffers(2, 1, buffer.GetAddressOf());
                FO4CS::ConstantBufferProbe::Sample sample;
                if (probe.Poll(context, buffer.Get(), first,
                        { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, sample)) {
                    SPDLOG_INFO("[CloudShadows][ViewDiag] Sky VS b2 technique={}{}",
                        technique, FO4CS::ConstantBufferProbe::Describe(sample));
                }
            } catch (...) {
            }
        }
#endif

        const char* CaptureRejectName(
            FO4CS::CloudGeometryCapture::RejectReason reason) noexcept
        {
            using Reason = FO4CS::CloudGeometryCapture::RejectReason;
            switch (reason) {
            case Reason::kInvalidRequest: return "invalid-request";
            case Reason::kTopology: return "topology";
            case Reason::kExtraShaderStage: return "geometry/hull/domain shader bound";
            case Reason::kClassLinkage: return "class-linked or missing VS/PS";
            case Reason::kStreamOutput: return "stream-output bound";
            case Reason::kPixelUav: return "pixel UAV bound";
            case Reason::kResources: return "capture shaders/resources unavailable";
            case Reason::kRasterizer: return "unsupported rasterizer state";
            case Reason::kDeviceRemoved: return "device removed";
            case Reason::kException: return "exception";
            default: return "none";
            }
        }

        void LogCaptureReject(FO4CS::CloudGeometryCapture::RejectReason reason,
            uint32_t technique) noexcept
        {
            static std::atomic<uint32_t> rejections{ 0 };
            const uint32_t count = rejections.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 4 || (count & (count - 1)) == 0) {
                SPDLOG_WARN(
                    "[CloudShadows] Cloud opacity capture rejected (#{}, technique={}): {}; "
                    "this frame stays neutral",
                    count, technique, CaptureRejectName(reason));
            }
        }

        struct CaptureEvidenceStats
        {
            uint64_t finiteCount{ 0 };
            uint64_t nonFiniteCount{ 0 };
            uint64_t outOfRangeCount{ 0 };
            uint64_t exactZeroCount{ 0 };
            uint64_t exactOneCount{ 0 };
            uint64_t nearClearCount{ 0 };
            uint64_t nearOpaqueCount{ 0 };
            double sum{ 0.0 };
            double sumSquares{ 0.0 };
            float minimum{ (std::numeric_limits<float>::infinity)() };
            float maximum{ -(std::numeric_limits<float>::infinity)() };

            void Add(float value) noexcept
            {
                if (!std::isfinite(value)) {
                    ++nonFiniteCount;
                    return;
                }
                ++finiteCount;
                minimum = (std::min)(minimum, value);
                maximum = (std::max)(maximum, value);
                sum += value;
                sumSquares += static_cast<double>(value) * value;
                if (value < 0.0f || value > 1.0f)
                    ++outOfRangeCount;
                if (value == 0.0f)
                    ++exactZeroCount;
                if (value == 1.0f)
                    ++exactOneCount;
                if (value <= 0.001f)
                    ++nearClearCount;
                if (value >= 0.999f)
                    ++nearOpaqueCount;
            }

            double Mean() const noexcept
            {
                return finiteCount != 0 ? sum / finiteCount : 0.0;
            }

            double StandardDeviation() const noexcept
            {
                if (finiteCount == 0)
                    return 0.0;
                const double mean = Mean();
                return std::sqrt((std::max)(
                    0.0, sumSquares / finiteCount - mean * mean));
            }
        };

        EvidenceDistributionSummary SummarizeCaptureEvidence(
            const CaptureEvidenceStats& stats) noexcept
        {
            EvidenceDistributionSummary summary{};
            summary.sampleCount = stats.finiteCount + stats.nonFiniteCount;
            summary.finiteCount = stats.finiteCount;
            summary.nonFiniteCount = stats.nonFiniteCount;
            summary.outOfRangeCount = stats.outOfRangeCount;
            summary.exactZeroCount = stats.exactZeroCount;
            summary.exactOneCount = stats.exactOneCount;
            summary.clearCount = stats.nearClearCount;
            summary.opaqueCount = stats.nearOpaqueCount;
            summary.minimum = stats.finiteCount ? stats.minimum : 0.0f;
            summary.maximum = stats.finiteCount ? stats.maximum : 0.0f;
            summary.mean = stats.Mean();
            summary.standardDeviation = stats.StandardDeviation();
            return summary;
        }

        struct AtomicEvidenceDistribution
        {
            std::atomic<uint64_t> sampleCount{ 0 };
            std::atomic<uint64_t> finiteCount{ 0 };
            std::atomic<uint64_t> nonFiniteCount{ 0 };
            std::atomic<uint64_t> outOfRangeCount{ 0 };
            std::atomic<uint64_t> exactZeroCount{ 0 };
            std::atomic<uint64_t> exactOneCount{ 0 };
            std::atomic<uint64_t> neutralCount{ 0 };
            std::atomic<uint64_t> strongShadowCount{ 0 };
            std::atomic<uint64_t> clearCount{ 0 };
            std::atomic<uint64_t> opaqueCount{ 0 };
            std::atomic<float> minimum{ 0.0f };
            std::atomic<float> maximum{ 0.0f };
            std::atomic<double> mean{ 0.0 };
            std::atomic<double> standardDeviation{ 0.0 };

            void Store(const EvidenceDistributionSummary& source) noexcept
            {
                sampleCount.store(source.sampleCount, std::memory_order_relaxed);
                finiteCount.store(source.finiteCount, std::memory_order_relaxed);
                nonFiniteCount.store(source.nonFiniteCount, std::memory_order_relaxed);
                outOfRangeCount.store(source.outOfRangeCount, std::memory_order_relaxed);
                exactZeroCount.store(source.exactZeroCount, std::memory_order_relaxed);
                exactOneCount.store(source.exactOneCount, std::memory_order_relaxed);
                neutralCount.store(source.neutralCount, std::memory_order_relaxed);
                strongShadowCount.store(source.strongShadowCount, std::memory_order_relaxed);
                clearCount.store(source.clearCount, std::memory_order_relaxed);
                opaqueCount.store(source.opaqueCount, std::memory_order_relaxed);
                minimum.store(source.minimum, std::memory_order_relaxed);
                maximum.store(source.maximum, std::memory_order_relaxed);
                mean.store(source.mean, std::memory_order_relaxed);
                standardDeviation.store(source.standardDeviation, std::memory_order_relaxed);
            }

            void Load(EvidenceDistributionSummary& destination) const noexcept
            {
                destination.sampleCount = sampleCount.load(std::memory_order_relaxed);
                destination.finiteCount = finiteCount.load(std::memory_order_relaxed);
                destination.nonFiniteCount = nonFiniteCount.load(std::memory_order_relaxed);
                destination.outOfRangeCount = outOfRangeCount.load(std::memory_order_relaxed);
                destination.exactZeroCount = exactZeroCount.load(std::memory_order_relaxed);
                destination.exactOneCount = exactOneCount.load(std::memory_order_relaxed);
                destination.neutralCount = neutralCount.load(std::memory_order_relaxed);
                destination.strongShadowCount = strongShadowCount.load(std::memory_order_relaxed);
                destination.clearCount = clearCount.load(std::memory_order_relaxed);
                destination.opaqueCount = opaqueCount.load(std::memory_order_relaxed);
                destination.minimum = minimum.load(std::memory_order_relaxed);
                destination.maximum = maximum.load(std::memory_order_relaxed);
                destination.mean = mean.load(std::memory_order_relaxed);
                destination.standardDeviation = standardDeviation.load(std::memory_order_relaxed);
            }
        };

        struct PublishedWorldCloudCubeEvidence
        {
            std::atomic_flag writer = ATOMIC_FLAG_INIT;
            std::atomic<uint64_t> sequence{ 0 };
            std::atomic<bool> valid{ false };
            std::atomic<uint64_t> requestId{ 0 };
            std::atomic<uint64_t> pairedScreenRequestId{ 0 };
            std::atomic<uint64_t> epoch{ 0 };
            std::atomic<uint64_t> worldGeneration{ 0 };
            std::atomic<uint32_t> width{ 0 };
            std::atomic<uint32_t> height{ 0 };
            std::atomic<uint32_t> mipLevels{ 0 };
            std::atomic<uint32_t> coarseMip{ 0 };
            std::atomic<uint32_t> activeLayerCount{ 0 };
            std::atomic<uint32_t> sampledLayerCount{ 0 };
            AtomicEvidenceDistribution baseAllFaces{};
            std::array<AtomicEvidenceDistribution,
                kWorldCloudCubeFaceCount> baseFaces{};
            AtomicEvidenceDistribution coarseAllFaces{};
        };

        PublishedWorldCloudCubeEvidence s_publishedWorldCloudCubeEvidence;
        std::atomic<uint64_t> s_worldCloudCubeEvidenceRequest{ 0 };
        std::atomic<uint64_t> s_completedWorldCloudCubeEvidenceRequest{ 0 };

        void AdvanceCompletedEvidenceRequest(uint64_t requestId) noexcept
        {
            uint64_t completed =
                s_completedWorldCloudCubeEvidenceRequest.load(
                    std::memory_order_acquire);
            while (completed < requestId &&
                !s_completedWorldCloudCubeEvidenceRequest.compare_exchange_weak(
                    completed, requestId, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
            }
        }

        void PublishWorldCloudCubeEvidence(
            const WorldCloudCubeEvidenceSnapshot& source) noexcept
        {
            while (s_publishedWorldCloudCubeEvidence.writer.test_and_set(
                std::memory_order_acquire)) {
            }
            const uint64_t oddSequence =
                s_publishedWorldCloudCubeEvidence.sequence.fetch_add(
                    1, std::memory_order_acq_rel) + 1u;
            s_publishedWorldCloudCubeEvidence.valid.store(source.valid, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.requestId.store(source.requestId, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.pairedScreenRequestId.store(source.pairedScreenRequestId, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.epoch.store(source.epoch, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.worldGeneration.store(source.worldGeneration, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.width.store(source.width, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.height.store(source.height, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.mipLevels.store(source.mipLevels, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.coarseMip.store(source.coarseMip, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.activeLayerCount.store(source.activeLayerCount, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.sampledLayerCount.store(source.sampledLayerCount, std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.baseAllFaces.Store(source.baseAllFaces);
            for (uint32_t face = 0; face < kWorldCloudCubeFaceCount; ++face)
                s_publishedWorldCloudCubeEvidence.baseFaces[face].Store(source.baseFaces[face]);
            s_publishedWorldCloudCubeEvidence.coarseAllFaces.Store(source.coarseAllFaces);
            s_publishedWorldCloudCubeEvidence.sequence.store(
                oddSequence + 1u, std::memory_order_release);
            s_publishedWorldCloudCubeEvidence.writer.clear(
                std::memory_order_release);
        }

        void InvalidatePublishedWorldCloudCubeEvidence() noexcept
        {
            PublishWorldCloudCubeEvidence({});
        }

        bool TryGetRenderCamera(
            XMFLOAT3& position,
            XMFLOAT3& previousPosition,
            std::uintptr_t& worldIdentity) noexcept
        {
#if defined(_MSC_VER)
            __try {
#endif
                auto* state = GetGraphicsState();
                auto* tes = FO4CS::EngineAPI::GetTES();
                if (!state || !tes || !tes->worldSpace)
                    return false;
                const auto translate =
                    FO4CS::EngineAPI::ReadCameraPosAdjust(state);
                const auto previousTranslate =
                    FO4CS::EngineAPI::ReadCameraPreviousPosAdjust(state);
                position = { translate.x, translate.y, translate.z };
                previousPosition = {
                    previousTranslate.x, previousTranslate.y,
                    previousTranslate.z
                };
                worldIdentity = reinterpret_cast<std::uintptr_t>(tes->worldSpace);
                return std::isfinite(position.x) &&
                    std::isfinite(position.y) &&
                    std::isfinite(position.z) &&
                    std::isfinite(previousPosition.x) &&
                    std::isfinite(previousPosition.y) &&
                    std::isfinite(previousPosition.z) && worldIdentity != 0;
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
#endif
        }

        bool IsMainPlayerView(ID3D11DeviceContext* context) noexcept
        {
            uint32_t primaryWidth = 0;
            uint32_t primaryHeight = 0;
            if (!context || !GetOutputDimensions(primaryWidth, primaryHeight))
                return false;

            ComPtr<ID3D11RenderTargetView> renderTarget;
            context->OMGetRenderTargets(1, renderTarget.GetAddressOf(), nullptr);
            if (!renderTarget || !IsMainRenderTargetView(renderTarget.Get()))
                return false;

            ComPtr<ID3D11Resource> resource;
            ComPtr<ID3D11Texture2D> texture;
            renderTarget->GetResource(resource.GetAddressOf());
            if (!resource || FAILED(resource.As(&texture)) || !texture)
                return false;

            D3D11_TEXTURE2D_DESC textureDescription{};
            D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
            texture->GetDesc(&textureDescription);
            renderTarget->GetDesc(&viewDescription);
            UINT mip = 0;
            if (viewDescription.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
                mip = viewDescription.Texture2D.MipSlice;
            else if (viewDescription.ViewDimension ==
                     D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
                mip = viewDescription.Texture2DArray.MipSlice;
            else if (viewDescription.ViewDimension !=
                         D3D11_RTV_DIMENSION_TEXTURE2DMS &&
                     viewDescription.ViewDimension !=
                         D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY)
                return false;

            const uint32_t width = (std::max)(
                1u, textureDescription.Width >> mip);
            const uint32_t height = (std::max)(
                1u, textureDescription.Height >> mip);
            if (width != primaryWidth || height != primaryHeight)
                return false;

            const bool stereoRuntime =
                FO4CS::RuntimeAPI::GetSingleton().Target() ==
                FO4CS::F4SECompat::RuntimeTarget::kVR;
            if (stereoRuntime && (primaryWidth & 1u) != 0u)
                return false;

            UINT viewportCount =
                D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            std::array<D3D11_VIEWPORT,
                D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                viewports{};
            context->RSGetViewports(&viewportCount, viewports.data());
            return viewportCount == 1 && viewports[0].Width > 0.0f &&
                viewports[0].Height > 0.0f &&
                viewports[0].TopLeftX >= 0.0f &&
                viewports[0].TopLeftY >= 0.0f &&
                viewports[0].TopLeftX + viewports[0].Width <=
                    static_cast<float>(width) + 0.5f &&
                viewports[0].TopLeftY + viewports[0].Height <=
                    static_cast<float>(height) + 0.5f &&
                (!stereoRuntime ||
                    (viewports[0].TopLeftX == 0.0f &&
                     viewports[0].TopLeftY == 0.0f &&
                     viewports[0].Width == static_cast<float>(width) &&
                     viewports[0].Height == static_cast<float>(height)));
        }

        bool IsMainSkyPlayerView(ID3D11DeviceContext* context) noexcept
        {
            uint32_t primaryWidth = 0;
            uint32_t primaryHeight = 0;
            if (!context || !GetOutputDimensions(primaryWidth, primaryHeight))
                return false;

            ComPtr<ID3D11RenderTargetView> renderTarget;
            context->OMGetRenderTargets(1, renderTarget.GetAddressOf(), nullptr);
            if (!renderTarget ||
                !IsMainSkyRenderTargetView(renderTarget.Get()))
                return false;

            ComPtr<ID3D11Resource> resource;
            ComPtr<ID3D11Texture2D> texture;
            renderTarget->GetResource(resource.GetAddressOf());
            if (!resource || FAILED(resource.As(&texture)) || !texture)
                return false;
            D3D11_TEXTURE2D_DESC textureDescription{};
            texture->GetDesc(&textureDescription);
            if (textureDescription.Width != primaryWidth ||
                textureDescription.Height != primaryHeight)
                return false;

            UINT viewportCount =
                D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            std::array<D3D11_VIEWPORT,
                D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                viewports{};
            context->RSGetViewports(&viewportCount, viewports.data());
            return FO4CS::IsMainSkyViewport(viewports[0], viewportCount,
                primaryWidth, primaryHeight,
                FO4CS::RuntimeAPI::GetSingleton().Target() ==
                    FO4CS::F4SECompat::RuntimeTarget::kVR);
        }

        void ClearCubeSet(
            ID3D11DeviceContext* context,
            CloudCubeSet& set) noexcept
        {
            if (!context)
                return;
            const float clear[4]{};
            ScopedUnpredicated unpredicated(context);
            for (auto& rtv : set.faceRTVs) {
                if (rtv)
                    context->ClearRenderTargetView(rtv.Get(), clear);
            }
        }

        void ResetCaptureProgress() noexcept
        {
            s_captureGate = CaptureGate::kUnknown;
            g_worldCloudPendingEpoch.store(0, std::memory_order_release);
        }

        void CancelCrossfade() noexcept
        {
            s_crossfadeActive = false;
            s_previousProjectionOrigin = {};
            s_crossfadeStartTick = 0;
        }

        void InvalidateCommittedField() noexcept
        {
            FO4CS::CloudComparison::RetirePreview();
            s_snapshots[0].Clear();
            s_snapshots[1].Clear();
            if (s_resources) {
                s_resources->resolvedSrv.Reset();
                s_resources->resolvedTexture.Reset();
                s_resources->evidencePending = false;
                s_resources->evidenceMapRetryCount = 0;
            }
            s_publishedMappingOrigin = {};
            s_publishedMappingOriginValid = false;
            s_mainSkyFrame.Withdraw();
            g_worldCloudPendingEpoch.store(0, std::memory_order_release);
            g_worldCloudPreviewSRV = nullptr;
            g_worldCloudActiveLayers.store(0, std::memory_order_release);
            g_worldCloudCommittedEpoch.store(0, std::memory_order_release);
            InvalidatePublishedWorldCloudCubeEvidence();
            InvalidateShadowMaskState();
        }

        void WithdrawResolvedFieldForRetry(
            bool preserveInFlightFrame = false,
            bool preserveFrameStamp = false) noexcept
        {
            FO4CS::CloudComparison::RetirePreview();
            // A malformed live Sky frame invalidates only the resolved opacity
            // snapshot.  The last fully authenticated geometry-to-UV mapping
            // is still valid and must remain available so the very next Sky
            // frame can resolve again; destroying it here caused multi-second
            // outages while waiting for another natural cubemap generation.
            s_snapshots[0].Clear();
            s_snapshots[1].Clear();
            if (s_resources) {
                s_resources->resolvedSrv.Reset();
                s_resources->resolvedTexture.Reset();
                s_resources->evidencePending = false;
                s_resources->evidenceMapRetryCount = 0;
            }
            s_mainSkyFrame.Withdraw(preserveInFlightFrame);
            g_worldCloudPendingEpoch.store(s_mainSkyFrame.PendingEpoch(),
                std::memory_order_release);
            g_worldCloudPreviewSRV = nullptr;
            g_worldCloudActiveLayers.store(0, std::memory_order_release);
            g_worldCloudCommittedEpoch.store(0, std::memory_order_release);
            InvalidatePublishedWorldCloudCubeEvidence();
            if (preserveFrameStamp) {
                // The frame's own dispatch already consumed the old field;
                // FinalizeFrameAtPresent must still report that dispatch.
                g_shadowMaskValid.store(false, std::memory_order_release);
            } else {
                InvalidateShadowMaskState();
            }
        }

        void ResetWorldFieldForTransition(
            ID3D11DeviceContext* context,
            std::uintptr_t worldIdentity,
            const XMFLOAT3& cameraPosition) noexcept
        {
            s_mainSkyFrame.Reset();
            ResetCaptureProgress();
            s_worldIdentity = worldIdentity;
            s_lastCameraPosition = cameraPosition;
            s_lastCameraPositionValid = worldIdentity != 0;
            s_projectionOrigin = cameraPosition;
            s_projectionOriginValid = worldIdentity != 0;
            s_confirmedProjectionOrigin.Reset();
            s_mainViewCameraReadback.Reset();
            CancelCrossfade();
            s_nextAnchorReadbackTick = 0;
            if (s_resources) {
                ClearCubeSet(context, s_resources->cubeSets[0]);
                ClearCubeSet(context, s_resources->cubeSets[1]);
                g_worldCloudPreviewSRV =
                    s_resources->cubeSets[s_mainSkyFrame.ReadIndex()].srv.Get();
            }
            InvalidateCommittedField();
            g_worldCloudResetGeneration.fetch_add(
                1, std::memory_order_acq_rel);
            SPDLOG_INFO(
                "[CloudShadows] Native cloud-opacity cube reset for "
                "world/load transition (world={} camera={:.1f},{:.1f},{:.1f})",
                reinterpret_cast<void*>(worldIdentity), cameraPosition.x,
                cameraPosition.y, cameraPosition.z);
        }

        bool SynchronizeWorldField(
            ID3D11DeviceContext* context,
            bool& fieldReset,
            XMFLOAT3* currentCameraPosition = nullptr,
            bool observeCamera = true) noexcept
        {
            fieldReset = false;
            const bool explicitReset =
                s_resetRequested.exchange(false, std::memory_order_acq_rel);
            XMFLOAT3 cameraPosition{};
            XMFLOAT3 previousCameraPosition{};
            std::uintptr_t worldIdentity = 0;
            if (!TryGetRenderCamera(
                    cameraPosition, previousCameraPosition, worldIdentity)) {
                if (explicitReset) {
                    ResetWorldFieldForTransition(context, 0, {});
                    fieldReset = true;
                }
                return false;
            }
            // Present may follow a temporary reflection/UI camera. It can
            // observe load/world changes, but cannot replace the origin that
            // an authenticated main-Sky/main-light draw already established.
            if (!observeCamera) {
                const auto action = FO4CS::ClassifyPresentWorld(
                    explicitReset, s_worldIdentity, worldIdentity);
                if (action == FO4CS::PresentWorldAction::kReset) {
                    // The next authenticated main draw must initialize the new
                    // world's origin; neither the old nor the Present camera can.
                    ResetWorldFieldForTransition(context, 0, {});
                    fieldReset = true;
                    return false;
                }
                // No established world yet: the field is already empty, so
                // wait for the main draw instead of resetting every Present.
                if (action == FO4CS::PresentWorldAction::kWait ||
                    !s_lastCameraPositionValid)
                    return false;
                cameraPosition = s_lastCameraPosition;
            }
            if (currentCameraPosition)
                *currentCameraPosition = cameraPosition;

            const float dx = cameraPosition.x - s_lastCameraPosition.x;
            const float dy = cameraPosition.y - s_lastCameraPosition.y;
            const float dz = cameraPosition.z - s_lastCameraPosition.z;
            constexpr float discontinuityThreshold = 65536.0f;
            const bool cameraDiscontinuity = s_lastCameraPositionValid &&
                dx * dx + dy * dy + dz * dz >
                    discontinuityThreshold * discontinuityThreshold;
            const bool worldChanged =
                s_worldIdentity != 0 && s_worldIdentity != worldIdentity;

            if (explicitReset || worldChanged || cameraDiscontinuity) {
                ResetWorldFieldForTransition(
                    context, worldIdentity, cameraPosition);
                fieldReset = true;
            } else if (s_worldIdentity == 0) {
                s_worldIdentity = worldIdentity;
                s_projectionOrigin = cameraPosition;
                s_projectionOriginValid = true;
                SPDLOG_INFO(
                    "[CloudShadows] Native cloud cube projection origin "
                    "initialized at {:.1f},{:.1f},{:.1f}",
                    cameraPosition.x, cameraPosition.y, cameraPosition.z);
            }

            s_lastCameraPosition = cameraPosition;
            s_lastCameraPositionValid = true;
            return true;
        }

        bool CreateCubeSet(
            ID3D11Device* device,
            uint32_t faceSize,
            CloudCubeSet& set,
            uint32_t& mipLevels) noexcept
        {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = faceSize;
            description.Height = faceSize;
            description.MipLevels = 0;
            description.ArraySize = kWorldCloudCubeFaceCount;
            description.Format = kWorldCloudFormat;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags =
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            description.MiscFlags =
                D3D11_RESOURCE_MISC_TEXTURECUBE |
                D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(device->CreateTexture2D(
                    &description, nullptr, set.texture.GetAddressOf())))
                return false;

            set.texture->GetDesc(&description);
            mipLevels = description.MipLevels;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
            srvDescription.Format = kWorldCloudFormat;
            srvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDescription.TextureCube.MostDetailedMip = 0;
            srvDescription.TextureCube.MipLevels = UINT(-1);
            if (FAILED(device->CreateShaderResourceView(
                    set.texture.Get(), &srvDescription,
                    set.srv.GetAddressOf())))
                return false;

            for (uint32_t face = 0;
                 face < kWorldCloudCubeFaceCount; ++face) {
                D3D11_RENDER_TARGET_VIEW_DESC rtvDescription{};
                rtvDescription.Format = kWorldCloudFormat;
                rtvDescription.ViewDimension =
                    D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDescription.Texture2DArray.MipSlice = 0;
                rtvDescription.Texture2DArray.FirstArraySlice = face;
                rtvDescription.Texture2DArray.ArraySize = 1;
                if (FAILED(device->CreateRenderTargetView(
                        set.texture.Get(), &rtvDescription,
                        set.faceRTVs[face].GetAddressOf())))
                    return false;
            }
            return true;
        }

        bool CreateEvidenceResources(
            WorldCloudResources& resources,
            ID3D11Texture2D* sourceTexture = nullptr) noexcept
        {
            D3D11_TEXTURE2D_DESC description{};
            auto* source = sourceTexture
                ? sourceTexture
                : resources.cubeSets[0].texture.Get();
            if (!source)
                return false;
            source->GetDesc(&description);
            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            description.MiscFlags = 0;
            resources.evidenceStaging.Reset();
            resources.evidenceQuery.Reset();
            D3D11_QUERY_DESC queryDescription{};
            queryDescription.Query = D3D11_QUERY_EVENT;
            if (FAILED(resources.device->CreateTexture2D(
                    &description, nullptr,
                    resources.evidenceStaging.GetAddressOf())) ||
                FAILED(resources.device->CreateQuery(
                    &queryDescription,
                    resources.evidenceQuery.GetAddressOf()))) {
                resources.evidenceStaging.Reset();
                resources.evidenceQuery.Reset();
                resources.nextEvidenceAllocationAttempt = GetTickCount64() + 5000;
                SPDLOG_WARN(
                    "[CloudShadows] Native cloud-cube GPU evidence "
                    "readback unavailable");
                return false;
            }
            return true;
        }

        bool EnsureCaptureDimensions(
            ID3D11DeviceContext* context,
            uint32_t width, uint32_t height) noexcept
        {
            if (!context || context != GetD3DContext() || !s_resources ||
                s_resources->device.Get() != GetD3DDevice())
                return false;

            if (s_resources->faceWidth == width &&
                s_resources->faceHeight == height &&
                s_resources->cubeSets[0].texture &&
                s_resources->cubeSets[1].texture)
                return true;
            if (s_cubeCreation.Blocked())
                return false;

            CloudCubeSet replacement[2];
            uint32_t mipLevels0 = 0;
            uint32_t mipLevels1 = 0;
            if (!CreateCubeSet(
                    s_resources->device.Get(), width, replacement[0],
                    mipLevels0) ||
                !CreateCubeSet(
                    s_resources->device.Get(), width, replacement[1],
                    mipLevels1) ||
                mipLevels0 == 0 || mipLevels0 != mipLevels1) {
                if (s_cubeCreation.Fail()) {
                    SPDLOG_ERROR(
                        "[CloudShadows] Failed creating {}x{} double-buffered "
                        "R16 cloud-opacity cube (failure #{}); retrying with backoff",
                        width, height, s_cubeCreation.failures);
                }
                return false;
            }
            s_cubeCreation.Succeed();

            s_resources->cubeSets[0] = std::move(replacement[0]);
            s_resources->cubeSets[1] = std::move(replacement[1]);
            s_resources->faceWidth = width;
            s_resources->faceHeight = height;
            s_resources->mipLevels = mipLevels0;
            s_resources->evidenceStaging.Reset();
            s_resources->evidenceQuery.Reset();
            s_resources->evidencePending = false;
            s_resources->nextEvidenceAllocationAttempt = 0;

            ClearCubeSet(context, s_resources->cubeSets[0]);
            ClearCubeSet(context, s_resources->cubeSets[1]);
            s_mainSkyFrame.Reset();
            ResetCaptureProgress();
            InvalidateCommittedField();
            g_worldCloudResetGeneration.fetch_add(
                1, std::memory_order_acq_rel);
            g_worldCloudPreviewSRV =
                s_resources->cubeSets[s_mainSkyFrame.ReadIndex()].srv.Get();
            g_worldCloudReady.store(true, std::memory_order_release);
            SPDLOG_INFO(
                "[CloudShadows] Native cloud-opacity field ready: "
                "{}x{} R16_FLOAT TextureCube, {} mips, double buffered",
                width, height, mipLevels0);
            return true;
        }

        bool EvidenceCaptureNeeded(
            const WorldCloudResources& resources) noexcept
        {
            return !resources.evidencePending &&
                s_worldCloudCubeEvidenceRequest.load(
                    std::memory_order_acquire) >
                s_completedWorldCloudCubeEvidenceRequest.load(
                    std::memory_order_acquire);
        }

        void ArmCaptureEvidence(
            ID3D11DeviceContext* context,
            WorldCloudResources& resources,
            ID3D11Texture2D* capturedCube,
            uint64_t epoch) noexcept
        {
            if (!context || !capturedCube || !EvidenceCaptureNeeded(resources))
                return;
            D3D11_TEXTURE2D_DESC sourceDescription{};
            capturedCube->GetDesc(&sourceDescription);
            D3D11_TEXTURE2D_DESC stagingDescription{};
            if (resources.evidenceStaging)
                resources.evidenceStaging->GetDesc(&stagingDescription);
            const bool matchingStaging = resources.evidenceStaging &&
                resources.evidenceQuery &&
                stagingDescription.Width == sourceDescription.Width &&
                stagingDescription.Height == sourceDescription.Height &&
                stagingDescription.MipLevels == sourceDescription.MipLevels &&
                stagingDescription.ArraySize == sourceDescription.ArraySize &&
                stagingDescription.Format == sourceDescription.Format &&
                stagingDescription.SampleDesc.Count ==
                    sourceDescription.SampleDesc.Count;
            if (!matchingStaging) {
                if (GetTickCount64() < resources.nextEvidenceAllocationAttempt ||
                    !CreateEvidenceResources(resources, capturedCube)) {
                    return;
                }
            }
            const uint64_t requestId =
                s_worldCloudCubeEvidenceRequest.load(
                    std::memory_order_acquire);
            // Production uses LOD zero. Build diagnostic coarse mips only
            // when their contents are actually going to be read back.
            context->GenerateMips(resources.resolvedSrv.Get());
            context->CopyResource(resources.evidenceStaging.Get(), capturedCube);
            context->End(resources.evidenceQuery.Get());
            resources.evidenceEpoch = epoch;
            resources.evidenceWorldGeneration =
                g_worldCloudResetGeneration.load(std::memory_order_acquire);
            resources.evidenceRequestId = requestId;
            resources.evidenceScreenRequestId =
                RequestScreenMaskEvidenceCapture();
            resources.evidenceMapRetryCount = 0;
            resources.evidencePending = true;
        }

        enum class EvidenceMapResult : uint8_t
        {
            kSuccess,
            kBusy,
            kFailed
        };

        EvidenceMapResult AccumulateEvidenceMip(
            ID3D11DeviceContext* context,
            WorldCloudResources& resources,
            uint32_t mip,
            std::array<CaptureEvidenceStats,
                kWorldCloudCubeFaceCount>& faceStats,
            CaptureEvidenceStats& globalStats) noexcept
        {
            if (!context || !resources.evidenceStaging)
                return EvidenceMapResult::kFailed;
            D3D11_TEXTURE2D_DESC description{};
            resources.evidenceStaging->GetDesc(&description);
            const bool supportedFormat =
                description.Format == DXGI_FORMAT_R8_UNORM ||
                description.Format == DXGI_FORMAT_R16_FLOAT ||
                description.Format == DXGI_FORMAT_R32_FLOAT;
            if (!supportedFormat || mip >= description.MipLevels ||
                description.ArraySize != kWorldCloudCubeFaceCount)
                return EvidenceMapResult::kFailed;

            const uint32_t width = (std::max)(
                1u, description.Width >> mip);
            const uint32_t height = (std::max)(
                1u, description.Height >> mip);
            for (uint32_t face = 0;
                 face < kWorldCloudCubeFaceCount; ++face) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                const uint32_t subresource = D3D11CalcSubresource(
                    mip, face, description.MipLevels);
                const HRESULT mapResult = context->Map(
                    resources.evidenceStaging.Get(), subresource,
                    D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (mapResult == DXGI_ERROR_WAS_STILL_DRAWING)
                    return EvidenceMapResult::kBusy;
                if (FAILED(mapResult) || !mapped.pData)
                    return EvidenceMapResult::kFailed;

                for (uint32_t y = 0; y < height; ++y) {
                    const auto* row =
                        static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch;
                    for (uint32_t x = 0; x < width; ++x) {
                        float value = 0.0f;
                        if (description.Format == DXGI_FORMAT_R8_UNORM) {
                            value = static_cast<float>(row[x]) / 255.0f;
                        } else if (description.Format ==
                                   DXGI_FORMAT_R16_FLOAT) {
                            value = DirectX::PackedVector::XMConvertHalfToFloat(
                                reinterpret_cast<const uint16_t*>(row)[x]);
                        } else {
                            value = reinterpret_cast<const float*>(row)[x];
                        }
                        faceStats[face].Add(value);
                        globalStats.Add(value);
                    }
                }
                context->Unmap(resources.evidenceStaging.Get(), subresource);
            }
            return EvidenceMapResult::kSuccess;
        }

        void PollCaptureEvidence(
            ID3D11DeviceContext* context,
            WorldCloudResources& resources) noexcept
        {
            if (!context || !resources.evidencePending ||
                !resources.evidenceQuery || !resources.evidenceStaging)
                return;

            auto abandon = [&](const char* reason) noexcept {
                SPDLOG_WARN(
                    "[CloudShadows] Discarding native cloud-cube evidence: {}",
                    reason);
                resources.evidencePending = false;
                resources.evidenceMapRetryCount = 0;
                InvalidatePublishedWorldCloudCubeEvidence();
            };

            BOOL complete = FALSE;
            const HRESULT queryResult = context->GetData(
                resources.evidenceQuery.Get(), &complete, sizeof(complete),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (queryResult == S_FALSE ||
                (queryResult == S_OK && !complete))
                return;
            if (queryResult != S_OK) {
                abandon("event-query failure");
                return;
            }
            if (resources.evidenceWorldGeneration !=
                g_worldCloudResetGeneration.load(std::memory_order_acquire)) {
                abandon("world generation changed before completion");
                return;
            }

            D3D11_TEXTURE2D_DESC description{};
            resources.evidenceStaging->GetDesc(&description);
            const uint32_t coarseMip = description.MipLevels != 0
                ? description.MipLevels - 1u
                : 0u;
            std::array<CaptureEvidenceStats,
                kWorldCloudCubeFaceCount> baseFaces{};
            std::array<CaptureEvidenceStats,
                kWorldCloudCubeFaceCount> coarseFaces{};
            CaptureEvidenceStats baseGlobal{};
            CaptureEvidenceStats coarseGlobal{};
            const auto baseResult = AccumulateEvidenceMip(
                context, resources, 0, baseFaces, baseGlobal);
            const auto coarseResult =
                baseResult == EvidenceMapResult::kSuccess
                ? AccumulateEvidenceMip(
                    context, resources, coarseMip,
                    coarseFaces, coarseGlobal)
                : baseResult;
            if (baseResult == EvidenceMapResult::kBusy ||
                coarseResult == EvidenceMapResult::kBusy) {
                constexpr uint32_t kMaximumMapRetries = 120;
                if (++resources.evidenceMapRetryCount >= kMaximumMapRetries)
                    abandon("map retry budget exhausted");
                return;
            }
            if (baseResult != EvidenceMapResult::kSuccess ||
                coarseResult != EvidenceMapResult::kSuccess) {
                abandon("staging map/resource failure");
                return;
            }

            WorldCloudCubeEvidenceSnapshot snapshot{};
            snapshot.valid = true;
            snapshot.requestId = resources.evidenceRequestId;
            snapshot.pairedScreenRequestId =
                resources.evidenceScreenRequestId;
            snapshot.epoch = resources.evidenceEpoch;
            snapshot.worldGeneration = resources.evidenceWorldGeneration;
            snapshot.width = description.Width;
            snapshot.height = description.Height;
            snapshot.mipLevels = description.MipLevels;
            snapshot.coarseMip = coarseMip;
            snapshot.activeLayerCount = 1;
            snapshot.sampledLayerCount = 1;
            snapshot.baseAllFaces = SummarizeCaptureEvidence(baseGlobal);
            for (uint32_t face = 0;
                 face < kWorldCloudCubeFaceCount; ++face)
                snapshot.baseFaces[face] =
                    SummarizeCaptureEvidence(baseFaces[face]);
            snapshot.coarseAllFaces =
                SummarizeCaptureEvidence(coarseGlobal);
            PublishWorldCloudCubeEvidence(snapshot);
            AdvanceCompletedEvidenceRequest(resources.evidenceRequestId);
            resources.evidencePending = false;
            resources.evidenceMapRetryCount = 0;

            SPDLOG_INFO(
                "[CloudShadows] Native cloud-cube evidence epoch={} "
                "min={:.6f} max={:.6f} mean={:.6f} stddev={:.6f} "
                "clear={}/{} opaque={}/{}",
                snapshot.epoch, snapshot.baseAllFaces.minimum,
                snapshot.baseAllFaces.maximum,
                snapshot.baseAllFaces.mean,
                snapshot.baseAllFaces.standardDeviation,
                snapshot.baseAllFaces.clearCount,
                snapshot.baseAllFaces.finiteCount,
                snapshot.baseAllFaces.opaqueCount,
                snapshot.baseAllFaces.finiteCount);
            for (uint32_t face = 0; face < kWorldCloudCubeFaceCount; ++face) {
                const auto& values = snapshot.baseFaces[face];
                SPDLOG_INFO(
                    "[CloudShadows] Cloud-cube face evidence epoch={} face={} "
                    "min={:.6f} max={:.6f} mean={:.6f} clear={}/{} opaque={}/{}",
                    snapshot.epoch, face, values.minimum, values.maximum,
                    values.mean, values.clearCount, values.finiteCount,
                    values.opaqueCount, values.finiteCount);
            }
        }

    }

    bool GetWorldCloudCubeEvidenceSnapshot(
        WorldCloudCubeEvidenceSnapshot& destination) noexcept
    {
        constexpr uint32_t kMaximumAttempts = 64;
        for (uint32_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
            const uint64_t before =
                s_publishedWorldCloudCubeEvidence.sequence.load(
                    std::memory_order_acquire);
            if ((before & 1u) != 0u)
                continue;

            WorldCloudCubeEvidenceSnapshot candidate{};
            candidate.valid = s_publishedWorldCloudCubeEvidence.valid.load(std::memory_order_relaxed);
            candidate.requestId = s_publishedWorldCloudCubeEvidence.requestId.load(std::memory_order_relaxed);
            candidate.pairedScreenRequestId = s_publishedWorldCloudCubeEvidence.pairedScreenRequestId.load(std::memory_order_relaxed);
            candidate.epoch = s_publishedWorldCloudCubeEvidence.epoch.load(std::memory_order_relaxed);
            candidate.worldGeneration = s_publishedWorldCloudCubeEvidence.worldGeneration.load(std::memory_order_relaxed);
            candidate.width = s_publishedWorldCloudCubeEvidence.width.load(std::memory_order_relaxed);
            candidate.height = s_publishedWorldCloudCubeEvidence.height.load(std::memory_order_relaxed);
            candidate.mipLevels = s_publishedWorldCloudCubeEvidence.mipLevels.load(std::memory_order_relaxed);
            candidate.coarseMip = s_publishedWorldCloudCubeEvidence.coarseMip.load(std::memory_order_relaxed);
            candidate.activeLayerCount = s_publishedWorldCloudCubeEvidence.activeLayerCount.load(std::memory_order_relaxed);
            candidate.sampledLayerCount = s_publishedWorldCloudCubeEvidence.sampledLayerCount.load(std::memory_order_relaxed);
            s_publishedWorldCloudCubeEvidence.baseAllFaces.Load(candidate.baseAllFaces);
            for (uint32_t face = 0;
                 face < kWorldCloudCubeFaceCount; ++face)
                s_publishedWorldCloudCubeEvidence.baseFaces[face].Load(candidate.baseFaces[face]);
            s_publishedWorldCloudCubeEvidence.coarseAllFaces.Load(candidate.coarseAllFaces);

            const uint64_t after =
                s_publishedWorldCloudCubeEvidence.sequence.load(
                    std::memory_order_acquire);
            if (before == after && (after & 1u) == 0u) {
                candidate.generation = after / 2u;
                destination = candidate;
                return true;
            }
        }
        return false;
    }

    uint64_t RequestWorldCloudCubeEvidenceCapture() noexcept
    {
        return s_worldCloudCubeEvidenceRequest.fetch_add(
            1, std::memory_order_acq_rel) + 1u;
    }

    void CancelPendingWorldCloudCubeEvidenceRequests() noexcept
    {
        // Mark only requests which have not yet armed as satisfied. A staging
        // copy/query already in flight remains pending and is polled normally;
        // abandoning it would allow unsafe resource reuse before GPU completion.
        AdvanceCompletedEvidenceRequest(
            s_worldCloudCubeEvidenceRequest.load(std::memory_order_acquire));
    }

    bool IsMainWorldView(ID3D11DeviceContext* context) noexcept
    {
        return context && context == GetD3DContext() &&
            IsMainPlayerView(context);
    }

    bool IsMainSkyWorldView(ID3D11DeviceContext* context) noexcept
    {
        return context && context == GetD3DContext() &&
            IsMainSkyPlayerView(context);
    }

    void ObserveMainWorldCloudCamera(
        ID3D11DeviceContext* context) noexcept
    {
        try {
            if (!context || context != GetD3DContext())
                return;
            std::lock_guard lock(s_resourceMutex);
            if (!s_resources)
                return;
            bool fieldReset = false;
            (void)SynchronizeWorldField(context, fieldReset);
        } catch (...) {
        }
    }

    bool CreateWorldCloudResources()
    {
        std::lock_guard lock(s_resourceMutex);
        auto* device = GetD3DDevice();
        if (!device || !GetD3DContext())
            return false;
        if (s_resources && s_resources->device.Get() == device)
            return true;

        g_worldCloudReady.store(false, std::memory_order_release);
        g_worldCloudPreviewSRV = nullptr;
        InvalidatePublishedWorldCloudCubeEvidence();
        InvalidateShadowMaskState();

        auto candidate = std::make_unique<WorldCloudResources>();
        candidate->device = device;
        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(
                &samplerDescription,
                candidate->sampler.GetAddressOf()))) {
            SPDLOG_ERROR(
                "[CloudShadows] Failed creating native cloud cube sampler");
            return false;
        }
        s_resources = std::move(candidate);
        s_snapshots[0].Clear();
        s_snapshots[1].Clear();
        s_mainSkyFrame.Reset();
        ResetCaptureProgress();
        s_worldIdentity = 0;
        s_lastCameraPosition = {};
        s_lastCameraPositionValid = false;
        s_projectionOrigin = {};
        s_projectionOriginValid = false;
        s_confirmedProjectionOrigin.Reset();
        s_mainViewCameraReadback.Reset();
        CancelCrossfade();
        s_nextAnchorReadbackTick = 0;
        s_mainViewRenderedSincePresent = false;
        s_cubeCreation.Succeed();
        s_publishedMappingOrigin = {};
        s_publishedMappingOriginValid = false;
        s_mainSkyFrame.Reset(true);
        s_mainSkyFrame.CancelCapture();
        s_resetRequested.store(false, std::memory_order_release);
        SPDLOG_INFO(
            "[CloudShadows] Native cloud-opacity manager ready; awaiting "
            "an authenticated main-Sky geometry frame");
        return true;
    }

    void ReleaseWorldCloudResources() noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        g_worldCloudReady.store(false, std::memory_order_release);
        g_worldCloudPreviewSRV = nullptr;
        FO4CS::CloudGeometryCapture::ReleaseDeviceResources();
        s_resources.reset();
        s_snapshots[0].Clear();
        s_snapshots[1].Clear();
        s_mainSkyFrame.Reset();
        ResetCaptureProgress();
        CancelCrossfade();
        s_nextAnchorReadbackTick = 0;
        s_mainViewRenderedSincePresent = false;
        s_cubeCreation.Succeed();
        s_worldIdentity = 0;
        s_lastCameraPosition = {};
        s_lastCameraPositionValid = false;
        s_projectionOrigin = {};
        s_projectionOriginValid = false;
        s_confirmedProjectionOrigin.Reset();
        s_mainViewCameraReadback.Reset();
        s_publishedMappingOrigin = {};
        s_publishedMappingOriginValid = false;
        s_mainSkyFrame.Reset(true);
        s_mainSkyFrame.CancelCapture();
        s_resetRequested.store(false, std::memory_order_release);
        g_worldCloudActiveLayers.store(0, std::memory_order_release);
        g_worldCloudCommittedEpoch.store(0, std::memory_order_release);
        InvalidatePublishedWorldCloudCubeEvidence();
        InvalidateShadowMaskState();
    }

    namespace
    {
        // Nothing the capture could produce is visible: the ground mask and
        // the lighting patch are both exactly neutral in these states, so the
        // cube clear, the per-layer replays and the full-screen dispatch are
        // skipped. Explicit diagnostics (sky preview, acceptance evidence,
        // debug views) keep the pipeline running.
        [[nodiscard]] bool DiagnosticConsumerActive() noexcept
        {
            return FO4CS::CloudComparison::GetPreview() !=
                    FO4CS::CloudComparison::Preview::Off ||
                AcceptanceRunner::IsRunning() || g_settings.DebugMode != 0.0f;
        }

        [[nodiscard]] bool CaptureCanBeVisible() noexcept
        {
            if (DiagnosticConsumerActive())
                return true;
            if (!(g_settings.Opacity > 0.0f) || !g_dfLightPatcher.IsReady())
                return false;
            XMFLOAT3 sun{};
            return FO4CS::EngineAPI::ReadVisibleSunDirection(
                       FO4CS::EngineAPI::GetSky(), sun) &&
                sun.z > 0.0f;
        }
    }

    bool CloudShadowsCanBeVisible() noexcept
    {
        try {
            return CaptureCanBeVisible();
        } catch (...) {
            return false;
        }
    }

    bool ProcessWorldCloudDraw(
        ID3D11DeviceContext* context,
        const CloudDrawCommand& command,
        CloudDrawReissue reissue) noexcept
    {
        FO4CS::CpuProfile::Scope cpuTotal(FO4CS::CpuProfile::Stage::WorldCapture);
        bool drawInvoked = false;
        try {
            FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CloudDrawValidation);
            // The caller has already proven the exact main-Sky view once for
            // this draw; repeating it here doubled the per-draw D3D queries.
            if (!context || context != GetD3DContext() || !reissue)
                return false;

            cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureSetup);
            bool captureThisDraw = false;
            {
                std::lock_guard lock(s_resourceMutex);
                if (!s_resources)
                    return false;
                s_mainViewRenderedSincePresent = true;
                if (s_captureGate == CaptureGate::kUnknown) {
                    s_captureGate = CaptureCanBeVisible()
                        ? CaptureGate::kCapture : CaptureGate::kSkip;
                }
                if (s_captureGate == CaptureGate::kCapture && !s_mainSkyFrame.Active()) {
                    bool reset = false;
                    if (EnsureCaptureDimensions(context,
                            kGeometryMappingFaceSize, kGeometryMappingFaceSize) &&
                        SynchronizeWorldField(context, reset) &&
                        s_projectionOriginValid) {
                        FO4CS::CloudComparison::GpuScope captureTiming(context,
                            FO4CS::CloudComparison::Work::Capture);
                        ClearCubeSet(context, s_resources->cubeSets[s_mainSkyFrame.WriteIndex()]);
                        s_mainSkyFrame.Begin();
                        s_publishedMappingOrigin = s_projectionOrigin;
                        s_publishedMappingOriginValid = true;
                        g_worldCloudPendingEpoch.store(s_mainSkyFrame.Serial(),
                            std::memory_order_release);
                        g_geometryCaptureAttempts.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                // A rejected frame is already withdrawn; replaying its remaining
                // layers would only spend GPU time on a field nobody reads.
                captureThisDraw = s_mainSkyFrame.Active() &&
                    s_mainSkyFrame.Authenticated() && IsCloudTechnique(command.skyTechnique);
            }

            // Submit the visible draw once, then blend every cloud primitive
            // into private targets. Overlapping surfaces cannot be encoded in a
            // single UV per texel; replaying alpha preserves all of them.
            cpuTiming.Set(FO4CS::CpuProfile::Stage::VisibleDraw);
            reissue(context, command);
            cpuTiming.Set(FO4CS::CpuProfile::Stage::CaptureSetup);
            drawInvoked = true;
            if (!captureThisDraw)
                return true;
#if FO4CS_ENABLE_DEVELOPER_TOOLS
            LogSkyDiagnostics(context, command.skyTechnique);
#endif

            const bool authenticated =
                command.authentication ==
                    CloudDrawAuthentication::kStockVisibleCloudShaders;
            std::array<ID3D11RenderTargetView*, 6> targets{};
            for (uint32_t face = 0; face < 6; ++face)
                targets[face] = s_resources->cubeSets[s_mainSkyFrame.WriteIndex()].faceRTVs[face].Get();
            struct Submission { ID3D11DeviceContext* context;
                const CloudDrawCommand* command; CloudDrawReissue reissue;
            } submission{context, &command, reissue};
            const auto layout = FO4CS::RuntimeAPI::GetSingleton().Target() ==
                    FO4CS::F4SECompat::RuntimeTarget::kVR
                ? FO4CS::CloudGeometryCapture::SkyConstantLayout::kVr
                : FO4CS::CloudGeometryCapture::SkyConstantLayout::kFlat;
            const auto submit = [](void* user) {
                const auto& draw = *static_cast<Submission*>(user);
                draw.reissue(draw.context, *draw.command);
            };
            const auto technique = static_cast<FO4CS::CloudGeometryCapture::CloudTechnique>(command.skyTechnique);
            auto reason = FO4CS::CloudGeometryCapture::RejectReason::kNone;
            bool accumulated = false;
            if (!authenticated) {
                reason = FO4CS::CloudGeometryCapture::RejectReason::kInvalidRequest;
            } else {
                FO4CS::CloudComparison::GpuScope captureTiming(context,
                    FO4CS::CloudComparison::Work::Capture,
                    FO4CS::CloudGeometryCapture::kCapturedCubeFaceCount);
                accumulated = FO4CS::CloudGeometryCapture::AccumulateOpacity(context, layout,
                    technique, kGeometryMappingFaceSize, targets, submit, &submission, &reason);
            }
            if (accumulated) {
                g_cloudDrawCount.fetch_add(1, std::memory_order_relaxed);
                g_geometryCaptureDraws.fetch_add(
                    FO4CS::CloudGeometryCapture::kCapturedCubeFaceCount,
                    std::memory_order_relaxed);
            } else {
                if (authenticated)
                    LogCaptureReject(reason, command.skyTechnique);
                std::lock_guard lock(s_resourceMutex);
                s_mainSkyFrame.Reject();
                // A known-invalid current Sky must not leave the previous
                // frame's cloud field visible to a later GameWorks volume
                // or DFLight draw in this same frame.
                WithdrawResolvedFieldForRetry(true);
            }
            return true;
        } catch (...) {
            std::lock_guard lock(s_resourceMutex);
            s_mainSkyFrame.Reject();
            if (s_mainSkyFrame.Active())
                WithdrawResolvedFieldForRetry(true);
            return drawInvoked;
        }
    }

    void CommitWorldCloudFrame(ID3D11DeviceContext* context) noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        if (!context || context != GetD3DContext() || !s_resources)
            return;
        s_mainViewRenderedSincePresent = true;
        PollCaptureEvidence(context, *s_resources);
        bool fieldReset = false;
        SynchronizeWorldField(context, fieldReset);
        // If this Sky was positively rejected, do not let a prior-frame cloud
        // field shade the later DFLight pass.  Keep the resolver frame alive so
        // Present can close it transactionally, and retain the captured mapping
        // so the next valid Sky retries immediately.
        if (!fieldReset && s_mainSkyFrame.Active() &&
            !s_mainSkyFrame.Authenticated()) {
            WithdrawResolvedFieldForRetry(true);
        }
    }

    void CommitWorldCloudFrameAtPresent(
        ID3D11DeviceContext* context) noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        if (!context || context != GetD3DContext() || !s_resources)
            return;
        const bool mainViewRendered = std::exchange(s_mainViewRenderedSincePresent, false);
        s_captureGate = CaptureGate::kUnknown;
        // Loading screens and companion windows present thousands of times
        // per second with nothing rendered, captured or published: skip the
        // world synchronization unless a reset or a real frame needs it.
        if (!mainViewRendered && !s_mainSkyFrame.Active() &&
            s_mainSkyFrame.PublishedEpoch() == 0 && !s_resources->evidencePending &&
            !s_resetRequested.load(std::memory_order_acquire))
            return;
        PollCaptureEvidence(context, *s_resources);
        bool reset = false;
        const bool worldReady = SynchronizeWorldField(context, reset, nullptr, false);
        const bool rejected = s_mainSkyFrame.Active() && !s_mainSkyFrame.Authenticated();
        const auto completedIndex = s_mainSkyFrame.WriteIndex();
        const auto publication = s_mainSkyFrame.CompletePresent(worldReady, mainViewRendered);
        if (publication == FO4CS::CloudFramePublication::PresentResult::Retained)
            return;
        if (publication == FO4CS::CloudFramePublication::PresentResult::Withdrawn) {
            if (rejected)
                g_geometryCaptureRejected.fetch_add(1, std::memory_order_relaxed);
            if (s_snapshots[s_mainSkyFrame.ReadIndex()].active)
                WithdrawResolvedFieldForRetry(false, true);
            g_worldCloudPendingEpoch.store(0, std::memory_order_release);
            return;
        }
        auto& completed = s_resources->cubeSets[completedIndex];
        if (g_settings.DebugMode > 3.5f && g_settings.DebugMode < 4.5f)
            context->GenerateMips(completed.srv.Get());

        CloudSnapshot snapshot{};
        snapshot.epoch = s_mainSkyFrame.Serial();
        snapshot.captureOrigin = s_publishedMappingOrigin;
        snapshot.captureOriginValid = true;
        snapshot.active = true;
        snapshot.layer.stableLayerId = kCompositeCloudFieldId;
        snapshot.layer.PlaneZ = s_publishedMappingOrigin.z +
            g_settings.CloudHeight;
        snapshot.layer.WorldScale = 1.0f;
        snapshot.layer.VerticalOpticalDepth = 1.0f;
        snapshot.layer.ActiveBlend = 1.0f;
        snapshot.layer.CoverageScale = 1.0f;
        snapshot.layer.MipBias = 0.0f;
        snapshot.layer.SliceIndex = 0;
        s_snapshots[s_mainSkyFrame.ReadIndex()] = snapshot;
        s_resources->resolvedSrv = completed.srv;
        s_resources->resolvedTexture = completed.texture;
        g_worldCloudPreviewSRV = s_resources->resolvedSrv.Get();
        g_worldCloudActiveLayers.store(1, std::memory_order_release);
        g_worldCloudCommittedEpoch.store(
            snapshot.epoch, std::memory_order_release);
        g_worldCloudPendingEpoch.store(0, std::memory_order_release);
        // Publication happens inside FinalizeFrameAtPresent, after this
        // frame's DFLight mask has already been consumed.  Do not clear the
        // success stamp here: the caller exchanges it immediately afterward
        // to report the completed frame.  The next Prepass always rebuilds the
        // mask from this newly published live cloud field.

        ArmCaptureEvidence(context, *s_resources, s_resources->resolvedTexture.Get(), snapshot.epoch);
        g_geometryCapturePublished.fetch_add(1, std::memory_order_relaxed);
    }

    void RequestWorldCloudReset() noexcept
    {
        s_resetRequested.store(true, std::memory_order_release);
        InvalidateShadowMaskState();
    }

    void InvalidateWorldCloudCaptureForToggle() noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        // F10 discards the incomplete capture and published field. The next
        // authenticated player Sky recreates opacity immediately.
        auto* context = GetD3DContext();
        ResetCaptureProgress();
        WithdrawResolvedFieldForRetry();
        if (s_resources && context) {
            ClearCubeSet(
                context, s_resources->cubeSets[s_mainSkyFrame.ReadIndex()]);
            ClearCubeSet(
                context, s_resources->cubeSets[s_mainSkyFrame.WriteIndex()]);
        }
    }

    uint32_t CopyCommittedWorldCloudLayers(
        WorldCloudLayerState* destination,
        uint32_t capacity) noexcept
    {
        if (!destination || capacity == 0)
            return 0;
        std::lock_guard lock(s_resourceMutex);
        const auto& current = s_snapshots[s_mainSkyFrame.ReadIndex()];
        if (!current.active || !current.captureOriginValid)
            return 0;
        destination[0] = current.layer;
        return 1;
    }

    ID3D11ShaderResourceView* GetCommittedWorldCloudTiles() noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        return g_worldCloudReady.load(std::memory_order_acquire) &&
            s_resources && s_resources->device.Get() == GetD3DDevice() &&
            s_snapshots[s_mainSkyFrame.ReadIndex()].active && s_resources->resolvedSrv
            ? s_resources->resolvedSrv.Get()
            : nullptr;
    }

    ID3D11SamplerState* GetWorldCloudSampler() noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        return g_worldCloudReady.load(std::memory_order_acquire) &&
            s_resources && s_resources->device.Get() == GetD3DDevice()
            ? s_resources->sampler.Get()
            : nullptr;
    }

    bool ConfirmWorldCloudOrigin(ID3D11DeviceContext* context,
        ID3D11Buffer* enginePerFrame, UINT firstConstant, bool vr) noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        if (!s_resources || s_resources->device.Get() != GetD3DDevice() ||
            s_worldIdentity == 0)
            return false;
        const bool confirmed = s_confirmedProjectionOrigin.IsConfirmed();
        const ULONGLONG now = GetTickCount64();
        if (s_crossfadeActive &&
            FO4CS::CrossfadeWeight(now - s_crossfadeStartTick,
                FO4CS::ReanchorPolicy::kCrossfadeMilliseconds) <= 0.0f)
            CancelCrossfade();
        // Once confirmed, VR samples the same exact b12 camera about twice a
        // second so its world-fixed anchor can follow the player across long
        // distances. Flat retains its confirmed world origin until a reset,
        // so normal camera movement cannot shift terrain shadows.
        if (confirmed && (!vr || (!s_mainViewCameraReadback.Pending() &&
                now < s_nextAnchorReadbackTick)))
            return true;
        FO4CS::MainViewCameraReadback::Point observed{};
        if (!s_mainViewCameraReadback.TryRead(context, enginePerFrame,
                firstConstant, vr,
                g_worldCloudResetGeneration.load(std::memory_order_acquire), observed))
            return confirmed;
        s_nextAnchorReadbackTick = now + FO4CS::ReanchorPolicy::kReadbackIntervalMilliseconds;
        const XMFLOAT3 gameplayCamera{observed[0], observed[1], observed[2]};
        const auto moveOrigin = [&](bool crossfade) {
            const bool moved = gameplayCamera.x != s_projectionOrigin.x ||
                gameplayCamera.y != s_projectionOrigin.y ||
                gameplayCamera.z != s_projectionOrigin.z;
            if (crossfade && s_projectionOriginValid && moved) {
                s_previousProjectionOrigin = s_projectionOrigin;
                s_crossfadeStartTick = now;
                s_crossfadeActive = true;
            } else {
                CancelCrossfade();
            }
            s_projectionOrigin = gameplayCamera;
            s_projectionOriginValid = true;
            if (s_publishedMappingOriginValid)
                s_publishedMappingOrigin = gameplayCamera;
            for (auto& snapshot : s_snapshots) {
                if (!snapshot.active) continue;
                snapshot.captureOrigin = gameplayCamera;
                snapshot.captureOriginValid = true;
                snapshot.layer.PlaneZ = gameplayCamera.z + g_settings.CloudHeight;
            }
        };
        if (!confirmed) {
            const auto confirmation = s_confirmedProjectionOrigin.ConfirmMainView(
                { gameplayCamera.x, gameplayCamera.y, gameplayCamera.z });
            if (confirmation != FO4CS::WorldCloudAnchor::Confirmation::kEstablished)
                return false;
            moveOrigin(false);
            SPDLOG_INFO("[CloudShadows] World-fixed cloud origin confirmed by main lighting GPU camera: "
                        "{:.2f},{:.2f},{:.2f}",
                gameplayCamera.x, gameplayCamera.y, gameplayCamera.z);
            return true;
        }
        if (vr && FO4CS::NeedsReanchor(
                { s_projectionOrigin.x, s_projectionOrigin.y, s_projectionOrigin.z },
                observed, g_settings.CloudHeight) &&
            s_confirmedProjectionOrigin.Reanchor(observed)) {
            const XMFLOAT3 previous = s_projectionOrigin;
            moveOrigin(true);
            SPDLOG_INFO("[CloudShadows] World-fixed cloud origin re-anchored after travel: "
                        "{:.0f},{:.0f},{:.0f} -> {:.0f},{:.0f},{:.0f} ({})",
                previous.x, previous.y, previous.z,
                gameplayCamera.x, gameplayCamera.y, gameplayCamera.z,
                s_crossfadeActive ? "crossfading" : "immediate");
        }
        return true;
    }

    bool GetWorldCloudCrossfade(XMFLOAT3& previousOrigin, float& previousWeight) noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        previousOrigin = {};
        previousWeight = 0.0f;
        if (!s_crossfadeActive)
            return false;
        previousWeight = FO4CS::CrossfadeWeight(
            GetTickCount64() - s_crossfadeStartTick,
            FO4CS::ReanchorPolicy::kCrossfadeMilliseconds);
        if (previousWeight <= 0.0f) {
            CancelCrossfade();
            previousWeight = 0.0f;
            return false;
        }
        previousOrigin = s_previousProjectionOrigin;
        return true;
    }

    bool GetCommittedWorldCloudOrigin(XMFLOAT3& destination) noexcept
    {
        std::lock_guard lock(s_resourceMutex);
        const auto& current = s_snapshots[s_mainSkyFrame.ReadIndex()];
        if (!s_confirmedProjectionOrigin.IsConfirmed() ||
            !g_worldCloudReady.load(std::memory_order_acquire) ||
            !s_resources || s_resources->device.Get() != GetD3DDevice() ||
            !current.active || !current.captureOriginValid) {
            destination = {};
            return false;
        }
        destination = current.captureOrigin;
        return std::isfinite(destination.x) &&
            std::isfinite(destination.y) &&
            std::isfinite(destination.z);
    }
}
