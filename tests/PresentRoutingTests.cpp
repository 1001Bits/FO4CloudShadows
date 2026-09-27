// SPDX-License-Identifier: GPL-3.0-only
// Real WARP/Detours regression for native creation followed by a wrapper.
#include "PresentDetours.h"
#include "CloudFramePublication.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

using Microsoft::WRL::ComPtr;
namespace Routes = FO4CS::PresentDetours;
extern "C" int __cdecl _CrtDbgReport(int, const char*, int, const char*, const char*, ...) { return 0; }

namespace
{
    IDXGISwapChain* authoritative{};
    FO4CS::CloudFramePublication publication;
    unsigned boundaries{}, completed{}, callbacks{}, overlayCalls{};
    std::array<unsigned, 3> wrapperCalls{};
    Routes::Function overlayOriginal{};

    void Require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    HRESULT Handle(Routes::Function original, IDXGISwapChain* receiver,
        UINT interval, UINT flags)
    {
        ++callbacks;
        Routes::CallScope scope(receiver, authoritative, flags);
        if (!scope.Accepted())
            return original(receiver, interval, flags);
        ++boundaries;
        (void)publication.CompletePresent(true, true);
        const auto result = original(receiver, interval, flags);
        ++completed;
        return result;
    }

    __declspec(noinline) HRESULT Present(IDXGISwapChain* chain, UINT flags = 0)
    {
        return chain->Present(0, flags);
    }

    std::uintptr_t Entry(IDXGISwapChain* chain)
    {
        return (*reinterpret_cast<std::uintptr_t**>(chain))[8];
    }

    void Select(IDXGISwapChain* chain)
    {
        const auto before = Entry(chain);
        const auto route = Routes::Ensure(before, &Handle);
        Require(route.ready && route.original, "selected method needs its own callable route");
        Require(Entry(chain) == before, "inline hooks must leave the vtable slot untouched");
        authoritative = chain;
    }

    template <unsigned Tag>
    class Proxy final : public IDXGISwapChain
    {
        ComPtr<IDXGISwapChain> downstream_;
        ULONG references_{ 1 };
    public:
        explicit Proxy(IDXGISwapChain* downstream) : downstream_(downstream) {}
        bool failPresent{};
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override
        {
            if (!output) return E_POINTER;
            *output = nullptr;
            if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIObject) ||
                id == __uuidof(IDXGIDeviceSubObject) || id == __uuidof(IDXGISwapChain)) {
                *output = static_cast<IDXGISwapChain*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const auto count = --references_;
            if (!count) delete this;
            return count;
        }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID a, UINT b, const void* c) override { return downstream_->SetPrivateData(a,b,c); }
        HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID a, const IUnknown* b) override { return downstream_->SetPrivateDataInterface(a,b); }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID a, UINT* b, void* c) override { return downstream_->GetPrivateData(a,b,c); }
        HRESULT STDMETHODCALLTYPE GetParent(REFIID a, void** b) override { return downstream_->GetParent(a,b); }
        HRESULT STDMETHODCALLTYPE GetDevice(REFIID a, void** b) override { return downstream_->GetDevice(a,b); }
        __declspec(noinline) HRESULT STDMETHODCALLTYPE Present(UINT a, UINT b) override
        {
            ++wrapperCalls[Tag];
            return failPresent ? DXGI_ERROR_DEVICE_RESET : downstream_->Present(a,b);
        }
        HRESULT STDMETHODCALLTYPE GetBuffer(UINT a, REFIID b, void** c) override { return downstream_->GetBuffer(a,b,c); }
        HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL a, IDXGIOutput* b) override { return downstream_->SetFullscreenState(a,b); }
        HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* a, IDXGIOutput** b) override { return downstream_->GetFullscreenState(a,b); }
        HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* a) override { return downstream_->GetDesc(a); }
        HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT a, UINT b, UINT c, DXGI_FORMAT d, UINT e) override { return downstream_->ResizeBuffers(a,b,c,d,e); }
        HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* a) override { return downstream_->ResizeTarget(a); }
        HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** a) override { return downstream_->GetContainingOutput(a); }
        HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* a) override { return downstream_->GetFrameStatistics(a); }
        HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* a) override { return downstream_->GetLastPresentCount(a); }
    };

    template <unsigned Tag>
    ComPtr<Proxy<Tag>> Wrap(IDXGISwapChain* chain)
    {
        ComPtr<Proxy<Tag>> proxy;
        proxy.Attach(new Proxy<Tag>(chain));
        return proxy;
    }

    // Models an overlay cloning a vtable after hooks are already installed,
    // and chaining to the former slot using the same receiver.
    __declspec(noinline) HRESULT WINAPI OverlayPresent(IDXGISwapChain* receiver, UINT interval, UINT flags)
    {
        ++overlayCalls;
        return overlayOriginal(receiver, interval, flags);
    }

    class ClonedTable
    {
        IDXGISwapChain* receiver_;
        std::uintptr_t* original_;
        std::array<std::uintptr_t, 18> table_{};
    public:
        explicit ClonedTable(IDXGISwapChain* receiver) : receiver_(receiver),
            original_(*reinterpret_cast<std::uintptr_t**>(receiver))
        {
            std::memcpy(table_.data(), original_, sizeof(table_));
            overlayOriginal = reinterpret_cast<Routes::Function>(table_[8]);
            table_[8] = reinterpret_cast<std::uintptr_t>(&OverlayPresent);
            *reinterpret_cast<std::uintptr_t**>(receiver_) = table_.data();
        }
        ~ClonedTable() { *reinterpret_cast<std::uintptr_t**>(receiver_) = original_; }
    };

    void RequireOneFrame(IDXGISwapChain* chain, const char* message)
    {
        const auto before = boundaries;
        publication.Begin();
        const auto epoch = publication.PendingEpoch();
        Require(SUCCEEDED(Present(chain)), "forwarded native Present failed");
        Require(boundaries == before + 1 && completed == boundaries &&
            publication.PublishedEpoch() == epoch && publication.PendingEpoch() == 0, message);
    }

    void Run(bool wrapperFirst)
    {
        WNDCLASSW klass{};
        klass.lpfnWndProc = DefWindowProcW;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = L"FO4CSPresentRoutingHidden";
        Require(RegisterClassW(&klass) != 0, "register hidden window");
        const HWND window = CreateWindowW(klass.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 128, 128, nullptr, nullptr, klass.hInstance, nullptr);
        const HWND auxiliaryWindow = CreateWindowW(klass.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 128, 128, nullptr, nullptr, klass.hInstance, nullptr);
        Require(window && auxiliaryWindow, "create hidden windows");
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = desc.BufferDesc.Height = 64;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<IDXGISwapChain> native, auxiliary;
        Require(SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &native, &device,
            nullptr, &context)), "create WARP swap chain");
        ComPtr<IDXGIFactory> factory;
        Require(SUCCEEDED(native->GetParent(IID_PPV_ARGS(&factory))), "get factory");
        desc.OutputWindow = auxiliaryWindow;
        Require(SUCCEEDED(factory->CreateSwapChain(device.Get(), &desc, &auxiliary)), "create auxiliary chain");
        auto proxy = Wrap<0>(native.Get());

        if (wrapperFirst) {
            Select(proxy.Get());
            RequireOneFrame(proxy.Get(), "wrapper-first publishes without a native hook");
        }
        Select(native.Get());
        RequireOneFrame(native.Get(), "native control publishes");
        const auto nativeRoute = Routes::Ensure(Entry(native.Get()), &Handle);
        Require(nativeRoute.ready && !nativeRoute.installed, "repeated native install reuses its trampoline");

        Select(proxy.Get());
        publication.Reset(true);
        const auto before = boundaries;
        const auto callsBefore = wrapperCalls[0];
        for (unsigned index = 0; index < 120; ++index)
            RequireOneFrame(proxy.Get(), "wrapper must publish each captured cloud frame exactly once");
        Require(boundaries == before + 120 && wrapperCalls[0] == callsBefore + 120 &&
            publication.PublishedEpoch() == 120, "120 wrapper frames retain real forwarding and publication");

        const auto filteredBefore = boundaries;
        publication.Begin();
        const auto pending = publication.PendingEpoch();
        Require(SUCCEEDED(Present(proxy.Get(), DXGI_PRESENT_TEST)), "TEST forwarded");
        Require(SUCCEEDED(Present(auxiliary.Get())), "auxiliary forwarded");
        Require(SUCCEEDED(Present(native.Get())), "unselected native forwarded");
        auto otherProxy = Wrap<0>(auxiliary.Get());
        Require(SUCCEEDED(Present(otherProxy.Get())), "unselected wrapper sharing a method forwarded");
        Require(boundaries == filteredBefore && publication.PendingEpoch() == pending,
            "TEST, auxiliary and unselected receivers must not consume captured clouds");
        RequireOneFrame(proxy.Get(), "next real main frame publishes after filtered calls");

        auto outer = Wrap<1>(proxy.Get());
        Select(outer.Get());
        RequireOneFrame(outer.Get(), "nested wrappers and native hook publish once");
        {
            ClonedTable overlay(outer.Get());
            Select(outer.Get());
            const auto hitsBefore = overlayCalls;
            RequireOneFrame(outer.Get(), "later overlay chains to old hooks without duplicate boundaries");
            Require(overlayCalls == hitsBefore + 1, "overlay original is invoked once");
        }
        Select(outer.Get());
        RequireOneFrame(outer.Get(), "restored vtable reuses the previous wrapper route");

        outer->failPresent = true;
        Require(Present(outer.Get()) == DXGI_ERROR_DEVICE_RESET, "downstream HRESULT must remain unchanged");
        outer->failPresent = false;
        RequireOneFrame(outer.Get(), "boundary nesting resets after a failed Present");
        Select(auxiliary.Get());
        RequireOneFrame(auxiliary.Get(), "a replacement native chain sharing the method works");
        Select(proxy.Get());
        RequireOneFrame(proxy.Get(), "switching back to an existing wrapper works");
        auto retryProxy = Wrap<2>(proxy.Get());
        Require(DetourTransactionBegin() == NO_ERROR, "open a foreign transaction");
        const auto failed = Routes::Ensure(Entry(retryProxy.Get()), &Handle);
        Require(!failed.ready && failed.attempted && failed.error != NO_ERROR,
            "failed attachment must never report another route as ready");
        Require(DetourTransactionCommit() == NO_ERROR,
            "a failed begin must not abort somebody else's transaction");
        Require(Routes::Ensure(Entry(retryProxy.Get()), &Handle).error == ERROR_RETRY,
            "failed route uses backoff");
        RequireOneFrame(proxy.Get(), "failed attachment preserves established trampolines");
        Sleep(2100);
        Select(retryProxy.Get());
        RequireOneFrame(retryProxy.Get(), "the same receiver recovers after a failed attachment");
        Require(!Routes::Ensure(0, &Handle).ready && !Routes::Ensure(Entry(native.Get()), nullptr).ready,
            "missing route inputs must never report ready");

        authoritative = nullptr;
        DestroyWindow(auxiliaryWindow);
        DestroyWindow(window);
        std::printf("PASS %s: 120 wrapped frames published exactly once; native, auxiliary, TEST, "
            "nested wrappers, later overlay, receiver replacement, error propagation and attach retry verified "
            "(%u hook callbacks / %u completed boundaries).\n",
            wrapperFirst ? "wrapper-first" : "native-first", callbacks, completed);
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        Run(argc > 1 && std::string_view(argv[1]) == "--wrapper-first");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
