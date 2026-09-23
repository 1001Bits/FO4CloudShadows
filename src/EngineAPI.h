#pragma once

#include "RuntimeAPI.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <DirectXMath.h>
#include <d3d11.h>

namespace FO4CS::EngineAPI
{
    namespace detail
    {
        template <class TValue>
        [[nodiscard]] inline TValue ReadField(
            const void* base,
            std::size_t offset = 0) noexcept
        {
            TValue value{};
            if (!base)
                return value;
            std::memcpy(
                &value,
                static_cast<const std::byte*>(base) + offset,
                sizeof(value));
            return value;
        }
    }

    enum class SkyMode : std::uint32_t
    {
        kNone = 0,
        kInterior = 1,
        kSkyDomeOnly = 2,
        kFull = 3
    };

    struct RenderTarget
    {
        [[nodiscard]] ID3D11Texture2D* Texture() const noexcept
        {
            return detail::ReadField<ID3D11Texture2D*>(this, 0x00);
        }

        [[nodiscard]] ID3D11RenderTargetView* TargetView() const noexcept
        {
            return detail::ReadField<ID3D11RenderTargetView*>(this, 0x10);
        }

        std::byte opaque[0x30]{};
    };
    static_assert(sizeof(RenderTarget) == 0x30);

    struct DepthStencilTarget
    {
        [[nodiscard]] ID3D11ShaderResourceView* DepthShaderResourceView()
            const noexcept
        {
            return detail::ReadField<ID3D11ShaderResourceView*>(this, 0x88);
        }

        std::byte opaque[0x98]{};
    };
    static_assert(sizeof(DepthStencilTarget) == 0x98);

    // Fallout4.exe and Fallout4VR.exe share this D3D11 renderer-window ABI.
    // Only the swap chain is consumed here; the surrounding bytes preserve the
    // reviewed 0x50-byte element layout without importing CommonLib types.
    struct RendererWindow
    {
        std::byte opaque[0x18]{};
        IDXGISwapChain* swapChain{};
        RenderTarget swapChainRenderTarget{};
    };
    static_assert(offsetof(RendererWindow, swapChain) == 0x18);
    static_assert(sizeof(RendererWindow) == 0x50);

    struct RendererData
    {
        std::byte opaque[0x48]{};
        ID3D11Device* device{};
        ID3D11DeviceContext* context{};
        std::array<RendererWindow, 32> renderWindow{};

        // The common prefix ends here. VR has 145 render targets and starts
        // depth at +0x2588; flat has 101 and starts depth at +0x1D48.
        [[nodiscard]] const RenderTarget* RenderTargetAt(uint32_t index) const noexcept;
        [[nodiscard]] const DepthStencilTarget* DepthTargetAt(uint32_t index) const noexcept;
    };
    static_assert(offsetof(RendererData, device) == 0x48);
    static_assert(offsetof(RendererData, context) == 0x50);
    static_assert(offsetof(RendererData, renderWindow) == 0x58);
    static_assert(sizeof(RendererData) == 0x0A58);

    struct alignas(16) Matrix3
    {
        DirectX::XMFLOAT4 entry[3]{};
    };
    static_assert(sizeof(Matrix3) == 0x30);

    struct alignas(16) Transform
    {
        Matrix3 rotate{};
        DirectX::XMFLOAT3 translate{};
        float scale{ 1.0f };
    };
    static_assert(sizeof(Transform) == 0x40);

    struct Camera
    {
        std::byte opaque[0x70]{};
        Transform world{};
    };
    static_assert(offsetof(Camera, world) == 0x70);

    struct alignas(16) State
    {
        std::byte opaque[0x5C0]{};
    };
    static_assert(sizeof(State) == 0x5C0);
    static_assert(alignof(State) == 16);

    struct Sky
    {
        std::byte opaque[0x36C]{};
        std::uint32_t mode{};
    };
    static_assert(offsetof(Sky, mode) == 0x36C);

    struct TESObjectCELL
    {
        std::byte opaque[0x40]{};
        std::uint16_t cellFlags{};
    };
    static_assert(offsetof(TESObjectCELL, cellFlags) == 0x40);

    struct PlayerCharacter
    {
        std::byte opaque[0x0B8]{};
        TESObjectCELL* parentCell{};
    };
    static_assert(offsetof(PlayerCharacter, parentCell) == 0x0B8);

    struct TES
    {
        std::byte opaque[0x0D8]{};
        void* worldSpace{};
    };
    static_assert(offsetof(TES, worldSpace) == 0x0D8);

    [[nodiscard]] RendererData* GetRendererData() noexcept;
    [[nodiscard]] uint32_t PhysicalRenderTargetCount() noexcept;
    [[nodiscard]] uint32_t PhysicalDepthTargetCount() noexcept;
    [[nodiscard]] State* GetGraphicsState() noexcept;
    [[nodiscard]] std::byte* GetRenderTargetManager() noexcept;
    [[nodiscard]] Sky* GetSky() noexcept;
    [[nodiscard]] Camera* GetWorldRootCamera() noexcept;
    [[nodiscard]] PlayerCharacter* GetPlayerCharacter() noexcept;
    [[nodiscard]] TES* GetTES() noexcept;
    [[nodiscard]] std::uintptr_t GetBeginTechniqueAddress() noexcept;
    [[nodiscard]] std::uintptr_t GetVRSetShadersAddress() noexcept;

    [[nodiscard]] DirectX::XMFLOAT3 ReadCameraPosAdjust(
        const State* state) noexcept;
    [[nodiscard]] DirectX::XMFLOAT3 ReadCameraPreviousPosAdjust(
        const State* state) noexcept;
    [[nodiscard]] DirectX::XMFLOAT4 ReadCameraViewDirection(
        const State* state) noexcept;
    [[nodiscard]] SkyMode ReadSkyMode(const Sky* sky) noexcept;
    [[nodiscard]] bool ReadVisibleSunDirection(
        const Sky* sky, DirectX::XMFLOAT3& direction) noexcept;
    [[nodiscard]] bool IsExteriorCell(const TESObjectCELL* cell) noexcept;
    // Requests the engine's own "CursorMenu" (UIMessageQueue kShow/kHide), the
    // vanilla cursor mode used by the Pip-Boy. False when unsupported (VR).
    bool ShowGameCursorMenu(bool show) noexcept;
}
