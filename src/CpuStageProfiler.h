// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Windows.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#ifndef FO4CS_ENABLE_PRIVATE_PROFILING
#define FO4CS_ENABLE_PRIVATE_PROFILING 0
#endif

namespace FO4CS::CpuProfile
{
    // Opt-in diagnostics. Counters are render-thread local; the controller
    // never resets them inside a scope. No allocation, I/O or GPU queries here.
    enum class Stage : unsigned {
        MemoryValidation, CloudDrawValidation, Technique, DrawHook,
        WorldCapture, CaptureSetup, VisibleDraw,
        CaptureTotal, CaptureValidation, CaptureSave, CaptureBind,
        CaptureBuffer, CaptureDraw, CaptureRestore,
        ProjectionTotal, ProjectionValidation, ProjectionSave,
        ProjectionBind, ProjectionBuffer, ProjectionDispatch, ProjectionRestore,
        GpuQuery, FrameCommit, Present, Count
    };
    inline constexpr std::array names{
        "memory_validation", "cloud_draw_validation", "technique_hook", "draw_hook",
        "world_capture", "capture_setup", "visible_draw",
        "capture_total", "capture_validation", "capture_save", "capture_bind",
        "capture_buffer", "capture_draw", "capture_restore",
        "projection_total", "projection_validation", "projection_save",
        "projection_bind", "projection_buffer", "projection_dispatch", "projection_restore",
        "gpu_queries", "frame_commit", "present_forward"
    };
    static_assert(names.size() == static_cast<unsigned>(Stage::Count));
    struct Counter {
        std::uint64_t calls{}, ticks{}, exclusiveTicks{}, maximumTicks{}, cycles{}, exclusiveCycles{};
    };
    using Counters = std::array<Counter, names.size()>;
    inline std::atomic<bool> enabled{false};
    inline std::atomic<bool> measureCycles{false};
    inline thread_local Counters counters{};
    class Scope;
    inline thread_local Scope* current{};
    inline std::uint64_t Now() noexcept {
        LARGE_INTEGER value{}; QueryPerformanceCounter(&value);
        return static_cast<std::uint64_t>(value.QuadPart);
    }
    inline std::uint64_t Cycles() noexcept {
        ULONG64 value{}; QueryThreadCycleTime(GetCurrentThread(), &value); return value;
    }
#if FO4CS_ENABLE_PRIVATE_PROFILING
    class Scope {
    public:
        explicit Scope(Stage stage) noexcept : stage_(stage) {
            if (!enabled.load(std::memory_order_relaxed)) return;
            active_ = true; cyclesEnabled_ = measureCycles.load(std::memory_order_relaxed);
            parent_ = current; current = this;
            startCycles_ = cyclesEnabled_ ? Cycles() : 0;
            start_ = Now();
        }
        ~Scope() { Stop(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        void Stop() noexcept {
            if (!active_) return;
            const auto elapsed = Now() - start_;
            const auto cycles = cyclesEnabled_ ? Cycles() - startCycles_ : 0;
            auto& counter = counters[static_cast<unsigned>(stage_)];
            ++counter.calls; counter.ticks += elapsed;
            counter.exclusiveTicks += elapsed - (std::min)(elapsed, childTicks_);
            counter.maximumTicks = (std::max)(counter.maximumTicks, elapsed);
            counter.cycles += cycles;
            counter.exclusiveCycles += cycles - (std::min)(cycles, childCycles_);
            if (parent_) { parent_->childTicks_ += elapsed; parent_->childCycles_ += cycles; }
            current = parent_; active_ = false;
        }
        void Set(Stage stage) noexcept {
            if (!active_) return;
            Stop(); stage_ = stage; childTicks_ = childCycles_ = 0;
            active_ = true; current = this;
            startCycles_ = cyclesEnabled_ ? Cycles() : 0; start_ = Now();
        }
    private:
        Stage stage_;
        Scope* parent_{};
        std::uint64_t start_{}, startCycles_{}, childTicks_{}, childCycles_{};
        bool active_{}, cyclesEnabled_{};
    };
#else
    class Scope {
    public:
        explicit Scope(Stage) noexcept {}
        void Stop() noexcept {}
        void Set(Stage) noexcept {}
    };
#endif
}
