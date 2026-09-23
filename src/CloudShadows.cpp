#include "CpuStageProfiler.h"
#include "CloudShadows.h"
#include "AcceptanceRunner.h"
#include "GodraysIntegration.h"
#include "ManualFpsLog.h"
#include "McmSettings.h"
#include "CloudComparison.h"
#include "Overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include <bcrypt.h>
#include <DirectXPackedVector.h>

#ifndef FO4CS_SCREEN_SHADER_SHA256
#define FO4CS_SCREEN_SHADER_SHA256 ""
#endif

namespace CloudShadows
{
    // WorldClouds.cpp owns the cube-evidence request watermark. Keep this
    // implementation detail out of the public plugin header.
    void CancelPendingWorldCloudCubeEvidenceRequests() noexcept;

    namespace
    {
        void InvalidatePublishedScreenMaskEvidence() noexcept;

        struct InitializationRetryState
        {
            std::chrono::steady_clock::time_point nextAttempt{};
            uint32_t consecutiveFailures{ 0 };
        };

        InitializationRetryState s_initializationRetry;
        fs::file_time_type s_settingsWriteTime{};
        bool s_settingsLoaded{ false };
        // Exhaustive D3D binding readback is a deployment/resource-contract
        // check, not useful work to repeat for every sunlight pixel pass. A
        // generation change makes each optional-UAV binding variant prove
        // itself once again before it may publish a valid mask.
        std::atomic<uint64_t> s_prepassBindingResourceGeneration{ 1 };

        void AdvancePrepassBindingResourceGeneration() noexcept
        {
            const uint64_t prior = s_prepassBindingResourceGeneration.fetch_add(
                1, std::memory_order_acq_rel);
            if (prior == (std::numeric_limits<uint64_t>::max)())
                s_prepassBindingResourceGeneration.store(
                    1, std::memory_order_release);
        }

        void ResetInitializationRetryState() noexcept
        {
            s_initializationRetry = {};
        }

        std::chrono::milliseconds InitializationRetryDelay(
            uint32_t consecutiveFailures) noexcept
        {
            // A missing or unauthenticated runtime shader is a persistent
            // deployment error, not a per-frame condition. Back off quickly
            // so BeginTechnique/Present cannot turn it into render-thread IO
            // and log spam. A new process or renderer-device transition resets
            // the delay and therefore still gets an immediate attempt.
            constexpr std::array delays{
                std::chrono::milliseconds(1000),
                std::chrono::milliseconds(5000),
                std::chrono::milliseconds(15000),
                std::chrono::milliseconds(30000)
            };
            const size_t index = std::min<size_t>(
                consecutiveFailures, delays.size() - 1u);
            return delays[index];
        }
    }

    bool ResolveGameRelativePath(
        const fs::path& relativePath, fs::path& destination) noexcept
    {
        destination.clear();
        try {
            if (relativePath.empty() || relativePath.is_absolute() ||
                relativePath.has_root_name() || relativePath.has_root_directory()) {
                SPDLOG_ERROR(
                    "[CloudShadows] Refusing non-relative game asset path: {}",
                    relativePath.string());
                return false;
            }
            for (const auto& component : relativePath) {
                if (component == L"..") {
                    SPDLOG_ERROR(
                        "[CloudShadows] Refusing traversing game asset path: {}",
                        relativePath.string());
                    return false;
                }
            }

            constexpr DWORD kMaximumHostPathCharacters = 32768;
            std::wstring hostPath(kMaximumHostPathCharacters, L'\0');
            const DWORD length = GetModuleFileNameW(
                nullptr, hostPath.data(), kMaximumHostPathCharacters);
            if (length == 0 || length >= kMaximumHostPathCharacters) {
                SPDLOG_ERROR(
                    "[CloudShadows] GetModuleFileNameW failed or truncated "
                    "while resolving {} (error={})",
                    relativePath.string(), GetLastError());
                return false;
            }
            hostPath.resize(length);
            const fs::path hostExecutable(hostPath);
            if (!hostExecutable.has_parent_path()) {
                SPDLOG_ERROR(
                    "[CloudShadows] Host executable has no parent directory");
                return false;
            }
            destination =
                (hostExecutable.parent_path() / relativePath).lexically_normal();
            return true;
        } catch (const std::exception& e) {
            SPDLOG_ERROR(
                "[CloudShadows] Failed resolving game asset path {}: {}",
                relativePath.string(), e.what());
        } catch (...) {
            SPDLOG_ERROR(
                "[CloudShadows] Failed resolving game asset path {}",
                relativePath.string());
        }
        destination.clear();
        return false;
    }

    bool ReadAuthenticatedShaderSource(
        const fs::path& relativePath,
        std::string_view expectedSha256,
        std::string& source) noexcept
    {
        source.clear();
        try {
            fs::path path;
            if (!ResolveGameRelativePath(relativePath, path))
                return false;
            if (expectedSha256.size() != 64u) {
                SPDLOG_ERROR(
                    "[CloudShadows] Shader manifest hash is missing/malformed "
                    "for {}",
                    relativePath.string());
                return false;
            }

            std::array<uint8_t, 32> expected{};
            auto hexNibble = [](char value, uint8_t& nibble) noexcept {
                if (value >= '0' && value <= '9') {
                    nibble = static_cast<uint8_t>(value - '0');
                    return true;
                }
                if (value >= 'a' && value <= 'f') {
                    nibble = static_cast<uint8_t>(value - 'a' + 10);
                    return true;
                }
                if (value >= 'A' && value <= 'F') {
                    nibble = static_cast<uint8_t>(value - 'A' + 10);
                    return true;
                }
                return false;
            };
            for (size_t i = 0; i < expected.size(); ++i) {
                uint8_t high = 0;
                uint8_t low = 0;
                if (!hexNibble(expectedSha256[i * 2u], high) ||
                    !hexNibble(expectedSha256[i * 2u + 1u], low)) {
                    SPDLOG_ERROR(
                        "[CloudShadows] Shader manifest hash is malformed for {}",
                        relativePath.string());
                    return false;
                }
                expected[i] = static_cast<uint8_t>((high << 4u) | low);
            }

            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                SPDLOG_ERROR(
                    "[CloudShadows] Shader asset missing/unreadable: {}",
                    path.string());
                return false;
            }
            const auto end = file.tellg();
            const std::streamoff byteCount = end;
            constexpr std::streamoff kMaximumShaderBytes = 8 * 1024 * 1024;
            if (byteCount <= 0 || byteCount > kMaximumShaderBytes) {
                SPDLOG_ERROR(
                    "[CloudShadows] Shader asset has invalid size: {}",
                    path.string());
                return false;
            }
            source.resize(static_cast<size_t>(byteCount));
            file.seekg(0, std::ios::beg);
            file.read(source.data(), static_cast<std::streamsize>(source.size()));
            if (!file || file.gcount() !=
                    static_cast<std::streamsize>(source.size())) {
                SPDLOG_ERROR(
                    "[CloudShadows] Shader asset read was incomplete: {}",
                    path.string());
                source.clear();
                return false;
            }

            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            std::vector<uint8_t> hashObject;
            std::array<uint8_t, 32> actual{};
            auto cleanup = [&]() noexcept {
                if (hash)
                    BCryptDestroyHash(hash);
                if (algorithm)
                    BCryptCloseAlgorithmProvider(algorithm, 0);
            };
            NTSTATUS status = BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
            DWORD objectBytes = 0;
            DWORD resultBytes = 0;
            DWORD digestBytes = 0;
            if (status >= 0) {
                status = BCryptGetProperty(
                    algorithm, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&objectBytes),
                    sizeof(objectBytes), &resultBytes, 0);
            }
            if (status >= 0) {
                status = BCryptGetProperty(
                    algorithm, BCRYPT_HASH_LENGTH,
                    reinterpret_cast<PUCHAR>(&digestBytes),
                    sizeof(digestBytes), &resultBytes, 0);
            }
            if (status >= 0 &&
                (objectBytes == 0 || digestBytes != actual.size())) {
                cleanup();
                SPDLOG_ERROR(
                    "[CloudShadows] SHA-256 provider returned an invalid "
                    "object/digest size for {}",
                    path.string());
                source.clear();
                return false;
            }
            if (status >= 0) {
                hashObject.resize(objectBytes);
                status = BCryptCreateHash(
                    algorithm, &hash, hashObject.data(), objectBytes,
                    nullptr, 0, 0);
            }
            if (status >= 0) {
                status = BCryptHashData(
                    hash, reinterpret_cast<PUCHAR>(source.data()),
                    static_cast<ULONG>(source.size()), 0);
            }
            if (status >= 0) {
                status = BCryptFinishHash(
                    hash, actual.data(), static_cast<ULONG>(actual.size()), 0);
            }
            cleanup();
            if (status < 0) {
                SPDLOG_ERROR(
                    "[CloudShadows] SHA-256 failed for {} (status=0x{:08X})",
                    path.string(), static_cast<uint32_t>(status));
                source.clear();
                return false;
            }
            if (!std::equal(actual.begin(), actual.end(), expected.begin())) {
                SPDLOG_ERROR(
                    "[CloudShadows] Shader asset hash mismatch; refusing stale "
                    "source: {}",
                    path.string());
                source.clear();
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            SPDLOG_ERROR(
                "[CloudShadows] Shader authentication failed for {}: {}",
                relativePath.string(), e.what());
        } catch (...) {
            SPDLOG_ERROR(
                "[CloudShadows] Shader authentication failed for {}",
                relativePath.string());
        }
        source.clear();
        return false;
    }

    void InvalidateShadowMaskState() noexcept
    {
        g_shadowMaskValid.store(false, std::memory_order_release);
        g_shadowMaskSuccessStamp.store(0, std::memory_order_release);
        g_lastCompletedShadowMaskValid.store(false, std::memory_order_release);
        g_cloudTelemetryValid.store(false, std::memory_order_release);
        g_cloudTelemetrySunEligible.store(false, std::memory_order_release);
        g_cloudTelemetryFieldCommitted.store(false, std::memory_order_release);
        g_cloudTelemetryEpoch.store(0, std::memory_order_relaxed);
        g_cloudTelemetryWorldGeneration.store(0, std::memory_order_relaxed);
        InvalidatePublishedScreenMaskEvidence();
    }

    namespace
    {
        // Exact Fallout4.exe 1.10.163 logical target ABI. Every logical ID is
        // resolved through RenderTargetManager's live mapping before touching
        // RendererData; hard-coding the usually observed physical slots is not
        // valid when the manager repoints targets. DrawWorld::DeferredLightsImpl
        // binds logical RT33 at OM0, optionally RT34 at OM1, and logical depth 1.
        constexpr uint32_t kMainRenderTargetPhysicalIndex = 3;
        constexpr uint32_t kMainTempRenderTargetPhysicalIndex = 4;
        constexpr uint32_t kMainDepthStencilLogicalIndex = 1;
        // RenderTargetManager logical->physical maps. Verified in the Ghidra
        // Combined project: 1.11.240 widened the 12-entry per-depth-target
        // record array at +0xC80 from 0x18 to 0x1C bytes (+0x30 total), which
        // moves both maps; the dynamic-resolution floats that follow the depth
        // map sit at +0xF88 (OG) versus +0xFB8 (1.11.240).
        constexpr std::size_t kRenderTargetMapOffsetLegacy = 0xDC4;
        constexpr std::size_t kDepthStencilTargetMapOffsetLegacy = 0xF54;
        constexpr std::size_t kRenderTargetMapOffsetAE = 0xDF4;
        constexpr std::size_t kDepthStencilTargetMapOffsetAE = 0xF84;
        // VR manager accessors at 0x1DB9DD0/0x1DB9E40 use these live maps.
        constexpr std::size_t kRenderTargetMapOffsetVR = 0x13BC;
        constexpr std::size_t kDepthStencilTargetMapOffsetVR = 0x15FC;
        [[nodiscard]] bool IsVRRuntime() noexcept
        {
            return FO4CS::RuntimeAPI::GetSingleton().Target() ==
                FO4CS::F4SECompat::RuntimeTarget::kVR;
        }
        [[nodiscard]] uint32_t DeferredLightingTargetLogicalIndex(uint32_t slot) noexcept
        {
            // VR's diffuse/specular logical IDs are 36/37. The authenticated
            // main sunlight draw resolves these to the live RT110/RT111 in
            // the reviewed VR capture; their physical slots remain dynamic.
            return (IsVRRuntime() ? 36u : 33u) + slot;
        }
        [[nodiscard]] bool IsAERuntime() noexcept
        {
            return FO4CS::RuntimeAPI::GetSingleton().Target() ==
                FO4CS::F4SECompat::RuntimeTarget::kAE;
        }
        [[nodiscard]] std::size_t RenderTargetMapOffset() noexcept
        {
            return IsVRRuntime() ? kRenderTargetMapOffsetVR :
                (IsAERuntime() ? kRenderTargetMapOffsetAE : kRenderTargetMapOffsetLegacy);
        }
        [[nodiscard]] std::size_t DepthStencilTargetMapOffset() noexcept
        {
            return IsVRRuntime() ? kDepthStencilTargetMapOffsetVR :
                (IsAERuntime() ? kDepthStencilTargetMapOffsetAE : kDepthStencilTargetMapOffsetLegacy);
        }

        struct RenderTargetDiagnostic
        {
            void* view{ nullptr };
            void* resource{ nullptr };
            uint32_t width{ 0 };
            uint32_t height{ 0 };
            uint32_t format{ 0 };
            uint32_t dimension{ 0 };
            uint32_t mip{ 0 };
            uint32_t firstSlice{ 0 };
            uint32_t arraySize{ 0 };
        };

        struct ShaderResourceDiagnostic
        {
            void* view{ nullptr };
            void* resource{ nullptr };
            uint32_t width{ 0 };
            uint32_t height{ 0 };
            uint32_t format{ 0 };
            uint32_t dimension{ 0 };
            uint32_t mostDetailedMip{ 0 };
            uint32_t mipLevels{ 0 };
            uint32_t firstSlice{ 0 };
            uint32_t arraySize{ 0 };
        };

        RenderTargetDiagnostic InspectRenderTarget(
            ID3D11RenderTargetView* view) noexcept
        {
            RenderTargetDiagnostic result{};
            result.view = view;
            if (!view)
                return result;

            D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
            view->GetDesc(&viewDescription);
            result.format = static_cast<uint32_t>(viewDescription.Format);
            result.dimension =
                static_cast<uint32_t>(viewDescription.ViewDimension);
            switch (viewDescription.ViewDimension) {
            case D3D11_RTV_DIMENSION_TEXTURE2D:
                result.mip = viewDescription.Texture2D.MipSlice;
                result.arraySize = 1;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
                result.mip = viewDescription.Texture2DArray.MipSlice;
                result.firstSlice =
                    viewDescription.Texture2DArray.FirstArraySlice;
                result.arraySize = viewDescription.Texture2DArray.ArraySize;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DMS:
                result.arraySize = 1;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
                result.firstSlice =
                    viewDescription.Texture2DMSArray.FirstArraySlice;
                result.arraySize =
                    viewDescription.Texture2DMSArray.ArraySize;
                break;
            default:
                break;
            }

            ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            result.resource = resource.Get();
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture)) && texture) {
                D3D11_TEXTURE2D_DESC textureDescription{};
                texture->GetDesc(&textureDescription);
                result.width = (std::max)(
                    1u, textureDescription.Width >> result.mip);
                result.height = (std::max)(
                    1u, textureDescription.Height >> result.mip);
            }
            return result;
        }

        ShaderResourceDiagnostic InspectShaderResource(
            ID3D11ShaderResourceView* view) noexcept
        {
            ShaderResourceDiagnostic result{};
            result.view = view;
            if (!view)
                return result;

            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            view->GetDesc(&viewDescription);
            result.format = static_cast<uint32_t>(viewDescription.Format);
            result.dimension =
                static_cast<uint32_t>(viewDescription.ViewDimension);
            switch (viewDescription.ViewDimension) {
            case D3D11_SRV_DIMENSION_TEXTURE2D:
                result.mostDetailedMip =
                    viewDescription.Texture2D.MostDetailedMip;
                result.mipLevels = viewDescription.Texture2D.MipLevels;
                result.arraySize = 1;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
                result.mostDetailedMip =
                    viewDescription.Texture2DArray.MostDetailedMip;
                result.mipLevels = viewDescription.Texture2DArray.MipLevels;
                result.firstSlice =
                    viewDescription.Texture2DArray.FirstArraySlice;
                result.arraySize = viewDescription.Texture2DArray.ArraySize;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DMS:
                result.arraySize = 1;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
                result.firstSlice =
                    viewDescription.Texture2DMSArray.FirstArraySlice;
                result.arraySize =
                    viewDescription.Texture2DMSArray.ArraySize;
                break;
            default:
                break;
            }

            ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            result.resource = resource.Get();
            ComPtr<ID3D11Texture2D> texture;
            if (resource && SUCCEEDED(resource.As(&texture)) && texture) {
                D3D11_TEXTURE2D_DESC textureDescription{};
                texture->GetDesc(&textureDescription);
                result.width = (std::max)(
                    1u, textureDescription.Width >> result.mostDetailedMip);
                result.height = (std::max)(
                    1u, textureDescription.Height >> result.mostDetailedMip);
            }
            return result;
        }

        bool SameRenderTargetSubresource(
            const D3D11_RENDER_TARGET_VIEW_DESC& left,
            const D3D11_RENDER_TARGET_VIEW_DESC& right) noexcept
        {
            if (left.Format != right.Format ||
                left.ViewDimension != right.ViewDimension) {
                return false;
            }
            switch (left.ViewDimension) {
            case D3D11_RTV_DIMENSION_BUFFER:
                return left.Buffer.FirstElement == right.Buffer.FirstElement &&
                    left.Buffer.NumElements == right.Buffer.NumElements;
            case D3D11_RTV_DIMENSION_TEXTURE1D:
                return left.Texture1D.MipSlice == right.Texture1D.MipSlice;
            case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
                return left.Texture1DArray.MipSlice == right.Texture1DArray.MipSlice &&
                    left.Texture1DArray.FirstArraySlice == right.Texture1DArray.FirstArraySlice &&
                    left.Texture1DArray.ArraySize == right.Texture1DArray.ArraySize;
            case D3D11_RTV_DIMENSION_TEXTURE2D:
                return left.Texture2D.MipSlice == right.Texture2D.MipSlice;
            case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
                return left.Texture2DArray.MipSlice == right.Texture2DArray.MipSlice &&
                    left.Texture2DArray.FirstArraySlice == right.Texture2DArray.FirstArraySlice &&
                    left.Texture2DArray.ArraySize == right.Texture2DArray.ArraySize;
            case D3D11_RTV_DIMENSION_TEXTURE2DMS:
                return true;
            case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
                return left.Texture2DMSArray.FirstArraySlice ==
                        right.Texture2DMSArray.FirstArraySlice &&
                    left.Texture2DMSArray.ArraySize == right.Texture2DMSArray.ArraySize;
            case D3D11_RTV_DIMENSION_TEXTURE3D:
                return left.Texture3D.MipSlice == right.Texture3D.MipSlice &&
                    left.Texture3D.FirstWSlice == right.Texture3D.FirstWSlice &&
                    left.Texture3D.WSize == right.Texture3D.WSize;
            default:
                return false;
            }
        }

        bool SameShaderResourceSubresource(
            const D3D11_SHADER_RESOURCE_VIEW_DESC& left,
            const D3D11_SHADER_RESOURCE_VIEW_DESC& right) noexcept
        {
            if (left.Format != right.Format ||
                left.ViewDimension != right.ViewDimension) {
                return false;
            }
            switch (left.ViewDimension) {
            case D3D11_SRV_DIMENSION_TEXTURE2D:
                return left.Texture2D.MostDetailedMip ==
                        right.Texture2D.MostDetailedMip &&
                    left.Texture2D.MipLevels == right.Texture2D.MipLevels;
            case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
                return left.Texture2DArray.MostDetailedMip ==
                        right.Texture2DArray.MostDetailedMip &&
                    left.Texture2DArray.MipLevels ==
                        right.Texture2DArray.MipLevels &&
                    left.Texture2DArray.FirstArraySlice ==
                        right.Texture2DArray.FirstArraySlice &&
                    left.Texture2DArray.ArraySize ==
                        right.Texture2DArray.ArraySize;
            case D3D11_SRV_DIMENSION_TEXTURE2DMS:
                return true;
            case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
                return left.Texture2DMSArray.FirstArraySlice ==
                        right.Texture2DMSArray.FirstArraySlice &&
                    left.Texture2DMSArray.ArraySize ==
                        right.Texture2DMSArray.ArraySize;
            default:
                return false;
            }
        }

        std::byte* GetRenderTargetManagerAddress() noexcept
        {
            // Reviewed OG/AE Address Library IDs and the exact VR RVA resolve
            // the same RenderTargetManager singleton object. The downstream
            // logical->physical map checks remain the fail-closed authority.
            return FO4CS::EngineAPI::GetRenderTargetManager();
        }

        bool ResolveLogicalRenderTarget(
            uint32_t logicalIndex,
            uint32_t& physicalIndex) noexcept
        {
            physicalIndex = 0;
            auto* manager = GetRenderTargetManagerAddress();
            if (!manager || logicalIndex >= FO4CS::EngineAPI::PhysicalRenderTargetCount() - 1u)
                return false;
            std::memcpy(
                &physicalIndex,
                manager + RenderTargetMapOffset() +
                    static_cast<std::size_t>(logicalIndex) * sizeof(uint32_t),
                sizeof(physicalIndex));
            return physicalIndex < FO4CS::EngineAPI::PhysicalRenderTargetCount();
        }

        bool ResolveLogicalDepthStencilTarget(
            uint32_t logicalIndex,
            uint32_t& physicalIndex) noexcept
        {
            physicalIndex = 0;
            auto* manager = GetRenderTargetManagerAddress();
            if (!manager || logicalIndex >= FO4CS::EngineAPI::PhysicalDepthTargetCount() - 1u)
                return false;
            std::memcpy(
                &physicalIndex,
                manager + DepthStencilTargetMapOffset() +
                    static_cast<std::size_t>(logicalIndex) * sizeof(uint32_t),
                sizeof(physicalIndex));
            return physicalIndex < FO4CS::EngineAPI::PhysicalDepthTargetCount();
        }

        bool IsRendererPhysicalRenderTargetView(
            ID3D11RenderTargetView* view,
            uint32_t physicalRenderTargetIndex) noexcept
        {
            if (!view)
                return false;
            auto* rendererData = GetRendererData();
            if (!rendererData ||
                physicalRenderTargetIndex >= FO4CS::EngineAPI::PhysicalRenderTargetCount()) {
                return false;
            }
            const auto* target = rendererData->RenderTargetAt(physicalRenderTargetIndex);
            if (!target)
                return false;
            auto* targetView = target->TargetView();
            auto* targetTexture = target->Texture();
            if (!targetView || !targetTexture)
                return false;

            ComPtr<ID3D11Resource> boundResource;
            ComPtr<ID3D11Resource> targetResource;
            view->GetResource(boundResource.GetAddressOf());
            targetView->GetResource(targetResource.GetAddressOf());
            if (!boundResource || !targetResource)
                return false;

            ComPtr<IUnknown> boundIdentity;
            ComPtr<IUnknown> targetIdentity;
            if (FAILED(boundResource.As(&boundIdentity)) ||
                FAILED(targetResource.As(&targetIdentity)) ||
                boundIdentity.Get() != targetIdentity.Get()) {
                return false;
            }

            // Cross-check the engine's resource field too. This rejects a
            // stale RTV if RendererData has advanced to a replacement.
            ComPtr<IUnknown> textureIdentity;
            if (FAILED(targetTexture->QueryInterface(
                    __uuidof(IUnknown),
                    reinterpret_cast<void**>(textureIdentity.GetAddressOf()))) ||
                textureIdentity.Get() != targetIdentity.Get()) {
                return false;
            }

            D3D11_RENDER_TARGET_VIEW_DESC boundDescription{};
            D3D11_RENDER_TARGET_VIEW_DESC targetDescription{};
            view->GetDesc(&boundDescription);
            targetView->GetDesc(&targetDescription);
            return SameRenderTargetSubresource(
                boundDescription, targetDescription);
        }

        bool IsRendererLogicalRenderTargetView(
            ID3D11RenderTargetView* view,
            uint32_t logicalRenderTargetIndex) noexcept
        {
            uint32_t physicalRenderTargetIndex = 0;
            return ResolveLogicalRenderTarget(
                       logicalRenderTargetIndex,
                       physicalRenderTargetIndex) &&
                IsRendererPhysicalRenderTargetView(
                       view, physicalRenderTargetIndex);
        }

        bool IsMainDepthShaderResourceView(
            ID3D11ShaderResourceView* view) noexcept
        {
            if (!view)
                return false;
            auto* rendererData = GetRendererData();
            if (!rendererData)
                return false;
            uint32_t physicalDepthStencilTargetIndex = 0;
            if (!ResolveLogicalDepthStencilTarget(
                    kMainDepthStencilLogicalIndex,
                    physicalDepthStencilTargetIndex)) {
                return false;
            }
            const auto* target = rendererData->DepthTargetAt(physicalDepthStencilTargetIndex);
            auto* targetView = target ? target->DepthShaderResourceView() : nullptr;
            if (!targetView)
                return false;

            ComPtr<ID3D11Resource> boundResource;
            ComPtr<ID3D11Resource> targetResource;
            view->GetResource(boundResource.GetAddressOf());
            targetView->GetResource(targetResource.GetAddressOf());
            if (!boundResource || !targetResource)
                return false;

            ComPtr<IUnknown> boundIdentity;
            ComPtr<IUnknown> targetIdentity;
            if (FAILED(boundResource.As(&boundIdentity)) ||
                FAILED(targetResource.As(&targetIdentity)) ||
                boundIdentity.Get() != targetIdentity.Get()) {
                return false;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC boundDescription{};
            D3D11_SHADER_RESOURCE_VIEW_DESC targetDescription{};
            view->GetDesc(&boundDescription);
            targetView->GetDesc(&targetDescription);
            return SameShaderResourceSubresource(
                boundDescription, targetDescription);
        }

        void LogMainDepthRejection(
            ID3D11ShaderResourceView* boundDepth,
            const D3D11_VIEWPORT& viewport) noexcept
        {
            static std::atomic<uint32_t> rejectionCount{ 0 };
            const uint32_t ordinal = rejectionCount.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (ordinal > 4u && (ordinal & (ordinal - 1u)) != 0u)
                return;

            ID3D11ShaderResourceView* expectedDepth = nullptr;
            uint32_t expectedDepthPhysicalIndex = 0;
            if (auto* rendererData = GetRendererData()) {
                if (ResolveLogicalDepthStencilTarget(
                        kMainDepthStencilLogicalIndex,
                        expectedDepthPhysicalIndex)) {
                    const auto* target = rendererData->DepthTargetAt(expectedDepthPhysicalIndex);
                    expectedDepth = target ? target->DepthShaderResourceView() : nullptr;
                }
            }
            const auto bound = InspectShaderResource(boundDepth);
            const auto expected = InspectShaderResource(expectedDepth);
            SPDLOG_INFO(
                "[CloudShadows] Main-depth reject detail #{} exactMatch={} "
                "boundSRV={} res={} {}x{} fmt={} dim={} mip={} levels={} "
                "slice={}/{} expectedDepthPhysical={} SRV={} res={} {}x{} fmt={} "
                "dim={} mip={} levels={} slice={}/{} "
                "viewport={:.1f},{:.1f} {:.1f}x{:.1f}",
                ordinal, IsMainDepthShaderResourceView(boundDepth),
                bound.view, bound.resource, bound.width, bound.height,
                bound.format, bound.dimension, bound.mostDetailedMip,
                bound.mipLevels, bound.firstSlice, bound.arraySize,
                expectedDepthPhysicalIndex, expected.view, expected.resource,
                expected.width, expected.height, expected.format,
                expected.dimension,
                expected.mostDetailedMip, expected.mipLevels,
                expected.firstSlice, expected.arraySize,
                viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                viewport.Height);
        }
    }

    bool IsMainRenderTargetView(ID3D11RenderTargetView* view) noexcept
    {
        return IsRendererPhysicalRenderTargetView(
            view, kMainRenderTargetPhysicalIndex);
    }

    bool IsMainSkyRenderTargetView(ID3D11RenderTargetView* view) noexcept
    {
        return IsRendererPhysicalRenderTargetView(
            view, kMainTempRenderTargetPhysicalIndex);
    }

    namespace
    {
        bool IsMainDeferredLightingTargetSet(
            ID3D11RenderTargetView* accumulation0,
            ID3D11RenderTargetView* accumulation1,
            UINT renderTargetCount) noexcept
        {
            if (!IsRendererLogicalRenderTargetView(
                    accumulation0,
                    DeferredLightingTargetLogicalIndex(0))) {
                return false;
            }
            if (renderTargetCount == 1)
                return accumulation1 == nullptr;
            return renderTargetCount == 2 &&
                IsRendererLogicalRenderTargetView(
                    accumulation1,
                    DeferredLightingTargetLogicalIndex(1));
        }
    }

    void LogMainSkyWorldViewRejection(
        ID3D11DeviceContext* context,
        uint32_t skyTechnique,
        uint32_t rejectionOrdinal) noexcept
    {
        if (!context)
            return;

        ComPtr<ID3D11RenderTargetView> boundView;
        context->OMGetRenderTargets(1, boundView.GetAddressOf(), nullptr);
        const auto bound = InspectRenderTarget(boundView.Get());

        ID3D11RenderTargetView* skyView = nullptr;
        ID3D11Texture2D* skyTexture = nullptr;
        ID3D11RenderTargetView* mainView = nullptr;
        ID3D11Texture2D* mainTexture = nullptr;
        if (auto* rendererData = GetRendererData()) {
            if (const auto* skyTarget = rendererData->RenderTargetAt(kMainTempRenderTargetPhysicalIndex)) {
                skyView = skyTarget->TargetView();
                skyTexture = skyTarget->Texture();
            }
            if (const auto* mainTarget = rendererData->RenderTargetAt(kMainRenderTargetPhysicalIndex)) {
                mainView = mainTarget->TargetView();
                mainTexture = mainTarget->Texture();
            }
        }
        const auto sky = InspectRenderTarget(skyView);
        const auto main = InspectRenderTarget(mainView);

        UINT viewportCount =
            D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        std::array<
            D3D11_VIEWPORT,
            D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        context->RSGetViewports(&viewportCount, viewports.data());
        const auto& viewport = viewports[0];
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        GetOutputDimensions(outputWidth, outputHeight);

        SPDLOG_INFO(
            "[CloudShadows] Main-Sky-view reject detail #{} technique={} "
            "skyMatch={} mainMatch={} "
            "boundRTV={} boundRes={} bound={}x{} dim={} mip={} slice={}/{} "
            "mainTempRTV={} mainTempTex={} mainTempRes={} mainTemp={}x{} "
            "mainRTV={} mainTex={} mainRes={} main={}x{}",
            rejectionOrdinal, skyTechnique,
            IsMainSkyRenderTargetView(boundView.Get()),
            IsMainRenderTargetView(boundView.Get()), bound.view, bound.resource,
            bound.width, bound.height, bound.dimension, bound.mip,
            bound.firstSlice, bound.arraySize, sky.view,
            static_cast<void*>(skyTexture), sky.resource, sky.width,
            sky.height, main.view, static_cast<void*>(mainTexture),
            main.resource, main.width, main.height);
        SPDLOG_INFO(
            "[CloudShadows] Main-Sky-view reject viewport #{} count={} "
            "rect={:.1f},{:.1f} {:.1f}x{:.1f} depth={:.3f}..{:.3f} "
            "output={}x{}",
            rejectionOrdinal, viewportCount, viewport.TopLeftX,
            viewport.TopLeftY, viewport.Width, viewport.Height,
            viewport.MinDepth, viewport.MaxDepth, outputWidth, outputHeight);
    }

    namespace
    {
        void LogDeferredLightingTargetRejection(
            ID3D11RenderTargetView* boundDiffuse,
            ID3D11RenderTargetView* boundProbe,
            UINT renderTargetCount) noexcept
        {
            static std::atomic<uint32_t> rejectionCount{ 0 };
            const uint32_t ordinal = rejectionCount.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (ordinal > 4u && (ordinal & (ordinal - 1u)) != 0u)
                return;

            ID3D11RenderTargetView* expected0 = nullptr;
            ID3D11RenderTargetView* expected1 = nullptr;
            uint32_t expectedPhysical0 = 0;
            uint32_t expectedPhysical1 = 0;
            if (auto* rendererData = GetRendererData()) {
                if (ResolveLogicalRenderTarget(
                        DeferredLightingTargetLogicalIndex(0),
                        expectedPhysical0)) {
                    const auto* target = rendererData->RenderTargetAt(expectedPhysical0);
                    expected0 = target ? target->TargetView() : nullptr;
                }
                if (ResolveLogicalRenderTarget(
                        DeferredLightingTargetLogicalIndex(1),
                        expectedPhysical1)) {
                    const auto* target = rendererData->RenderTargetAt(expectedPhysical1);
                    expected1 = target ? target->TargetView() : nullptr;
                }
            }
            const auto bound0 = InspectRenderTarget(boundDiffuse);
            const auto bound1 = InspectRenderTarget(boundProbe);
            const auto expectedTarget0 = InspectRenderTarget(expected0);
            const auto expectedTarget1 = InspectRenderTarget(expected1);
            SPDLOG_INFO(
                "[CloudShadows] Deferred-target reject detail #{} count={} "
                "target0Match={} target1Match={} "
                "bound0RTV={} res={} {}x{} fmt={} dim={} mip={} slice={}/{} "
                "bound1RTV={} res={} {}x{} fmt={} dim={} mip={} slice={}/{} "
                "logical0={} physical0={} expectedRTV={} res={} {}x{} fmt={} "
                "logical1={} physical1={} expectedRTV={} res={} {}x{} fmt={}",
                ordinal, renderTargetCount,
                IsRendererLogicalRenderTargetView(
                    boundDiffuse,
                    DeferredLightingTargetLogicalIndex(0)),
                IsRendererLogicalRenderTargetView(
                    boundProbe,
                    DeferredLightingTargetLogicalIndex(1)),
                bound0.view, bound0.resource, bound0.width, bound0.height,
                bound0.format, bound0.dimension, bound0.mip,
                bound0.firstSlice, bound0.arraySize,
                bound1.view, bound1.resource, bound1.width, bound1.height,
                bound1.format, bound1.dimension, bound1.mip,
                bound1.firstSlice, bound1.arraySize,
                DeferredLightingTargetLogicalIndex(0), expectedPhysical0, expectedTarget0.view,
                expectedTarget0.resource,
                expectedTarget0.width, expectedTarget0.height,
                expectedTarget0.format, DeferredLightingTargetLogicalIndex(1), expectedPhysical1,
                expectedTarget1.view, expectedTarget1.resource,
                expectedTarget1.width,
                expectedTarget1.height, expectedTarget1.format);
        }
    }

    // RendererData's main render target is the authoritative player-view
    // extent. In VR this is the full side-by-side eye surface; the desktop
    // swap-chain is only a mirror and can have unrelated dimensions. The main
    // target may be null during startup (or virtual under a translation
    // wrapper), so the swap-chain remains a fail-soft desktop fallback.
    bool GetOutputDimensions(uint32_t& width, uint32_t& height)
    {
        width = height = 0;
        if (auto* rd = GetRendererData()) {
            const auto* target = rd->RenderTargetAt(kMainRenderTargetPhysicalIndex);
            auto* texture = target ? target->Texture() : nullptr;
            if (texture) {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                width = desc.Width;
                height = desc.Height;
                return width != 0 && height != 0;
            }
        }

        if (!g_capturedSwapChain)
            return false;
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(g_capturedSwapChain->GetBuffer(
                0, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        width = desc.Width;
        height = desc.Height;
        return width != 0 && height != 0;
    }

    namespace
    {
        constexpr std::int64_t kProjectionModelVersion = 4;

        struct ScreenMaskEvidenceStats
        {
            uint64_t finiteCount{ 0 };
            uint64_t nonFiniteCount{ 0 };
            uint64_t outOfRangeCount{ 0 };
            uint64_t exactZeroCount{ 0 };
            uint64_t exactOneCount{ 0 };
            uint64_t neutralCount{ 0 };
            uint64_t strongShadowCount{ 0 };
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
                if (value >= 0.999f)
                    ++neutralCount;
                if (value <= 0.9f)
                    ++strongShadowCount;
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

        EvidenceDistributionSummary SummarizeScreenMaskEvidence(
            const ScreenMaskEvidenceStats& stats) noexcept
        {
            EvidenceDistributionSummary summary{};
            summary.sampleCount = stats.finiteCount + stats.nonFiniteCount;
            summary.finiteCount = stats.finiteCount;
            summary.nonFiniteCount = stats.nonFiniteCount;
            summary.outOfRangeCount = stats.outOfRangeCount;
            summary.exactZeroCount = stats.exactZeroCount;
            summary.exactOneCount = stats.exactOneCount;
            summary.neutralCount = stats.neutralCount;
            summary.strongShadowCount = stats.strongShadowCount;
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
                nonFiniteCount.store(
                    source.nonFiniteCount, std::memory_order_relaxed);
                outOfRangeCount.store(
                    source.outOfRangeCount, std::memory_order_relaxed);
                exactZeroCount.store(
                    source.exactZeroCount, std::memory_order_relaxed);
                exactOneCount.store(
                    source.exactOneCount, std::memory_order_relaxed);
                neutralCount.store(
                    source.neutralCount, std::memory_order_relaxed);
                strongShadowCount.store(
                    source.strongShadowCount, std::memory_order_relaxed);
                clearCount.store(source.clearCount, std::memory_order_relaxed);
                opaqueCount.store(source.opaqueCount, std::memory_order_relaxed);
                minimum.store(source.minimum, std::memory_order_relaxed);
                maximum.store(source.maximum, std::memory_order_relaxed);
                mean.store(source.mean, std::memory_order_relaxed);
                standardDeviation.store(
                    source.standardDeviation, std::memory_order_relaxed);
            }

            void Load(EvidenceDistributionSummary& destination) const noexcept
            {
                destination.sampleCount =
                    sampleCount.load(std::memory_order_relaxed);
                destination.finiteCount =
                    finiteCount.load(std::memory_order_relaxed);
                destination.nonFiniteCount =
                    nonFiniteCount.load(std::memory_order_relaxed);
                destination.outOfRangeCount =
                    outOfRangeCount.load(std::memory_order_relaxed);
                destination.exactZeroCount =
                    exactZeroCount.load(std::memory_order_relaxed);
                destination.exactOneCount =
                    exactOneCount.load(std::memory_order_relaxed);
                destination.neutralCount =
                    neutralCount.load(std::memory_order_relaxed);
                destination.strongShadowCount =
                    strongShadowCount.load(std::memory_order_relaxed);
                destination.clearCount =
                    clearCount.load(std::memory_order_relaxed);
                destination.opaqueCount =
                    opaqueCount.load(std::memory_order_relaxed);
                destination.minimum = minimum.load(std::memory_order_relaxed);
                destination.maximum = maximum.load(std::memory_order_relaxed);
                destination.mean = mean.load(std::memory_order_relaxed);
                destination.standardDeviation =
                    standardDeviation.load(std::memory_order_relaxed);
            }
        };

        struct PublishedScreenMaskEvidence
        {
            std::atomic_flag writer = ATOMIC_FLAG_INIT;
            std::atomic<uint64_t> sequence{ 0 };
            std::atomic<bool> valid{ false };
            std::atomic<uint64_t> requestId{ 0 };
            std::atomic<uint64_t> epoch{ 0 };
            std::atomic<uint64_t> worldGeneration{ 0 };
            std::atomic<uint32_t> dispatchOrdinal{ 0 };
            std::atomic<uint32_t> width{ 0 };
            std::atomic<uint32_t> height{ 0 };
            std::atomic<uint32_t> sampleStride{ 0 };
            std::atomic<float> debugMode{ 0.0f };
            std::array<AtomicEvidenceDistribution,
                kScreenMaskEvidenceScopeCount> scopes{};
            struct AtomicValidReceiverEvidence
            {
                std::atomic<uint64_t> validReceiverCount{ 0 };
                std::atomic<uint64_t> validReceiverExactOneCount{ 0 };
                std::atomic<uint64_t> validReceiverAttenuatedCount{ 0 };
                AtomicEvidenceDistribution distribution{};

                void Store(
                    const ValidReceiverEvidenceSummary& source) noexcept
                {
                    validReceiverCount.store(
                        source.validReceiverCount,
                        std::memory_order_relaxed);
                    validReceiverExactOneCount.store(
                        source.validReceiverExactOneCount,
                        std::memory_order_relaxed);
                    validReceiverAttenuatedCount.store(
                        source.validReceiverAttenuatedCount,
                        std::memory_order_relaxed);
                    distribution.Store(source.distribution);
                }

                void Load(
                    ValidReceiverEvidenceSummary& destination) const noexcept
                {
                    destination.validReceiverCount =
                        validReceiverCount.load(std::memory_order_relaxed);
                    destination.validReceiverExactOneCount =
                        validReceiverExactOneCount.load(
                            std::memory_order_relaxed);
                    destination.validReceiverAttenuatedCount =
                        validReceiverAttenuatedCount.load(
                            std::memory_order_relaxed);
                    distribution.Load(destination.distribution);
                }
            };
            std::array<AtomicValidReceiverEvidence,
                kScreenMaskEvidenceScopeCount> validReceivers{};
        };

        PublishedScreenMaskEvidence s_publishedScreenMaskEvidence;
        std::atomic<uint64_t> s_screenMaskEvidenceRequest{ 0 };
        std::atomic<uint64_t> s_completedScreenMaskEvidenceRequest{ 0 };

        void AdvanceCompletedRequest(
            std::atomic<uint64_t>& completedRequest,
            uint64_t requestId) noexcept
        {
            uint64_t completed = completedRequest.load(
                std::memory_order_acquire);
            while (completed < requestId &&
                !completedRequest.compare_exchange_weak(
                    completed, requestId, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
            }
        }

        void PublishScreenMaskEvidence(
            const ScreenMaskEvidenceSnapshot& source) noexcept
        {
            while (s_publishedScreenMaskEvidence.writer.test_and_set(
                std::memory_order_acquire)) {
            }
            const uint64_t writeSequence =
                s_publishedScreenMaskEvidence.sequence.fetch_add(
                    1, std::memory_order_acq_rel) + 1u;
            s_publishedScreenMaskEvidence.valid.store(
                source.valid, std::memory_order_relaxed);
            s_publishedScreenMaskEvidence.requestId.store(
                source.requestId, std::memory_order_relaxed);
            s_publishedScreenMaskEvidence.epoch.store(
                source.epoch, std::memory_order_relaxed);
            s_publishedScreenMaskEvidence.worldGeneration.store(
                source.worldGeneration, std::memory_order_relaxed);
            s_publishedScreenMaskEvidence.dispatchOrdinal.store(
                source.dispatchOrdinal, std::memory_order_relaxed);
            s_publishedScreenMaskEvidence.width.store(
                source.width, std::memory_order_relaxed);
            s_publishedScreenMaskEvidence.height.store(
                source.height, std::memory_order_relaxed);
            s_publishedScreenMaskEvidence.sampleStride.store(
                source.sampleStride, std::memory_order_relaxed);
            s_publishedScreenMaskEvidence.debugMode.store(
                source.debugMode, std::memory_order_relaxed);
            for (uint32_t i = 0; i < kScreenMaskEvidenceScopeCount; ++i) {
                s_publishedScreenMaskEvidence.scopes[i].Store(source.scopes[i]);
                s_publishedScreenMaskEvidence.validReceivers[i].Store(
                    source.validReceivers[i]);
            }
            s_publishedScreenMaskEvidence.sequence.store(
                writeSequence + 1u, std::memory_order_release);
            s_publishedScreenMaskEvidence.writer.clear(
                std::memory_order_release);
        }

        void InvalidatePublishedScreenMaskEvidence() noexcept
        {
            PublishScreenMaskEvidence({});
        }

        struct ScreenMaskEvidenceState
        {
            ComPtr<ID3D11Texture2D> staging;
            ComPtr<ID3D11Texture2D> receiverValidity;
            ComPtr<ID3D11UnorderedAccessView> receiverValidityUAV;
            ComPtr<ID3D11Texture2D> receiverValidityStaging;
            ComPtr<ID3D11Query> query;
            uint64_t epoch{ 0 };
            uint32_t dispatchOrdinal{ 0 };
            uint64_t capturedWorldGeneration{
                (std::numeric_limits<uint64_t>::max)() };
            float debugMode{ 0.0f };
            uint64_t requestId{ 0 };
            uint32_t mapRetryCount{ 0 };
            uint64_t nextAllocationAttempt{ 0 };
            bool pending{ false };
        };

        ScreenMaskEvidenceState s_screenMaskEvidence;
        ComPtr<ID3D11Device> s_maskFormatDevice;
        DXGI_FORMAT s_maskFormat = DXGI_FORMAT_UNKNOWN;

        constexpr uint32_t kCloudTelemetryRecordCount = 8;
        constexpr uint32_t kCloudTelemetryReadbackInterval = 30;

        struct CloudTelemetryReadbackState
        {
            ComPtr<ID3D11Buffer> gpuBuffer;
            ComPtr<ID3D11UnorderedAccessView> gpuUAV;
            ComPtr<ID3D11Buffer> staging;
            ComPtr<ID3D11Query> query;
            uint64_t epoch{ 0 };
            uint64_t worldGeneration{ 0 };
            uint32_t dispatchOrdinal{ 0 };
            uint32_t mapRetryCount{ 0 };
            bool logProjection{ false };
            bool pending{ false };
        };

        CloudTelemetryReadbackState s_cloudTelemetry;

        void ResetScreenMaskEvidence() noexcept
        {
            s_screenMaskEvidence = {};
            InvalidatePublishedScreenMaskEvidence();
        }

        void ResetCloudTelemetry() noexcept
        {
            s_cloudTelemetry = {};
            g_cloudTelemetryValid.store(false, std::memory_order_release);
            g_receiverSunRayCloudOpacity.store(0.0f, std::memory_order_relaxed);
            g_localCloudOpacityMinimum.store(0.0f, std::memory_order_relaxed);
            g_localCloudOpacityMean.store(0.0f, std::memory_order_relaxed);
            g_localCloudOpacityMaximum.store(0.0f, std::memory_order_relaxed);
            g_localCloudShadowedFraction.store(0.0f, std::memory_order_relaxed);
            g_localCloudTelemetrySampleCount.store(
                0, std::memory_order_relaxed);
            g_cloudTelemetryEpoch.store(0, std::memory_order_relaxed);
            g_cloudTelemetryWorldGeneration.store(
                0, std::memory_order_relaxed);
            g_cloudTelemetryDispatchOrdinal.store(
                0, std::memory_order_relaxed);
            g_cloudTelemetrySunEligible.store(
                false, std::memory_order_release);
            g_cloudTelemetryFieldCommitted.store(
                false, std::memory_order_release);
        }

        bool EnsureCloudTelemetryResources(ID3D11Device* device) noexcept
        {
            if (!device)
                return false;
            if (s_cloudTelemetry.gpuBuffer && s_cloudTelemetry.gpuUAV &&
                s_cloudTelemetry.staging && s_cloudTelemetry.query) {
                return true;
            }

            ResetCloudTelemetry();
            D3D11_BUFFER_DESC gpuDescription{};
            gpuDescription.ByteWidth =
                sizeof(XMFLOAT4) * kCloudTelemetryRecordCount;
            gpuDescription.Usage = D3D11_USAGE_DEFAULT;
            gpuDescription.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            gpuDescription.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            gpuDescription.StructureByteStride = sizeof(XMFLOAT4);
            if (FAILED(device->CreateBuffer(
                    &gpuDescription, nullptr,
                    s_cloudTelemetry.gpuBuffer.GetAddressOf()))) {
                ResetCloudTelemetry();
                return false;
            }

            D3D11_UNORDERED_ACCESS_VIEW_DESC viewDescription{};
            viewDescription.Format = DXGI_FORMAT_UNKNOWN;
            viewDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            viewDescription.Buffer.FirstElement = 0;
            viewDescription.Buffer.NumElements = kCloudTelemetryRecordCount;
            if (FAILED(device->CreateUnorderedAccessView(
                    s_cloudTelemetry.gpuBuffer.Get(), &viewDescription,
                    s_cloudTelemetry.gpuUAV.GetAddressOf()))) {
                ResetCloudTelemetry();
                return false;
            }

            D3D11_BUFFER_DESC stagingDescription = gpuDescription;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.BindFlags = 0;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDescription.MiscFlags = 0;
            stagingDescription.StructureByteStride = 0;
            if (FAILED(device->CreateBuffer(
                    &stagingDescription, nullptr,
                    s_cloudTelemetry.staging.GetAddressOf()))) {
                ResetCloudTelemetry();
                return false;
            }

            D3D11_QUERY_DESC queryDescription{};
            queryDescription.Query = D3D11_QUERY_EVENT;
            if (FAILED(device->CreateQuery(
                    &queryDescription,
                    s_cloudTelemetry.query.GetAddressOf()))) {
                ResetCloudTelemetry();
                return false;
            }
            AdvancePrepassBindingResourceGeneration();
            return true;
        }

        void CreateScreenMaskEvidenceResources(
            ID3D11Device* device,
            const D3D11_TEXTURE2D_DESC& sourceDescription) noexcept
        {
            ResetScreenMaskEvidence();
            if (!device)
                return;

            D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.BindFlags = 0;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDescription.MiscFlags = 0;

            D3D11_TEXTURE2D_DESC validityDescription = sourceDescription;
            validityDescription.Format = DXGI_FORMAT_R8_UINT;
            validityDescription.Usage = D3D11_USAGE_DEFAULT;
            validityDescription.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            validityDescription.CPUAccessFlags = 0;
            validityDescription.MiscFlags = 0;
            D3D11_UNORDERED_ACCESS_VIEW_DESC validityViewDescription{};
            validityViewDescription.Format = validityDescription.Format;
            validityViewDescription.ViewDimension =
                D3D11_UAV_DIMENSION_TEXTURE2D;
            validityViewDescription.Texture2D.MipSlice = 0;
            D3D11_TEXTURE2D_DESC validityStagingDescription =
                validityDescription;
            validityStagingDescription.Usage = D3D11_USAGE_STAGING;
            validityStagingDescription.BindFlags = 0;
            validityStagingDescription.CPUAccessFlags =
                D3D11_CPU_ACCESS_READ;
            D3D11_QUERY_DESC queryDescription{};
            queryDescription.Query = D3D11_QUERY_EVENT;
            if (FAILED(device->CreateTexture2D(
                    &stagingDescription, nullptr,
                    s_screenMaskEvidence.staging.GetAddressOf())) ||
                FAILED(device->CreateTexture2D(
                    &validityDescription, nullptr,
                    s_screenMaskEvidence.receiverValidity.GetAddressOf())) ||
                FAILED(device->CreateUnorderedAccessView(
                    s_screenMaskEvidence.receiverValidity.Get(),
                    &validityViewDescription,
                    s_screenMaskEvidence.receiverValidityUAV.GetAddressOf())) ||
                FAILED(device->CreateTexture2D(
                    &validityStagingDescription, nullptr,
                    s_screenMaskEvidence.receiverValidityStaging.GetAddressOf())) ||
                FAILED(device->CreateQuery(
                    &queryDescription,
                    s_screenMaskEvidence.query.GetAddressOf()))) {
                ResetScreenMaskEvidence();
                s_screenMaskEvidence.nextAllocationAttempt = GetTickCount64() + 5000;
                SPDLOG_WARN(
                    "[CloudShadows] GPU screen-mask evidence readback unavailable");
            }
        }

        float ReadScreenMaskValue(
            const uint8_t* row,
            uint32_t x,
            DXGI_FORMAT format) noexcept
        {
            if (format == DXGI_FORMAT_R16_FLOAT) {
                uint16_t packed = 0;
                std::memcpy(
                    &packed, row + static_cast<size_t>(x) * sizeof(packed),
                    sizeof(packed));
                return DirectX::PackedVector::XMConvertHalfToFloat(packed);
            }
            float value = 0.0f;
            std::memcpy(
                &value, row + static_cast<size_t>(x) * sizeof(value),
                sizeof(value));
            return value;
        }

        void LogScreenMaskEvidenceStats(
            const char* scope,
            const ScreenMaskEvidenceStats& stats) noexcept
        {
            const double finite = static_cast<double>(stats.finiteCount);
            const double neutralPercent = finite != 0.0
                ? 100.0 * stats.neutralCount / finite
                : 0.0;
            const double strongPercent = finite != 0.0
                ? 100.0 * stats.strongShadowCount / finite
                : 0.0;
            const double range = stats.finiteCount != 0
                ? static_cast<double>(stats.maximum) - stats.minimum
                : 0.0;
            const double standardDeviation = stats.StandardDeviation();
            const char* verdict = stats.finiteCount == 0 ||
                    stats.nonFiniteCount != 0
                ? "INVALID"
                : (stats.minimum >= 0.995f
                    ? "ALL_WHITE"
                    : (stats.maximum <= 0.005f
                        ? "ALL_BLACK"
                        : (range < 0.025 || standardDeviation < 0.005
                            ? "UNIFORM"
                            : "VARIED")));
            SPDLOG_INFO(
                "[CloudShadows] GPU screen-mask evidence dispatch={} epoch={} "
                "debug={:.0f} scope={} verdict={} samples={} nonFinite={} "
                "outOfRange={} exactOne={} "
                "min={:.6f} max={:.6f} "
                "mean={:.6f} stddev={:.6f} neutralPct={:.2f} "
                "strongShadowPct={:.2f}",
                s_screenMaskEvidence.dispatchOrdinal,
                s_screenMaskEvidence.epoch, s_screenMaskEvidence.debugMode,
                scope, verdict, stats.finiteCount, stats.nonFiniteCount,
                stats.outOfRangeCount, stats.exactOneCount,
                stats.finiteCount ? stats.minimum : 0.0f,
                stats.finiteCount ? stats.maximum : 0.0f,
                stats.Mean(), standardDeviation, neutralPercent,
                strongPercent);
        }

        void PollScreenMaskEvidence(ID3D11DeviceContext* context) noexcept
        {
            if (!context || !s_screenMaskEvidence.pending ||
                !s_screenMaskEvidence.staging ||
                !s_screenMaskEvidence.receiverValidityStaging ||
                !s_screenMaskEvidence.query)
                return;

            auto abandon = [](const char* reason) noexcept {
                SPDLOG_WARN(
                    "[CloudShadows] Discarding GPU screen-mask evidence: {}",
                    reason);
                s_screenMaskEvidence.pending = false;
                s_screenMaskEvidence.mapRetryCount = 0;
                s_screenMaskEvidence.capturedWorldGeneration =
                    (std::numeric_limits<uint64_t>::max)();
                InvalidatePublishedScreenMaskEvidence();
            };

            BOOL complete = FALSE;
            const HRESULT queryResult = context->GetData(
                s_screenMaskEvidence.query.Get(), &complete, sizeof(complete),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (queryResult == S_FALSE ||
                (queryResult == S_OK && !complete)) {
                return;
            }
            if (queryResult != S_OK) {
                abandon("event-query failure");
                return;
            }
            if (s_screenMaskEvidence.capturedWorldGeneration !=
                g_worldCloudResetGeneration.load(std::memory_order_acquire)) {
                abandon("world generation changed before completion");
                return;
            }

            D3D11_TEXTURE2D_DESC description{};
            s_screenMaskEvidence.staging->GetDesc(&description);
            D3D11_TEXTURE2D_DESC validityDescription{};
            s_screenMaskEvidence.receiverValidityStaging->GetDesc(
                &validityDescription);
            if (description.Format != DXGI_FORMAT_R16_FLOAT &&
                    description.Format != DXGI_FORMAT_R32_FLOAT ||
                validityDescription.Format != DXGI_FORMAT_R8_UINT ||
                validityDescription.Width != description.Width ||
                validityDescription.Height != description.Height) {
                SPDLOG_WARN(
                    "[CloudShadows] GPU screen-mask evidence has unsupported format {}",
                    static_cast<uint32_t>(description.Format));
                abandon("resource contract changed");
                return;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT maskMapResult = context->Map(
                s_screenMaskEvidence.staging.Get(), 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            constexpr uint32_t kMaximumMapRetries = 120;
            if (maskMapResult == DXGI_ERROR_WAS_STILL_DRAWING) {
                if (++s_screenMaskEvidence.mapRetryCount >= kMaximumMapRetries)
                    abandon("mask map retry budget exhausted");
                return;
            }
            if (FAILED(maskMapResult) || !mapped.pData) {
                abandon("mask map failure");
                return;
            }

            D3D11_MAPPED_SUBRESOURCE validityMapped{};
            const HRESULT validityMapResult = context->Map(
                s_screenMaskEvidence.receiverValidityStaging.Get(), 0,
                D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT,
                &validityMapped);
            if (validityMapResult == DXGI_ERROR_WAS_STILL_DRAWING) {
                context->Unmap(s_screenMaskEvidence.staging.Get(), 0);
                if (++s_screenMaskEvidence.mapRetryCount >= kMaximumMapRetries)
                    abandon("receiver-validity map retry budget exhausted");
                return;
            }
            if (FAILED(validityMapResult) || !validityMapped.pData) {
                context->Unmap(s_screenMaskEvidence.staging.Get(), 0);
                abandon("receiver-validity map failure");
                return;
            }

            std::array<ScreenMaskEvidenceStats,
                kScreenMaskEvidenceScopeCount> allPixels{};
            std::array<ScreenMaskEvidenceStats,
                kScreenMaskEvidenceScopeCount> validPixels{};
            std::array<uint64_t, kScreenMaskEvidenceScopeCount>
                validReceiverCounts{};
            std::array<uint64_t, kScreenMaskEvidenceScopeCount>
                validExactOneCounts{};
            std::array<uint64_t, kScreenMaskEvidenceScopeCount>
                validAttenuatedCounts{};
            constexpr uint32_t kSampleStride = 2;
            for (uint32_t y = 0; y < description.Height; y += kSampleStride) {
                const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                    static_cast<size_t>(y) * mapped.RowPitch;
                const auto* validityRow =
                    static_cast<const uint8_t*>(validityMapped.pData) +
                    static_cast<size_t>(y) * validityMapped.RowPitch;
                for (uint32_t x = 0; x < description.Width;
                    x += kSampleStride) {
                    const float value = ReadScreenMaskValue(
                        row, x, description.Format);
                    const bool inLowerHalf = y >= description.Height / 2u;
                    const bool inCentreGround = inLowerHalf &&
                        x >= description.Width / 4u &&
                        x < (description.Width * 3u) / 4u;
                    const std::array<bool, kScreenMaskEvidenceScopeCount>
                        inScope{ true, inLowerHalf, inCentreGround };
                    const bool receiverValid = validityRow[x] != 0u;
                    for (uint32_t scope = 0;
                        scope < kScreenMaskEvidenceScopeCount; ++scope) {
                        if (!inScope[scope])
                            continue;
                        allPixels[scope].Add(value);
                        if (!receiverValid)
                            continue;
                        ++validReceiverCounts[scope];
                        validPixels[scope].Add(value);
                        if (value == 1.0f)
                            ++validExactOneCounts[scope];
                        else if (std::isfinite(value) && value >= 0.0f &&
                            value < 1.0f) {
                            ++validAttenuatedCounts[scope];
                        }
                    }
                }
            }
            context->Unmap(s_screenMaskEvidence.staging.Get(), 0);
            context->Unmap(
                s_screenMaskEvidence.receiverValidityStaging.Get(), 0);

            static constexpr std::array<const char*,
                kScreenMaskEvidenceScopeCount> kScopeNames{
                    "all", "lower-half", "centre-ground"
                };
            for (uint32_t scope = 0;
                scope < kScreenMaskEvidenceScopeCount; ++scope) {
                LogScreenMaskEvidenceStats(
                    kScopeNames[scope], allPixels[scope]);
            }

            ScreenMaskEvidenceSnapshot snapshot{};
            snapshot.valid = true;
            snapshot.requestId = s_screenMaskEvidence.requestId;
            snapshot.epoch = s_screenMaskEvidence.epoch;
            snapshot.worldGeneration =
                s_screenMaskEvidence.capturedWorldGeneration;
            snapshot.dispatchOrdinal =
                s_screenMaskEvidence.dispatchOrdinal;
            snapshot.width = description.Width;
            snapshot.height = description.Height;
            snapshot.sampleStride = kSampleStride;
            snapshot.debugMode = s_screenMaskEvidence.debugMode;
            for (uint32_t scope = 0;
                scope < kScreenMaskEvidenceScopeCount; ++scope) {
                snapshot.scopes[scope] =
                    SummarizeScreenMaskEvidence(allPixels[scope]);
                auto& valid = snapshot.validReceivers[scope];
                valid.validReceiverCount = validReceiverCounts[scope];
                valid.validReceiverExactOneCount =
                    validExactOneCounts[scope];
                valid.validReceiverAttenuatedCount =
                    validAttenuatedCounts[scope];
                valid.distribution =
                    SummarizeScreenMaskEvidence(validPixels[scope]);
            }
            PublishScreenMaskEvidence(snapshot);
            AdvanceCompletedRequest(
                s_completedScreenMaskEvidenceRequest,
                s_screenMaskEvidence.requestId);
            s_screenMaskEvidence.pending = false;
            s_screenMaskEvidence.mapRetryCount = 0;
        }

        void ArmScreenMaskEvidence(
            ID3D11DeviceContext* context,
            uint32_t dispatchOrdinal,
            uint64_t epoch,
            uint64_t requestId) noexcept
        {
            // Screen evidence is strictly request-driven. requestId was
            // selected before this dispatch and also enabled its diagnostic
            // receiver-validity writes, so never capture a different/newer
            // request that arrived after the constants were submitted.
            const float debugMode = g_settings.DebugMode;
            const uint64_t worldGeneration =
                g_worldCloudResetGeneration.load(std::memory_order_acquire);
            if (!context ||
                requestId == 0 ||
                s_screenMaskEvidence.pending ||
                !s_screenMaskEvidence.staging ||
                !s_screenMaskEvidence.receiverValidity ||
                !s_screenMaskEvidence.receiverValidityStaging ||
                !s_screenMaskEvidence.query ||
                !g_cloudShadowTex)
                return;

            // The compute dispatch has completed in command order. Unbind the
            // UAV before copying; ScopedPrepassState restores the caller's CS
            // state when Prepass returns.
            ID3D11UnorderedAccessView* nullOutput = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
            context->CSSetUnorderedAccessViews(2, 1, &nullOutput, nullptr);
            context->CopyResource(
                s_screenMaskEvidence.staging.Get(), g_cloudShadowTex);
            context->CopyResource(
                s_screenMaskEvidence.receiverValidityStaging.Get(),
                s_screenMaskEvidence.receiverValidity.Get());
            context->End(s_screenMaskEvidence.query.Get());
            s_screenMaskEvidence.dispatchOrdinal = dispatchOrdinal;
            s_screenMaskEvidence.epoch = epoch;
            s_screenMaskEvidence.debugMode = debugMode;
            s_screenMaskEvidence.requestId = requestId;
            s_screenMaskEvidence.capturedWorldGeneration = worldGeneration;
            s_screenMaskEvidence.mapRetryCount = 0;
            s_screenMaskEvidence.pending = true;
        }

        uint64_t GetArmableScreenMaskEvidenceRequest() noexcept
        {
            if (s_screenMaskEvidence.pending || !g_cloudShadowTex)
                return 0;
            const uint64_t requestedId =
                s_screenMaskEvidenceRequest.load(std::memory_order_acquire);
            const uint64_t completedId =
                s_completedScreenMaskEvidenceRequest.load(
                    std::memory_order_acquire);
            if (requestedId <= completedId)
                return 0;
            if (!s_screenMaskEvidence.query) {
                if (GetTickCount64() < s_screenMaskEvidence.nextAllocationAttempt)
                    return 0;
                D3D11_TEXTURE2D_DESC description{};
                g_cloudShadowTex->GetDesc(&description);
                CreateScreenMaskEvidenceResources(GetD3DDevice(), description);
                AdvancePrepassBindingResourceGeneration();
            }
            return s_screenMaskEvidence.query ? requestedId : 0;
        }

        void PollCloudTelemetry(ID3D11DeviceContext* context) noexcept
        {
            if (!context || !s_cloudTelemetry.pending ||
                !s_cloudTelemetry.staging || !s_cloudTelemetry.query) {
                return;
            }

            BOOL complete = FALSE;
            const HRESULT queryResult = context->GetData(
                s_cloudTelemetry.query.Get(), &complete, sizeof(complete),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (queryResult == S_FALSE ||
                (queryResult == S_OK && !complete)) {
                return;
            }
            if (queryResult != S_OK) {
                s_cloudTelemetry.pending = false;
                g_cloudTelemetryValid.store(false, std::memory_order_release);
                g_cloudTelemetrySunEligible.store(
                    false, std::memory_order_release);
                g_cloudTelemetryFieldCommitted.store(
                    false, std::memory_order_release);
                g_cloudTelemetryEpoch.store(0, std::memory_order_relaxed);
                g_cloudTelemetryWorldGeneration.store(
                    0, std::memory_order_relaxed);
                g_cloudTelemetryDispatchOrdinal.store(
                    0, std::memory_order_relaxed);
                return;
            }
            if (s_cloudTelemetry.worldGeneration !=
                g_worldCloudResetGeneration.load(std::memory_order_acquire)) {
                s_cloudTelemetry.pending = false;
                g_cloudTelemetryValid.store(false, std::memory_order_release);
                g_cloudTelemetrySunEligible.store(
                    false, std::memory_order_release);
                g_cloudTelemetryFieldCommitted.store(
                    false, std::memory_order_release);
                g_cloudTelemetryEpoch.store(0, std::memory_order_relaxed);
                g_cloudTelemetryWorldGeneration.store(
                    0, std::memory_order_relaxed);
                g_cloudTelemetryDispatchOrdinal.store(
                    0, std::memory_order_relaxed);
                return;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT mapResult = context->Map(
                s_cloudTelemetry.staging.Get(), 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (mapResult == DXGI_ERROR_WAS_STILL_DRAWING) {
                constexpr uint32_t kMaximumMapRetries = 120;
                if (++s_cloudTelemetry.mapRetryCount >= kMaximumMapRetries) {
                    s_cloudTelemetry.pending = false;
                    s_cloudTelemetry.mapRetryCount = 0;
                    g_cloudTelemetryValid.store(
                        false, std::memory_order_release);
                    g_cloudTelemetrySunEligible.store(
                        false, std::memory_order_release);
                    g_cloudTelemetryFieldCommitted.store(
                        false, std::memory_order_release);
                    g_cloudTelemetryEpoch.store(0, std::memory_order_relaxed);
                    g_cloudTelemetryWorldGeneration.store(
                        0, std::memory_order_relaxed);
                    g_cloudTelemetryDispatchOrdinal.store(
                        0, std::memory_order_relaxed);
                }
                return;
            }
            if (FAILED(mapResult) || !mapped.pData) {
                s_cloudTelemetry.pending = false;
                g_cloudTelemetryValid.store(false, std::memory_order_release);
                g_cloudTelemetrySunEligible.store(
                    false, std::memory_order_release);
                g_cloudTelemetryFieldCommitted.store(
                    false, std::memory_order_release);
                g_cloudTelemetryEpoch.store(0, std::memory_order_relaxed);
                g_cloudTelemetryWorldGeneration.store(
                    0, std::memory_order_relaxed);
                g_cloudTelemetryDispatchOrdinal.store(
                    0, std::memory_order_relaxed);
                return;
            }

            std::array<XMFLOAT4, kCloudTelemetryRecordCount> records{};
            std::memcpy(records.data(), mapped.pData, sizeof(records));
            context->Unmap(s_cloudTelemetry.staging.Get(), 0);
            s_cloudTelemetry.pending = false;
            s_cloudTelemetry.mapRetryCount = 0;

            const auto& ray = records[0];
            const auto& area = records[1];
            const auto& definition = records[2];
            if (s_cloudTelemetry.logProjection) {
                const auto& sun = records[3];
                const auto& receiver = records[4];
                const auto& sample = records[5];
                const auto& camera = records[6];
                const auto& mappingOrigin = records[7];
                SPDLOG_INFO(
                    "[CloudShadows] GPU projection evidence dispatch={} epoch={} "
                    "sun={:.6f},{:.6f},{:.6f} sunValid={} "
                    "receiver={:.2f},{:.2f},{:.2f} depth={:.8f} "
                    "sampleDir={:.6f},{:.6f},{:.6f} sampledOpacity={:.8f} "
                    "cameraOpacity={:.8f} localOpacityMinMeanMax={:.6f}/{:.6f}/{:.6f} "
                    "camera={:.2f},{:.2f},{:.2f} cloudHeight={:.1f} "
                    "mappingOrigin={:.2f},{:.2f},{:.2f}",
                    s_cloudTelemetry.dispatchOrdinal, s_cloudTelemetry.epoch,
                    sun.x, sun.y, sun.z, sun.w > 0.5f,
                    receiver.x, receiver.y, receiver.z, receiver.w,
                    sample.x, sample.y, sample.z, sample.w,
                    ray.x, definition.x, area.x, area.y,
                    camera.x, camera.y, camera.z, camera.w,
                    mappingOrigin.x, mappingOrigin.y, mappingOrigin.z);
            }
            const float values[] = {
                ray.x, ray.y, ray.z, ray.w,
                area.x, area.y, area.z, area.w,
                definition.x, definition.y, definition.z, definition.w
            };
            bool finite = true;
            for (float value : values)
                finite = finite && std::isfinite(value);
            g_cloudTelemetryValid.store(false, std::memory_order_release);
            g_cloudTelemetrySunEligible.store(
                false, std::memory_order_release);
            g_cloudTelemetryFieldCommitted.store(
                false, std::memory_order_release);
            g_cloudTelemetryEpoch.store(
                s_cloudTelemetry.epoch, std::memory_order_relaxed);
            g_cloudTelemetryWorldGeneration.store(
                s_cloudTelemetry.worldGeneration, std::memory_order_relaxed);
            g_cloudTelemetryDispatchOrdinal.store(
                s_cloudTelemetry.dispatchOrdinal, std::memory_order_relaxed);
            const bool sunEligible = finite && ray.y > 0.0f && ray.y <= 1.0f;
            const bool fieldCommitted = finite && ray.z >= 1.0f &&
                ray.z <= static_cast<float>(kMaxWorldCloudLayers);
            g_cloudTelemetrySunEligible.store(
                sunEligible, std::memory_order_release);
            g_cloudTelemetryFieldCommitted.store(
                fieldCommitted, std::memory_order_release);
            // A negative validity marker is a successfully completed
            // observation whose sun/field preconditions were not eligible.
            // Keep epoch/generation so acceptance can classify weather/night
            // as inconclusive rather than confusing it with a readback fault.
            if (finite && ray.w < -0.5f)
                return;
            const uint32_t sampleCount = finite && definition.y >= 0.0f
                ? static_cast<uint32_t>(std::lround(definition.y))
                : 0u;
            const bool valid = finite && ray.w > 0.5f && area.w > 0.5f &&
                ray.x >= 0.0f && ray.x <= 1.0f &&
                ray.y > 0.0f && ray.y <= 1.0f &&
                ray.z >= 1.0f && ray.z <= kMaxWorldCloudLayers &&
                area.x >= 0.0f && area.x <= 1.0f &&
                area.y >= 0.0f && area.y <= 1.0f &&
                area.z >= 0.0f && area.z <= 1.0f &&
                definition.x >= 0.0f && definition.x <= 1.0f &&
                sampleCount > 0u && sampleCount <= 81u &&
                std::abs(definition.y - static_cast<float>(sampleCount)) < 0.01f &&
                std::abs(definition.z - kCloudTelemetryRadiusWorldUnits) < 0.5f &&
                std::abs(definition.w - kCloudTelemetryOpacityThreshold) < 0.0001f;
            if (!valid) {
                g_cloudTelemetryValid.store(false, std::memory_order_release);
                return;
            }

            g_receiverSunRayCloudOpacity.store(
                ray.x, std::memory_order_relaxed);
            g_localCloudOpacityMinimum.store(
                definition.x, std::memory_order_relaxed);
            g_localCloudOpacityMean.store(area.x, std::memory_order_relaxed);
            g_localCloudOpacityMaximum.store(area.y, std::memory_order_relaxed);
            g_localCloudShadowedFraction.store(
                area.z, std::memory_order_relaxed);
            g_localCloudTelemetrySampleCount.store(
                sampleCount, std::memory_order_relaxed);
            g_cloudTelemetryEpoch.store(
                s_cloudTelemetry.epoch, std::memory_order_relaxed);
            g_cloudTelemetryWorldGeneration.store(
                s_cloudTelemetry.worldGeneration, std::memory_order_relaxed);
            g_cloudTelemetryDispatchOrdinal.store(
                s_cloudTelemetry.dispatchOrdinal, std::memory_order_relaxed);
            g_cloudTelemetrySunEligible.store(
                true, std::memory_order_release);
            g_cloudTelemetryFieldCommitted.store(
                true, std::memory_order_release);
            g_cloudTelemetryValid.store(true, std::memory_order_release);
        }

        void ArmCloudTelemetry(
            ID3D11DeviceContext* context,
            uint32_t dispatchOrdinal,
            uint64_t epoch,
            bool logProjection) noexcept
        {
            if (!context ||
                s_cloudTelemetry.pending || !s_cloudTelemetry.gpuBuffer ||
                !s_cloudTelemetry.staging || !s_cloudTelemetry.query) {
                return;
            }

            // Dispatch wrote the six records in command order. Unbind only
            // diagnostic u1, enqueue a 128-byte copy and poll it on later frames;
            // never flush or wait on the render thread.
            ID3D11UnorderedAccessView* nullTelemetry = nullptr;
            context->CSSetUnorderedAccessViews(1, 1, &nullTelemetry, nullptr);
            context->CopyResource(
                s_cloudTelemetry.staging.Get(),
                s_cloudTelemetry.gpuBuffer.Get());
            context->End(s_cloudTelemetry.query.Get());
            s_cloudTelemetry.epoch = epoch;
            s_cloudTelemetry.worldGeneration =
                g_worldCloudResetGeneration.load(std::memory_order_acquire);
            s_cloudTelemetry.dispatchOrdinal = dispatchOrdinal;
            s_cloudTelemetry.logProjection = logProjection;
            s_cloudTelemetry.mapRetryCount = 0;
            s_cloudTelemetry.pending = true;
            g_cloudTelemetryValid.store(false, std::memory_order_release);
            g_cloudTelemetrySunEligible.store(
                false, std::memory_order_release);
            g_cloudTelemetryFieldCommitted.store(
                false, std::memory_order_release);
            g_cloudTelemetryEpoch.store(0, std::memory_order_relaxed);
            g_cloudTelemetryWorldGeneration.store(
                0, std::memory_order_relaxed);
            g_cloudTelemetryDispatchOrdinal.store(
                0, std::memory_order_relaxed);
        }
    }

    bool GetScreenMaskEvidenceSnapshot(
        ScreenMaskEvidenceSnapshot& destination) noexcept
    {
        // Every payload field is atomic, and the even sequence brackets one
        // coherent publication. Retry if a publication overlaps this read.
        constexpr uint32_t kMaximumAttempts = 64;
        for (uint32_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
            const uint64_t before =
                s_publishedScreenMaskEvidence.sequence.load(
                    std::memory_order_acquire);
            if ((before & 1u) != 0u)
                continue;

            ScreenMaskEvidenceSnapshot candidate{};
            candidate.valid = s_publishedScreenMaskEvidence.valid.load(
                std::memory_order_relaxed);
            candidate.requestId =
                s_publishedScreenMaskEvidence.requestId.load(
                    std::memory_order_relaxed);
            candidate.epoch = s_publishedScreenMaskEvidence.epoch.load(
                std::memory_order_relaxed);
            candidate.worldGeneration =
                s_publishedScreenMaskEvidence.worldGeneration.load(
                    std::memory_order_relaxed);
            candidate.dispatchOrdinal =
                s_publishedScreenMaskEvidence.dispatchOrdinal.load(
                    std::memory_order_relaxed);
            candidate.width = s_publishedScreenMaskEvidence.width.load(
                std::memory_order_relaxed);
            candidate.height = s_publishedScreenMaskEvidence.height.load(
                std::memory_order_relaxed);
            candidate.sampleStride =
                s_publishedScreenMaskEvidence.sampleStride.load(
                    std::memory_order_relaxed);
            candidate.debugMode =
                s_publishedScreenMaskEvidence.debugMode.load(
                    std::memory_order_relaxed);
            for (uint32_t i = 0; i < kScreenMaskEvidenceScopeCount; ++i) {
                s_publishedScreenMaskEvidence.scopes[i].Load(
                    candidate.scopes[i]);
                s_publishedScreenMaskEvidence.validReceivers[i].Load(
                    candidate.validReceivers[i]);
            }

            const uint64_t after =
                s_publishedScreenMaskEvidence.sequence.load(
                    std::memory_order_acquire);
            if (before == after && (after & 1u) == 0u) {
                candidate.generation = after / 2u;
                destination = candidate;
                return true;
            }
        }
        return false;
    }

    uint64_t RequestScreenMaskEvidenceCapture() noexcept
    {
        return s_screenMaskEvidenceRequest.fetch_add(
            1, std::memory_order_acq_rel) + 1u;
    }

    void CancelPendingAcceptanceEvidenceRequests() noexcept
    {
        // Cancel only requests which have not yet armed. An already pending
        // query remains owned by PollScreenMaskEvidence until completion.
        AdvanceCompletedRequest(
            s_completedScreenMaskEvidenceRequest,
            s_screenMaskEvidenceRequest.load(std::memory_order_acquire));
        CancelPendingWorldCloudCubeEvidenceRequests();
    }

	void ValidateSettings(Settings& value) noexcept
	{
		auto finiteOr = [](float input, float fallback) {
			return std::isfinite(input) ? input : fallback;
		};
		value.Opacity = std::clamp(finiteOr(value.Opacity, 2.0f), 0.0f, 4.0f);
		value.CloudHeight = std::clamp(finiteOr(value.CloudHeight, Settings{}.CloudHeight), 10000.0f, 200000.0f);
		value.LayerHeightStep = std::clamp(finiteOr(value.LayerHeightStep, 6000.0f), 0.0f, 50000.0f);
		value.WorldTileSize = std::clamp(finiteOr(value.WorldTileSize, 20480.0f), 1024.0f, 1000000.0f);
		value.LayerScaleMultiplier = std::clamp(finiteOr(value.LayerScaleMultiplier, 1.18f), 1.0f, 4.0f);
		value.VerticalOpticalDepth = std::clamp(finiteOr(value.VerticalOpticalDepth, 0.85f), 0.0f, 8.0f);
		value.SunAngularRadius = std::clamp(finiteOr(value.SunAngularRadius, 0.00465f), 0.0001f, 0.05f);
		value.MaxOpticalSlant = std::clamp(finiteOr(value.MaxOpticalSlant, 8.0f), 1.0f, 32.0f);
		value.BaseMipBias = std::clamp(finiteOr(value.BaseMipBias, 0.0f), -2.0f, 6.0f);
		// Legacy bridge fields only. A low-sun fade would make a real cloud hit
		// return fully lit, violating the exact receiver-to-sun visibility rule.
		value.SunFadeStart = 0.0f;
		value.SunFadeEnd = 0.0f;
		value.MaxLayers = std::clamp(finiteOr(value.MaxLayers, 16.0f), 1.0f,
			static_cast<float>(kMaxWorldCloudLayers));
		value.DebugMode = FO4CS::BuildFeatures::kDeveloperTools
            ? std::clamp(finiteOr(value.DebugMode, 0.0f), 0.0f, 4.0f) : 0.0f;
	}

    bool LoadSettings(bool applyEnabledFromFile)
    {
        if (FO4CS::McmSettings::Available())
            return FO4CS::McmSettings::Load(applyEnabledFromFile);
        fs::path path;
        if (!ResolveGameRelativePath(
                fs::path("Data") / "Shaders" / "Features" /
                    "CloudShadows.json",
                path)) {
            return false;
        }
        try {
            if (!fs::exists(path))
                return false;
            const auto writeTime = fs::last_write_time(path);
            std::ifstream file(path);
            if (!file.is_open()) {
                SPDLOG_WARN("[CloudShadows] Cannot open settings: {}", path.string());
                return false;
            }

            nlohmann::json j;
            file >> j;
            if (!j.is_object())
                throw std::runtime_error("settings root must be a JSON object");
            const auto version = j.find("ProjectionModelVersion");
            if (version == j.end() || !version->is_number_integer() ||
                version->get<std::int64_t>() != kProjectionModelVersion) {
                throw std::runtime_error(
                    "ProjectionModelVersion must be exactly " +
                    std::to_string(kProjectionModelVersion));
            }

            Settings next = g_settings;
			bool enabled = g_shadowsEnabled.load(std::memory_order_relaxed);
            bool godrayOcclusion =
                FO4CS::GodraysIntegration::IsCloudOcclusionEnabled();
            auto read = [&](const char* key, float& target) {
                if (j.contains(key))
                    target = j.at(key).get<float>();
            };
			// Adopt saved toggles on the first successful load, a file change,
			// or an explicit reload. Subsequent initialization retries preserve
			// the user's current session switches.
			// Initialize() re-reads this file on every retry, and while the
			// capture contract is unmet those retries run for the whole
			// session, so honouring Enabled there would silently revert F10
			// and the menu master switch seconds after the user set them.
			if (applyEnabledFromFile || !s_settingsLoaded) {
                if (j.contains("Enabled"))
                    enabled = j.at("Enabled").get<bool>();
                if (j.contains("GodrayCloudOcclusion"))
                    godrayOcclusion = j.at("GodrayCloudOcclusion").get<bool>();
            }
            read("Opacity", next.Opacity);
            read("CloudHeight", next.CloudHeight);
            if (j.contains("Hotkeys"))
                g_hotkeysEnabled.store(j.at("Hotkeys").get<bool>(), std::memory_order_release);
            if (j.contains("CaptureMethod")) {
                const auto method = j.at("CaptureMethod").get<int>();
                if (method != 0 && method != 1)
                    throw std::runtime_error("CaptureMethod must be 0 (cubemap) or 1 (sun 2D)");
                FO4CS::CloudComparison::SetMethod(method == 1
                    ? FO4CS::CloudComparison::Method::SunMask
                    : FO4CS::CloudComparison::Method::Cubemap);
            }
            read("LayerHeightStep", next.LayerHeightStep);
            read("WorldTileSize", next.WorldTileSize);
            read("LayerScaleMultiplier", next.LayerScaleMultiplier);
            read("VerticalOpticalDepth", next.VerticalOpticalDepth);
            read("SunAngularRadius", next.SunAngularRadius);
            read("MaxOpticalSlant", next.MaxOpticalSlant);
            read("BaseMipBias", next.BaseMipBias);
            // Debug output is deliberately session-only. Never let a saved
            // diagnostic replace the physical sunlight mask on the next boot.
            next.SunFadeStart = 0.0f;
            next.SunFadeEnd = 0.0f;
            next.DebugMode = 0.0f;
            read("MaxLayers", next.MaxLayers);
            ValidateSettings(next);
            g_settings = next;
            if (g_shadowsEnabled.exchange(enabled, std::memory_order_acq_rel) != enabled)
                InvalidateWorldCloudCaptureForToggle();
            FO4CS::GodraysIntegration::SetCloudOcclusionEnabled(godrayOcclusion);
            s_settingsWriteTime = writeTime;
            s_settingsLoaded = true;
            return true;
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[CloudShadows] Settings rejected; keeping previous values: {}", e.what());
            return false;
        }
    }

    // Re-read the JSON when its mtime changes so physical tuning can be changed
    // live in-game. Present polls at 0.5 Hz, including when shadows are off.
    void HotReloadSettingsIfChanged()
    {
        if (FO4CS::McmSettings::Available()) {
            FO4CS::McmSettings::Load(false);
            return;
        }
        try {
            fs::path path;
            if (!ResolveGameRelativePath(
                    fs::path("Data") / "Shaders" / "Features" /
                        "CloudShadows.json",
                    path)) {
                return;
            }
            if (!fs::exists(path))
                return;
            auto t = fs::last_write_time(path);
            if (t != s_settingsWriteTime) {
                s_settingsWriteTime = t;
                if (LoadSettings()) {
                    SPDLOG_INFO("[CloudShadows] Settings hot-reloaded: opacity={:.2f} "
                                "cloudHeight={:.0f}",
                        g_settings.Opacity, g_settings.CloudHeight);
                }
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[CloudShadows] Settings hot-reload check failed: {}", e.what());
        }
    }

    void SaveSettings()
    {
        if (FO4CS::McmSettings::Available()) {
            FO4CS::McmSettings::Save();
            return;
        }
        if (FO4CS::GodraysIntegration::IsCloudOcclusionEnabled())
            FO4CS::GodraysIntegration::EnsureNativeGodraysEnabled();
        fs::path dir;
        if (!ResolveGameRelativePath(
                fs::path("Data") / "Shaders" / "Features", dir)) {
            return;
        }
        auto path = dir / "CloudShadows.json";

        nlohmann::json j;
        j["ProjectionModelVersion"] = kProjectionModelVersion;
		j["Enabled"] = g_shadowsEnabled.load(std::memory_order_relaxed);
        j["GodrayCloudOcclusion"] =
            FO4CS::GodraysIntegration::IsCloudOcclusionEnabled();
        j["Opacity"] = g_settings.Opacity;
        j["CloudHeight"] = g_settings.CloudHeight;
        j["CaptureMethod"] = FO4CS::CloudComparison::GetMethod() ==
            FO4CS::CloudComparison::Method::SunMask ? 1 : 0;
        j["Hotkeys"] = g_hotkeysEnabled.load(std::memory_order_relaxed);

        try {
            fs::create_directories(dir);
            const auto temporary = path.wstring() + L".tmp";
            {
                std::ofstream file(fs::path(temporary), std::ios::binary | std::ios::trunc);
                if (!file.is_open())
                    throw std::runtime_error("cannot create temporary settings file");
                file << j.dump(4) << '\n';
                file.flush();
                if (!file.good())
                    throw std::runtime_error("failed writing temporary settings file");
            }
            if (!MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                    "atomic settings replace failed");
            }
            s_settingsWriteTime = fs::last_write_time(path);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("[CloudShadows] Failed to save settings: {}", e.what());
        }
    }

    void CompileComputeShader()
    {
        auto* device = GetD3DDevice();
        if (!device) {
            SPDLOG_ERROR("[CloudShadows] D3D device not available");
            return;
        }

        const auto shaderRelativePath = fs::path("Data") / "Shaders" /
            "CloudShadows" / "FO4CloudShadowScreenCS.hlsl";
        std::string src;
        if (!ReadAuthenticatedShaderSource(
                shaderRelativePath, FO4CS_SCREEN_SHADER_SHA256, src)) {
            SPDLOG_ERROR(
                "[CloudShadows] Compute shader unavailable because its "
                "runtime source is not the build-authenticated asset");
            return;
        }
        fs::path shaderPath;
        if (!ResolveGameRelativePath(shaderRelativePath, shaderPath))
            return;
        const auto sourceName = shaderPath.string();
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
            D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        const bool vrTarget =
            FO4CS::RuntimeAPI::GetSingleton().Target() ==
            FO4CS::F4SECompat::RuntimeTarget::kVR;
        const D3D_SHADER_MACRO defines[] = {
            { "FO4CS_SHADER_VR", vrTarget ? "1" : "0" },
            { nullptr, nullptr }
        };
        auto compileEntry = [&](const char* entryPoint,
                                ComPtr<ID3D11ComputeShader>& destination) {
            ComPtr<ID3DBlob> bytecode;
            ComPtr<ID3DBlob> errors;
            const HRESULT compileResult = D3DCompile(
                src.data(), src.size(), sourceName.c_str(), defines, nullptr,
                entryPoint, "cs_5_0", flags, 0,
                bytecode.GetAddressOf(), errors.GetAddressOf());
            if (FAILED(compileResult) || !bytecode) {
                SPDLOG_ERROR(
                    "[CloudShadows] Compute shader entry {} compile failed "
                    "(0x{:08X}): {}",
                    entryPoint, static_cast<uint32_t>(compileResult),
                    errors
                        ? static_cast<const char*>(errors->GetBufferPointer())
                        : "<no diagnostics>");
                return false;
            }

            const HRESULT createResult = device->CreateComputeShader(
                bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                nullptr, destination.GetAddressOf());
            if (FAILED(createResult) || !destination) {
                SPDLOG_ERROR(
                    "[CloudShadows] Failed to create compute shader entry {} "
                    "(0x{:08X})",
                    entryPoint, static_cast<uint32_t>(createResult));
                return false;
            }
            return true;
        };

        ComPtr<ID3D11ComputeShader> productionCandidate;
        ComPtr<ID3D11ComputeShader> diagnosticCandidate;
        if (!compileEntry("mainProduction", productionCandidate) ||
            !compileEntry("main", diagnosticCandidate) ||
            !FO4CS::CloudComparison::CompileShaders(device, src, sourceName, vrTarget)) {
            return;
        }

        if (g_cloudShadowProductionCS)
            g_cloudShadowProductionCS->Release();
        if (g_cloudShadowCS)
            g_cloudShadowCS->Release();
        g_cloudShadowProductionCS = productionCandidate.Detach();
        g_cloudShadowCS = diagnosticCandidate.Detach();
        AdvancePrepassBindingResourceGeneration();
        SPDLOG_INFO(
            "[CloudShadows] World-cloud production/diagnostic compute shaders "
            "ready ({})",
            vrTarget ? "VR" : "flat");
    }

    bool EnsureScreenShadowTexture(uint32_t width, uint32_t height)
    {
        auto device = GetD3DDevice();
        auto context = GetD3DContext();
        if (!device || !context || width == 0 || height == 0)
            return false;

        auto supportsMaskFormat = [&](DXGI_FORMAT format) noexcept {
            UINT support = 0;
            constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
                D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
                D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
            return SUCCEEDED(device->CheckFormatSupport(format, &support)) &&
                (support & required) == required;
        };
        // Half precision avoids visible 8-bit transmittance stepping while
        // keeping a 4K mask compact. R32_FLOAT is the guaranteed typed-UAV
        // fallback on feature-level 11 hardware.
        if (s_maskFormatDevice.Get() != device) {
            s_maskFormatDevice = device;
            s_maskFormat = supportsMaskFormat(DXGI_FORMAT_R16_FLOAT)
                ? DXGI_FORMAT_R16_FLOAT
                : (supportsMaskFormat(DXGI_FORMAT_R32_FLOAT)
                    ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN);
        }
        const DXGI_FORMAT maskFormat = s_maskFormat;
        if (maskFormat == DXGI_FORMAT_UNKNOWN)
            return false;

        if (g_cloudShadowTex && g_cloudShadowSRV && g_cloudShadowUAV) {
            D3D11_TEXTURE2D_DESC existing{};
            g_cloudShadowTex->GetDesc(&existing);
            if (existing.Width == width && existing.Height == height &&
                existing.Format == maskFormat) {
                return true;
            }
        }

        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = maskFormat;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        ComPtr<ID3D11Texture2D> newTexture;
        ComPtr<ID3D11ShaderResourceView> newSRV;
        ComPtr<ID3D11UnorderedAccessView> newUAV;
        HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, newTexture.GetAddressOf());
        if (FAILED(hr))
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        hr = device->CreateShaderResourceView(newTexture.Get(), &srvDesc, newSRV.GetAddressOf());
        if (FAILED(hr))
            return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = texDesc.Format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;

        hr = device->CreateUnorderedAccessView(newTexture.Get(), &uavDesc, newUAV.GetAddressOf());
        if (FAILED(hr))
            return false;

        const float clearVal[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        context->ClearUnorderedAccessViewFloat(newUAV.Get(), clearVal);

        if (g_cloudShadowUAV) g_cloudShadowUAV->Release();
        if (g_cloudShadowSRV) g_cloudShadowSRV->Release();
        if (g_cloudShadowTex) g_cloudShadowTex->Release();
        g_cloudShadowTex = newTexture.Detach();
        g_cloudShadowSRV = newSRV.Detach();
        g_cloudShadowUAV = newUAV.Detach();
        ResetScreenMaskEvidence();
        AdvancePrepassBindingResourceGeneration();
        InvalidateShadowMaskState();
        SPDLOG_INFO("[CloudShadows] Screen mask ready: {}x{} {}", width, height,
            maskFormat == DXGI_FORMAT_R16_FLOAT ? "R16_FLOAT" : "R32_FLOAT");
        return true;
    }

    void SetupResources()
    {
        auto* device = GetD3DDevice();
        if (!device) {
            SPDLOG_ERROR("[CloudShadows] D3D device not available");
            return;
        }

        // The mask follows the active DFLight render target.  Create it here
        // when the target already exists, and let Prepass resize it exactly at
        // the first eligible technique otherwise.
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        if (GetOutputDimensions(outputWidth, outputHeight) &&
            !EnsureScreenShadowTexture(outputWidth, outputHeight)) {
            SPDLOG_ERROR("[CloudShadows] Failed to create {}x{} screen mask",
                outputWidth, outputHeight);
            return;
        }

        // Optional diagnostics must never gate or alter production shadows.
        if (!EnsureCloudTelemetryResources(device)) {
            SPDLOG_WARN(
                "[CloudShadows] Physical cloud telemetry unavailable; "
                "shadow rendering remains enabled");
        }

        if (!g_cloudShadowCB) {
            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = sizeof(CloudShadowScreenCBData);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> candidate;
            const HRESULT result = device->CreateBuffer(
                &cbDesc, nullptr, candidate.GetAddressOf());
            if (FAILED(result) || !candidate) {
                SPDLOG_ERROR("[CloudShadows] Failed to create screen constant buffer (0x{:08X})",
                    static_cast<uint32_t>(result));
                return;
            }
            g_cloudShadowCB = candidate.Detach();
        }

        if (!g_cloudShadowProductionCS || !g_cloudShadowCS)
            CompileComputeShader();
        if (!CreateWorldCloudResources()) {
            SPDLOG_ERROR("[CloudShadows] World-cloud resources are not ready");
            return;
        }
        SPDLOG_INFO("[CloudShadows] Standalone world-cloud resources created");
    }

    void InitializeDFLightPatcher()
    {
        static std::mutex initMutex;
        std::lock_guard<std::mutex> lock(initMutex);
        if (g_dfLightPatcherInitialized)
            return;
        auto device = GetD3DDevice();
        if (!device)
            return;
        g_dfLightPatcher.Initialize(device);
        g_dfLightPatcherInitialized.store(
            g_dfLightPatcher.IsInitialized(), std::memory_order_release);
    }

    void ReleaseDeviceResources() noexcept
    {
        g_initialized = false;
        ResetInitializationRetryState();
        InvalidateShadowMaskState();
        ResetScreenMaskEvidence();
        ResetCloudTelemetry();
        s_maskFormatDevice.Reset();
        s_maskFormat = DXGI_FORMAT_UNKNOWN;
        ReleaseWorldCloudResources();
        g_dfLightPatcher.Release();
        g_dfLightPatcherInitialized.store(false, std::memory_order_release);
        FO4CS::GodraysIntegration::ReleaseDeviceResources();
        FO4CS::CloudComparison::ReleaseDeviceResources();

        auto release = [](auto*& object) noexcept {
            if (object) {
                object->Release();
                object = nullptr;
            }
        };
        release(g_cloudShadowUAV);
        release(g_cloudShadowSRV);
        release(g_cloudShadowTex);
        release(g_cloudShadowProductionCS);
        release(g_cloudShadowCS);
        release(g_cloudShadowCB);
        AdvancePrepassBindingResourceGeneration();
        SPDLOG_INFO("[CloudShadows] Released resources for renderer-device transition");
    }

    // F10 toggles cloud shadows on/off for instant A/B comparison. Polled on
    // the render thread like the main project's F11 menu key (BSInputDevice
    // hooks use Skyrim IDs and don't work on FO4).
    std::atomic<bool> g_shadowsEnabled{ true };
    std::atomic<bool> g_hotkeysEnabled{ false };


    namespace
    {
        // Flat/AE uses the bound inverse-view rows and camera position through
        // b12 c35, plus b2 c27. Vanilla VR DFLight instead
        // reads b12 through c60, b2 through c45, and b8 c0 for per-eye inverse
        // reprojection, absolute eye origins, sun direction, depth scaling,
        // and the exact side-by-side NDC transform.
        constexpr UINT kEnginePerFrameRegisterCountFlat = 36;
        constexpr UINT kEnginePerCallRegisterCountFlat = 28;
        constexpr UINT kEnginePerFrameRegisterCountVR = 61;
        constexpr UINT kEnginePerCallRegisterCountVR = 46;
        constexpr UINT kEngineStereoRegisterCountVR = 1;

        UINT RequiredEnginePerFrameRegisterCount() noexcept
        {
            return FO4CS::RuntimeAPI::GetSingleton().Target() ==
                    FO4CS::F4SECompat::RuntimeTarget::kVR
                ? kEnginePerFrameRegisterCountVR
                : kEnginePerFrameRegisterCountFlat;
        }

        UINT RequiredEnginePerCallRegisterCount() noexcept
        {
            return FO4CS::RuntimeAPI::GetSingleton().Target() ==
                    FO4CS::F4SECompat::RuntimeTarget::kVR
                ? kEnginePerCallRegisterCountVR
                : kEnginePerCallRegisterCountFlat;
        }

        enum class PrepassRejectReason : uint32_t
        {
            kResources,
            kDisabled,
            kExteriorFrame,
            kDeferredRenderTargets,
            kOutputExtent,
            kViewport,
            kCommittedField,
            kScreenMask,
            kDepth,
            kConstantBuffers,
            kDepthSampler,
            kConstantBufferMap,
            kBindingSet,
            kDevice,
            kCount
        };

        constexpr uint32_t kPrepassRejectReasonCount =
            static_cast<uint32_t>(PrepassRejectReason::kCount);
        constexpr std::array<const char*, kPrepassRejectReasonCount>
            kPrepassRejectReasonNames{
                "resources", "disabled", "exterior-frame",
                "deferred-render-targets", "output-extent", "viewport",
                "committed-field", "screen-mask", "depth",
                "constant-buffers", "depth-sampler", "constant-buffer-map",
                "binding-set", "device"
            };
        std::array<std::atomic<uint32_t>, kPrepassRejectReasonCount>
            s_prepassRejectCounts{};

        bool RejectPrepass(PrepassRejectReason reason) noexcept
        {
            const uint32_t reasonIndex = static_cast<uint32_t>(reason);
            if (reasonIndex >= kPrepassRejectReasonCount)
                return false;
            const uint32_t ordinal =
                s_prepassRejectCounts[reasonIndex].fetch_add(
                    1, std::memory_order_relaxed) + 1;
            if (ordinal <= 4u || (ordinal & (ordinal - 1u)) == 0u) {
                SPDLOG_WARN(
                    "[CloudShadows] Shadow prepass reject group={} #{} "
                    "layers={} pendingEpoch={} committedEpoch={}",
                    kPrepassRejectReasonNames[reasonIndex], ordinal,
                    g_worldCloudActiveLayers.load(std::memory_order_relaxed),
                    g_worldCloudPendingEpoch.load(std::memory_order_relaxed),
                    g_worldCloudCommittedEpoch.load(std::memory_order_relaxed));
            }
            return false;
        }

        bool IsConstantBufferBindingLargeEnough(
            ID3D11Buffer* buffer,
            UINT firstConstant,
            UINT constantCount,
            UINT minimumConstants,
            bool hasExplicitRange) noexcept
        {
            if (!buffer)
                return false;
            D3D11_BUFFER_DESC description{};
            buffer->GetDesc(&description);
            if ((description.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0)
                return false;
            const UINT availableConstants = description.ByteWidth / 16u;
            if (!hasExplicitRange)
                return availableConstants >= minimumConstants;
            return constantCount >= minimumConstants &&
                firstConstant <= availableConstants &&
                minimumConstants <= availableConstants - firstConstant;
        }

        bool IsFiniteMatrix(const XMFLOAT4X4& value) noexcept
        {
            const auto* values = reinterpret_cast<const float*>(&value);
            for (uint32_t i = 0; i < 16; ++i) {
                if (!std::isfinite(values[i]))
                    return false;
            }
            return true;
        }

        // Keep structured exception handling in a small POD-only function.
        // The renderer singletons can be transient while a save is loading;
        // an invalid engine pointer must make this frame neutral, never crash.
        bool TryReadExteriorFrame(
            XMFLOAT4X4& viewToWorld,
            XMFLOAT3& cameraPosition,
            XMFLOAT3* cameraForward = nullptr) noexcept
        {
#if defined(_MSC_VER)
            __try {
#endif
                auto* sky = FO4CS::EngineAPI::GetSky();
                auto* camera = FO4CS::EngineAPI::GetWorldRootCamera();
                auto* graphicsState = GetGraphicsState();
                auto* player = FO4CS::EngineAPI::GetPlayerCharacter();
                auto* cell = player ? player->parentCell : nullptr;
                if (!FO4CS::EngineAPI::IsExteriorCell(cell)) {
                    RequestWorldCloudReset();
                    return false;
                }
                if (!sky || !camera || !graphicsState ||
                    FO4CS::EngineAPI::ReadSkyMode(sky) !=
                        FO4CS::EngineAPI::SkyMode::kFull) {
                    return false;
                }

                const auto& cameraWorld = camera->world;
                const auto& rotation = cameraWorld.rotate;
                const auto translation =
                    FO4CS::EngineAPI::ReadCameraPosAdjust(graphicsState);
                viewToWorld = {
                    rotation.entry[0].x, rotation.entry[0].y,
                    rotation.entry[0].z, translation.x,
                    rotation.entry[1].x, rotation.entry[1].y,
                    rotation.entry[1].z, translation.y,
                    rotation.entry[2].x, rotation.entry[2].y,
                    rotation.entry[2].z, translation.z,
                    0.0f, 0.0f, 0.0f, 1.0f
                };
                cameraPosition = { translation.x, translation.y, translation.z };
                bool forwardValid = true;
                if (cameraForward) {
                    const auto renderViewDirection =
                        FO4CS::EngineAPI::ReadCameraViewDirection(
                            graphicsState);
                    float x = renderViewDirection.x;
                    float y = renderViewDirection.y;
                    float z = renderViewDirection.z;
                    const float lengthSquared = x * x + y * y + z * z;
                    forwardValid = std::isfinite(lengthSquared) &&
                        lengthSquared > 1.0e-8f;
                    if (forwardValid) {
                        const float inverseLength =
                            1.0f / std::sqrt(lengthSquared);
                        x *= inverseLength;
                        y *= inverseLength;
                        z *= inverseLength;

                        // ViewData carries the render camera's full pitched
                        // direction. Resolve its possible sign convention
                        // against NiCamera's proven yaw direction so looking
                        // up always selects the sky in front of the player.
                        const float yawX = rotation.entry[0].z;
                        const float yawY = rotation.entry[1].z;
                        const float yawZ = rotation.entry[2].z;
                        if (x * yawX + y * yawY + z * yawZ < 0.0f) {
                            x = -x;
                            y = -y;
                            z = -z;
                        }
                        *cameraForward = { x, y, z };
                    }
                }
                return IsFiniteMatrix(viewToWorld) && forwardValid &&
                    std::isfinite(cameraPosition.x) &&
                    std::isfinite(cameraPosition.y) &&
                    std::isfinite(cameraPosition.z);
#if defined(_MSC_VER)
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
#endif
        }

        bool GetRTVExtent(
            ID3D11RenderTargetView* view,
            uint32_t& width,
            uint32_t& height) noexcept
        {
            width = height = 0;
            if (!view)
                return false;
            ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            ComPtr<ID3D11Texture2D> texture;
            if (!resource || FAILED(resource.As(&texture)) || !texture)
                return false;

            D3D11_TEXTURE2D_DESC textureDescription{};
            D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
            texture->GetDesc(&textureDescription);
            view->GetDesc(&viewDescription);
            UINT mip = 0;
            switch (viewDescription.ViewDimension) {
            case D3D11_RTV_DIMENSION_TEXTURE2D:
                mip = viewDescription.Texture2D.MipSlice;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
                mip = viewDescription.Texture2DArray.MipSlice;
                break;
            case D3D11_RTV_DIMENSION_TEXTURE2DMS:
            case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
                mip = 0;
                break;
            default:
                return false;
            }
            width = (std::max)(1u, textureDescription.Width >> mip);
            height = (std::max)(1u, textureDescription.Height >> mip);
            return true;
        }

        bool GetDepthExtent(
            ID3D11ShaderResourceView* view,
            uint32_t& width,
            uint32_t& height) noexcept
        {
            width = height = 0;
            if (!view)
                return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            view->GetDesc(&viewDescription);
            // FO4's main depth is a non-MSAA Texture2D.  The shader declaration
            // is deliberately strict; silently accepting a different view type
            // would cause D3D to null t0 and reuse a stale mask.
            if (viewDescription.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
                return false;

            ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            ComPtr<ID3D11Texture2D> texture;
            if (!resource || FAILED(resource.As(&texture)) || !texture)
                return false;
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            const UINT mip = viewDescription.Texture2D.MostDetailedMip;
            width = (std::max)(1u, description.Width >> mip);
            height = (std::max)(1u, description.Height >> mip);
            return true;
        }

        bool ResourcesAlias(
            ID3D11View* first,
            ID3D11View* second) noexcept
        {
            if (!first || !second)
                return false;
            ComPtr<ID3D11Resource> firstResource;
            ComPtr<ID3D11Resource> secondResource;
            first->GetResource(firstResource.GetAddressOf());
            second->GetResource(secondResource.GetAddressOf());
            return firstResource && firstResource.Get() == secondResource.Get();
        }

        class ScopedPrepassState
        {
        public:
            explicit ScopedPrepassState(ID3D11DeviceContext* a_context) : context(a_context)
            {
                FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::ProjectionSave);
                std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawRTVs{};
                ID3D11DepthStencilView* rawDSV = nullptr;
                context->OMGetRenderTargets(
                    static_cast<UINT>(rawRTVs.size()), rawRTVs.data(), &rawDSV);
                for (uint32_t i = 0; i < rawRTVs.size(); ++i) {
                    renderTargets[i].Attach(rawRTVs[i]);
                    if (rawRTVs[i])
                        renderTargetCount = i + 1;
                }
                depthStencil.Attach(rawDSV);

                std::array<ID3D11ShaderResourceView*, 2> rawSRVs{};
                context->CSGetShaderResources(0, static_cast<UINT>(rawSRVs.size()), rawSRVs.data());
                for (uint32_t i = 0; i < rawSRVs.size(); ++i)
                    shaderResources[i].Attach(rawSRVs[i]);
                std::array<ID3D11UnorderedAccessView*, 3> rawUAVs{};
                context->CSGetUnorderedAccessViews(
                    0, static_cast<UINT>(rawUAVs.size()), rawUAVs.data());
                for (uint32_t i = 0; i < rawUAVs.size(); ++i)
                    unorderedAccess[i].Attach(rawUAVs[i]);
                std::array<ID3D11Buffer*, 4> rawCBs{};
                if (SUCCEEDED(context->QueryInterface(
                        __uuidof(ID3D11DeviceContext1),
                        reinterpret_cast<void**>(context1.GetAddressOf()))) &&
                    context1) {
                    context1->CSGetConstantBuffers1(
                        0, static_cast<UINT>(rawCBs.size()), rawCBs.data(),
                        constantFirst.data(), constantCount.data());
                } else {
                    context->CSGetConstantBuffers(
                        0, static_cast<UINT>(rawCBs.size()), rawCBs.data());
                }
                for (uint32_t i = 0; i < rawCBs.size(); ++i)
                    constantBuffers[i].Attach(rawCBs[i]);
                std::array<ID3D11SamplerState*, 2> rawSamplers{};
                context->CSGetSamplers(
                    0, static_cast<UINT>(rawSamplers.size()), rawSamplers.data());
                for (uint32_t i = 0; i < rawSamplers.size(); ++i)
                    samplers[i].Attach(rawSamplers[i]);
                ID3D11ComputeShader* rawShader = nullptr;
                std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES>
                    rawClassInstances{};
                computeClassInstanceCount =
                    static_cast<UINT>(rawClassInstances.size());
                context->CSGetShader(
                    &rawShader, rawClassInstances.data(),
                    &computeClassInstanceCount);
                computeShader.Attach(rawShader);
                computeClassInstanceCount = (std::min)(
                    computeClassInstanceCount,
                    static_cast<UINT>(computeClassInstances.size()));
                for (UINT i = 0; i < computeClassInstanceCount; ++i)
                    computeClassInstances[i].Attach(rawClassInstances[i]);

                ID3D11ShaderResourceView* rawPS47 = nullptr;
                context->PSGetShaderResources(47, 1, &rawPS47);
                pixelShader47.Attach(rawPS47);

                context->GetPredication(
                    predicate.GetAddressOf(), &predicateValue);
                // Predication applies to Dispatch. The mask is a required
                // side effect of the accepted sunlight draw, so execute it
                // unconditionally and restore the game's predicate afterward.
                context->SetPredication(nullptr, FALSE);
            }

            ScopedPrepassState(const ScopedPrepassState&) = delete;
            ScopedPrepassState& operator=(const ScopedPrepassState&) = delete;

            ~ScopedPrepassState()
            {
                FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::ProjectionRestore);
                std::array<ID3D11ShaderResourceView*, 2> nullSRVs{};
                std::array<ID3D11UnorderedAccessView*, 3> nullUAVs{};
                context->CSSetShaderResources(0, static_cast<UINT>(nullSRVs.size()), nullSRVs.data());
                context->CSSetUnorderedAccessViews(
                    0, static_cast<UINT>(nullUAVs.size()),
                    nullUAVs.data(), nullptr);

                std::array<ID3D11ShaderResourceView*, 2> rawSRVs{
                    shaderResources[0].Get(), shaderResources[1].Get()
                };
                context->CSSetShaderResources(0, static_cast<UINT>(rawSRVs.size()), rawSRVs.data());
                std::array<ID3D11UnorderedAccessView*, 3> rawUAVs{
                    unorderedAccess[0].Get(), unorderedAccess[1].Get(),
                    unorderedAccess[2].Get()
                };
                context->CSSetUnorderedAccessViews(
                    0, static_cast<UINT>(rawUAVs.size()), rawUAVs.data(), nullptr);
                std::array<ID3D11Buffer*, 4> rawCBs{
                    constantBuffers[0].Get(), constantBuffers[1].Get(),
                    constantBuffers[2].Get(), constantBuffers[3].Get()
                };
                if (context1) {
                    context1->CSSetConstantBuffers1(
                        0, static_cast<UINT>(rawCBs.size()), rawCBs.data(),
                        constantFirst.data(), constantCount.data());
                } else {
                    context->CSSetConstantBuffers(
                        0, static_cast<UINT>(rawCBs.size()), rawCBs.data());
                }
                std::array<ID3D11SamplerState*, 2> rawSamplers{
                    samplers[0].Get(), samplers[1].Get()
                };
                context->CSSetSamplers(
                    0, static_cast<UINT>(rawSamplers.size()), rawSamplers.data());
                std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES>
                    rawClassInstances{};
                for (UINT i = 0; i < computeClassInstanceCount; ++i)
                    rawClassInstances[i] = computeClassInstances[i].Get();
                context->CSSetShader(
                    computeShader.Get(), rawClassInstances.data(),
                    computeClassInstanceCount);

                ID3D11ShaderResourceView* rawPS47 = pixelShader47.Get();
                context->PSSetShaderResources(47, 1, &rawPS47);
                if (detachedDepth) {
                    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawRTVs{};
                    for (uint32_t i = 0; i < rawRTVs.size(); ++i)
                        rawRTVs[i] = renderTargets[i].Get();
                    context->OMSetRenderTargetsAndUnorderedAccessViews(
                        renderTargetCount,
                        renderTargetCount ? rawRTVs.data() : nullptr,
                        depthStencil.Get(), renderTargetCount,
                        D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
                }
                context->SetPredication(predicate.Get(), predicateValue);
            }

            ID3D11RenderTargetView* RenderTarget(UINT slot) const noexcept
            {
                return slot < static_cast<UINT>(renderTargets.size())
                    ? renderTargets[slot].Get()
                    : nullptr;
            }

            UINT RenderTargetCount() const noexcept
            {
                return renderTargetCount;
            }

            ID3D11DepthStencilView* DepthStencil() const noexcept
            {
                return depthStencil.Get();
            }

            void DetachDepthForRead() noexcept
            {
                if (detachedDepth)
                    return;
                std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawRTVs{};
                for (uint32_t i = 0; i < rawRTVs.size(); ++i)
                    rawRTVs[i] = renderTargets[i].Get();
                context->OMSetRenderTargetsAndUnorderedAccessViews(
                    renderTargetCount,
                    renderTargetCount ? rawRTVs.data() : nullptr,
                    nullptr, renderTargetCount,
                    D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
                detachedDepth = true;
            }

        private:
            ID3D11DeviceContext* context;
            std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets;
            ComPtr<ID3D11DepthStencilView> depthStencil;
            std::array<ComPtr<ID3D11ShaderResourceView>, 2> shaderResources;
            std::array<ComPtr<ID3D11UnorderedAccessView>, 3> unorderedAccess;
            std::array<ComPtr<ID3D11Buffer>, 4> constantBuffers;
            std::array<UINT, 4> constantFirst{};
            std::array<UINT, 4> constantCount{};
            std::array<ComPtr<ID3D11SamplerState>, 2> samplers;
            ComPtr<ID3D11ComputeShader> computeShader;
            std::array<ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES>
                computeClassInstances;
            UINT computeClassInstanceCount{ 0 };
            ComPtr<ID3D11DeviceContext1> context1;
            ComPtr<ID3D11ShaderResourceView> pixelShader47;
            ComPtr<ID3D11Predicate> predicate;
            BOOL predicateValue{ FALSE };
            UINT renderTargetCount{ 0 };
            bool detachedDepth{ false };
        };
    }

    bool LockSingleCloudToCurrentView() noexcept
    {
        XMFLOAT4X4 viewToWorld{};
        XMFLOAT3 cameraPosition{};
        XMFLOAT3 cameraForward{};
        if (!TryReadExteriorFrame(
                viewToWorld, cameraPosition, &cameraForward)) {
            SPDLOG_WARN(
                "[CloudShadows] Single-cloud selector could not read an "
                "exterior camera frame");
            return false;
        }

        g_singleCloudSelectorX.store(
            cameraForward.x, std::memory_order_relaxed);
        g_singleCloudSelectorY.store(
            cameraForward.y, std::memory_order_relaxed);
        g_singleCloudSelectorZ.store(
            cameraForward.z, std::memory_order_relaxed);
        g_singleCloudIsolationEnabled.store(true, std::memory_order_release);
        SPDLOG_INFO(
            "[CloudShadows] Single-cloud selector locked to view direction "
            "({:.4f}, {:.4f}, {:.4f}) radius={:.1f} degrees",
            cameraForward.x, cameraForward.y, cameraForward.z,
            g_singleCloudRadiusDegrees.load(std::memory_order_relaxed));
        return true;
    }

    void PollShadowToggle() noexcept
    {
        static auto nextSettingsPoll = std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSettingsPoll) {
            nextSettingsPoll = now + std::chrono::seconds(2);
            HotReloadSettingsIfChanged();
        }
        // F10 must remain available while every intercepted Draw call is taking
        // the disabled, near-zero-overhead pass-through path. Present is the
        // authoritative once-per-frame boundary and does not depend on Prepass.
        static SHORT previousF10 = 0;
        if (!g_hotkeysEnabled.load(std::memory_order_relaxed)) {
            previousF10 = 0;
            return;
        }
        if (Overlay::IsExternalHostActive()) {
            previousF10 = 0;
            return;
        }
        DWORD foregroundProcess = 0;
        if (const auto foreground = GetForegroundWindow())
            GetWindowThreadProcessId(foreground, &foregroundProcess);
        if (foregroundProcess != GetCurrentProcessId()) {
            previousF10 = GetAsyncKeyState(VK_F10);
            return;
        }

        const SHORT currentF10 = GetAsyncKeyState(VK_F10);
        const bool pressedSinceLastPoll = (currentF10 & 0x0001) != 0;
        const bool downEdge = (currentF10 & 0x8000) != 0 &&
            (previousF10 & 0x8000) == 0;
        previousF10 = currentF10;
        if (!pressedSinceLastPoll && !downEdge)
            return;

        const bool enabled =
            !g_shadowsEnabled.load(std::memory_order_relaxed);
        g_shadowsEnabled.store(enabled, std::memory_order_release);
        // Never reuse a field captured before the master-toggle boundary.
        // Enabling waits for a newly captured visible sky.
        InvalidateWorldCloudCaptureForToggle();
        SPDLOG_INFO(
            "[CloudShadows] F10 toggle: cloud shadows {}",
            enabled ? "ON" : "OFF");
        FO4CS::ManualFpsLog::OnToggle(enabled);
        if (auto* logger = spdlog::default_logger_raw())
            logger->flush();
    }

    bool Prepass()
    {
        FO4CS::CpuProfile::Scope cpuTotal(FO4CS::CpuProfile::Stage::ProjectionTotal);
        FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::ProjectionValidation);
        g_shadowMaskValid.store(false, std::memory_order_release);
        auto* context = GetD3DContext();
        PollScreenMaskEvidence(context);
        PollCloudTelemetry(context);
        if (!context || !g_cloudShadowProductionCS || !g_cloudShadowCS ||
            !g_cloudShadowCB ||
            !g_worldCloudReady.load(std::memory_order_acquire)) {
            return RejectPrepass(PrepassRejectReason::kResources);
        }
        static std::atomic<uint32_t> successfulPrepasses{ 0 };

        if (!g_shadowsEnabled.load(std::memory_order_relaxed))
            return RejectPrepass(PrepassRejectReason::kDisabled);

        XMFLOAT4X4 viewToWorld{};
        XMFLOAT3 cameraPosition{};
        if (!TryReadExteriorFrame(viewToWorld, cameraPosition)) {
            return RejectPrepass(PrepassRejectReason::kExteriorFrame);
        }

        FO4CS::CloudComparison::GpuScope projectionTiming(context,
            FO4CS::CloudComparison::Work::Projection);
        ScopedPrepassState savedState(context);
        auto* accumulationTarget0 = savedState.RenderTarget(0);
        auto* accumulationTarget1 = savedState.RenderTarget(1);
        const UINT renderTargetCount = savedState.RenderTargetCount();
        uint32_t outputWidth = 0;
        uint32_t outputHeight = 0;
        uint32_t accumulation1Width = 0;
        uint32_t accumulation1Height = 0;
        if ((renderTargetCount != 1 && renderTargetCount != 2) ||
            !IsMainDeferredLightingTargetSet(
                accumulationTarget0, accumulationTarget1,
                renderTargetCount) ||
            !GetRTVExtent(
                accumulationTarget0, outputWidth, outputHeight) ||
            (renderTargetCount == 2 &&
                !GetRTVExtent(
                    accumulationTarget1, accumulation1Width,
                    accumulation1Height))) {
            LogDeferredLightingTargetRejection(
                accumulationTarget0, accumulationTarget1,
                renderTargetCount);
            return RejectPrepass(
                PrepassRejectReason::kDeferredRenderTargets);
        }
        const bool accumulationExtentsMatch =
            renderTargetCount != 2 ||
            (accumulation1Width == outputWidth &&
                accumulation1Height == outputHeight);
        uint32_t primaryWidth = 0;
        uint32_t primaryHeight = 0;
        const bool primaryExtentMatches =
            GetOutputDimensions(primaryWidth, primaryHeight) &&
            outputWidth == primaryWidth &&
            outputHeight == primaryHeight;
        if (!primaryExtentMatches || !accumulationExtentsMatch) {
            return RejectPrepass(PrepassRejectReason::kOutputExtent);
        }

        UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        context->RSGetViewports(&viewportCount, viewports.data());
        const bool stereoRuntime =
            FO4CS::RuntimeAPI::GetSingleton().Target() ==
            FO4CS::F4SECompat::RuntimeTarget::kVR;
        const bool validContainedViewport =
            viewportCount == 1 && viewports[0].Width > 0.0f &&
            viewports[0].Height > 0.0f &&
            viewports[0].TopLeftX >= 0.0f &&
            viewports[0].TopLeftY >= 0.0f &&
            viewports[0].TopLeftX + viewports[0].Width <=
                static_cast<float>(outputWidth) + 0.5f &&
            viewports[0].TopLeftY + viewports[0].Height <=
                static_cast<float>(outputHeight) + 0.5f;
        // Fallout4VR's stock stereo shaders split clip-space X into two halves
        // using SV_InstanceID and draw both eyes through one full side-by-side
        // viewport. A partial/mirror viewport has no valid eye split for the
        // mask compute shader and must remain neutral.
        const bool validStereoViewport =
            !stereoRuntime ||
            ((outputWidth & 1u) == 0u &&
             viewports[0].TopLeftX == 0.0f &&
             viewports[0].TopLeftY == 0.0f &&
             viewports[0].Width == static_cast<float>(outputWidth) &&
             viewports[0].Height == static_cast<float>(outputHeight));
        if (!validContainedViewport || !validStereoViewport) {
            return RejectPrepass(PrepassRejectReason::kViewport);
        }

        // Publication is a persistent frame-state mutation. Perform it only
        // after the exact runtime-specific deferred-light target set and
        // contained viewport are proven; reflection and secondary sunlight
        // draws remain read-only.
        CommitWorldCloudFrame(context);
        if (!EnsureScreenShadowTexture(outputWidth, outputHeight))
            return RejectPrepass(PrepassRejectReason::kScreenMask);

        ComPtr<ID3D11ShaderResourceView> depthSRV;
        context->PSGetShaderResources(3, 1, depthSRV.GetAddressOf());
        uint32_t depthWidth = 0;
        uint32_t depthHeight = 0;
        const bool exactMainDepth =
            IsMainDepthShaderResourceView(depthSRV.Get());
        const bool hasDepthExtent =
            GetDepthExtent(depthSRV.Get(), depthWidth, depthHeight);
        const bool validStereoDepthExtent =
            !stereoRuntime ||
            (depthWidth == outputWidth && depthHeight == outputHeight);
        if (!depthSRV || !exactMainDepth || !hasDepthExtent ||
            !validStereoDepthExtent ||
            viewports[0].TopLeftX + viewports[0].Width > static_cast<float>(depthWidth) + 0.5f ||
            viewports[0].TopLeftY + viewports[0].Height > static_cast<float>(depthHeight) + 0.5f) {
            LogMainDepthRejection(depthSRV.Get(), viewports[0]);
            return RejectPrepass(PrepassRejectReason::kDepth);
        }

        ComPtr<ID3D11Buffer> enginePerFrame;
        ComPtr<ID3D11Buffer> enginePerCall;
        ComPtr<ID3D11Buffer> engineStereoParams;
        UINT enginePerFrameFirstConstant = 0;
        UINT enginePerFrameConstantCount = 0;
        UINT enginePerCallFirstConstant = 0;
        UINT enginePerCallConstantCount = 0;
        UINT engineStereoFirstConstant = 0;
        UINT engineStereoConstantCount = 0;
        ComPtr<ID3D11DeviceContext1> activeContext1;
        const bool hasConstantBufferRanges = SUCCEEDED(context->QueryInterface(
            __uuidof(ID3D11DeviceContext1),
            reinterpret_cast<void**>(activeContext1.GetAddressOf()))) &&
            activeContext1;
        if (hasConstantBufferRanges) {
            activeContext1->PSGetConstantBuffers1(
                12, 1, enginePerFrame.GetAddressOf(),
                &enginePerFrameFirstConstant, &enginePerFrameConstantCount);
            activeContext1->PSGetConstantBuffers1(
                2, 1, enginePerCall.GetAddressOf(),
                &enginePerCallFirstConstant, &enginePerCallConstantCount);
            if (stereoRuntime) {
                activeContext1->PSGetConstantBuffers1(
                    8, 1, engineStereoParams.GetAddressOf(),
                    &engineStereoFirstConstant, &engineStereoConstantCount);
            }
        } else {
            context->PSGetConstantBuffers(12, 1, enginePerFrame.GetAddressOf());
            context->PSGetConstantBuffers(2, 1, enginePerCall.GetAddressOf());
            if (stereoRuntime) {
                context->PSGetConstantBuffers(
                    8, 1, engineStereoParams.GetAddressOf());
            }
        }
        if (!IsConstantBufferBindingLargeEnough(
                enginePerFrame.Get(), enginePerFrameFirstConstant,
                enginePerFrameConstantCount,
                RequiredEnginePerFrameRegisterCount(),
                hasConstantBufferRanges) ||
            !IsConstantBufferBindingLargeEnough(
                enginePerCall.Get(), enginePerCallFirstConstant,
                enginePerCallConstantCount,
                RequiredEnginePerCallRegisterCount(),
                hasConstantBufferRanges) ||
            (stereoRuntime &&
                !IsConstantBufferBindingLargeEnough(
                    engineStereoParams.Get(), engineStereoFirstConstant,
                    engineStereoConstantCount,
                    kEngineStereoRegisterCountVR,
                    hasConstantBufferRanges))) {
            return RejectPrepass(PrepassRejectReason::kConstantBuffers);
        }

        // Mirror the exact depth sampling contract of the consuming DFLight
        // shader. It samples t3 with the native s3 and explicit gradients;
        // substituting a Load would diverge if the engine sampler ever changes.
        ComPtr<ID3D11SamplerState> engineDepthSampler;
        context->PSGetSamplers(3, 1, engineDepthSampler.GetAddressOf());
        if (!engineDepthSampler)
            return RejectPrepass(PrepassRejectReason::kDepthSampler);

        // The CPU GraphicsState camera may still belong to loading/secondary
        // rendering even at this authenticated DFLight draw. Establish the
        // retained origin from its exact GPU b12 instead, without waiting for
        // the readback. Until it completes, keep sunlight neutral.
        if (!ConfirmWorldCloudOrigin(context, enginePerFrame.Get(),
                enginePerFrameFirstConstant, stereoRuntime))
            return RejectPrepass(PrepassRejectReason::kCommittedField);
        std::array<WorldCloudLayerState, kMaxWorldCloudLayers> layers{};
        const uint32_t layerCount = CopyCommittedWorldCloudLayers(
            layers.data(), static_cast<uint32_t>(layers.size()));
        XMFLOAT3 committedCaptureOrigin{};
        const bool hasCommittedCaptureOrigin =
            GetCommittedWorldCloudOrigin(committedCaptureOrigin);
        const uint64_t dispatchEpoch =
            g_worldCloudCommittedEpoch.load(std::memory_order_acquire);
        auto* worldTiles = GetCommittedWorldCloudTiles();
        auto* worldSampler = GetWorldCloudSampler();
        if (!worldTiles || !worldSampler ||
            (layerCount != 0 && !hasCommittedCaptureOrigin) ||
            (layerCount == 0 && g_settings.DebugMode < 0.5f)) {
            return RejectPrepass(PrepassRejectReason::kCommittedField);
        }

        if (auto* depthStencil = savedState.DepthStencil();
            depthStencil && ResourcesAlias(depthSRV.Get(), depthStencil)) {
            D3D11_DEPTH_STENCIL_VIEW_DESC description{};
            depthStencil->GetDesc(&description);
            if ((description.Flags & D3D11_DSV_READ_ONLY_DEPTH) == 0)
                savedState.DetachDepthForRead();
        }

        CloudShadowScreenCBData constants{};
        constants.ViewToWorld = viewToWorld;
        constants.OutputSizeAndInvSize = {
            static_cast<float>(outputWidth), static_cast<float>(outputHeight),
            1.0f / static_cast<float>(outputWidth),
            1.0f / static_cast<float>(outputHeight)
        };
        constants.OutputPixelToDepthUV = {
            1.0f / static_cast<float>(depthWidth),
            1.0f / static_cast<float>(depthHeight), 0.0f, 0.0f
        };
        const auto& viewport = viewports[0];
        constants.OutputPixelToNDC = {
            2.0f / viewport.Width,
            -2.0f / viewport.Height,
            -1.0f - (2.0f * viewport.TopLeftX / viewport.Width),
            1.0f + (2.0f * viewport.TopLeftY / viewport.Height)
        };
        // Isolation is a capture-analysis diagnostic, never a production
        // coverage filter. Debug Off must always include every visible cloud.
        const bool captureAnalysis =
            g_settings.DebugMode > 3.5f && g_settings.DebugMode < 4.5f;
        const bool isolateSingleCloud = captureAnalysis &&
            g_singleCloudIsolationEnabled.load(std::memory_order_acquire);
        const float isolationRadiusDegrees = std::clamp(
            g_singleCloudRadiusDegrees.load(std::memory_order_relaxed),
            2.0f, 30.0f);
        const float isolationInnerCos = std::cos(
            XMConvertToRadians(isolationRadiusDegrees));
        const float isolationOuterCos = std::cos(
            XMConvertToRadians((std::min)(
                isolationRadiusDegrees + 2.0f, 32.0f)));
        constants.SunDirectionAndAngularRadius = {
            isolateSingleCloud
                ? g_singleCloudSelectorX.load(std::memory_order_relaxed)
                : 0.0f,
            isolateSingleCloud
                ? g_singleCloudSelectorY.load(std::memory_order_relaxed)
                : 0.0f,
            isolateSingleCloud
                ? g_singleCloudSelectorZ.load(std::memory_order_relaxed)
                : 0.0f,
            g_settings.SunAngularRadius
        };
        XMFLOAT3 visibleSun{};
        const bool visibleSunValid = FO4CS::EngineAPI::ReadVisibleSunDirection(
            FO4CS::EngineAPI::GetSky(), visibleSun);
        constants.VisibleSunDirectionAndValidity = {
            visibleSun.x, visibleSun.y, visibleSun.z, visibleSunValid ? 1.0f : -1.0f };
        // Snapshot the request before dispatch. A later request waits for the
        // next prepass instead of copying a validity surface this dispatch did
        // not write. This also makes the costly full-resolution u2 stores
        // strictly explicit-request-only.
        const bool telemetryCanArm = !s_cloudTelemetry.pending &&
            s_cloudTelemetry.gpuUAV && s_cloudTelemetry.staging &&
            s_cloudTelemetry.query;
        // A screen readback is useful to acceptance only when physical
        // telemetry is emitted by this exact dispatch. If an older 128-byte
        // telemetry copy is still in flight, defer the screen request rather
        // than producing an uncorrelatable full-resolution readback.
        const uint64_t screenMaskEvidenceRequestId = telemetryCanArm
            ? GetArmableScreenMaskEvidenceRequest()
            : 0;
        ID3D11UnorderedAccessView* receiverValidityOutput =
            screenMaskEvidenceRequestId != 0
                ? s_screenMaskEvidence.receiverValidityUAV.Get()
                : nullptr;
        const uint32_t prospectivePrepassOrdinal =
            successfulPrepasses.load(std::memory_order_relaxed) + 1u;
        const bool periodicTelemetryRequested =
            ((FO4CS::BuildFeatures::kDeveloperTools && Overlay::IsVisible()) || AcceptanceRunner::IsRunning()) &&
            prospectivePrepassOrdinal % kCloudTelemetryReadbackInterval == 0u;
        const bool cloudTelemetryRequested = telemetryCanArm &&
            (screenMaskEvidenceRequestId != 0 || periodicTelemetryRequested);
        ID3D11UnorderedAccessView* cloudTelemetryOutput =
            cloudTelemetryRequested ? s_cloudTelemetry.gpuUAV.Get() : nullptr;
        const bool useDiagnosticShader =
            g_settings.DebugMode != 0.0f ||
            screenMaskEvidenceRequestId != 0 || cloudTelemetryRequested;
        ID3D11ComputeShader* dispatchShader = useDiagnosticShader
            ? g_cloudShadowCS
            : g_cloudShadowProductionCS;
        if (FO4CS::CloudComparison::EffectiveMethod() == FO4CS::CloudComparison::Method::SunMask) {
            if (!GetCommittedSunMaskProjection(constants.SunProjection))
                return RejectPrepass(PrepassRejectReason::kCommittedField);
            dispatchShader = FO4CS::CloudComparison::SunShader(useDiagnosticShader);
            if (!dispatchShader) return RejectPrepass(PrepassRejectReason::kResources);
        }
        constants.ShadowParams = {
            g_settings.Opacity, 1.0f,
            screenMaskEvidenceRequestId != 0 ? 1.0f : 0.0f,
            cloudTelemetryRequested ? 1.0f : 0.0f
        };
        constants.ModelParams = {
            static_cast<float>(layerCount),
            isolateSingleCloud ? isolationOuterCos : -2.0f,
            isolateSingleCloud ? isolationInnerCos : -2.0f,
            g_settings.DebugMode
        };
        constants.DiagnosticReceiverAndRadius = {
            cameraPosition.x, cameraPosition.y, cameraPosition.z,
            kCloudTelemetryRadiusWorldUnits
        };
        for (uint32_t i = 0; i < kMaxWorldCloudLayers; ++i) {
            constants.LayerGeometry[i] = { 0.0f, 1.0f, 0.0f, -1.0f };
            constants.LayerOptics[i] = {};
        }
		// Fallout exposes visual Sky draws, not independently measured physical
		// altitudes. Project the composited visible field on the one configured
		// cloud shell; deriving height from draw identity made one receiver sample
		// unrelated sky directions and union them into a uniform mask.
		constexpr float planetRadiusWorld = 6371000.0f * 70.0f;
		for (uint32_t i = 0; i < layerCount; ++i) {
			const auto& layer = layers[i];
			if (layer.SliceIndex >= kMaxWorldCloudLayers)
				continue;
			const float shellHeight = constants.SunProjection.centerAndValid.w == 1.0f
                ? constants.SunProjection.upAndHeight.w : g_settings.CloudHeight;
			constants.LayerGeometry[i] = {
				shellHeight, planetRadiusWorld,
				g_settings.WorldTileSize,
				static_cast<float>(layer.SliceIndex)
			};
			constants.LayerOptics[i] = {
				committedCaptureOrigin.x, committedCaptureOrigin.y,
				committedCaptureOrigin.z,
				std::clamp(layer.ActiveBlend, 0.0f, 1.0f)
			};
		}

        cpuTiming.Set(FO4CS::CpuProfile::Stage::ProjectionBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(
                g_cloudShadowCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) ||
            !mapped.pData) {
            return RejectPrepass(PrepassRejectReason::kConstantBufferMap);
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(g_cloudShadowCB, 0);

        // t47 may still contain this mask after a third-party hook. Unbind it
        // while the texture is a UAV; ScopedPrepassState restores it exactly.
        cpuTiming.Set(FO4CS::CpuProfile::Stage::ProjectionBind);
        ID3D11ShaderResourceView* nullPS47 = nullptr;
        context->PSSetShaderResources(47, 1, &nullPS47);
        std::array<ID3D11ShaderResourceView*, 2> shaderResources{
            depthSRV.Get(), worldTiles
        };
        context->CSSetShaderResources(
            0, static_cast<UINT>(shaderResources.size()), shaderResources.data());
        std::array<ID3D11UnorderedAccessView*, 3> outputs{
            g_cloudShadowUAV, cloudTelemetryOutput,
            receiverValidityOutput
        };
        context->CSSetUnorderedAccessViews(
            0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
        ID3D11Buffer* cloudShadowConstants = g_cloudShadowCB;
        context->CSSetConstantBuffers(0, 1, &cloudShadowConstants);
        std::array<ID3D11Buffer*, 3> engineConstantBuffers{
            enginePerFrame.Get(), enginePerCall.Get(),
            engineStereoParams.Get()
        };
        const UINT engineConstantBufferCount = stereoRuntime ? 3u : 2u;
        if (hasConstantBufferRanges) {
            const std::array<UINT, 3> firstConstants{
                enginePerFrameFirstConstant, enginePerCallFirstConstant,
                engineStereoFirstConstant
            };
            const std::array<UINT, 3> constantCounts{
                enginePerFrameConstantCount, enginePerCallConstantCount,
                engineStereoConstantCount
            };
            activeContext1->CSSetConstantBuffers1(
                1, engineConstantBufferCount,
                engineConstantBuffers.data(), firstConstants.data(),
                constantCounts.data());
        } else {
            context->CSSetConstantBuffers(
                1, engineConstantBufferCount,
                engineConstantBuffers.data());
        }
        std::array<ID3D11SamplerState*, 2> samplers{
            worldSampler, engineDepthSampler.Get()
        };
        context->CSSetSamplers(
            0, static_cast<UINT>(samplers.size()), samplers.data());
        context->CSSetShader(dispatchShader, nullptr, 0);

        // D3D11 resolves illegal read/write hazards by silently nulling a
        // binding. Prove each distinct production/diagnostic binding set once
        // while it remains unchanged. The committed cube SRV alternates when a
        // new field is published, and engine depth/constant-buffer objects or
        // ranges may also change without recreating our owned resources, so a
        // variant bit alone is not a sufficient cache key.
        cpuTiming.Set(FO4CS::CpuProfile::Stage::ProjectionValidation);
        struct BindingVerificationKey
        {
            std::array<const void*, 12> objects{};
            std::array<UINT, 6> ranges{};
            UINT engineConstantBufferCount{ 0 };
            bool hasConstantBufferRanges{ false };
        };
        thread_local uint64_t verifiedResourceGeneration = 0;
        thread_local uint8_t verifiedBindingVariants = 0;
        thread_local std::array<BindingVerificationKey, 8>
            verifiedBindingKeys{};
        const uint64_t resourceGeneration =
            s_prepassBindingResourceGeneration.load(std::memory_order_acquire);
        if (verifiedResourceGeneration != resourceGeneration) {
            verifiedResourceGeneration = resourceGeneration;
            verifiedBindingVariants = 0;
        }
        const uint32_t bindingVariant =
            (cloudTelemetryOutput ? 1u : 0u) |
            (receiverValidityOutput ? 2u : 0u) |
            (useDiagnosticShader ? 4u : 0u);
        const uint8_t bindingVariantBit = static_cast<uint8_t>(
            1u << bindingVariant);
        const BindingVerificationKey bindingKey{
            {
                depthSRV.Get(), worldTiles,
                g_cloudShadowUAV, cloudTelemetryOutput,
                receiverValidityOutput,
                g_cloudShadowCB, enginePerFrame.Get(), enginePerCall.Get(),
                engineStereoParams.Get(), worldSampler,
                engineDepthSampler.Get(), dispatchShader
            },
            {
                enginePerFrameFirstConstant, enginePerFrameConstantCount,
                enginePerCallFirstConstant, enginePerCallConstantCount,
                engineStereoFirstConstant, engineStereoConstantCount
            },
            engineConstantBufferCount,
            hasConstantBufferRanges
        };
        const auto& verifiedBindingKey = verifiedBindingKeys[bindingVariant];
        const bool sameBindingKey =
            (verifiedBindingVariants & bindingVariantBit) != 0u &&
            verifiedBindingKey.objects == bindingKey.objects &&
            verifiedBindingKey.ranges == bindingKey.ranges &&
            verifiedBindingKey.engineConstantBufferCount ==
                bindingKey.engineConstantBufferCount &&
            verifiedBindingKey.hasConstantBufferRanges ==
                bindingKey.hasConstantBufferRanges;
        if (!sameBindingKey) {
            std::array<ComPtr<ID3D11ShaderResourceView>, 2> verifiedSRVs;
            std::array<ID3D11ShaderResourceView*, 2> rawVerifiedSRVs{};
            context->CSGetShaderResources(
                0, static_cast<UINT>(rawVerifiedSRVs.size()),
                rawVerifiedSRVs.data());
            for (uint32_t i = 0; i < rawVerifiedSRVs.size(); ++i)
                verifiedSRVs[i].Attach(rawVerifiedSRVs[i]);
            std::array<ComPtr<ID3D11UnorderedAccessView>, 3> verifiedUAVs;
            std::array<ID3D11UnorderedAccessView*, 3> rawVerifiedUAVs{};
            context->CSGetUnorderedAccessViews(
                0, static_cast<UINT>(rawVerifiedUAVs.size()),
                rawVerifiedUAVs.data());
            for (uint32_t i = 0; i < rawVerifiedUAVs.size(); ++i)
                verifiedUAVs[i].Attach(rawVerifiedUAVs[i]);
            std::array<ComPtr<ID3D11Buffer>, 4> verifiedCBs;
            std::array<ID3D11Buffer*, 4> rawVerifiedCBs{};
            context->CSGetConstantBuffers(
                0, engineConstantBufferCount + 1u, rawVerifiedCBs.data());
            for (uint32_t i = 0; i < engineConstantBufferCount + 1u; ++i)
                verifiedCBs[i].Attach(rawVerifiedCBs[i]);
            std::array<UINT, 3> verifiedFirstConstants{};
            std::array<UINT, 3> verifiedConstantCounts{};
            if (hasConstantBufferRanges) {
                std::array<ID3D11Buffer*, 3> rawRangeBuffers{};
                activeContext1->CSGetConstantBuffers1(
                    1, engineConstantBufferCount,
                    rawRangeBuffers.data(), verifiedFirstConstants.data(),
                    verifiedConstantCounts.data());
                for (uint32_t i = 0; i < engineConstantBufferCount; ++i) {
                    if (rawRangeBuffers[i])
                        rawRangeBuffers[i]->Release();
                }
            }
            std::array<ComPtr<ID3D11SamplerState>, 2> verifiedSamplers;
            std::array<ID3D11SamplerState*, 2> rawVerifiedSamplers{};
            context->CSGetSamplers(
                0, static_cast<UINT>(rawVerifiedSamplers.size()),
                rawVerifiedSamplers.data());
            for (uint32_t i = 0; i < rawVerifiedSamplers.size(); ++i)
                verifiedSamplers[i].Attach(rawVerifiedSamplers[i]);
            ComPtr<ID3D11ComputeShader> verifiedShader;
            ID3D11ComputeShader* rawVerifiedShader = nullptr;
            context->CSGetShader(&rawVerifiedShader, nullptr, nullptr);
            verifiedShader.Attach(rawVerifiedShader);
            // Third-party filtering overrides (observed on a 1.11.240 install)
            // substitute sampler objects at Set time, so slot 0 can read back
            // as a different object. Accept a substitute that is a plain
            // non-comparison sampler; the cube lookup only needs filtering.
            bool worldSamplerAccepted = verifiedSamplers[0].Get() == worldSampler;
            if (!worldSamplerAccepted && verifiedSamplers[0] && worldSampler) {
                D3D11_SAMPLER_DESC actual{};
                D3D11_SAMPLER_DESC expected{};
                verifiedSamplers[0]->GetDesc(&actual);
                worldSampler->GetDesc(&expected);
                worldSamplerAccepted =
                    actual.ComparisonFunc == expected.ComparisonFunc &&
                    actual.MaxLOD >= expected.MaxLOD &&
                    actual.MinLOD <= expected.MinLOD;
                static std::atomic<bool> loggedSubstitute{ false };
                if (worldSamplerAccepted && !loggedSubstitute.exchange(true))
                    SPDLOG_WARN(
                        "[CloudShadows] Cube sampler {} was substituted by {} at bind time "
                        "(filter={} addr={}/{}/{}); accepted as an external filtering override",
                        static_cast<void*>(worldSampler), static_cast<void*>(verifiedSamplers[0].Get()),
                        static_cast<int>(actual.Filter), static_cast<int>(actual.AddressU),
                        static_cast<int>(actual.AddressV), static_cast<int>(actual.AddressW));
            }
            if (verifiedSRVs[0].Get() != depthSRV.Get() ||
                verifiedSRVs[1].Get() != worldTiles ||
                verifiedUAVs[0].Get() != g_cloudShadowUAV ||
                verifiedUAVs[1].Get() != cloudTelemetryOutput ||
                verifiedUAVs[2].Get() != receiverValidityOutput ||
                verifiedCBs[0].Get() != g_cloudShadowCB ||
                verifiedCBs[1].Get() != enginePerFrame.Get() ||
                verifiedCBs[2].Get() != enginePerCall.Get() ||
                (stereoRuntime &&
                    verifiedCBs[3].Get() != engineStereoParams.Get()) ||
                (hasConstantBufferRanges &&
                    (verifiedFirstConstants[0] !=
                         enginePerFrameFirstConstant ||
                     verifiedFirstConstants[1] !=
                         enginePerCallFirstConstant ||
                     verifiedConstantCounts[0] !=
                         enginePerFrameConstantCount ||
                     verifiedConstantCounts[1] !=
                         enginePerCallConstantCount ||
                     (stereoRuntime &&
                        (verifiedFirstConstants[2] !=
                             engineStereoFirstConstant ||
                         verifiedConstantCounts[2] !=
                             engineStereoConstantCount)))) ||
                !worldSamplerAccepted ||
                verifiedSamplers[1].Get() != engineDepthSampler.Get() ||
                verifiedShader.Get() != dispatchShader) {
                static std::atomic<uint32_t> bindingRejectLogs{ 0 };
                if (bindingRejectLogs.fetch_add(1) < 4) {
                    SPDLOG_ERROR(
                        "[CloudShadows] D3D rejected the validated prepass "
                        "binding set; frame left neutral. srv0 {}/{} srv1 {}/{} "
                        "uav0 {}/{} uav1 {}/{} uav2 {}/{} cb0 {}/{} cb1 {}/{} cb2 {}/{} cb3 {}/{} "
                        "ranges={} first {},{},{} / {},{},{} count {},{},{} / {},{},{} "
                        "smp0 {}/{} smp1 {}/{} cs {}/{} stereo={}",
                        static_cast<void*>(verifiedSRVs[0].Get()), static_cast<void*>(depthSRV.Get()),
                        static_cast<void*>(verifiedSRVs[1].Get()), static_cast<void*>(worldTiles),
                        static_cast<void*>(verifiedUAVs[0].Get()), static_cast<void*>(g_cloudShadowUAV),
                        static_cast<void*>(verifiedUAVs[1].Get()), static_cast<void*>(cloudTelemetryOutput),
                        static_cast<void*>(verifiedUAVs[2].Get()), static_cast<void*>(receiverValidityOutput),
                        static_cast<void*>(verifiedCBs[0].Get()), static_cast<void*>(g_cloudShadowCB),
                        static_cast<void*>(verifiedCBs[1].Get()), static_cast<void*>(enginePerFrame.Get()),
                        static_cast<void*>(verifiedCBs[2].Get()), static_cast<void*>(enginePerCall.Get()),
                        static_cast<void*>(verifiedCBs[3].Get()), static_cast<void*>(engineStereoParams.Get()),
                        hasConstantBufferRanges,
                        verifiedFirstConstants[0], verifiedFirstConstants[1], verifiedFirstConstants[2],
                        enginePerFrameFirstConstant, enginePerCallFirstConstant, engineStereoFirstConstant,
                        verifiedConstantCounts[0], verifiedConstantCounts[1], verifiedConstantCounts[2],
                        enginePerFrameConstantCount, enginePerCallConstantCount, engineStereoConstantCount,
                        static_cast<void*>(verifiedSamplers[0].Get()), static_cast<void*>(worldSampler),
                        static_cast<void*>(verifiedSamplers[1].Get()), static_cast<void*>(engineDepthSampler.Get()),
                        static_cast<void*>(verifiedShader.Get()), static_cast<void*>(dispatchShader),
                        stereoRuntime);
                }
                return RejectPrepass(PrepassRejectReason::kBindingSet);
            }
            verifiedBindingKeys[bindingVariant] = bindingKey;
            verifiedBindingVariants |= bindingVariantBit;
        }

        cpuTiming.Set(FO4CS::CpuProfile::Stage::ProjectionDispatch);
        context->Dispatch((outputWidth + 7u) / 8u, (outputHeight + 7u) / 8u, 1);
        cpuTiming.Set(FO4CS::CpuProfile::Stage::ProjectionValidation);
        const uint32_t successfulPrepassOrdinal = successfulPrepasses.fetch_add(
            1, std::memory_order_relaxed) + 1;
        ArmScreenMaskEvidence(
            context, successfulPrepassOrdinal, dispatchEpoch,
            screenMaskEvidenceRequestId);
        if (cloudTelemetryRequested) {
            ArmCloudTelemetry(
                context, successfulPrepassOrdinal, dispatchEpoch,
                screenMaskEvidenceRequestId != 0);
        }
        g_projectionAnchorX.store(committedCaptureOrigin.x, std::memory_order_relaxed);
        g_projectionAnchorY.store(committedCaptureOrigin.y, std::memory_order_relaxed);
        g_projectionAnchorZ.store(committedCaptureOrigin.z, std::memory_order_relaxed);
        // +1 reserves zero for "no successful dispatch" even when debug mode
        // dispatches against committed epoch zero. The stamp is an OR latch,
        // not a post-Present generation-equality requirement.
        const uint64_t encodedEpoch = dispatchEpoch + 1u;
        g_shadowMaskSuccessStamp.store(
            encodedEpoch != 0u ? encodedEpoch : 1u,
            std::memory_order_release);
        g_shadowMaskValid.store(true, std::memory_order_release);
        {
            if (successfulPrepassOrdinal <= 4u ||
                successfulPrepassOrdinal == 100u ||
                (successfulPrepassOrdinal % 5000u) == 0u) {
                SPDLOG_INFO(
                    "[CloudShadows] Shadow prepass dispatch #{} layers={} epoch={} "
                    "output={}x{} depth={}x{}",
                    successfulPrepassOrdinal, layerCount, dispatchEpoch, outputWidth,
                    outputHeight, depthWidth, depthHeight);
            }
        }
        if (FO4CS::CloudComparison::GetPreview() != FO4CS::CloudComparison::Preview::Off) {
            // Match the immutable field consumed by this shadow pass. The next
            // sky capture has not begun yet and its write cube is unavailable.
            FO4CS::CloudComparison::RenderPreview(context, constants, worldTiles);
        }
        return true;
    }

    void Initialize()
    {
        if (g_initialized)
            return;

        // BeginTechnique can fire tens of thousands of times while the renderer
        // is constructing targets. A persistent deployment failure must never
        // repeat shader hashing, resource setup, and synchronous logging on the
        // render thread every few hundred milliseconds.
        const auto now = std::chrono::steady_clock::now();
        if (now < s_initializationRetry.nextAttempt)
            return;

        auto device = GetD3DDevice();
        if (!device)
            return;  // called again from Present hook once device is ready

        SPDLOG_INFO("[CloudShadows] Initializing...");
        LoadSettings(false);
        SetupResources();
        InitializeDFLightPatcher();

        if (g_cloudShadowProductionCS && g_cloudShadowCS && g_cloudShadowCB &&
            g_worldCloudReady.load(std::memory_order_acquire) &&
            g_dfLightPatcherInitialized.load(std::memory_order_acquire)) {
            g_initialized = true;
            ResetInitializationRetryState();
            SPDLOG_INFO("[CloudShadows] Initialization complete");
        } else {
            const auto retryDelay = InitializationRetryDelay(
                s_initializationRetry.consecutiveFailures);
            ++s_initializationRetry.consecutiveFailures;
            s_initializationRetry.nextAttempt = now + retryDelay;
            SPDLOG_WARN("[CloudShadows] Initialization incomplete (productionCS={}, diagnosticCS={}, CB={}, world={}, patcher={}); retrying in {} ms",
                (void*)g_cloudShadowProductionCS, (void*)g_cloudShadowCS,
                (void*)g_cloudShadowCB,
                g_worldCloudReady.load(std::memory_order_relaxed),
                g_dfLightPatcherInitialized.load(std::memory_order_relaxed),
                retryDelay.count());
        }
    }
}
