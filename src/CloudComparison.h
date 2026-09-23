// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d11.h>
#include <chrono>
#include <cstdint>
#include <string>

namespace CloudShadows { struct CloudShadowScreenCBData; }

namespace FO4CS::CloudComparison
{
    enum class Method : unsigned { Cubemap, SunMask };
    enum class Preview : unsigned { Off, SkyOverlay, RawMask };
    Method GetMethod() noexcept;
    // The saved preference may name the Sun 2D method while its shaders are
    // unavailable on this device; producers and the prepass use this value.
    Method EffectiveMethod() noexcept;
    Preview GetPreview() noexcept;
    const char* MethodName() noexcept;
    void SetMethod(Method method) noexcept;
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
    bool SunMethodAvailable() noexcept;

    bool CompileShaders(ID3D11Device* device, const std::string& source,
        const std::string& name, bool vr);
    ID3D11ComputeShader* SunShader(bool diagnostic) noexcept;
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
