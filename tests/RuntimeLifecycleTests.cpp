// Tests the state owner used by WorldClouds and actual callback retirement.
#include "CloudFramePublication.h"
#include "RendererLifetime.h"
#include <array>
#include <atomic>
#include <iostream>
#include <semaphore>
#include <stdexcept>
#include <thread>

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void TestPublication()
{
    FO4CS::CloudFramePublication frame;
    std::array<unsigned, 2> cubes{};
    Require(!frame.Present(true) && frame.PublishedEpoch() == 0,
        "Present without a Sky cannot publish");
    frame.Begin();
    const auto firstEpoch = frame.PendingEpoch();
    frame.Begin();
    Require(frame.PendingEpoch() == firstEpoch, "several Sky draws form one frame");
    cubes[frame.WriteIndex()] = 1;
    Require(cubes[frame.ReadIndex()] == 0, "in-flight writes do not change the read cube");
    Require(frame.Present(true) && frame.PublishedEpoch() == firstEpoch &&
        frame.PendingEpoch() == 0 && cubes[frame.ReadIndex()] == 1,
        "only Present publishes a completed frame");
    frame.Begin();
    cubes[frame.WriteIndex()] = 2;
    Require(cubes[frame.ReadIndex()] == 1, "preceding cloud frame remains immutable during capture");
    frame.Reject();
    frame.Withdraw(true);
    frame.Begin(); // A later valid layer cannot undo a rejected layer.
    Require(frame.PublishedEpoch() == 0 && frame.Active() && !frame.Authenticated(),
        "one rejected layer withdraws sunlight coverage for the entire frame");
    Require(!frame.Present(true) && frame.PendingEpoch() == 0,
        "Present rejects a partial capture and clears the pending epoch");
    frame.Begin();
    Require(frame.Present(true), "the next valid frame recovers immediately");
    Require(!frame.Present(true) && frame.PublishedEpoch() == 0,
        "a subsequent frame with no Sky withdraws previous weather");
    frame.Begin();
    Require(!frame.Present(false), "world/load changes at Present reject the captured frame");
    frame.Begin();
    frame.Withdraw();
    Require(!frame.Present(true), "disable discards an in-flight frame");
    frame.Begin();
    Require(frame.Present(true), "enable can publish the next complete frame");
    const auto beforeReset = frame.Serial();
    frame.Reset();
    Require(frame.PublishedEpoch() == 0 && frame.PendingEpoch() == 0 &&
        frame.ReadIndex() != frame.WriteIndex(), "scene reset clears both publication states");
    frame.Begin();
    Require(frame.PendingEpoch() > beforeReset, "scene reset preserves monotonic epochs");
    frame.Reset(true);
    Require(frame.Serial() == 0 && !frame.Present(true),
        "renderer replacement discards old device publication");
    for (unsigned i = 1; i <= 10000; ++i) {
        frame.Begin();
        const auto read = frame.ReadIndex();
        cubes[frame.WriteIndex()] = i;
        Require(frame.Present(true) && frame.ReadIndex() != read &&
            frame.ReadIndex() != frame.WriteIndex() && cubes[frame.ReadIndex()] == i,
            "sustained publication alternates exactly two distinct cube slots");
    }

    using Result = FO4CS::CloudFramePublication::PresentResult;
    const auto lastEyeFrame = frame.PublishedEpoch();
    const auto lastReadIndex = frame.ReadIndex();
    for (unsigned i = 0; i < 10000; ++i) {
        Require(frame.CompletePresent(true, true) == Result::Retained &&
            frame.PublishedEpoch() == lastEyeFrame && frame.ReadIndex() == lastReadIndex,
            "repeated VR companion presents cannot withdraw or swap the last eye frame");
    }
    frame.Begin();
    cubes[frame.WriteIndex()] = 0; // The next real Sky can be entirely clear.
    Require(frame.CompletePresent(true, true) == Result::Published &&
        frame.PublishedEpoch() > lastEyeFrame && cubes[frame.ReadIndex()] == 0,
        "a cloudless VR Sky replaces the previous cloud field");
    frame.Begin();
    frame.Reject();
    Require(frame.CompletePresent(true, true) == Result::Withdrawn &&
        frame.PublishedEpoch() == 0,
        "a rejected VR layer cannot reuse an older cloud field");
    frame.Begin();
    Require(frame.CompletePresent(true, true) == Result::Published,
        "VR publication recovers on the next authenticated Sky");
    Require(frame.CompletePresent(false, true) == Result::Withdrawn &&
        frame.CompletePresent(true, true) == Result::Withdrawn,
        "world transitions invalidate VR fields and duplicate presents cannot resurrect them");
    frame.Begin();
    Require(frame.CompletePresent(true, true) == Result::Published,
        "new exterior Sky can publish again");
    frame.Withdraw();
    Require(frame.CompletePresent(true, true) == Result::Withdrawn,
        "master disable discards the VR field even during companion-only presentation");
    frame.Begin();
    Require(frame.CompletePresent(true, false) == Result::Published &&
        frame.CompletePresent(true, false) == Result::Withdrawn,
        "flat presentation still withdraws a frame with no Sky");
}

class TrackedObject final : public IUnknown
{
public:
    explicit TrackedObject(std::atomic<unsigned>& destroyed) : destroyed_(destroyed) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** destination) override
    {
        if (!destination) return E_POINTER;
        *destination = nullptr;
        if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
        *destination = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto remaining = --references_;
        if (remaining == 0) {
            // A native COM destructor may re-enter plugin callbacks.
            FO4CS::RendererLifetime::Scope callback;
            ++destroyed_;
            delete this;
        }
        return remaining;
    }
private:
    std::atomic<ULONG> references_{ 1 };
    std::atomic<unsigned>& destroyed_;
};

void TestRetirement()
{
    namespace Lifetime = FO4CS::RendererLifetime;
    std::atomic<unsigned> destroyed{ 0 };
    Microsoft::WRL::ComPtr<IUnknown> current;
    current.Attach(new TrackedObject(destroyed));
    std::binary_semaphore ready(0), resume(0);
    std::thread worker([&] {
        Lifetime::Scope callback;
        Lifetime::Scope nested;
        ready.release();
        resume.acquire();
    });
    ready.acquire();
    {
        Lifetime::Scope replacement;
        Lifetime::Retire(current);
        current.Reset();
    }
    Lifetime::Collect();
    const bool retainedDuringCallback = destroyed.load() == 0;
    resume.release();
    worker.join();
    Require(retainedDuringCallback && destroyed.load() == 1,
        "retired renderer survives concurrent callback and releases at quiescence");
    for (unsigned i = 0; i < 1000; ++i) {
        {
            Lifetime::Scope replacement;
            current.Attach(new TrackedObject(destroyed));
            Lifetime::Retire(current);
            current.Reset();
            Lifetime::Collect();
            Require(destroyed.load() == i + 1, "nested collection cannot release active bindings");
        }
        Require(destroyed.load() == i + 2, "repeated replacement does not retain old renderers");
    }
}

int main()
{
    try {
        TestPublication();
        TestRetirement();
        std::cout << "PASS: current cloud-frame publication and concurrent renderer retirement\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
