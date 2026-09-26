// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d11.h>
#include <chrono>
#include <cstdint>
#include <string>

namespace CloudShadows { struct CloudShadowScreenCBData; }

namespace FO4CS::CloudComparison
{
    enum class Preview : unsigned { Off, SkyOverlay, RawMask };
    Preview GetPreview() noexcept;
    void SetPreview(Preview preview) noexcept;
    void PollControls() noexcept;
    void ArmMeasurements() noexcept;
    void StopMeasurements() noexcept;
    // Private profiler can measure timestamps without keyboard input or a HUD.
    void SetProfilingMeasurements(bool enabled) noexcept;
    void ResetTimings() noexcept;
    void SetStatus(std::string status);
    std::string Status();
    bool HudVisible() noexcept;
    // Release on-screen feedback for F10: shows the HUD for a few seconds.
    void FlashStatus(std::string status, double seconds) noexcept;

    // Records the authenticated screen-shader source for this device. The
    // sky-preview variant compiles on first use (MaintainShaders), so players
    // who never open it pay no startup compile.
    bool CompileShaders(ID3D11Device* device, const std::string& source,
        const std::string& name, bool vr);
    // Render thread, once per Present: compiles an active preview's shader
    // the first time it is needed on a device.
    void MaintainShaders(ID3D11Device* device) noexcept;
    // Uses the exact CS b1/b2/b3 bindings established by the accepted DFLight
    // prepass and the same completed cube as its ground-shadow lookup. Caller
    // restores CS state; no Present-time camera read or stall.
    bool RenderPreview(ID3D11DeviceContext* context,
        const CloudShadows::CloudShadowScreenCBData& constants,
        ID3D11ShaderResourceView* committedCube) noexcept;
    ID3D11ShaderResourceView* PreviewImage() noexcept;
    void RetirePreview() noexcept;
    void ReleaseDeviceResources() noexcept;

    enum class Work { Capture, Projection };
    struct Timings
    {
        std::uint64_t frames{}, dropped{};
        double captureMs{}, projectionMs{}, cpuMs{}, draws{};
    };
    Timings GetTimings() noexcept;
    void EndGpuFrame(ID3D11DeviceContext* context, bool eligible) noexcept;
    class GpuScope
    {
    public:
        GpuScope(ID3D11DeviceContext* context, Work work, unsigned draws = 0) noexcept;
        ~GpuScope();
        GpuScope(const GpuScope&) = delete;
        GpuScope& operator=(const GpuScope&) = delete;
    private:
        ID3D11DeviceContext* context_{};
        unsigned frame_{}, pair_{};
        std::chrono::steady_clock::time_point start_{};
    };
}
