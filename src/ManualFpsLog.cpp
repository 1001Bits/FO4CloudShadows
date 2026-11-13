#include "PCH.h"
#include "ManualFpsLog.h"
#include "CloudShadows.h"
#include "Overlay.h"
#include "CloudComparison.h"
#include "AcceptanceRunner.h"

namespace FO4CS::ManualFpsLog
{
    namespace
    {
        // Present owns the session. Other callbacks read only atomic gates.
        FpsTestSession& session = *new FpsTestSession();
        std::atomic<bool> active{}, automatic{}, originalEnabled{}, worldObserved{};
        bool showing{}, haveAnchor{};
        DWORD ownerThread{};
        IDXGISwapChain* presentationSource{};
        const char* message = "Stand still outdoors, with the same view and all game menus closed.";
        struct Scene { void* world{}; EngineAPI::Transform camera{}; };
        Scene anchor;
        float originalOpacity{}, originalHeight{};

        bool ReadScene(Scene& scene) noexcept
        {
            __try {
                const auto* player = EngineAPI::GetPlayerCharacter();
                const auto* sky = EngineAPI::GetSky();
                const auto* tes = EngineAPI::GetTES();
                const auto* camera = EngineAPI::GetWorldRootCamera();
                if (!player || !EngineAPI::IsExteriorCell(player->parentCell) || !sky ||
                    EngineAPI::ReadSkyMode(sky) != EngineAPI::SkyMode::kFull ||
                    !(CloudShadows::DaylightFactor() > 0.0f) ||
                    !tes || !tes->worldSpace || !camera) return false;
                scene.world = tes->worldSpace; scene.camera = camera->world;
                const auto& p = scene.camera.translate;
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
                for (const auto& row : scene.camera.rotate.entry)
                    if (!std::isfinite(row.x) || !std::isfinite(row.y) || !std::isfinite(row.z)) return false;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool SameView(const Scene& first, const Scene& second) noexcept
        {
            const auto& a = first.camera.translate; const auto& b = second.camera.translate;
            const float position = (a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y) + (a.z-b.z)*(a.z-b.z);
            float rotation{};
            for (std::size_t i = 0; i < 3; ++i) {
                const auto& x = first.camera.rotate.entry[i]; const auto& y = second.camera.rotate.entry[i];
                rotation += (x.x-y.x)*(x.x-y.x) + (x.y-y.y)*(x.y-y.y) + (x.z-y.z)*(x.z-y.z);
            }
            return first.world == second.world && position <= 64.0f && rotation <= 0.0012f;
        }
        void Apply(bool enabled) noexcept
        {
            if (CloudShadows::g_shadowsEnabled.exchange(enabled, std::memory_order_acq_rel) != enabled)
                CloudShadows::InvalidateWorldCloudCaptureForToggle();
        }
        void Flush() noexcept
        {
            try { if (auto* logger = spdlog::default_logger_raw()) logger->flush(); } catch (...) {}
        }
        void Cancel(const char* reason) noexcept
        {
            const bool wasAutomatic = session.Automatic();
            if (const auto restore = session.Cancel()) Apply(*restore);
            active.store(false, std::memory_order_release);
            automatic.store(false, std::memory_order_release);
            worldObserved.store(false, std::memory_order_release);
            message = reason;
            try { SPDLOG_INFO("[CloudShadows][FPS] {}; restoredOriginalSetting={}", reason, wasAutomatic); } catch (...) {}
            Flush();
        }
    }

    bool IsActive() noexcept { return active.load(std::memory_order_relaxed); }
    bool IsAutomatic() noexcept { return automatic.load(std::memory_order_acquire); }
    bool Showing() noexcept { return showing; }
    bool EnabledForSave(bool live) noexcept
    { return IsAutomatic() ? originalEnabled.load(std::memory_order_relaxed) : live; }
    FpsTestSession::Snapshot Snapshot() noexcept { return session.GetSnapshot(); }
    const char* Message() noexcept { return message; }
    bool NeedsWorldObservation() noexcept
    { return IsActive() && !worldObserved.load(std::memory_order_relaxed); }
    void ObserveWorldPass() noexcept { worldObserved.store(true, std::memory_order_relaxed); }

    void Start(bool runAutomatic) noexcept
    {
        if (RuntimeAPI::GetSingleton().Target() == F4SECompat::RuntimeTarget::kVR ||
            !CloudShadows::g_hotkeysEnabled.load(std::memory_order_acquire)) return;
        if (session.Active()) Stop();
        const bool enabled = CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire);
        originalEnabled.store(enabled, std::memory_order_relaxed);
        session.Start(runAutomatic, enabled);
        originalOpacity = CloudShadows::g_settings.Opacity;
        originalHeight = CloudShadows::g_settings.CloudHeight;
        haveAnchor = false; showing = true; ownerThread = GetCurrentThreadId(); presentationSource = nullptr;
        worldObserved.store(false, std::memory_order_relaxed);
        automatic.store(runAutomatic, std::memory_order_release);
        active.store(true, std::memory_order_release);
        message = runAutomatic ? "Keep the same view. Original shadow setting is restored when the test ends." :
            "F10 switches modes. Keep the same view; each toggle starts a fresh average for that mode.";
        try {
            SPDLOG_INFO("[CloudShadows][FPS] {} started; application Present FPS, including frame caps and stalls. "
                "Close menus and stand still outdoors. Automatic sequence OFF/ON/ON/OFF: 5s countdown, "
                "3s settle + 10s measurement per step. CPU/GPU execution time is not measured.",
                runAutomatic ? "Automatic test" : "Manual comparison");
        } catch (...) {}
        Flush();
    }
    void Stop() noexcept { if (session.Active()) Cancel("FPS test stopped"); }
    void HideResults() noexcept { Stop(); showing = false; }
    void OnToggle(bool enabled) noexcept
    {
        // F10 is an explicit user choice, so retain it when cancelling a test.
        if (IsAutomatic()) {
            try { SPDLOG_INFO("[CloudShadows][FPS] Automatic test cancelled by F10; user's toggle retained"); } catch (...) {}
            (void)session.Cancel();
            automatic.store(false, std::memory_order_release);
            active.store(false, std::memory_order_release);
        }
        if (!session.Active()) { Start(false); if (!IsActive()) return; }
        session.ToggleManual(enabled);
    }

    void AfterPresentUnchecked(IDXGISwapChain* source, bool frameEnabled,
        bool succeeded, bool maskValid, bool lightingApplied, bool vsync)
    {
        if (!IsActive()) return;
        if (GetCurrentThreadId() != ownerThread) { Cancel("FPS test cancelled: render thread changed"); return; }
        if (presentationSource && presentationSource != source) { Cancel("FPS test cancelled: output changed"); return; }
        presentationSource = source;
        if (!CloudShadows::g_hotkeysEnabled.load(std::memory_order_acquire) || Overlay::IsExternalHostActive()) {
            Cancel("FPS test cancelled: Development menu disabled or externally hosted"); return;
        }
        if (CloudShadows::g_settings.Opacity != originalOpacity || CloudShadows::g_settings.CloudHeight != originalHeight) {
            Cancel("FPS test cancelled: shadow settings changed"); return;
        }
        if (IsAutomatic() && CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire) != session.GetSnapshot().enabled) {
            Cancel("FPS test cancelled: shadow state changed outside the test"); return;
        }
        DWORD foregroundProcess{};
        if (const auto foreground = GetForegroundWindow()) GetWindowThreadProcessId(foreground, &foregroundProcess);
        Scene scene;
        const bool rendered = worldObserved.exchange(false, std::memory_order_acq_rel);
        const bool exterior = ReadScene(scene);
        bool eligible = succeeded && rendered && exterior && foregroundProcess == GetCurrentProcessId() &&
            !Overlay::IsVisible() && CloudShadows::g_settings.DebugMode == 0.0f &&
            CloudComparison::GetPreview() == CloudComparison::Preview::Off &&
            !CloudShadows::AcceptanceRunner::IsRunning() &&
            !CloudShadows::g_singleCloudIsolationEnabled.load(std::memory_order_relaxed) &&
            frameEnabled == CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire);
        if (eligible && !session.GetSnapshot().countdown) {
            if (!haveAnchor) { anchor = scene; haveAnchor = true; }
            else if (!SameView(anchor, scene)) {
                if (IsAutomatic()) { Cancel("FPS test cancelled: camera, exterior or daylight changed"); return; }
                Start(false); anchor = scene; haveAnchor = true;
                message = "View changed; both averages were reset. Stand still to compare ON and OFF.";
                eligible = false;
            }
        }
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        UINT presentCount{};
        const bool counted = source && SUCCEEDED(source->GetLastPresentCount(&presentCount));
        const auto update = session.Observe({ now, 1, reinterpret_cast<std::uintptr_t>(source), frameEnabled,
            eligible, maskValid, lightingApplied, vsync, presentCount, counted,
            static_cast<double>(GetTickCount64()) * 0.001 });
        if (update.setEnabled) Apply(*update.setEnabled);
        if (update.finished) {
            automatic.store(false, std::memory_order_release);
            active.store(false, std::memory_order_release);
        }
        if (update.result) {
            const auto& r = update.window;
            SPDLOG_INFO("[CloudShadows][FPS] mode={} step={} shadows={} avg={:.2f} FPS mean={:.3f} ms "
                "p95={:.1f} ms p99={:.1f} ms max={:.3f} ms frames={} seconds={:.3f} maskFrames={} "
                "lightingFrames={} vsyncFrames={} counterMatches={} clockMatches={} sampleValid={}",
                session.GetSnapshot().mode == FpsTestSession::Mode::Manual ? "manual" : "automatic",
                update.completedStep + 1, frameEnabled ? "ON" : "OFF", r.FPS(), r.Milliseconds(),
                r.p95Ms, r.p99Ms, r.maxMs, r.frames, r.seconds, r.maskFrames, r.lightingFrames,
                r.vsyncFrames, r.countersOK, r.clocksOK, r.Verified(frameEnabled));
            Flush();
        }
        if (update.finished) {
            const auto& s = session.GetSnapshot();
            const auto& off = s.averages[0]; const auto& on = s.averages[1];
            const bool valid = off.Verified(false) && on.Verified(true) && s.driftOK;
            message = valid ? "Test complete. Original shadow setting restored; results saved to FO4CloudShadows.log." :
                "Comparison inconclusive: check sample validation and baseline drift below. Original setting restored.";
            SPDLOG_INFO("[CloudShadows][FPS] COMPLETE offFPS={:.2f} onFPS={:.2f} deltaFPS={:+.2f} "
                "deltaFrameMs={:+.3f} offMs={:.3f} onMs={:.3f} baselineDriftOK={} comparisonValid={} "
                "restoredEnabled={}; application frame time, not isolated CPU or GPU cost",
                off.FPS(), on.FPS(), on.FPS() - off.FPS(), on.Milliseconds() - off.Milliseconds(),
                off.Milliseconds(), on.Milliseconds(), s.driftOK, valid, session.OriginalEnabled());
            Flush();
        }
    }
    void AfterPresent(IDXGISwapChain* source, bool frameEnabled,
        bool succeeded, bool maskValid, bool lightingApplied, bool vsync) noexcept
    {
        try { AfterPresentUnchecked(source, frameEnabled, succeeded, maskValid, lightingApplied, vsync); }
        catch (...) { Cancel("FPS test cancelled after an internal error"); }
    }
}
