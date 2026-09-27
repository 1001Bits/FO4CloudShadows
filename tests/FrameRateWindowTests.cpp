#include "FrameRateWindow.h"
#include "FpsTestSession.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using Window = FO4CS::FrameRateWindow;
using Event = Window::Event;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Near(double a, double b) { return std::abs(a - b) < 0.00001; }

void TestCadenceAndBoundaries()
{
    Window window;
    Window::Result result;
    Window::Sample sample{ 0.0, 1, 42, false, true, false, false, true };
    Require(window.Push(sample, result) == Event::kSettling, "initial endpoint must not count as a frame");
    for (int frame = 1; frame < 1300; ++frame) {
        sample.seconds = static_cast<double>(frame) / 100.0;
        Require(window.Push(sample, result) == Event::kNone, "settle plus ten complete seconds required");
    }
    sample.seconds = 13.0;
    Require(window.Push(sample, result) == Event::kResult && result.frames == 1000 &&
        Near(result.fps, 100.0) && Near(result.meanFrameMs, 10.0) &&
        !result.enabled && result.vsyncFrames == 1000 && result.maskFrames == 0,
        "FPS must equal intervals divided by elapsed time, OFF needs no valid mask");
    for (int frame = 1301; frame < 2300; ++frame) {
        sample.seconds = static_cast<double>(frame) / 100.0;
        Require(window.Push(sample, result) == Event::kNone, "adjacent window must not count an extra endpoint");
    }
    sample.seconds = 23.0;
    Require(window.Push(sample, result) == Event::kResult && result.frames == 1000,
        "periodic results neither lose nor duplicate a frame interval");

    sample.request = 2;
    sample.enabled = true;
    sample.maskValid = sample.lightingApplied = true;
    sample.seconds = 23.5;
    Require(window.Push(sample, result) == Event::kSettling, "F10 resets the warmup and state");
    sample.seconds = 26.5;
    window.Push(sample, result);
    for (int frame = 1; frame <= 200; ++frame) {
        sample.seconds = 26.5 + static_cast<double>(frame) / 20.0;
        window.Push(sample, result);
    }
    Require(result.enabled && result.frames == 200 && Near(result.fps, 20.0) &&
        result.maskFrames == 200 && result.lightingFrames == 200,
        "the ON sample must not inherit faster OFF frames");
}

void TestStallsAreRetained()
{
    Window window;
    Window::Result result;
    Window::Sample sample{ 0.0, 1, 42, true, true, true, true, false };
    window.Push(sample, result);
    sample.seconds = 3.0;
    window.Push(sample, result);
    for (int frame = 1; frame <= 900; ++frame) {
        sample.seconds = 3.0 + static_cast<double>(frame) / 100.0;
        Require(window.Push(sample, result) == Event::kNone, "nine seconds is not a complete window");
    }
    sample.seconds = 14.0; // Retain a two-second stall; do not average reciprocal frame times.
    Require(window.Push(sample, result) == Event::kResult && result.frames == 901 &&
        Near(result.seconds, 11.0) && Near(result.fps, 901.0 / 11.0) &&
        Near(result.maxFrameMs, 2000.0) && result.framesOver50Ms == 1,
        "stalls must remain in the denominator and maximum, including an overshooting final interval");
}

void TestInvalidation()
{
    Window window;
    Window::Result result;
    Window::Sample sample{ 0.0, 1, 42, true, true, true, true, false };
    window.Push(sample, result);
    sample.seconds = 3.0;
    window.Push(sample, result);
    sample.seconds = 12.0;
    window.Push(sample, result);
    sample.seconds = 12.1;
    sample.eligible = false;
    Require(window.Push(sample, result) == Event::kDiscarded && result.frames == 0,
        "focus loss, menu, failed Present or boundary state changes discard the WHOLE partial window");
    Require(window.Push(sample, result) == Event::kNone, "paused frames must not spam discard messages");
    sample.eligible = true;
    sample.seconds = 20.0;
    Require(window.Push(sample, result) == Event::kSettling, "resume requires a new settling period");
    sample.source = 99;
    sample.seconds = 24.0;
    Require(window.Push(sample, result) == Event::kSettling, "never mix swap chains");
    sample.enabled = false;
    sample.seconds = 27.0;
    Require(window.Push(sample, result) == Event::kSettling, "menu/config state changes cannot mix ON and OFF");
    sample.seconds = 26.0;
    Require(window.Push(sample, result) == Event::kSettling, "nonmonotonic clock resets the window");
    sample.seconds = std::numeric_limits<double>::quiet_NaN();
    Require(window.Push(sample, result) == Event::kDiscarded, "invalid timestamps cannot produce an FPS result");
    sample.seconds = 50.0;
    sample.request = 0;
    Require(window.Push(sample, result) == Event::kNone, "logging is dormant before F10");
}

void TestIndependentCounters()
{
    // Verify coverage rather than assuming each hook call is a displayed frame.
    for (int scenario = 0; scenario < 5; ++scenario) {
        Window window;
        Window::Result result;
        Window::Sample sample{0, 1, 42, true, true, true, true, false};
        sample.presentCountValid = true;
        sample.presentCount = scenario == 4 ? UINT32_MAX - 500u : 100u;
        window.Push(sample, result);
        for (int frame = 1; frame <= 1300; ++frame) {
            sample.seconds = frame / 100.0;
            sample.wallSeconds = sample.seconds * (scenario == 2 ? 0.5 : 1.0);
            sample.presentCount += scenario == 1 ? 2u : 1u;
            sample.presentCountValid = !(scenario == 3 && frame == 600);
            window.Push(sample, result);
        }
        Require(result.frames == 1000 && Near(result.fps, 100.0),
            "independent checks do not silently rescale the measured cadence");
        Require(result.counterMatches == (scenario != 1 && scenario != 3),
            "missed hooks or an unavailable counter invalidate coverage; UINT wrap is supported");
        Require(result.clockMatches == (scenario != 2),
            "a scaled/stale high-resolution clock cannot validate an FPS comparison");
    }
}

void TestAutomaticComparison()
{
    for (const bool original : { false, true }) {
        FO4CS::FpsTestSession session;
        session.Start(true, original);
        bool enabled = original;
        unsigned results{};
        double now{};
        std::uint32_t presents{};
        for (unsigned frame = 0; session.Active() && frame < 8000; ++frame) {
            now += enabled ? 0.012 : 0.010;
            const auto update = session.Observe({ now, 1, 42, enabled, true, enabled, enabled,
                false, ++presents, true, now });
            if (update.result) {
                Require(update.completedStep == results && update.window.Verified(enabled),
                    "automatic steps retain correct frame labels and independently checked counters");
                Require(std::abs(update.window.FPS() - (enabled ? 1000.0/12.0 : 100.0)) < 0.001,
                    "each phase reports the actual application cadence");
                ++results;
            }
            if (update.setEnabled) enabled = *update.setEnabled;
        }
        const auto& snapshot = session.GetSnapshot();
        Require(results == 4 && snapshot.mode == FO4CS::FpsTestSession::Mode::Finished && enabled == original,
            "OFF/ON/ON/OFF completes and restores both possible original settings");
        Require(snapshot.driftOK && snapshot.averages[0].Verified(false) && snapshot.averages[1].Verified(true),
            "a stationary repeated baseline validates the comparison");
        Require(std::abs(snapshot.averages[1].Milliseconds() - snapshot.averages[0].Milliseconds() - 2.0) < 0.001,
            "combined ON/OFF cost uses total measured time and frames");
        Require(snapshot.averages[1].p99Ms >= 12.0 && snapshot.averages[1].p99Ms <= 12.1,
            "percentiles cover all ON intervals at 0.1 ms resolution");
    }
}

void TestComparisonCancellationAndValidation()
{
    for (const bool original : { false, true }) {
        FO4CS::FpsTestSession session;
        session.Start(true, original);
        const auto restored = session.Cancel();
        Require(restored && *restored == original && !session.Active() && !session.Cancel(),
            "cancellation restores once, including an original OFF setting");
    }
    FO4CS::FpsTestSession session;
    session.Start(true, true);
    bool enabled = true;
    double now{};
    std::uint32_t presents{};
    for (unsigned frame = 0; session.Active() && frame < 10000; ++frame) {
        const bool last = session.GetSnapshot().step == 3;
        now += last ? 0.020 : 0.010;
        const auto update = session.Observe({ now, 1, 42, enabled, true, true, false,
            true, ++presents, true, now });
        if (update.setEnabled) enabled = *update.setEnabled;
    }
    Require(!session.GetSnapshot().driftOK && !session.GetSnapshot().averages[1].Verified(true),
        "baseline drift and missing ON lighting cannot pass comparison validation");

    session.Start(false, false);
    now = 0; presents = 0;
    for (unsigned frame = 0; frame < 1800; ++frame) {
        now += .01;
        const bool eligible = frame < 850 || frame >= 900;
        session.Observe({ now, 1, 42, false, eligible, false, false,
            false, ++presents, true, now });
    }
    Require(session.GetSnapshot().averages[0].frames == 0,
        "a focus/menu interruption discards the whole partial measurement and settles again");
    for (unsigned frame = 0; frame < 600; ++frame) {
        now += .01;
        session.Observe({ now, 1, 42, false, true, false, false,
            false, ++presents, true, now });
    }
    Require(session.GetSnapshot().averages[0].Verified(false), "manual sampling recovers after a complete new window");
    session.ToggleManual(true);
    Require(session.GetSnapshot().averages[0].frames > 0 && session.GetSnapshot().averages[1].frames == 0,
        "manual toggle retains the opposite mode and clears only the new mode");
    Require(!session.Cancel(), "stopping manual comparison does not undo the user's toggle");

    FO4CS::FpsHistogram histogram;
    for (unsigned i = 0; i < 98; ++i) histogram.Add(10);
    histogram.Add(2000); histogram.Add(3000);
    Require(histogram.Percentile(.95) <= 10.1 && histogram.Percentile(.99) >= 2000,
        "long stalls are retained instead of silently clamped out of percentile reporting");
}

int main()
{
    try {
        TestCadenceAndBoundaries();
        TestStallsAreRetained();
        TestInvalidation();
        TestIndependentCounters();
        TestAutomaticComparison();
        TestComparisonCancellationAndValidation();
        std::cout << "Manual FPS tests passed: cadence, state boundaries, warmup, stalls, focus/menu failure, stream changes.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
