// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "FrameRateWindow.h"
#include <array>
#include <optional>

namespace FO4CS
{
    // As in the Fallout mirrors meter, use frame-time distributions and total
    // frames / total time. Never average instantaneous FPS values.
    struct FpsHistogram
    {
        std::array<std::uint32_t, 5001> buckets{};
        std::uint64_t count{};
        double maximum{};
        void Clear() noexcept { buckets.fill(0); count = 0; maximum = 0; }
        void Add(double ms) noexcept
        {
            if (!std::isfinite(ms) || ms <= 0) return;
            const auto index = static_cast<std::size_t>(std::min(ms * 10.0, 5000.0));
            ++buckets[index]; ++count; maximum = std::max(maximum, ms);
        }
        void Merge(const FpsHistogram& other) noexcept
        {
            for (std::size_t i = 0; i < buckets.size(); ++i) buckets[i] += other.buckets[i];
            count += other.count; maximum = std::max(maximum, other.maximum);
        }
        double Percentile(double fraction) const noexcept
        {
            if (!count) return 0;
            const auto rank = std::max<std::uint64_t>(1,
                static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(count))));
            std::uint64_t seen{};
            for (std::size_t i = 0; i < buckets.size(); ++i) {
                seen += buckets[i];
                if (seen >= rank)
                    return i + 1 == buckets.size() ? maximum : static_cast<double>(i + 1) / 10.0;
            }
            return maximum;
        }
    };

    class FpsTestSession
    {
    public:
        enum class Mode { Idle, Manual, Automatic, Finished, Cancelled };
        static constexpr std::array<bool, 4> kSteps{ false, true, true, false };
        static constexpr double kCountdownSeconds = 5.0;
        struct Average
        {
            std::uint64_t frames{}, maskFrames{}, lightingFrames{}, vsyncFrames{};
            double seconds{}, p95Ms{}, p99Ms{}, maxMs{};
            bool countersOK{ true }, clocksOK{ true };
            double FPS() const noexcept { return seconds > 0 ? static_cast<double>(frames) / seconds : 0; }
            double Milliseconds() const noexcept { return frames ? seconds * 1000.0 / static_cast<double>(frames) : 0; }
            bool Verified(bool enabled) const noexcept
            { return frames && countersOK && clocksOK && (!enabled || (maskFrames == frames && lightingFrames == frames)); }
        };
        struct Snapshot
        {
            Mode mode{ Mode::Idle };
            std::array<Average, 2> averages{}; // OFF, ON
            std::array<Average, 4> steps{};
            std::size_t step{};
            bool enabled{}, countdown{}, measuring{}, paused{}, driftOK{};
            double remaining{}, liveFPS{}, liveMs{};
        };
        struct Update
        {
            std::optional<bool> setEnabled;
            bool result{}, finished{}, discarded{};
            std::size_t completedStep{};
            Average window;
        };

        void Start(bool automatic, bool originalEnabled) noexcept
        {
            snapshot_ = {};
            snapshot_.mode = automatic ? Mode::Automatic : Mode::Manual;
            snapshot_.countdown = automatic;
            snapshot_.enabled = original_ = originalEnabled;
            for (auto& histogram : totals_) histogram.Clear();
            ResetWindow();
            snapshot_.remaining = automatic ? kCountdownSeconds : FrameRateWindow::kSettleSeconds;
            countdownStart_ = -1;
        }
        bool Active() const noexcept { return snapshot_.mode == Mode::Manual || Automatic(); }
        bool Automatic() const noexcept { return snapshot_.mode == Mode::Automatic; }
        bool OriginalEnabled() const noexcept { return original_; }
        const Snapshot& GetSnapshot() const noexcept { return snapshot_; }
        std::optional<bool> Cancel() noexcept
        {
            const auto restore = Automatic() ? std::optional<bool>(original_) : std::nullopt;
            if (Active()) snapshot_.mode = Mode::Cancelled;
            snapshot_.paused = snapshot_.measuring = snapshot_.countdown = false;
            ResetWindow();
            return restore;
        }
        void ToggleManual(bool enabled) noexcept
        {
            if (snapshot_.mode != Mode::Manual) Start(false, enabled);
            snapshot_.enabled = enabled;
            snapshot_.averages[enabled ? 1 : 0] = {};
            totals_[enabled ? 1 : 0].Clear();
            ResetWindow();
        }

        Update Observe(FrameRateWindow::Sample sample) noexcept
        {
            Update update;
            if (!Active()) return update;
            snapshot_.paused = !sample.eligible;
            if (snapshot_.countdown) {
                if (!sample.eligible || !std::isfinite(sample.seconds)) countdownStart_ = -1;
                else if (countdownStart_ < 0) countdownStart_ = sample.seconds;
                snapshot_.remaining = countdownStart_ < 0 ? kCountdownSeconds :
                    std::max(0.0, kCountdownSeconds - (sample.seconds - countdownStart_));
                if (countdownStart_ >= 0 && snapshot_.remaining == 0) {
                    snapshot_.countdown = false;
                    snapshot_.enabled = kSteps[0];
                    update.setEnabled = kSteps[0];
                    ResetWindow();
                }
                return update;
            }
            sample.request = request_;
            sample.eligible = sample.eligible && sample.enabled == snapshot_.enabled;
            snapshot_.paused = !sample.eligible;
            const bool wasMeasuring = window_.IsMeasuring();
            FrameRateWindow::Result result;
            const auto event = window_.Push(sample, result);
            const double delta = sample.seconds - previous_;
            if (event == FrameRateWindow::Event::kSettling || event == FrameRateWindow::Event::kDiscarded) {
                pending_.Clear(); liveSeconds_ = 0; liveFrames_ = 0;
                snapshot_.liveFPS = snapshot_.liveMs = 0;
            } else if (sample.eligible && wasMeasuring && delta > 0 && std::isfinite(delta)) {
                pending_.Add(delta * 1000.0);
                liveSeconds_ += delta; ++liveFrames_;
                if (liveSeconds_ >= 0.5) {
                    snapshot_.liveFPS = static_cast<double>(liveFrames_) / liveSeconds_;
                    snapshot_.liveMs = liveSeconds_ * 1000.0 / static_cast<double>(liveFrames_);
                    liveSeconds_ = 0; liveFrames_ = 0;
                }
            }
            previous_ = sample.seconds;
            snapshot_.measuring = window_.IsMeasuring();
            snapshot_.remaining = sample.eligible ? window_.RemainingSeconds(sample.seconds) : FrameRateWindow::kSettleSeconds;
            update.discarded = event == FrameRateWindow::Event::kDiscarded;
            if (event != FrameRateWindow::Event::kResult) return update;
            update.result = true;
            update.completedStep = snapshot_.step;
            update.window = { result.frames, result.maskFrames, result.lightingFrames, result.vsyncFrames,
                result.seconds, pending_.Percentile(0.95), pending_.Percentile(0.99), result.maxFrameMs,
                result.counterMatches && pending_.count == result.frames, result.clockMatches };
            const auto mode = result.enabled ? 1u : 0u;
            auto& average = snapshot_.averages[mode];
            average.frames += result.frames; average.seconds += result.seconds;
            average.maskFrames += result.maskFrames; average.lightingFrames += result.lightingFrames;
            average.vsyncFrames += result.vsyncFrames;
            average.maxMs = std::max(average.maxMs, result.maxFrameMs);
            average.countersOK = average.countersOK && update.window.countersOK;
            average.clocksOK = average.clocksOK && result.clockMatches;
            totals_[mode].Merge(pending_); pending_.Clear();
            average.p95Ms = totals_[mode].Percentile(0.95);
            average.p99Ms = totals_[mode].Percentile(0.99);
            if (Automatic()) {
                snapshot_.steps[snapshot_.step] = update.window;
                if (++snapshot_.step == kSteps.size()) {
                    const auto first = snapshot_.steps[0].Milliseconds();
                    const auto last = snapshot_.steps[3].Milliseconds();
                    snapshot_.driftOK = std::abs(first - last) <= std::max(0.2, 0.1 * first);
                    snapshot_.mode = Mode::Finished;
                    snapshot_.measuring = false;
                    snapshot_.liveFPS = snapshot_.liveMs = 0;
                    snapshot_.enabled = original_;
                    update.finished = true;
                    update.setEnabled = original_;
                } else {
                    snapshot_.enabled = kSteps[snapshot_.step];
                    update.setEnabled = snapshot_.enabled;
                    ResetWindow();
                }
            }
            return update;
        }

    private:
        void ResetWindow() noexcept
        {
            window_ = {}; pending_.Clear(); ++request_;
            previous_ = liveSeconds_ = 0; liveFrames_ = 0;
            snapshot_.liveFPS = snapshot_.liveMs = 0;
            snapshot_.measuring = false;
            snapshot_.remaining = FrameRateWindow::kSettleSeconds;
        }
        Snapshot snapshot_;
        FrameRateWindow window_;
        FpsHistogram pending_;
        std::array<FpsHistogram, 2> totals_;
        std::uint64_t request_{}, liveFrames_{};
        double countdownStart_{ -1 }, previous_{}, liveSeconds_{};
        bool original_{};
    };
}
