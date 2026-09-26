// Tests the state owner used by WorldClouds and actual callback retirement.
#include "CloudFramePublication.h"
#include "GraphicsProxyCompat.h"
#include "MainViewViewport.h"
#include "RendererLifetime.h"
#include <array>
#include <atomic>
#include <iostream>
#include <limits>
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
        Require(frame.CompletePresent(true, false) == Result::Retained &&
            frame.PublishedEpoch() == lastEyeFrame && frame.ReadIndex() == lastReadIndex,
            "repeated presents without a main-view render cannot withdraw or swap the last frame");
    }
    frame.Begin();
    cubes[frame.WriteIndex()] = 0; // The next real Sky can be entirely clear.
    Require(frame.CompletePresent(true, false) == Result::Published &&
        frame.PublishedEpoch() > lastEyeFrame && cubes[frame.ReadIndex()] == 0,
        "a cloudless VR Sky replaces the previous cloud field");
    frame.Begin();
    frame.Reject();
    Require(frame.CompletePresent(true, false) == Result::Withdrawn &&
        frame.PublishedEpoch() == 0,
        "a rejected layer cannot reuse an older cloud field");
    frame.Begin();
    Require(frame.CompletePresent(true, false) == Result::Published,
        "publication recovers on the next authenticated Sky");
    Require(frame.CompletePresent(false, false) == Result::Withdrawn &&
        frame.CompletePresent(true, false) == Result::Withdrawn,
        "world transitions invalidate fields and duplicate presents cannot resurrect them");
    frame.Begin();
    Require(frame.CompletePresent(true, false) == Result::Published,
        "new exterior Sky can publish again");
    frame.Withdraw();
    Require(frame.CompletePresent(true, false) == Result::Withdrawn,
        "master disable discards the field even during render-less presentation");
    frame.Begin();
    Require(frame.CompletePresent(true, false) == Result::Published &&
        frame.CompletePresent(true, true) == Result::Withdrawn,
        "a rendered main view without a Sky withdraws the field on every runtime");
    frame.Begin();
    Require(frame.CompletePresent(true, true) == Result::Published &&
        frame.CompletePresent(true, false) == Result::Retained &&
        frame.CompletePresent(true, false) == Result::Retained,
        "flat frame-generation presents without a render keep the field");
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

class ForwardingDevice final : public IUnknown
{
public:
    enum class Unwrap { Native, Unsupported, Self };
    explicit ForwardingDevice(IUnknown* native) : native_(native) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** destination) override
    {
        if (!destination) return E_POINTER;
        *destination = nullptr;
        if (id == FO4CS::GraphicsProxyCompat::kReShadeUnwrappedObject) {
            ++unwrapQueries;
            if (unwrap == Unwrap::Unsupported) return E_NOINTERFACE;
            *destination = unwrap == Unwrap::Self ? this : native_.Get();
            static_cast<IUnknown*>(*destination)->AddRef();
            return S_OK;
        }
        if (id != __uuidof(IUnknown)) return E_NOINTERFACE;
        *destination = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    ULONG references{ 1 };
    unsigned unwrapQueries{};
    Unwrap unwrap{ Unwrap::Native };
private:
    Microsoft::WRL::ComPtr<IUnknown> native_;
};

void TestGraphicsProxyCapture()
{
    namespace Compat = FO4CS::GraphicsProxyCompat;
    std::atomic<unsigned> destroyed{};
    Microsoft::WRL::ComPtr<IUnknown> native;
    native.Attach(new TrackedObject(destroyed));
    Microsoft::WRL::ComPtr<IUnknown> captured;
    {
        ForwardingDevice proxy(native.Get());
        const auto gameDevice = static_cast<IUnknown*>(&proxy);
        {
            const auto unchanged = Compat::ShaderCaptureDevice(gameDevice, false);
            Require(unchanged.Get() == gameDevice && proxy.unwrapQueries == 0 &&
                proxy.references == 2,
                "ordinary and ENB devices retain their own receiver without a private query");
        }
        Require(proxy.references == 1, "ordinary device capture balances its reference");
        captured = Compat::ShaderCaptureDevice(gameDevice, true);
        Require(captured.Get() == native.Get() && proxy.unwrapQueries == 1 &&
            proxy.references == 1,
            "ReShade capture retains the native device without replacing the game proxy");
        proxy.unwrap = ForwardingDevice::Unwrap::Unsupported;
        Require(!Compat::ShaderCaptureDevice(gameDevice, true),
            "a proxy without the native-interface contract cannot be captured as native");
        proxy.unwrap = ForwardingDevice::Unwrap::Self;
        Require(!Compat::ShaderCaptureDevice(gameDevice, true) && proxy.references == 1,
            "self-unwrapping is rejected without leaking a reference");

        // ReShade and Upscaling forward private data: both receivers can read
        // the same marker, even though their methods require different This.
        const auto proxyAddress = reinterpret_cast<std::uintptr_t>(gameDevice);
        const auto nativeAddress = reinterpret_cast<std::uintptr_t>(native.Get());
        const auto target = reinterpret_cast<std::uintptr_t>(&TestGraphicsProxyCapture);
        const auto thunk = reinterpret_cast<std::uintptr_t>(&TestRetirement);
        Compat::ComRouteData sharedMarker{ Compat::ComRouteData::kMagic,
            Compat::ComRouteData::kVersion, target, proxyAddress };
        Require(sharedMarker.Matches(proxyAddress, thunk) &&
            !sharedMarker.Matches(nativeAddress, thunk),
            "forwarded proxy private data cannot route a call on the native receiver");
        sharedMarker.receiver = nativeAddress;
        Require(sharedMarker.Matches(nativeAddress, thunk) &&
            !sharedMarker.Matches(proxyAddress, thunk),
            "native route markers cannot be called on a wrapping receiver either");
        Require(!sharedMarker.Matches(nativeAddress, target),
            "a copied route must not recurse through the capture thunk");
        sharedMarker.version = 1;
        Require(!sharedMarker.Matches(nativeAddress, thunk),
            "legacy unscoped route data is rejected");
        native.Reset();
    }
    Require(destroyed == 0, "native capture survives the game proxy's release");
    captured.Reset();
    Require(destroyed == 1, "native capture releases the last retained reference");
    Require(!Compat::ShaderCaptureDevice<IUnknown>(nullptr, true),
        "missing proxy device is harmless");
}

void TestUpscaledSkyViewport()
{
    using FO4CS::IsMainSkyViewport;
    D3D11_VIEWPORT view{ 0, 0, 1920, 1080, 0.01f, 1 };
    Require(IsMainSkyViewport(view, 1, 1920, 1080, false),
        "native flat Sky remains a main view");
    view.Width = 1280;
    view.Height = 720;
    Require(IsMainSkyViewport(view, 1, 1920, 1080, false),
        "AE Upscaling's 1280x720 scene within a 1920x1080 target captures clouds");
    view.Width = 853;
    view.Height = 480;
    Require(IsMainSkyViewport(view, 1, 1920, 1080, false),
        "rounded render scales remain valid");
    view.TopLeftX = 8;
    Require(!IsMainSkyViewport(view, 1, 1920, 1080, false),
        "an offset secondary viewport cannot establish the main Sky origin");
    view.TopLeftX = 0;
    view.Width = 512;
    view.Height = 512;
    Require(!IsMainSkyViewport(view, 1, 1920, 1080, false),
        "a square reflection viewport is not the main widescreen view");
    view = { 0, 0, 4992, 2688, 0.01f, 1 };
    Require(IsMainSkyViewport(view, 1, 4992, 2688, true),
        "the full native VR stereo view remains valid");
    view.Width /= 2;
    Require(!IsMainSkyViewport(view, 1, 4992, 2688, true),
        "one VR eye cannot be treated as a full stereo viewport");
    view.Height /= 2;
    Require(!IsMainSkyViewport(view, 1, 4992, 2688, true),
        "flat upscaling support cannot admit a reduced VR mirror viewport");
    view = { 0, 0, 1920, 1080, 0.01f, 1 };
    Require(!IsMainSkyViewport(view, 2, 1920, 1080, false) &&
        !IsMainSkyViewport(view, 0, 1920, 1080, false) &&
        !IsMainSkyViewport(view, 1, 1280, 720, false),
        "ambiguous or out-of-bounds viewports remain rejected");
    view.Width = std::numeric_limits<float>::quiet_NaN();
    Require(!IsMainSkyViewport(view, 1, 1920, 1080, false),
        "non-finite viewport dimensions cannot enter capture");
}

int main()
{
    try {
        TestPublication();
        TestRetirement();
        TestGraphicsProxyCapture();
        TestUpscaledSkyViewport();
        std::cout << "PASS: cloud-frame publication, renderer retirement, graphics proxies and scaled Sky\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
