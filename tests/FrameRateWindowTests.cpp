#include "FrameRateWindow.h"

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

int main()
{
    try {
        TestCadenceAndBoundaries();
        TestStallsAreRetained();
        TestInvalidation();
        TestIndependentCounters();
        std::cout << "Manual FPS tests passed: cadence, state boundaries, warmup, stalls, focus/menu failure, stream changes.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
