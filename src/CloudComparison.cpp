// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "CloudComparison.h"
#include "CloudShadows.h"
#include "ManualFpsLog.h"
#include "Overlay.h"
#include "PrivateProfile.h"
#include "CpuStageProfiler.h"
#include <cstring>
#include <mutex>

namespace FO4CS::CloudComparison
{
    namespace
    {
        std::atomic<Preview> preview{ Preview::Off };
        std::atomic<bool> hud{}, measuring{};
        std::atomic<double> hudDeadline{ 0.0 };
        double Now() noexcept
        {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        std::mutex statusMutex;
        std::string status = BuildFeatures::kDeveloperTools
            ? "F10: shadows on/off | F7: stop measurement"
            : "F10: shadows on/off | F8: sky preview | F7: hide";
        // Device objects are never destroyed by a DLL static destructor: at
        // process exit that would run under the loader lock.
        struct DeviceObjects
        {
            ComPtr<ID3D11ComputeShader> previewShader;
            ComPtr<ID3D11Texture2D> previewTexture;
            ComPtr<ID3D11ShaderResourceView> previewSrv;
            ComPtr<ID3D11UnorderedAccessView> previewUav;
            ComPtr<ID3D11Buffer> previewCB;
            // Recorded screen-shader source for the lazily compiled variants.
            ComPtr<ID3D11Device> shaderDevice;
            std::string shaderSource;
            std::string shaderName;
            bool shaderVr{};
            bool previewAttempted{};
        };
        DeviceObjects& objects = *new DeviceObjects();
        auto& previewShader = objects.previewShader;
        auto& previewTexture = objects.previewTexture;
        auto& previewSrv = objects.previewSrv;
        auto& previewUav = objects.previewUav;
        auto& previewCB = objects.previewCB;
        UINT previewWidth{}, previewHeight{};
        bool previewReady{};

        bool CompileScreenEntry(const char* entry,
            ComPtr<ID3D11ComputeShader>& shader)
        {
            const D3D_SHADER_MACRO macros[]{ {"FO4CS_SHADER_VR", objects.shaderVr ? "1" : "0"}, {} };
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(D3DCompile(objects.shaderSource.data(), objects.shaderSource.size(),
                    objects.shaderName.c_str(), macros, nullptr,
                    entry, "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS |
                    D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                    0, &code, &errors))) {
                SPDLOG_ERROR("[CloudShadows][Compare] {} compile: {}", entry,
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "failed");
                return false;
            }
            return SUCCEEDED(objects.shaderDevice->CreateComputeShader(code->GetBufferPointer(),
                code->GetBufferSize(), nullptr, &shader));
        }

        // Bounded, asynchronous timestamps. Never Flush, wait, or map a
        // staging resource on the render thread. A full ring drops samples.
        constexpr unsigned kFrames = 8, kPairs = 96;
        struct Pair {
            ComPtr<ID3D11Query> first, last;
            Work work{};
        };
        struct Frame {
            ComPtr<ID3D11Query> disjoint;
            std::array<Pair, kPairs> pairs;
            unsigned count{}, draws{};
            double cpuMs{};
            std::uint64_t generation{};
            bool pending{}, eligible{}, overflow{};
        };
        std::array<Frame, kFrames>& frames = *new std::array<Frame, kFrames>();
        ComPtr<ID3D11Device>& timingDevice = *new ComPtr<ID3D11Device>();
        unsigned current = kFrames, next{};
        bool droppedCurrent{};
        std::uint64_t generation = 1;
        Timings totals;

        bool IsForeground() noexcept
        {
            DWORD process{};
            if (auto window = GetForegroundWindow()) GetWindowThreadProcessId(window, &process);
            return process == GetCurrentProcessId();
        }

        bool EnsureTimingFrame(ID3D11DeviceContext* context)
        {
            if (current != kFrames) return true;
            if (frames[next].pending) {
                if (!droppedCurrent) ++totals.dropped;
                droppedCurrent = true;
                return false;
            }
            ComPtr<ID3D11Device> device;
            context->GetDevice(&device);
            if (timingDevice && timingDevice != device) return false;
            timingDevice = device;
            auto& frame = frames[next];
            if (!frame.disjoint) {
                const D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
                if (FAILED(device->CreateQuery(&desc, &frame.disjoint))) return false;
            }
            frame.count = frame.draws = 0;
            frame.cpuMs = 0;
            frame.overflow = false;
            frame.generation = generation;
            current = next;
            context->Begin(frame.disjoint.Get());
            return true;
        }
    }

    Preview GetPreview() noexcept { return preview.load(std::memory_order_acquire); }
    bool HudVisible() noexcept
    {
        if (!hud.load(std::memory_order_acquire)) return false;
        const double deadline = hudDeadline.load(std::memory_order_acquire);
        if (deadline > 0.0 && Now() >= deadline) {
            hudDeadline.store(0.0, std::memory_order_release);
            hud.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }
    void FlashStatus(std::string text, double seconds) noexcept
    {
        SetStatus(std::move(text));
        // An active F8 preview owns the HUD; never arm an expiry that would
        // hide the preview while it keeps rendering.
        if (GetPreview() != Preview::Off) {
            hud.store(true, std::memory_order_release);
            return;
        }
        hudDeadline.store(Now() + seconds, std::memory_order_release);
        hud.store(true, std::memory_order_release);
    }
    void SetStatus(std::string text) { std::lock_guard lock(statusMutex); status = std::move(text); }
    std::string Status() { std::lock_guard lock(statusMutex); return status; }
    void ResetTimings() noexcept { ++generation; totals = {}; }
    void SetProfilingMeasurements(bool enabled) noexcept
    {
        ManualFpsLog::Stop();
        hud.store(false, std::memory_order_release);
        preview.store(Preview::Off, std::memory_order_release);
        measuring.store(enabled, std::memory_order_release);
        ResetTimings();
    }
    void ArmMeasurements() noexcept
    {
        hudDeadline.store(0.0, std::memory_order_release);
        hud.store(true, std::memory_order_release);
        measuring.store(BuildFeatures::kExperimental, std::memory_order_release);
        ResetTimings();
    }

    void StopMeasurements() noexcept
    {
        ManualFpsLog::Stop();
        measuring.store(false, std::memory_order_release);
        hud.store(false, std::memory_order_release);
        preview.store(Preview::Off, std::memory_order_release);
        RetirePreview();
        ResetTimings();
        SPDLOG_INFO("[CloudShadows][Compare] Measurement and preview stopped; F10 starts a new comparison");
    }

    void SetPreview(Preview value) noexcept
    {
        if (value != Preview::Off && value != Preview::SkyOverlay && value != Preview::RawMask) return;
        preview.store(value, std::memory_order_release);
        previewReady = false;
        // Release: leaving the preview shows the status briefly instead of a
        // permanent HUD. Developer builds keep the open-ended comparison HUD.
        hudDeadline.store(value == Preview::Off && !BuildFeatures::kDeveloperTools
            ? Now() + 3.0 : 0.0, std::memory_order_release);
        hud.store(true, std::memory_order_release);
        ResetTimings();
        SPDLOG_INFO("[CloudShadows][Compare] Sky alignment={} (performance windows excluded while active)",
            value == Preview::Off ? "OFF" : value == Preview::SkyOverlay ? "magenta overlay" : "raw opacity");
    }

    void PollControls() noexcept
    {
        static bool previous7{}, previous8{};
        if (!CloudShadows::g_hotkeysEnabled.load(std::memory_order_relaxed)) {
            previous7 = previous8 = false;
            return;
        }
        const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (!Overlay::IsExternalHostActive() && IsForeground()) {
            if (f7 && !previous7) StopMeasurements();
            if (f8 && !previous8)
                SetPreview(static_cast<Preview>((static_cast<unsigned>(GetPreview()) + 1) % 3));
        }
        previous7 = f7; previous8 = f8;
    }

    bool CompileShaders(ID3D11Device* device, const std::string& source,
        const std::string& name, bool vr)
    {
        if (!device)
            return false;
        objects.shaderDevice = device;
        objects.shaderSource = source;
        objects.shaderName = name;
        objects.shaderVr = vr;
        objects.previewAttempted = false;
        previewShader.Reset();
        previewReady = false;
        return true;
    }

    void MaintainShaders(ID3D11Device* device) noexcept
    {
        try {
            if (!device || device != objects.shaderDevice.Get() || objects.shaderSource.empty())
                return;
            if (GetPreview() != Preview::Off && !objects.previewAttempted) {
                objects.previewAttempted = true;
                ComPtr<ID3D11ComputeShader> sky;
                // A failed optional preview must never disable the ground shadows.
                if (CompileScreenEntry("mainSkyPreview", sky))
                    previewShader = std::move(sky);
                else
                    SPDLOG_WARN("[CloudShadows][Compare] Sky preview unavailable; ground shadows remain enabled");
            }
        } catch (...) {
            SPDLOG_ERROR("[CloudShadows][Compare] Optional shader preparation failed");
        }
    }

    bool RenderPreview(ID3D11DeviceContext* context,
        const CloudShadows::CloudShadowScreenCBData& constants,
        ID3D11ShaderResourceView* cube) noexcept
    {
        previewReady = false;
        if (GetPreview() == Preview::Off || !cube || !previewShader)
            return false;
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        const auto width = static_cast<UINT>(constants.OutputSizeAndInvSize.x);
        const auto height = static_cast<UINT>(constants.OutputSizeAndInvSize.y);
        if (!previewTexture || width != previewWidth || height != previewHeight) {
            previewSrv.Reset(); previewUav.Reset(); previewTexture.Reset();
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &previewTexture)) ||
                FAILED(device->CreateShaderResourceView(previewTexture.Get(), nullptr, &previewSrv)) ||
                FAILED(device->CreateUnorderedAccessView(previewTexture.Get(), nullptr, &previewUav))) {
                previewTexture.Reset(); return false;
            }
            previewWidth = width; previewHeight = height;
        }
        if (!previewCB) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(constants); desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&desc, nullptr, &previewCB))) return false;
        }
        auto data = constants;
        data.ModelParams.w = static_cast<float>(GetPreview());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(previewCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        std::memcpy(mapped.pData, &data, sizeof(data)); context->Unmap(previewCB.Get(), 0);
        ID3D11Buffer* buffer = previewCB.Get();
        ID3D11UnorderedAccessView* outputs[]{ previewUav.Get(), nullptr, nullptr };
        context->CSSetConstantBuffers(0, 1, &buffer);
        context->CSSetShaderResources(1, 1, &cube);
        context->CSSetUnorderedAccessViews(0, 3, outputs, nullptr);
        context->CSSetShader(previewShader.Get(), nullptr, 0);
        context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        previewReady = true;
        return true;
    }

    ID3D11ShaderResourceView* PreviewImage() noexcept { return previewReady ? previewSrv.Get() : nullptr; }
    void RetirePreview() noexcept { previewReady = false; }

    GpuScope::GpuScope(ID3D11DeviceContext* context, Work work, unsigned draws) noexcept
    {
        CpuProfile::Scope queryTiming(CpuProfile::Stage::GpuQuery);
        if (!context || !measuring.load(std::memory_order_relaxed) ||
            GetPreview() != Preview::Off || Overlay::IsVisible()) return;
        if (!EnsureTimingFrame(context)) return;
        auto& frame = frames[current];
        if (frame.count == kPairs) { frame.overflow = true; return; }
        auto& pair = frame.pairs[frame.count];
        if (!pair.first || !pair.last) {
            const D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
            if (FAILED(timingDevice->CreateQuery(&desc, &pair.first)) ||
                FAILED(timingDevice->CreateQuery(&desc, &pair.last))) { frame.overflow = true; return; }
        }
        frame_ = current; pair_ = frame.count++; context_ = context;
        frame.draws += draws; pair.work = work;
        context->End(pair.first.Get()); start_ = std::chrono::steady_clock::now();
    }

    GpuScope::~GpuScope()
    {
        CpuProfile::Scope queryTiming(CpuProfile::Stage::GpuQuery);
        if (!context_) return;
        auto& frame = frames[frame_];
        frame.cpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
        context_->End(frame.pairs[pair_].last.Get());
    }

    void EndGpuFrame(ID3D11DeviceContext* context, bool eligible) noexcept
    {
        CpuProfile::Scope queryTiming(CpuProfile::Stage::GpuQuery);
        droppedCurrent = false;
        if (!context || !timingDevice) return;
        if (current != kFrames) {
            auto& frame = frames[current];
            context->End(frame.disjoint.Get()); frame.pending = true;
            if (frame.overflow && eligible && frame.generation == generation) ++totals.dropped;
            frame.eligible = eligible && !frame.overflow && frame.count != 0 &&
                GetPreview() == Preview::Off && !Overlay::IsVisible() &&
                (IsForeground() || PrivateProfile::Active());
            next = (current + 1) % kFrames; current = kFrames;
        }
        for (auto& frame : frames) {
            if (!frame.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            const auto hr = context->GetData(frame.disjoint.Get(), &disjoint, sizeof(disjoint),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) continue;
            if (!frame.eligible || frame.generation != generation) { frame.pending = false; continue; }
            if (FAILED(hr) || disjoint.Disjoint || !disjoint.Frequency) {
                ++totals.dropped; frame.pending = false; continue;
            }
            double capture{}, projection{};
            bool complete = true;
            for (unsigned i = 0; i < frame.count; ++i) {
                UINT64 first{}, last{};
                const auto a = context->GetData(frame.pairs[i].first.Get(), &first, sizeof(first), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                const auto b = context->GetData(frame.pairs[i].last.Get(), &last, sizeof(last), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (a == S_FALSE || b == S_FALSE) { complete = false; break; }
                if (FAILED(a) || FAILED(b) || last < first) { frame.eligible = false; break; }
                const double ms = static_cast<double>(last - first) * 1000.0 / static_cast<double>(disjoint.Frequency);
                (frame.pairs[i].work == Work::Capture ? capture : projection) += ms;
            }
            if (!complete) continue;
            frame.pending = false;
            if (!frame.eligible) { ++totals.dropped; continue; }
            ++totals.frames; totals.captureMs += capture; totals.projectionMs += projection;
            totals.cpuMs += frame.cpuMs; totals.draws += frame.draws;
        }
    }

    Timings GetTimings() noexcept
    {
        auto result = totals;
        if (result.frames) {
            const auto n = static_cast<double>(result.frames);
            result.captureMs /= n; result.projectionMs /= n; result.cpuMs /= n; result.draws /= n;
        }
        return result;
    }

    void ReleaseDeviceResources() noexcept
    {
        objects.shaderDevice.Reset();
        objects.shaderSource.clear();
        objects.shaderName.clear();
        objects.previewAttempted = false;
        previewShader.Reset();
        previewTexture.Reset(); previewSrv.Reset(); previewUav.Reset(); previewCB.Reset();
        previewReady = false; previewWidth = previewHeight = 0;
        frames = {}; timingDevice.Reset(); current = kFrames; next = 0;
        droppedCurrent = false;
        ResetTimings();
    }
}
