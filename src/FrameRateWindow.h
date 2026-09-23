#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FO4CS
{
    // Application frame cadence, not GPU duration or displayed/VR FPS. One
    // sample per successful authoritative Present, including time in Present.
    // No allocation, sorting, GPU queries or I/O in the accumulator.
    class FrameRateWindow
    {
    public:
        static constexpr double kSettleSeconds = 3.0;
        static constexpr double kMeasureSeconds = 10.0;

        struct Sample
        {
            double seconds{};
            std::uint64_t request{};
            std::uintptr_t source{};
            bool enabled{};
            bool eligible{};
            bool maskValid{};
            bool lightingApplied{};
            bool vsync{};
            std::uint32_t presentCount{};
            bool presentCountValid{};
            double wallSeconds{};
        };

        struct Result
        {
            std::uint64_t frames{};
            double seconds{};
            double fps{};
            double meanFrameMs{};
            double maxFrameMs{};
            std::uint64_t framesOver50Ms{};
            std::uint64_t maskFrames{};
            std::uint64_t lightingFrames{};
            std::uint64_t vsyncFrames{};
            bool enabled{};
            std::uint32_t dxgiPresents{};
            bool counterMatches{}, clockMatches{};
        };

        enum class Event { kNone, kSettling, kDiscarded, kResult };
        bool IsMeasuring() const noexcept { return measuring_; }
        double RemainingSeconds(double now) const noexcept
        { return std::max(0.0, (measuring_ ? kMeasureSeconds : kSettleSeconds) - (now - start_)); }

        Event Push(const Sample& sample, Result& result) noexcept
        {
            result = {};
            if (!std::isfinite(sample.seconds) || sample.request == 0 ||
                !sample.eligible) {
                const bool hadWindow = primed_;
                Reset();
                return hadWindow ? Event::kDiscarded : Event::kNone;
            }

            if (!primed_ || sample.request != request_ ||
                sample.source != source_ || sample.enabled != enabled_ ||
                sample.seconds <= previous_) {
                Start(sample);
                return Event::kSettling;
            }

            const double interval = sample.seconds - previous_;
            previous_ = sample.seconds;
            if (!measuring_) {
                if (sample.seconds - start_ >= kSettleSeconds) {
                    measuring_ = true;
                    start_ = sample.seconds; // First endpoint, no interval yet.
                    presentStart_ = sample.presentCount;
                    counterValid_ = sample.presentCountValid;
                    wallStart_ = sample.wallSeconds;
                }
                return Event::kNone;
            }

            ++frames_;
            counterValid_ = counterValid_ && sample.presentCountValid;
            maxInterval_ = std::max(maxInterval_, interval);
            framesOver50Ms_ += interval > 0.050 ? 1u : 0u;
            maskFrames_ += sample.maskValid ? 1u : 0u;
            lightingFrames_ += sample.lightingApplied ? 1u : 0u;
            vsyncFrames_ += sample.vsync ? 1u : 0u;
            const double elapsed = sample.seconds - start_;
            if (elapsed < kMeasureSeconds)
                return Event::kNone;

            result = {
                frames_, elapsed, static_cast<double>(frames_) / elapsed,
                1000.0 * elapsed / static_cast<double>(frames_),
                1000.0 * maxInterval_, framesOver50Ms_, maskFrames_,
                lightingFrames_, vsyncFrames_, enabled_
            };
            result.dxgiPresents = sample.presentCount - presentStart_;
            result.counterMatches = counterValid_ && result.dxgiPresents == frames_;
            const double wallElapsed = sample.wallSeconds - wallStart_;
            result.clockMatches = std::isfinite(wallElapsed) && wallElapsed > 0.0 &&
                std::abs(wallElapsed - elapsed) <= std::max(0.1, elapsed * 0.02);
            // Adjacent windows share one endpoint, never a frame interval.
            start_ = sample.seconds;
            presentStart_ = sample.presentCount;
            counterValid_ = sample.presentCountValid;
            wallStart_ = sample.wallSeconds;
            ClearCounters();
            return Event::kResult;
        }

    private:
        void ClearCounters() noexcept
        {
            frames_ = framesOver50Ms_ = maskFrames_ = lightingFrames_ = vsyncFrames_ = 0;
            maxInterval_ = 0.0;
        }

        void Reset() noexcept
        {
            primed_ = measuring_ = false;
            ClearCounters();
        }

        void Start(const Sample& sample) noexcept
        {
            Reset();
            primed_ = true;
            start_ = previous_ = sample.seconds;
            request_ = sample.request;
            source_ = sample.source;
            enabled_ = sample.enabled;
        }

        bool primed_{}, measuring_{}, enabled_{};
        double start_{}, previous_{}, maxInterval_{};
        std::uint64_t request_{}, frames_{}, framesOver50Ms_{};
        std::uint64_t maskFrames_{}, lightingFrames_{}, vsyncFrames_{};
        std::uintptr_t source_{};
        std::uint32_t presentStart_{};
        bool counterValid_{};
        double wallStart_{};
    };
}
