#include "CpuStageProfiler.h"
#include "PCH.h"
#include "RendererLifetime.h"
#include "AcceptanceRunner.h"
#include "CloudShadows.h"
#include "CloudShadowsMenuBridge.h"
#include "CloudMotionResolver.h"
#include "EnbCompat.h"
#include "GodraysIntegration.h"
#include "NativeSkyCube.h"
#include "ManualFpsLog.h"
#include "CloudComparison.h"
#include "PrivateProfile.h"
#include "McmSettings.h"
#include "Overlay.h"
#include "RuntimeAPI.h"
#include "ShaderTools/DFLightDescriptor.h"
#include "ShaderTools/SkyCloudMrtPatcher.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <ShlObj.h>
#include <string_view>
#include <TlHelp32.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

// The vcpkg-built detours release static library contains an unexpected
// reference to _CrtDbgReport (debug CRT). Provide a no-op stub so Release
// links without pulling ucrtd.lib.
extern "C" int __cdecl _CrtDbgReport(
    int /*reportType*/, const char* /*filename*/, int /*linenumber*/,
    const char* /*moduleName*/, const char* /*format*/, ...)
{
    return 0;
}

namespace
{
    [[nodiscard]] const char* RuntimeName(
        FO4CS::F4SECompat::RuntimeTarget target) noexcept
    {
        using FO4CS::F4SECompat::RuntimeTarget;
        switch (target) {
        case RuntimeTarget::kLegacy:
            return "OG 1.10.163";
        case RuntimeTarget::kAE:
            return "AE 1.11.240";
        case RuntimeTarget::kVR:
            return "VR 1.2.72";
        default:
            return "unsupported";
        }
    }

    [[nodiscard]] bool InitializePluginLogger(
        FO4CS::F4SECompat::RuntimeTarget target) noexcept
    {
        try {
            std::filesystem::path logDirectory;
            PWSTR documents = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(
                    FOLDERID_Documents, 0, nullptr, &documents)) &&
                documents) {
                logDirectory = documents;
                CoTaskMemFree(documents);
                logDirectory /= L"My Games";
                logDirectory /=
                    target == FO4CS::F4SECompat::RuntimeTarget::kVR ?
                        L"Fallout4VR" : L"Fallout4";
                logDirectory /= L"F4SE";
            } else {
                if (documents)
                    CoTaskMemFree(documents);
                const auto host =
                    FO4CS::RuntimeAPI::GetSingleton().Host();
                logDirectory = host.executablePath.parent_path() /
                    L"Data" / L"F4SE" / L"Plugins";
            }
            std::filesystem::create_directories(logDirectory);
            auto pluginLogger = spdlog::basic_logger_mt(
                "FO4CloudShadowsStandalone",
                (logDirectory / L"FO4CloudShadows.log").string(), true);
            pluginLogger->set_level(spdlog::level::info);
            pluginLogger->set_pattern("[%T.%e] [%L] %v");
            spdlog::set_default_logger(std::move(pluginLogger));
            spdlog::flush_on(spdlog::level::warn);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool WriteProtectedPointer(
        std::uintptr_t* destination,
        std::uintptr_t value) noexcept
    {
        if (!destination ||
            reinterpret_cast<std::uintptr_t>(destination) %
                alignof(std::uintptr_t) != 0) {
            return false;
        }
        DWORD oldProtection = 0;
        if (!VirtualProtect(
                destination, sizeof(*destination), PAGE_READWRITE,
                &oldProtection)) {
            return false;
        }
        std::atomic_ref<std::uintptr_t>(*destination).store(
            value, std::memory_order_release);
        DWORD discarded = 0;
        return VirtualProtect(
                   destination, sizeof(*destination), oldProtection,
                   &discarded) != FALSE;
    }

    bool IsExecutableMainModuleAddress(uintptr_t address) noexcept
    {
        auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
        if (!module || !address)
            return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;
        const auto* section = IMAGE_FIRST_SECTION(nt);
        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            const uintptr_t begin = base + section->VirtualAddress;
            const uintptr_t end = begin + (std::max)(section->Misc.VirtualSize, section->SizeOfRawData);
            if (address >= begin && address < end)
                return (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
        return false;
    }

    // Detours suspends every thread passed to DetourUpdateThread until the
    // transaction commits or aborts.  Keep the real thread handles alive for
    // that entire interval; closing one while Detours still owns its suspended
    // context is invalid.  Runtime driver-entry rebinding can happen after
    // worker/overlay threads exist, so enlisting only the render thread is not
    // safe when an old prologue may be executing elsewhere.
    class DetourThreadEnlistment
    {
    public:
        ~DetourThreadEnlistment()
        {
            for (HANDLE thread : threads_)
                CloseHandle(thread);
        }

        DetourThreadEnlistment(const DetourThreadEnlistment&) = delete;
        DetourThreadEnlistment& operator=(const DetourThreadEnlistment&) = delete;
        DetourThreadEnlistment() = default;

        LONG EnlistProcessThreads() noexcept
        {
            LONG error = DetourUpdateThread(GetCurrentThread());
            if (error != NO_ERROR)
                return error;

            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return static_cast<LONG>(GetLastError());

            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry)) {
                error = static_cast<LONG>(GetLastError());
                CloseHandle(snapshot);
                return error;
            }

            const DWORD processId = GetCurrentProcessId();
            const DWORD currentThreadId = GetCurrentThreadId();
            do {
                if (entry.th32OwnerProcessID != processId ||
                    entry.th32ThreadID == currentThreadId) {
                    continue;
                }

                HANDLE thread = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                        THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, entry.th32ThreadID);
                if (!thread) {
                    const DWORD openError = GetLastError();
                    // A thread can legitimately exit after the snapshot.
                    if (openError == ERROR_INVALID_PARAMETER)
                        continue;
                    error = static_cast<LONG>(openError);
                    break;
                }

                // Collect every handle before suspending anything. This keeps
                // vector growth and snapshot enumeration out of the suspended
                // interval.
                threads_.push_back(thread);
            } while (Thread32Next(snapshot, &entry));

            if (error == NO_ERROR) {
                const DWORD enumerationError = GetLastError();
                if (enumerationError != ERROR_NO_MORE_FILES)
                    error = static_cast<LONG>(enumerationError);
            }
            CloseHandle(snapshot);
            if (error != NO_ERROR)
                return error;

            for (HANDLE thread : threads_) {
                error = DetourUpdateThread(thread);
                if (error != NO_ERROR)
                    return error;
            }
            return error;
        }

    private:
        std::vector<HANDLE> threads_;
    };

    struct IDXGISwapChain_Present;
    struct ID3D11Device_CreateVertexShader;
    struct ID3D11Device_CreatePixelShader;

    // Aliases — all capture goes through CloudShadows:: so CloudShadows.h
    // inlines (GetD3DDevice/GetD3DContext) see the same pointers without
    // cross-TU coupling.
    auto& g_d3dDevice  = CloudShadows::g_capturedDevice;
    auto& g_d3dContext = CloudShadows::g_capturedContext;
    auto& g_swapChain  = CloudShadows::g_capturedSwapChain;

    using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
    using CreateVertexShaderFn = HRESULT(WINAPI*)(
        ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
        ID3D11VertexShader**);
    using CreatePixelShaderFn = HRESULT(WINAPI*)(
        ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
        ID3D11PixelShader**);
    std::mutex g_comHookMutex;
    std::unordered_map<std::uintptr_t*, PresentFn> g_presentOriginals;
    std::unordered_map<std::uintptr_t*, CreateVertexShaderFn> g_createVSOriginals;
    std::unordered_map<std::uintptr_t*, CreatePixelShaderFn> g_createPSOriginals;
    std::unordered_set<std::uintptr_t*> g_presentOuterHooksObserved;
    // Present is intercepted with an inline Detours hook on the stock dxgi.dll
    // implementation rather than a vtable slot write. Steam's overlay
    // (gameoverlayrenderer64) re-hooks the swap-chain vtable after plugins and
    // records the previous slot as its "next" while also inline-patching the
    // dxgi function; a vtable thunk whose downstream is that function then
    // loops overlay -> thunk -> overlay until the stack overflows (observed on
    // 1.11.240 launched under Steam, 1083 nested frames). A code detour has a
    // private trampoline into the real body, so no ordering can form a cycle.
    PresentFn g_presentDetourOriginal = nullptr;     // Detours trampoline
    std::atomic<std::uintptr_t> g_presentDetourTarget{ 0 }; // stock dxgi Present
    std::unordered_set<std::uintptr_t*> g_createVSOuterHooksObserved;
    std::unordered_set<std::uintptr_t*> g_createPSOuterHooksObserved;

    constexpr GUID kPresentRouteMarker{
        0x4f5f1c91, 0xa3de, 0x47a2,
        { 0xb0, 0x5d, 0x53, 0x89, 0xcb, 0x38, 0x12, 0x45 }
    };
    constexpr GUID kCreatePSRouteMarker{
        0xd10c70a4, 0x6b45, 0x4bf1,
        { 0x8c, 0x14, 0xe9, 0xa4, 0x3f, 0x72, 0x90, 0x63 }
    };
    constexpr GUID kCreateVSRouteMarker{
        0x225179e7, 0xdf65, 0x4bbf,
        { 0xa6, 0xe4, 0x42, 0xe6, 0x95, 0x96, 0x93, 0x6b }
    };
    // The stock cloud shader owns this private interface. D3D therefore
    // releases the cubemap-only variant with the vanilla object and pointer
    // reuse cannot alias a stale replacement in a process-global map.
    constexpr GUID kSkyCloudMrtShaderMarker{
        0x1c5f0f4d, 0x61de, 0x4acb,
        { 0x9d, 0x17, 0x26, 0x3b, 0xb7, 0x74, 0xe8, 0x51 }
    };
    std::atomic<uint32_t> g_nativeCloudMrtShaderCount{ 0 };
    // The native capture routes are armed only after all intercepted context
    // entry points are current. Passive capture never suppresses Fallout's
    // primary clear or any draw; authenticated cloud draws append private RT1
    // while preserving the stock colour output at RT0.
    std::atomic<bool> g_nativePrimaryClearHookReady{ false };

    [[nodiscard]] ID3D11PixelShader* AcquireNativeCloudMrtShader(
        ID3D11PixelShader* stockShader) noexcept
    {
        if (!stockShader)
            return nullptr;
        ID3D11PixelShader* patchedShader = nullptr;
        UINT byteCount = sizeof(patchedShader);
        if (FAILED(stockShader->GetPrivateData(
                kSkyCloudMrtShaderMarker, &byteCount, &patchedShader)) ||
            byteCount != sizeof(patchedShader) || !patchedShader) {
            if (patchedShader)
                patchedShader->Release();
            return nullptr;
        }
        // GetPrivateData AddRefs an interface installed through
        // SetPrivateDataInterface; ownership transfers to the caller.
        return patchedShader;
    }
    constexpr uint32_t kComRouteMagic = 0x46534353u;  // "FSCS"
    constexpr uint32_t kComRouteVersion = 1;

    struct ComRouteData {
        uint32_t magic{ kComRouteMagic };
        uint32_t version{ kComRouteVersion };
        std::uintptr_t downstream{ 0 };
    };
    static_assert(std::is_trivially_copyable_v<ComRouteData>);

    // True for a d3d11.dll/dxgi.dll proxy loaded from the game folder (ENB and
    // similar wrappers). Such a proxy owns the game-visible swap chain and calls
    // the real Present itself, so its Present is the per-frame boundary.
    [[nodiscard]] bool ClassifyGameDirectoryGraphicsProxy(HMODULE module) noexcept
    {
        if (!module)
            return false;
        std::wstring modulePath(1024, L'\0');
        std::wstring gamePath(1024, L'\0');
        const DWORD moduleLength = GetModuleFileNameW(
            module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        const DWORD gameLength = GetModuleFileNameW(
            nullptr, gamePath.data(), static_cast<DWORD>(gamePath.size()));
        if (!moduleLength || moduleLength >= modulePath.size() ||
            !gameLength || gameLength >= gamePath.size())
            return false;
        modulePath.resize(moduleLength);
        gamePath.resize(gameLength);
        try {
            const fs::path proxy(modulePath);
            const auto name = proxy.filename().wstring();
            if (_wcsicmp(name.c_str(), L"d3d11.dll") != 0 &&
                _wcsicmp(name.c_str(), L"dxgi.dll") != 0)
                return false;
            std::error_code error;
            const bool sameDirectory = fs::equivalent(
                proxy.parent_path(), fs::path(gamePath).parent_path(), error);
            return !error && sameDirectory;
        } catch (...) {
            return false;
        }
    }

    // Classified once per module: this check runs on the render thread from
    // every RendererData maintenance tick (through ReadComRoute), and the
    // file-system work in ClassifyGameDirectoryGraphicsProxy must not repeat.
    [[nodiscard]] bool IsGameDirectoryGraphicsProxy(HMODULE module) noexcept
    {
        if (!module)
            return false;
        static std::mutex cacheMutex;
        static std::array<std::pair<HMODULE, bool>, 8> cache{};
        static std::size_t cacheCount = 0;
        {
            std::lock_guard lock(cacheMutex);
            for (std::size_t index = 0; index < cacheCount; ++index) {
                if (cache[index].first == module)
                    return cache[index].second;
            }
        }
        const bool proxy = ClassifyGameDirectoryGraphicsProxy(module);
        std::lock_guard lock(cacheMutex);
        if (cacheCount < cache.size())
            cache[cacheCount++] = { module, proxy };
        return proxy;
    }

    // Behind a graphics proxy (ENB) the device created inside the proxy is
    // expected to be refused while the game-visible device is hooked; report
    // that case at INFO so logs do not show a false error.
    [[nodiscard]] spdlog::level::level_enum ShaderInterceptorLogLevel() noexcept
    {
        return IsGameDirectoryGraphicsProxy(GetModuleHandleW(L"d3d11.dll"))
            ? spdlog::level::info
            : spdlog::level::err;
    }

    bool IsExecutableAddressInModule(
        std::uintptr_t address,
        const wchar_t* moduleName,
        HMODULE* owner = nullptr) noexcept
    {
        if (!address || !moduleName)
            return false;

        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address),
                &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT) {
            return false;
        }

        const DWORD protection = memory.Protect & 0xffu;
        const bool executable =
            protection == PAGE_EXECUTE ||
            protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
        if (!executable)
            return false;

        const auto allocationModule =
            static_cast<HMODULE>(memory.AllocationBase);
        if (owner)
            *owner = allocationModule;
        const HMODULE expectedModule = GetModuleHandleW(moduleName);
        if (expectedModule && allocationModule == expectedModule)
            return true;
        // Deliberately no system-directory fallback: ENB wraps the device and
        // forwards Set/GetPrivateData to the wrapped device. Hooking the wrapped
        // device's shared system vtable as well made both routes point at ENB's
        // wrapper and recurse (access violation in ENB's CreateVertexShader,
        // 10 Sep 2026). The game-visible (outermost) device vtable is the hook.
        // A game-folder graphics proxy that owns the swap chain's Present (ENB)
        // is the frame boundary the game actually calls; accept it as the
        // Present implementation to detour. Release generalisation of the
        // private-profile pinned-ENB exception below.
        if (std::wstring_view(moduleName) == L"dxgi.dll" &&
            IsGameDirectoryGraphicsProxy(allocationModule))
            return true;
        return std::wstring_view(moduleName) == L"dxgi.dll" &&
            FO4CS::PrivateProfile::IsPinnedEnbPresent(address, allocationModule);
    }

    [[nodiscard]] bool IsExecutableAddress(
        std::uintptr_t address) noexcept
    {
        if (!address)
            return false;
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(address),
                &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        const DWORD protection = memory.Protect & 0xffu;
        return protection == PAGE_EXECUTE ||
            protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
    }

    template <class TObject>
    [[nodiscard]] bool ReadValidatedComVTable(
        TObject* object,
        std::size_t lastRequiredSlot,
        std::uintptr_t** output) noexcept
    {
        if (output)
            *output = nullptr;
        if (!object || !output)
            return false;
        auto& runtime = FO4CS::RuntimeAPI::GetSingleton();
        const auto objectAddress = reinterpret_cast<std::uintptr_t>(object);
        if (!runtime.ValidateMemory(
                objectAddress, sizeof(std::uintptr_t),
                FO4CS::AddressKind::kReadable)) {
            return false;
        }
        std::uintptr_t* vtable = nullptr;
        std::memcpy(&vtable, object, sizeof(vtable));
        if (!vtable || lastRequiredSlot >=
                (std::numeric_limits<std::size_t>::max)() /
                    sizeof(std::uintptr_t) ||
            !runtime.ValidateMemory(
                reinterpret_cast<std::uintptr_t>(vtable),
                (lastRequiredSlot + 1u) * sizeof(std::uintptr_t),
                FO4CS::AddressKind::kReadable)) {
            return false;
        }
        // IUnknown::QueryInterface/AddRef/Release and the requested method must
        // all be callable before we retain or patch this engine-supplied COM
        // pointer. Intermediate D3D slots are not consumed here.
        for (const std::size_t slot : {
                 std::size_t{ 0 }, std::size_t{ 1 }, std::size_t{ 2 },
                 lastRequiredSlot }) {
            if (!IsExecutableAddress(vtable[slot]))
                return false;
        }
        *output = vtable;
        return true;
    }

    template <class TFunction, class TObject>
    TFunction ReadComRoute(
        TObject* object,
        const GUID& key,
        std::uintptr_t thunk,
        const wchar_t* moduleName) noexcept
    {
        if (!object)
            return nullptr;

        ComRouteData route{};
        UINT size = sizeof(route);
        if (FAILED(object->GetPrivateData(key, &size, &route)) ||
            size != sizeof(route) || route.magic != kComRouteMagic ||
            route.version != kComRouteVersion || !route.downstream ||
            route.downstream == thunk ||
            !IsExecutableAddressInModule(route.downstream, moduleName)) {
            return nullptr;
        }
        return reinterpret_cast<TFunction>(route.downstream);
    }

    template <class TObject, class TFunction>
    bool SetComRoute(
        TObject* object,
        const GUID& key,
        TFunction downstream,
        std::uintptr_t thunk,
        const wchar_t* moduleName) noexcept
    {
        const auto address = reinterpret_cast<std::uintptr_t>(downstream);
        if (!object || !address || address == thunk ||
            !IsExecutableAddressInModule(address, moduleName)) {
            return false;
        }
        const ComRouteData route{ kComRouteMagic, kComRouteVersion, address };
        return SUCCEEDED(object->SetPrivateData(key, sizeof(route), &route));
    }

    inline std::atomic<bool> g_createVSHookInstalled{ false };
    inline std::atomic<bool> g_createPSHookInstalled{ false };
    inline std::atomic<std::uint32_t> g_hookUnavailableMask{
        FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable |
        FO4CloudShadowsMenuBridge::kPresentHookUnavailable |
        FO4CloudShadowsMenuBridge::kCreateVertexShaderHookUnavailable |
        FO4CloudShadowsMenuBridge::kCreatePixelShaderHookUnavailable |
        FO4CloudShadowsMenuBridge::kDrawHookUnavailable |
        FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable
    };

    void SetHookAvailable(std::uint32_t bit, bool available) noexcept
    {
        if (available)
            g_hookUnavailableMask.fetch_and(~bit, std::memory_order_release);
        else
            g_hookUnavailableMask.fetch_or(bit, std::memory_order_release);
    }

    // Strong ownership is retained for every raw renderer pointer published
    // through CloudShadows.h. Publication/replacement happens on the render
    // thread; auxiliary device-creation callbacks never replace this trio.
    std::mutex g_rendererBindingMutex;
    ComPtr<ID3D11Device> g_ownedRendererDevice;
    ComPtr<ID3D11DeviceContext> g_ownedRendererContext;
    ComPtr<IDXGISwapChain> g_ownedRendererSwapChain;
    std::atomic<ID3D11Device*> g_publishedRendererDevice{ nullptr };
    std::atomic<ID3D11DeviceContext*> g_publishedRendererContext{ nullptr };
    std::atomic<IDXGISwapChain*> g_publishedRendererSwapChain{ nullptr };
    // Loading screens and gameplay can Present on different threads. A worker
    // BeginTechnique must never permanently claim ownership of menu/settings
    // mutations. Only the authenticated main-chain Present scope owns them.
    thread_local uint32_t g_presentBoundaryDepth = 0;
    struct ScopedPresentBoundary
    {
        ScopedPresentBoundary() noexcept { ++g_presentBoundaryDepth; }
        ~ScopedPresentBoundary() { --g_presentBoundaryDepth; }
    };

    [[nodiscard]] bool IsRenderThread() noexcept
    {
        return g_presentBoundaryDepth != 0;
    }

    struct BridgeSettingsState
    {
        std::mutex mutex;
        CloudShadows::Settings pending{};
        bool pendingEnabled{ true };
        bool pendingResetGodrayOcclusion{ false };
        bool hasPending{ false };
        FO4CloudShadowsMenuBridge::SettingsV1 snapshot{};
        bool snapshotInitialized{ false };
    };
    BridgeSettingsState g_bridgeSettings;

    [[nodiscard]] FO4CloudShadowsMenuBridge::SettingsV1 MakeBridgeSettingsSnapshot(
        const CloudShadows::Settings& source,
        bool enabled) noexcept
    {
        using FO4CloudShadowsMenuBridge::SettingsV1;
        return SettingsV1{
            .structSize = sizeof(SettingsV1),
            .abiVersion = FO4CloudShadowsMenuBridge::kAbiVersion,
            .enabled = enabled ? 1u : 0u,
            .reserved = 0u,
            .opacity = source.Opacity,
            .cloudHeight = source.CloudHeight,
            .layerHeightStep = source.LayerHeightStep,
            .worldTileSize = source.WorldTileSize,
            .layerScaleMultiplier = source.LayerScaleMultiplier,
            .verticalOpticalDepth = source.VerticalOpticalDepth,
            .sunAngularRadius = source.SunAngularRadius,
            .maxOpticalSlant = source.MaxOpticalSlant,
            .baseMipBias = source.BaseMipBias,
            .sunFadeStart = source.SunFadeStart,
            .sunFadeEnd = source.SunFadeEnd,
            .debugMode = source.DebugMode,
            .maxLayers = source.MaxLayers,
        };
    }

    void RefreshBridgeSettingsSnapshotOnRenderThread() noexcept
    {
        if (!IsRenderThread())
            return;
        const auto snapshot = MakeBridgeSettingsSnapshot(
            CloudShadows::g_settings,
            CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire));
        std::lock_guard lock(g_bridgeSettings.mutex);
        g_bridgeSettings.snapshot = snapshot;
        g_bridgeSettings.snapshotInitialized = true;
    }

    void ApplyPendingBridgeSettingsOnRenderThread() noexcept
    {
        if (!IsRenderThread())
            return;
        CloudShadows::Settings pending{};
        bool enabled = true;
        bool apply = false;
        bool resetGodrayOcclusion = false;
        {
            std::lock_guard lock(g_bridgeSettings.mutex);
            if (g_bridgeSettings.hasPending) {
                pending = g_bridgeSettings.pending;
                enabled = g_bridgeSettings.pendingEnabled;
                resetGodrayOcclusion = g_bridgeSettings.pendingResetGodrayOcclusion;
                g_bridgeSettings.pendingResetGodrayOcclusion = false;
                g_bridgeSettings.hasPending = false;
                apply = true;
            }
        }
        if (apply) {
            if (resetGodrayOcclusion)
                FO4CS::GodraysIntegration::SetCloudOcclusionEnabled(false);
            const bool wasEnabled = CloudShadows::g_shadowsEnabled.load(
                std::memory_order_acquire);
            CloudShadows::g_settings = pending;
            CloudShadows::g_shadowsEnabled.store(
                enabled, std::memory_order_release);
            if (wasEnabled != enabled)
                CloudShadows::InvalidateWorldCloudCaptureForToggle();
        }
        RefreshBridgeSettingsSnapshotOnRenderThread();
    }

    enum class TechniqueKind : uint8_t {
        kOther,
        kDFLight,
        kDFPrepass,
        kDFComposite,
        kSky
    };

    // BeginTechnique and immediate-context draws execute serially on the
    // render thread. Keeping phase state TLS prevents deferred worker draws
    // from observing a process-global DFLight/Sky classification.
    struct TechniqueState {
        TechniqueKind kind{ TechniqueKind::kOther };
        uint32_t skyTechnique{ 0 };
        uint32_t dfLightPixelDescriptor{ 0 };
        uint64_t captureEpoch{ 0 };
        bool sunShaderValidated{ false };
        bool captureEpochStarted{ false };
        bool skyShadersAuthenticated{ false };
        ComPtr<ID3D11PixelShader> dfLightVanillaPS;
        ComPtr<ID3D11PixelShader> dfLightPatchedPS;
        ComPtr<ID3D11VertexShader> skyVertexShader;
        ComPtr<ID3D11PixelShader> skyPixelShader;
    };
    thread_local TechniqueState g_techniqueState;
    thread_local bool g_worldRenderPhaseActive = false;
    thread_local uint32_t g_internalDrawDepth = 0;
    thread_local uint32_t g_nativeBeginTechniqueDepth = 0;

    [[nodiscard]] constexpr bool IsDirectionalSunDescriptor(
        uint32_t pixelDescriptor) noexcept
    {
        return SIE::IsPotentialDirectionalSunDescriptor(pixelDescriptor);
    }

    struct ScopedInternalDraw {
        ScopedInternalDraw() { ++g_internalDrawDepth; }
        ~ScopedInternalDraw() { --g_internalDrawDepth; }
        ScopedInternalDraw(const ScopedInternalDraw&) = delete;
        ScopedInternalDraw& operator=(const ScopedInternalDraw&) = delete;
    };

    inline std::atomic<uint64_t> g_captureEpochCounter{ 0 };
    inline std::atomic<uint32_t> g_dfLightSwapHits{0};
    inline std::atomic<uint32_t> g_dfLightMissDuringPhase{0};
	inline std::atomic<uint64_t> g_actualDfLightSwapHits{ 0 };
	inline std::atomic<uint64_t> g_lastActualDfLightShaderHash{ 0 };
	inline std::atomic<uint32_t> g_lastActualDfLightPixelDescriptor{ 0 };

    bool IsCapturedImmediateContext(ID3D11DeviceContext* ctx) noexcept
    {
        // RendererData::context is the reviewed immediate-context field on all
        // three supported runtimes. Pointer identity avoids a virtual GetType
        // call on a stale/non-authoritative worker context in this hot path.
        return ctx && ctx == g_publishedRendererContext.load(
                                  std::memory_order_acquire);
    }

    thread_local uint64_t g_nativeIntegrationSerial = 0;
    thread_local uint32_t g_nativeCaptureFailedFaceMask = 0;
    thread_local uint32_t g_nativeCapturePreparedFaceMask = 0;
    // Distinguishes a genuinely cloudless native face from a cloud draw which
    // reached the hook but failed authentication/MRT setup. A clear face is a
    // valid all-zero mapping; a rejected cloud candidate is not.
    thread_local uint32_t g_nativeCaptureCloudCandidateFaceMask = 0;

    enum class NativeFaceRejectReason : std::uint32_t {
        kMissingRT0,
        kRT1Occupied,
        kConsumerPrepare,
        kLifecycleAuthentication,
        kCount
    };
    std::array<std::atomic<std::uint64_t>,
        static_cast<std::size_t>(NativeFaceRejectReason::kCount)>
        g_nativeFaceRejectCounts{};

    void LogNativeFaceReject(
        NativeFaceRejectReason reason,
        std::uint32_t faceMask,
        std::uint64_t serial,
        std::uint32_t preparedMask,
        std::uint32_t failedMask) noexcept
    {
        const auto index = static_cast<std::size_t>(reason);
        const auto count = g_nativeFaceRejectCounts[index].fetch_add(
            1, std::memory_order_relaxed) + 1u;
        if (count > 4u && (count & (count - 1u)) != 0u)
            return;

        const char* name = "unknown";
        switch (reason) {
        case NativeFaceRejectReason::kMissingRT0:
            name = "missing-rt0";
            break;
        case NativeFaceRejectReason::kRT1Occupied:
            name = "rt1-occupied";
            break;
        case NativeFaceRejectReason::kConsumerPrepare:
            name = "consumer-prepare";
            break;
        case NativeFaceRejectReason::kLifecycleAuthentication:
            name = "lifecycle-authentication";
            break;
        default:
            break;
        }
        SPDLOG_WARN(
            "[CloudShadows] Native face rejected reason={} count={} "
            "faceMask=0x{:02X} serial={} prepared=0x{:02X} failed=0x{:02X}",
            name, count, faceMask, serial, preparedMask, failedMask);
    }

    void MarkNativeCaptureFaceFailed(uint32_t faceIndex) noexcept
    {
        if (faceIndex < CloudShadows::kWorldCloudCubeFaceCount)
            g_nativeCaptureFailedFaceMask |= 1u << faceIndex;
    }

    [[nodiscard]] bool EnsureNativeCaptureFacePrepared(
        ID3D11DeviceContext* context,
        uint32_t faceIndex) noexcept
    {
        if (!IsCapturedImmediateContext(context) ||
            faceIndex >= CloudShadows::kWorldCloudCubeFaceCount)
            return false;
        const uint32_t faceBit = 1u << faceIndex;
        if ((g_nativeCapturePreparedFaceMask & faceBit) != 0)
            return true;
        std::array<ID3D11RenderTargetView*, 2> targets{};
        context->OMGetRenderTargets(
            static_cast<UINT>(targets.size()), targets.data(), nullptr);
        ComPtr<ID3D11RenderTargetView> nativeFace;
        ComPtr<ID3D11RenderTargetView> occupiedTarget1;
        nativeFace.Attach(targets[0]);
        occupiedTarget1.Attach(targets[1]);
        if (!nativeFace || occupiedTarget1) {
            MarkNativeCaptureFaceFailed(faceIndex);
            LogNativeFaceReject(
                !nativeFace ? NativeFaceRejectReason::kMissingRT0 :
                    NativeFaceRejectReason::kRT1Occupied,
                faceBit, FO4CS::NativeSkyCube::ActiveCaptureSerial(),
                g_nativeCapturePreparedFaceMask,
                g_nativeCaptureFailedFaceMask);
            return false;
        }

        const bool prepared =
            CloudShadows::PrepareNativeWorldCloudCaptureFaces(
                context, nativeFace.Get(), faceBit,
                FO4CS::NativeSkyCube::ActiveCaptureSerial());
        if (prepared)
            g_nativeCapturePreparedFaceMask |= faceBit;
        else {
            MarkNativeCaptureFaceFailed(faceIndex);
            LogNativeFaceReject(
                NativeFaceRejectReason::kConsumerPrepare,
                faceBit, FO4CS::NativeSkyCube::ActiveCaptureSerial(),
                g_nativeCapturePreparedFaceMask,
                g_nativeCaptureFailedFaceMask);
        }
        return prepared;
    }

    void ObserveNativeCubeDraw(ID3D11DeviceContext* context) noexcept
    {
        // This hook sees every game draw. Test the thread-local capture flag
        // first so ordinary gameplay never pays an authoritative-context
        // atomic lookup for native-cube bookkeeping.
        if (!FO4CS::NativeSkyCube::IsCaptureActive() ||
            g_internalDrawDepth != 0 ||
            !IsCapturedImmediateContext(context)) {
            return;
        }
        const uint32_t activeMask =
            FO4CS::NativeSkyCube::ActiveFaceMask();
        if (activeMask != 0 &&
            (g_nativeCapturePreparedFaceMask & activeMask) == activeMask) {
            return;
        }
        const uint32_t face =
            FO4CS::NativeSkyCube::ResolveActiveFace(context);
        if (face < CloudShadows::kWorldCloudCubeFaceCount)
            (void)EnsureNativeCaptureFacePrepared(context, face);
    }

    void OnNativeSkyCubeFaceLifecycle(
        const FO4CS::NativeSkyCube::FaceLifecycleEvent& event) noexcept
    {
        FO4CS::RendererLifetime::Scope rendererLifetime;
        auto* context = g_publishedRendererContext.load(
            std::memory_order_acquire);
        const uint32_t faceMask = event.faceMask &
            CloudShadows::kCompleteWorldCloudCubeFaceMask;
        if (!IsCapturedImmediateContext(context) || faceMask == 0)
            return;

        if (event.phase == FO4CS::NativeSkyCube::FacePhase::kBegin) {
            if (event.captureSerial != g_nativeIntegrationSerial) {
                g_nativeIntegrationSerial = event.captureSerial;
                g_nativeCaptureFailedFaceMask = 0;
                g_nativeCapturePreparedFaceMask = 0;
                g_nativeCaptureCloudCandidateFaceMask = 0;
            }
            // Once the runtime face contract has been learned, this clears
            // even genuinely cloudless faces before their native draw. The
            // first-ever face is prepared lazily from its authoritative RT0.
            if (CloudShadows::PrepareNativeWorldCloudCaptureFaces(
                    context, nullptr, faceMask, event.captureSerial)) {
                g_nativeCapturePreparedFaceMask |= faceMask;
            }
            return;
        }

        const uint32_t preparedFaceMask =
            g_nativeCapturePreparedFaceMask & faceMask;
        const bool cloudCandidateObserved =
            (g_nativeCaptureCloudCandidateFaceMask & faceMask) != 0;
        const bool authenticated = event.producerAuthenticated &&
            cloudCandidateObserved && event.cloudDrawObserved &&
            preparedFaceMask == faceMask &&
            (g_nativeCaptureFailedFaceMask & faceMask) == 0;
        if (!authenticated) {
            LogNativeFaceReject(
                NativeFaceRejectReason::kLifecycleAuthentication,
                faceMask, event.captureSerial, preparedFaceMask,
                g_nativeCaptureFailedFaceMask);
        }
        const bool accepted =
            CloudShadows::CompleteNativeWorldCloudCaptureFaces(
                context, faceMask, event.captureSerial, authenticated);
        if (!FO4CS::NativeSkyCube::AcknowledgeFaceCompletion(
                faceMask, event.captureSerial, accepted)) {
            SPDLOG_ERROR(
                "[CloudShadows] Native face-completion acknowledgement "
                "rejected (faceMask=0x{:02X}, serial={}, accepted={})",
                faceMask, event.captureSerial, accepted);
        }
        g_nativeCaptureFailedFaceMask &= ~faceMask;
        g_nativeCaptureCloudCandidateFaceMask &= ~faceMask;
    }

    void FinalizeFrameAtPresent() noexcept
    {
        auto& phase = g_techniqueState;
        // DFLight normally commits and closes the Sky epoch. Clear-sky,
        // menu, and unusual renderer paths may omit DFLight, so Present is
        // the authoritative frame boundary and must publish that epoch too.
        // A lighting technique closes the Sky phase even when its shader is
        // unsupported or the feature is disabled. Commit by advertised epoch,
        // not by the open-phase bit, so clear-sky/weather changes cannot stall.
        auto* publishedContext = g_publishedRendererContext.load(
            std::memory_order_acquire);
        if (IsCapturedImmediateContext(publishedContext))
            CloudShadows::CommitWorldCloudFrameAtPresent(publishedContext);
        phase = {};
        g_worldRenderPhaseActive = false;
        const uint64_t completedDispatchStamp =
            CloudShadows::g_shadowMaskSuccessStamp.exchange(
                0, std::memory_order_acq_rel);
        CloudShadows::g_shadowMaskValid.store(
            false, std::memory_order_release);
        auto* publishedDevice = g_publishedRendererDevice.load(
            std::memory_order_acquire);
        const bool deviceHealthy = publishedDevice &&
            SUCCEEDED(publishedDevice->GetDeviceRemovedReason());
        if (!deviceHealthy) {
            CloudShadows::InvalidateShadowMaskState();
            return;
        }
        // Do not compare the dispatch stamp with the epoch published above:
        // a valid ordering is DFLight(E), Sky capture(E+1), Present commit(E+1).
        CloudShadows::g_lastCompletedShadowMaskValid.store(
            completedDispatchStamp != 0, std::memory_order_release);
    }

    struct IDXGISwapChain_Present
    {
        static HRESULT WINAPI thunk(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
        {
            FO4CS::RendererLifetime::Scope rendererLifetime;
            // The Detours trampoline is the only valid continuation: the vtable
            // slot and the object's route pointer now both lead back to this
            // detour through the patched function entry.
            PresentFn original = g_presentDetourOriginal;
            if (!original)
                return DXGI_ERROR_INVALID_CALL;
            // Nested arrival on the same thread (another interceptor calling
            // Present from inside Present) is forwarded without frame work.
            static thread_local unsigned depth = 0;
            struct DepthGuard {
                unsigned& value; explicit DepthGuard(unsigned& v) : value(v) { ++value; }
                ~DepthGuard() { --value; }
            } depthGuard(depth);
            if (depth > 1)
                return original(This, SyncInterval, Flags);

            // DXGI vtables are shared by multiple swap-chain instances.  Only
            // the renderer's authoritative chain defines the main frame
            // boundary, and a TEST present explicitly does not present a frame.
            if (!This || This != g_publishedRendererSwapChain.load(
                                     std::memory_order_acquire) ||
                (Flags & DXGI_PRESENT_TEST) != 0)
                return original(This, SyncInterval, Flags);

            ScopedPresentBoundary boundary;
            // Attribute this already-rendered frame before F10 or a settings
            // reload changes the state for the NEXT frame.
            const bool fpsFrameEnabled = CloudShadows::g_shadowsEnabled.load(
                std::memory_order_acquire);
            // A later rejected secondary-view attempt does not undo a mask
            // already dispatched for this main frame. Use the frame latch.
            const bool fpsMaskValid = CloudShadows::g_shadowMaskSuccessStamp.load(
                std::memory_order_acquire) != 0;
            static thread_local uint64_t fpsPreviousSwapHits = 0;
            const auto fpsSwapHits = g_actualDfLightSwapHits.load(std::memory_order_acquire);
            const bool fpsLightingApplied = fpsSwapHits != fpsPreviousSwapHits;
            fpsPreviousSwapHits = fpsSwapHits;
            FO4CS::CloudComparison::EndGpuFrame(CloudShadows::GetD3DContext(),
                fpsMaskValid && fpsLightingApplied && CloudShadows::g_settings.DebugMode == 0.0f);
            ApplyPendingBridgeSettingsOnRenderThread();
            CloudShadows::PollShadowToggle();
            FO4CS::CloudComparison::PollControls();
            static uint32_t presentCount = 0;
            ++presentCount;
            if (!CloudShadows::g_initialized) {
                CloudShadows::Initialize();
            }
            // Main Sky now bootstraps its own geometry mapping. Keep native
            // reflection hooks as observation/secondary-camera guards only;
            // two producers must never share one resolver staging generation.
            FO4CS::NativeSkyCube::SetCaptureConsumerReady(false);
            if ((presentCount % 600) == 0) {
                const auto nativeCube =
                    FO4CS::NativeSkyCube::GetDiagnostics();
                SPDLOG_INFO(
                    "[CloudShadows] Present #{} init={} CS={} "
                    "attemptMaskValid={} frameDispatchStamp={} "
                    "nativeInstalled={} nativeReady={} nativeCubes={} "
                    "nativeFaces={}/{} nativeRejected={} "
                    "nativeReflUpdates={}/{} nativeSettle={} "
                    "nativeClicksSky={}/{} geometryMapping={}/{}/{} draws={}",
                    presentCount, CloudShadows::g_initialized,
                    (void*)CloudShadows::g_cloudShadowProductionCS,
                    CloudShadows::g_shadowMaskValid.load(std::memory_order_acquire),
                    CloudShadows::g_shadowMaskSuccessStamp.load(
                        std::memory_order_acquire),
                    nativeCube.installed,
                    nativeCube.captureConsumerReady,
                    nativeCube.completedCubeCount,
                    nativeCube.cloudDrawFaceCount,
                    nativeCube.consumerAcknowledgedFaceCount,
                    nativeCube.consumerRejectedFaceCount,
                    // eligible/total natural reflection updates, then natural
                    // cube clicks that carried the sky / that excluded it.
                    nativeCube.naturalUpdateCount,
                    nativeCube.reflectionUpdateCount,
                    nativeCube.settleUpdatesRemaining,
                    nativeCube.skyBearingClickCount,
                    nativeCube.skylessClickCount,
                    CloudShadows::g_geometryCaptureAttempts.load(std::memory_order_relaxed),
                    CloudShadows::g_geometryCapturePublished.load(std::memory_order_relaxed),
                    CloudShadows::g_geometryCaptureRejected.load(std::memory_order_relaxed),
                    CloudShadows::g_geometryCaptureDraws.load(std::memory_order_relaxed));
                // Bounded diagnostics must reach disk even while alt-tab pauses
                // the game before the next warning or full stdio buffer.
                spdlog::default_logger_raw()->flush();
            }
            {
                FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::FrameCommit);
                FinalizeFrameAtPresent();
            }
			CloudShadows::AcceptanceRunner::TickAtPresent({
				.actualDfLightSwapHits =
					g_actualDfLightSwapHits.load(std::memory_order_acquire),
				.lastVanillaShaderHash =
					g_lastActualDfLightShaderHash.load(std::memory_order_acquire),
				.lastPixelDescriptor =
					g_lastActualDfLightPixelDescriptor.load(std::memory_order_acquire)
			});
            // F11 Community Shaders-style menu — draw last so ImGui composites
            // on top of the finished frame, just before the flip.
            Overlay::Draw(This);
            FO4CS::CloudComparison::RetirePreview();
            HRESULT result;
            {
                FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::Present);
                result = original(This, SyncInterval, Flags);
            }
            CloudShadows::AcceptanceRunner::CompletePresent(
                SUCCEEDED(result));
            FO4CS::ManualFpsLog::AfterPresent(
                This, fpsFrameEnabled,
                result == S_OK, fpsMaskValid, fpsLightingApplied, SyncInterval != 0);
            FO4CS::PrivateProfile::AfterPresent(This, result, fpsMaskValid, fpsLightingApplied);
            if (result == DXGI_ERROR_DEVICE_REMOVED ||
                result == DXGI_ERROR_DEVICE_RESET) {
                CloudShadows::InvalidateShadowMaskState();
            }
            return result;
        }
    };

    bool InstallPresentHook(
        IDXGISwapChain* swapChain,
        const char* source,
        bool authoritativeRendererChain)
    {
        FO4CS::RendererLifetime::Scope rendererLifetime;
        if (!swapChain) {
            if (authoritativeRendererChain)
                SetHookAvailable(
                    FO4CloudShadowsMenuBridge::kPresentHookUnavailable,
                    false);
            return false;
        }

        // Hook every vtable so a later renderer-selected chain is already
        // routed through our thunk, but never let an auxiliary/dummy chain
        // steal the frame-boundary identity.  Before RendererData exists the
        // first chain is only provisional; rendererWindow[0] replaces it as
        // soon as the engine exposes the authoritative chain.
        bool selectChain = authoritativeRendererChain;
        {
            std::lock_guard bindingLock(g_rendererBindingMutex);
            selectChain = selectChain || !g_ownedRendererSwapChain;
        }
        std::uintptr_t* vtable = nullptr;
        if (!ReadValidatedComVTable(swapChain, 8, &vtable)) {
            SPDLOG_ERROR("[CloudShadows] Present hook has no swap-chain vtable");
            if (selectChain)
                SetHookAvailable(
                    FO4CloudShadowsMenuBridge::kPresentHookUnavailable,
                    false);
            return false;
        }

        const auto thunk = reinterpret_cast<std::uintptr_t>(&IDXGISwapChain_Present::thunk);
        const std::uintptr_t current = vtable[8];
        PresentFn downstream = ReadComRoute<PresentFn>(
            swapChain, kPresentRouteMarker, thunk, L"dxgi.dll");
        bool objectRouteReady = downstream != nullptr;
        bool routeDataFailed = false;
        bool installThunk = false;
        bool newerOuterHook = false;
        bool unsupportedInterceptor = false;
        bool hookReady = false;
        std::uintptr_t expectedCurrent = 0;
        HMODULE unsupportedOwner = nullptr;
        {
            std::lock_guard lock(g_comHookMutex);
            if (!downstream) {
                if (current == thunk || g_presentDetourTarget.load(std::memory_order_acquire) != 0) {
                    // Another object may already have installed our thunk on
                    // this shared table. Only then is its immutable native
                    // route safe to inherit.
                    if (const auto vtableRoute = g_presentOriginals.find(vtable);
                        vtableRoute != g_presentOriginals.end() &&
                        IsExecutableAddressInModule(
                            reinterpret_cast<std::uintptr_t>(vtableRoute->second),
                            L"dxgi.dll")) {
                        downstream = vtableRoute->second;
                    }
                } else if (current && IsExecutableAddressInModule(
                               current, L"dxgi.dll", &unsupportedOwner)) {
                    // An unmarked object with a non-thunk slot is a fresh COM
                    // identity. Discard any stale map entry at a reused vtable
                    // address and learn only the stock DXGI implementation.
                    downstream = reinterpret_cast<PresentFn>(current);
                    g_presentOriginals.erase(vtable);
                    g_presentOriginals.emplace(vtable, downstream);
                } else {
                    unsupportedInterceptor =
                        g_presentOuterHooksObserved.insert(vtable).second;
                }
            }

            if (downstream) {
                // Object private data is the authoritative immutable route for
                // cloned vtables. Refreshing this native-only cache also heals
                // a stale vtable address reused by the runtime.
                g_presentOriginals.insert_or_assign(vtable, downstream);

                if (!objectRouteReady) {
                    objectRouteReady = SetComRoute(
                        swapChain, kPresentRouteMarker, downstream,
                        thunk, L"dxgi.dll");
                    routeDataFailed = !objectRouteReady;
                }

                if (objectRouteReady && current != thunk) {
                    const auto permanentDownstream =
                        reinterpret_cast<std::uintptr_t>(downstream);
                    if (current == permanentDownstream) {
                        // This equality is safe to repair because every route
                        // is verified executable inside stock dxgi.dll.
                        installThunk = true;
                        expectedCurrent = current;
                    } else {
                        // A later hook may already chain to our thunk. Keep it
                        // outermost and retain the permanent downstream route.
                        newerOuterHook =
                            g_presentOuterHooksObserved.insert(vtable).second;
                    }
                }
                hookReady = objectRouteReady &&
                    g_presentDetourTarget.load(std::memory_order_acquire) ==
                        reinterpret_cast<std::uintptr_t>(downstream);
            }
        }
        (void)expectedCurrent;
        (void)installThunk;
        if (downstream) {
            const auto target = reinterpret_cast<std::uintptr_t>(downstream);
            const auto active = g_presentDetourTarget.load(std::memory_order_acquire);
            if (active == target) {
                hookReady = true;
            } else if (active != 0) {
                // A second distinct dxgi Present implementation is not
                // expected; keep the single established detour.
                hookReady = false;
                SPDLOG_WARN(
                    "[CloudShadows] Present detour already targets {}; ignoring {} on swap={}",
                    reinterpret_cast<const void*>(active),
                    reinterpret_cast<const void*>(target), (void*)swapChain);
            } else {
                std::lock_guard lock(g_comHookMutex);
                if (g_presentDetourTarget.load(std::memory_order_acquire) == target) {
                    hookReady = true;
                } else {
                    g_presentDetourOriginal = downstream;
                    DetourThreadEnlistment threads;
                    LONG err = DetourTransactionBegin();
                    if (err == NO_ERROR) err = threads.EnlistProcessThreads();
                    if (err == NO_ERROR)
                        err = DetourAttach(
                            reinterpret_cast<PVOID*>(&g_presentDetourOriginal),
                            reinterpret_cast<PVOID>(thunk));
                    if (err == NO_ERROR) err = DetourTransactionCommit();
                    else DetourTransactionAbort();
                    if (err == NO_ERROR) {
                        g_presentDetourTarget.store(target, std::memory_order_release);
                        hookReady = true;
                        HMODULE targetModule = nullptr;
                        wchar_t targetPath[512]{};
                        if (GetModuleHandleExW(
                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(target), &targetModule))
                            GetModuleFileNameW(targetModule, targetPath, 512);
                        // UTF-8 without exceptions: fs::path::string() converts via the
                        // ANSI code page and throws for unrepresentable characters.
                        char targetUtf8[1024]{};
                        if (!WideCharToMultiByte(CP_UTF8, 0, targetPath, -1, targetUtf8,
                                static_cast<int>(sizeof(targetUtf8)), nullptr, nullptr))
                            targetUtf8[0] = 0;
                        SPDLOG_INFO(
                            "[CloudShadows] Present inline detour installed via {} on Present {} in {} "
                            "(trampoline={}) for swap={}; vtable slot untouched (current={})",
                            source, reinterpret_cast<const void*>(target),
                            targetUtf8,
                            reinterpret_cast<const void*>(g_presentDetourOriginal),
                            (void*)swapChain, reinterpret_cast<const void*>(current));
                    } else {
                        g_presentDetourOriginal = nullptr;
                        hookReady = false;
                        SPDLOG_ERROR(
                            "[CloudShadows] Present inline detour failed ({}) on swap={}",
                            err, (void*)swapChain);
                    }
                }
            }
            if (newerOuterHook)
                SPDLOG_INFO(
                    "[CloudShadows] Another Present interceptor occupies the vtable slot on swap={}; "
                    "the inline detour still receives every Present",
                    (void*)swapChain);
        }
        if (routeDataFailed) {
            hookReady = false;
            static std::atomic<bool> loggedMarkerFailure{ false };
            if (!loggedMarkerFailure.exchange(true)) {
                SPDLOG_ERROR(
                    "[CloudShadows] Swap-chain route data rejected; Present hook left unchanged");
            }
        }
        if (unsupportedInterceptor) {
            hookReady = false;
            SPDLOG_ERROR(
                "[CloudShadows] Unsupported pre-existing Present interceptor at {} "
                "(owner={}); expected executable code in dxgi.dll; hook left unchanged",
                reinterpret_cast<const void*>(current),
                static_cast<void*>(unsupportedOwner));
        }
        if (selectChain) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kPresentHookUnavailable,
                hookReady);
            if (hookReady) {
                std::lock_guard bindingLock(g_rendererBindingMutex);
                if (g_ownedRendererSwapChain.Get() != swapChain) {
                    SPDLOG_INFO(
                        "[CloudShadows] {} Present chain selected via {}: {}",
                        authoritativeRendererChain ? "Authoritative" : "Provisional",
                        source, (void*)swapChain);
                    if (g_ownedRendererSwapChain)
                        FO4CS::RendererLifetime::Retire(
                            g_ownedRendererSwapChain);
                    g_ownedRendererSwapChain = swapChain;
                    g_swapChain = g_ownedRendererSwapChain.Get();
                    g_publishedRendererSwapChain.store(
                        g_swapChain, std::memory_order_release);
                }
            }
        }
        return hookReady;
    }

    struct ID3D11Device_CreateVertexShader
    {
        static HRESULT WINAPI thunk(
            ID3D11Device* This,
            const void* pShaderBytecode,
            SIZE_T BytecodeLength,
            ID3D11ClassLinkage* pClassLinkage,
            ID3D11VertexShader** ppVertexShader)
        {
            CreateVertexShaderFn original = nullptr;
            if (This) {
                auto* vtable = *reinterpret_cast<std::uintptr_t**>(This);
                original = ReadComRoute<CreateVertexShaderFn>(
                    This, kCreateVSRouteMarker,
                    reinterpret_cast<std::uintptr_t>(&thunk), L"d3d11.dll");
                if (!original) {
                    std::lock_guard lock(g_comHookMutex);
                    if (const auto it = g_createVSOriginals.find(vtable);
                        it != g_createVSOriginals.end() &&
                        IsExecutableAddressInModule(
                            reinterpret_cast<std::uintptr_t>(it->second),
                            L"d3d11.dll")) {
                        original = it->second;
                    }
                }
            }
            if (!original)
                return E_UNEXPECTED;

            const HRESULT hr = original(
                This, pShaderBytecode, BytecodeLength, pClassLinkage,
                ppVertexShader);
            if (SUCCEEDED(hr) && ppVertexShader && *ppVertexShader &&
                pShaderBytecode && BytecodeLength > 0) {
                // Capture identity only for the static-linkage ABI replayed by
                // the world-cloud path. A bytecode match does not authenticate
                // class-instance bindings.
                if (pClassLinkage) {
                    static std::atomic<bool> loggedLinkedShader{ false };
                    if (!loggedLinkedShader.exchange(true)) {
                        SPDLOG_WARN(
                            "[CloudShadows] Dynamic-linkage vertex shader "
                            "observed; excluded from cloud capture");
                    }
                    return hr;
                }

                const uint64_t hash = SIE::DFLightPatcher::HashDXBC(
                    pShaderBytecode, static_cast<size_t>(BytecodeLength));
                if (!SIE::DFLightPatcher::RecordShaderHash(
                        *ppVertexShader, hash)) {
                    static std::atomic<uint32_t> failures{ 0 };
                    const uint32_t n = ++failures;
                    if (n <= 5) {
                        SPDLOG_ERROR(
                            "[CloudShadows] Failed to attach immutable VS "
                            "identity #{} vs={} hash=0x{:016X}",
                            n, static_cast<void*>(*ppVertexShader), hash);
                    }
                }
            }
            return hr;
        }
    };

    bool InstallCreateVertexShaderHook(ID3D11Device* device) noexcept
    {
        if (!device) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreateVertexShaderHookUnavailable,
                false);
            return false;
        }
        std::uintptr_t* vtable = nullptr;
        if (!ReadValidatedComVTable(device, 12, &vtable)) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreateVertexShaderHookUnavailable,
                false);
            return false;
        }
        const auto thunk = reinterpret_cast<std::uintptr_t>(
            &ID3D11Device_CreateVertexShader::thunk);
        constexpr size_t kCreateVertexShaderSlot = 12;
        const std::uintptr_t current = vtable[kCreateVertexShaderSlot];
        CreateVertexShaderFn downstream = ReadComRoute<CreateVertexShaderFn>(
            device, kCreateVSRouteMarker, thunk, L"d3d11.dll");
        bool objectRouteReady = downstream != nullptr;
        bool routeDataFailed = false;
        bool installThunk = false;
        bool newerOuterHook = false;
        bool unsupportedInterceptor = false;
        bool hookReady = false;
        std::uintptr_t expectedCurrent = 0;
        HMODULE unsupportedOwner = nullptr;
        {
            std::lock_guard lock(g_comHookMutex);
            if (!downstream) {
                if (current == thunk) {
                    if (const auto vtableRoute = g_createVSOriginals.find(vtable);
                        vtableRoute != g_createVSOriginals.end() &&
                        IsExecutableAddressInModule(
                            reinterpret_cast<std::uintptr_t>(vtableRoute->second),
                            L"d3d11.dll")) {
                        downstream = vtableRoute->second;
                    }
                } else if (current && IsExecutableAddressInModule(
                               current, L"d3d11.dll", &unsupportedOwner)) {
                    downstream = reinterpret_cast<CreateVertexShaderFn>(current);
                    g_createVSOriginals.erase(vtable);
                    g_createVSOriginals.emplace(vtable, downstream);
                } else {
                    unsupportedInterceptor =
                        g_createVSOuterHooksObserved.insert(vtable).second;
                }
            }

            if (downstream) {
                g_createVSOriginals.insert_or_assign(vtable, downstream);
                if (!objectRouteReady) {
                    objectRouteReady = SetComRoute(
                        device, kCreateVSRouteMarker, downstream,
                        thunk, L"d3d11.dll");
                    routeDataFailed = !objectRouteReady;
                }
                if (objectRouteReady && current != thunk) {
                    const auto permanentDownstream =
                        reinterpret_cast<std::uintptr_t>(downstream);
                    if (current == permanentDownstream) {
                        installThunk = true;
                        expectedCurrent = current;
                    } else {
                        newerOuterHook =
                            g_createVSOuterHooksObserved.insert(vtable).second;
                    }
                }
                hookReady = objectRouteReady && current == thunk;
            }
        }
        if (installThunk) {
            if (vtable[kCreateVertexShaderSlot] == expectedCurrent) {
                if (WriteProtectedPointer(
                        std::addressof(vtable[kCreateVertexShaderSlot]), thunk)) {
                    hookReady = true;
                    SPDLOG_INFO(
                        "[CloudShadows] CreateVertexShader hook "
                        "installed/repaired on device={}",
                        static_cast<void*>(device));
                } else {
                    hookReady = false;
                    SPDLOG_ERROR(
                        "[CloudShadows] CreateVertexShader vtable write failed "
                        "on device={}", static_cast<void*>(device));
                }
            } else {
                hookReady = vtable[kCreateVertexShaderSlot] == thunk;
                SPDLOG_INFO(
                    "[CloudShadows] Concurrent CreateVertexShader hook retained "
                    "outermost on device={}", static_cast<void*>(device));
            }
        } else if (newerOuterHook) {
            hookReady = false;
            SPDLOG_INFO(
                "[CloudShadows] Later CreateVertexShader hook retained "
                "outermost on device={}", static_cast<void*>(device));
        }
        if (routeDataFailed) {
            hookReady = false;
            static std::atomic<bool> loggedMarkerFailure{ false };
            if (!loggedMarkerFailure.exchange(true)) {
                SPDLOG_ERROR(
                    "[CloudShadows] D3D-device route data rejected; "
                    "CreateVertexShader hook left unchanged");
            }
        }
        if (unsupportedInterceptor) {
            hookReady = false;
            SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), ShaderInterceptorLogLevel(),
                "[CloudShadows] Unsupported pre-existing CreateVertexShader "
                "interceptor at {} (owner={}); expected executable code in "
                "d3d11.dll; hook left unchanged",
                reinterpret_cast<const void*>(current),
                static_cast<void*>(unsupportedOwner));
        }
        SetHookAvailable(
            FO4CloudShadowsMenuBridge::kCreateVertexShaderHookUnavailable,
            hookReady);
        return hookReady;
    }

    struct ID3D11Device_CreatePixelShader
    {
        static void AttachNativeCloudMrtVariant(
            ID3D11Device* device,
            CreatePixelShaderFn createPixelShader,
            const void* stockBytecode,
            SIZE_T stockBytecodeLength,
            ID3D11PixelShader* stockShader) noexcept
        {
            if (!device || !createPixelShader || !stockBytecode ||
                stockBytecodeLength == 0 || !stockShader ||
                SIE::SkyCloudMrtPatcher::Identify(
                    stockBytecode,
                    static_cast<size_t>(stockBytecodeLength)) ==
                    SIE::SkyCloudPixelShaderVariant::kUnsupported) {
                return;
            }

            try {
                SIE::SkyCloudMrtPatchInfo patchInfo{};
                ComPtr<ID3DBlob> patchedBlob;
                patchedBlob.Attach(SIE::SkyCloudMrtPatcher::Patch(
                    stockBytecode,
                    static_cast<size_t>(stockBytecodeLength),
                    SIE::SkyCloudMrtPayload::kUvMapping, &patchInfo));
                if (!patchedBlob || !patchInfo.patchVerified) {
                    SPDLOG_ERROR(
                        "[CloudShadows] Native cloud MRT patch failed closed "
                        "for stock hash=0x{:016X} bytes={} identity={} "
                        "structure={} verified={}",
                        patchInfo.sourceHash, patchInfo.sourceSize,
                        patchInfo.identityMatched, patchInfo.structureMatched,
                        patchInfo.patchVerified);
                    return;
                }

                ComPtr<ID3D11PixelShader> patchedShader;
                const HRESULT createResult = createPixelShader(
                    device, patchedBlob->GetBufferPointer(),
                    patchedBlob->GetBufferSize(), nullptr,
                    patchedShader.GetAddressOf());
                if (FAILED(createResult) || !patchedShader) {
                    SPDLOG_ERROR(
                        "[CloudShadows] Native cloud MRT shader creation "
                        "failed for stock hash=0x{:016X}: 0x{:08X}",
                        patchInfo.sourceHash,
                        static_cast<uint32_t>(createResult));
                    return;
                }

                const HRESULT attachResult =
                    stockShader->SetPrivateDataInterface(
                        kSkyCloudMrtShaderMarker, patchedShader.Get());
                if (FAILED(attachResult)) {
                    SPDLOG_ERROR(
                        "[CloudShadows] Native cloud MRT shader lifetime "
                        "attachment failed for stock hash=0x{:016X}: "
                        "0x{:08X}",
                        patchInfo.sourceHash,
                        static_cast<uint32_t>(attachResult));
                    return;
                }
                g_nativeCloudMrtShaderCount.fetch_add(
                    1, std::memory_order_release);
                SPDLOG_INFO(
                    "[CloudShadows] Native cloud MRT shader ready: "
                    "stock hash=0x{:016X} bytes={} variant={}",
                    patchInfo.sourceHash, patchInfo.sourceSize,
                    static_cast<uint32_t>(patchInfo.variant));
            } catch (...) {
                SPDLOG_ERROR(
                    "[CloudShadows] Native cloud MRT shader creation threw; "
                    "capture remains fail-neutral");
            }
        }

        static HRESULT WINAPI thunk(
            ID3D11Device* This,
            const void* pShaderBytecode,
            SIZE_T BytecodeLength,
            ID3D11ClassLinkage* pClassLinkage,
            ID3D11PixelShader** ppPixelShader)
        {
            CreatePixelShaderFn original = nullptr;
            if (This) {
                auto* vtable = *reinterpret_cast<std::uintptr_t**>(This);
                original = ReadComRoute<CreatePixelShaderFn>(
                    This, kCreatePSRouteMarker,
                    reinterpret_cast<std::uintptr_t>(&thunk), L"d3d11.dll");
                if (!original) {
                    std::lock_guard lock(g_comHookMutex);
                    if (const auto it = g_createPSOriginals.find(vtable);
                        it != g_createPSOriginals.end() &&
                        IsExecutableAddressInModule(
                            reinterpret_cast<std::uintptr_t>(it->second),
                            L"d3d11.dll")) {
                        original = it->second;
                    }
                }
            }
            if (!original)
                return E_UNEXPECTED;
            HRESULT hr = original(
                This, pShaderBytecode, BytecodeLength, pClassLinkage,
                ppPixelShader);
            if (SUCCEEDED(hr) && ppPixelShader && *ppPixelShader &&
                pShaderBytecode && BytecodeLength > 0)
            {
                // Patched shaders are compiled without dynamic linkage. Never
                // classify a linked live shader as interchangeable with one;
                // an exact bytecode hash alone does not prove linkage ABI.
                if (pClassLinkage) {
                    static std::atomic<bool> loggedLinkedShader{ false };
                    if (!loggedLinkedShader.exchange(true)) {
                        SPDLOG_WARN("[CloudShadows] Dynamic-linkage pixel shader observed; excluded from cloud-shadow replacement");
                    }
                    return hr;
                }
                AttachNativeCloudMrtVariant(
                    This, original, pShaderBytecode, BytecodeLength,
                    *ppPixelShader);
                FO4CS::GodraysIntegration::ObservePixelShaderCreated(
                    This, original, pShaderBytecode,
                    static_cast<size_t>(BytecodeLength), *ppPixelShader);
                uint64_t h = SIE::DFLightPatcher::HashDXBC(pShaderBytecode, static_cast<size_t>(BytecodeLength));
                // Record EVERY PS unconditionally — we need to catch shaders
                // created before DFLightPatcher finishes initializing. A post-
                // init rematch pass picks up anything missed here.
                CloudShadows::g_dfLightPatcher.RecordPSHash(*ppPixelShader, h);
                // Save raw bytecode so strict sunlight variants can be patched
                // lazily even when no loose VanillaDXBC corpus is installed.
                CloudShadows::g_dfLightPatcher.StoreBytecode(h, pShaderBytecode, static_cast<size_t>(BytecodeLength));
                // If the strict payload is initialized now, classify this
                // bytecode immediately. Operational readiness is only raised
                // after at least one sunlight signature actually succeeds.
                if (CloudShadows::g_dfLightPatcher.IsInitialized()) {
                    CloudShadows::g_dfLightPatcher.RegisterVanillaPS(h, *ppPixelShader);
                }
                static std::atomic<uint32_t> psCreateCount = 0;
                uint32_t n = ++psCreateCount;
                if (n <= 10 || n == 50 || n == 100 || n == 500 || n == 1000 || (n % 2000) == 0) {
                    SPDLOG_INFO("[CloudShadows] CreatePS #{} hash=0x{:016x} ps={} bytes={} patcherReady={}",
                        n, h, (void*)*ppPixelShader, (uint64_t)BytecodeLength,
                        CloudShadows::g_dfLightPatcher.IsReady());
                }
            }
            return hr;
        }
    };

    bool InstallCreatePixelShaderHook(ID3D11Device* device) noexcept
    {
        if (!device) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreatePixelShaderHookUnavailable,
                false);
            return false;
        }
        std::uintptr_t* vtable = nullptr;
        if (!ReadValidatedComVTable(device, 15, &vtable)) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreatePixelShaderHookUnavailable,
                false);
            return false;
        }
        const auto thunk = reinterpret_cast<std::uintptr_t>(
            &ID3D11Device_CreatePixelShader::thunk);
        const std::uintptr_t current = vtable[15];
        CreatePixelShaderFn downstream = ReadComRoute<CreatePixelShaderFn>(
            device, kCreatePSRouteMarker, thunk, L"d3d11.dll");
        bool objectRouteReady = downstream != nullptr;
        bool routeDataFailed = false;
        bool installThunk = false;
        bool newerOuterHook = false;
        bool unsupportedInterceptor = false;
        bool hookReady = false;
        std::uintptr_t expectedCurrent = 0;
        HMODULE unsupportedOwner = nullptr;
        {
            std::lock_guard lock(g_comHookMutex);
            if (!downstream) {
                if (current == thunk) {
                    if (const auto vtableRoute = g_createPSOriginals.find(vtable);
                        vtableRoute != g_createPSOriginals.end() &&
                        IsExecutableAddressInModule(
                            reinterpret_cast<std::uintptr_t>(vtableRoute->second),
                            L"d3d11.dll")) {
                        downstream = vtableRoute->second;
                    }
                } else if (current && IsExecutableAddressInModule(
                               current, L"d3d11.dll", &unsupportedOwner)) {
                    downstream = reinterpret_cast<CreatePixelShaderFn>(current);
                    g_createPSOriginals.erase(vtable);
                    g_createPSOriginals.emplace(vtable, downstream);
                } else {
                    unsupportedInterceptor =
                        g_createPSOuterHooksObserved.insert(vtable).second;
                }
            }

            if (downstream) {
                g_createPSOriginals.insert_or_assign(vtable, downstream);

                if (!objectRouteReady) {
                    objectRouteReady = SetComRoute(
                        device, kCreatePSRouteMarker, downstream,
                        thunk, L"d3d11.dll");
                    routeDataFailed = !objectRouteReady;
                }

                if (objectRouteReady && current != thunk) {
                    const auto permanentDownstream =
                        reinterpret_cast<std::uintptr_t>(downstream);
                    if (current == permanentDownstream) {
                        installThunk = true;
                        expectedCurrent = current;
                    } else {
                        newerOuterHook =
                            g_createPSOuterHooksObserved.insert(vtable).second;
                    }
                }
                hookReady = objectRouteReady && current == thunk;
            }
        }
        if (installThunk) {
            if (vtable[15] == expectedCurrent) {
                if (WriteProtectedPointer(std::addressof(vtable[15]), thunk)) {
                    hookReady = true;
                    SPDLOG_INFO(
                        "[CloudShadows] CreatePixelShader hook installed/repaired on device={}",
                        (void*)device);
                } else {
                    hookReady = false;
                    SPDLOG_ERROR(
                        "[CloudShadows] CreatePixelShader vtable write failed on device={}",
                        (void*)device);
                }
            } else {
                hookReady = vtable[15] == thunk;
                SPDLOG_INFO(
                    "[CloudShadows] Concurrent CreatePixelShader hook retained outermost on device={}",
                    (void*)device);
            }
        } else if (newerOuterHook) {
            hookReady = false;
            SPDLOG_INFO(
                "[CloudShadows] Later CreatePixelShader hook retained outermost on device={}",
                (void*)device);
        }
        if (routeDataFailed) {
            hookReady = false;
            static std::atomic<bool> loggedMarkerFailure{ false };
            if (!loggedMarkerFailure.exchange(true)) {
                SPDLOG_ERROR(
                    "[CloudShadows] D3D-device route data rejected; CreatePixelShader hook left unchanged");
            }
        }
        if (unsupportedInterceptor) {
            hookReady = false;
            SPDLOG_LOGGER_CALL(spdlog::default_logger_raw(), ShaderInterceptorLogLevel(),
                "[CloudShadows] Unsupported pre-existing CreatePixelShader interceptor at {} "
                "(owner={}); expected executable code in d3d11.dll; hook left unchanged",
                reinterpret_cast<const void*>(current),
                static_cast<void*>(unsupportedOwner));
        }
        SetHookAvailable(
            FO4CloudShadowsMenuBridge::kCreatePixelShaderHookUnavailable,
            hookReady);
        return hookReady;
    }

    struct SwapState {
        ID3D11PixelShader* vanillaPS = nullptr;
        ID3D11ShaderResourceView* prevT47 = nullptr;
        ComPtr<ID3D11PixelShader> patchedPS;
        uint64_t shaderHash = 0;
        uint32_t pixelDescriptor = 0;
        bool swapped = false;
    };

    bool GetStaticBoundPixelShader(
        ID3D11DeviceContext* context,
        ID3D11PixelShader** shader) noexcept
    {
        if (!context || !shader)
            return false;
        *shader = nullptr;
        std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
        UINT instanceCount = static_cast<UINT>(instances.size());
        context->PSGetShader(shader, instances.data(), &instanceCount);
        for (UINT i = 0; i < (std::min)(instanceCount,
                 static_cast<UINT>(instances.size())); ++i) {
            if (instances[i])
                instances[i]->Release();
        }
        if (instanceCount != 0) {
            if (*shader) {
                (*shader)->Release();
                *shader = nullptr;
            }
            static std::atomic<bool> loggedLinkedBinding{ false };
            if (!loggedLinkedBinding.exchange(true)) {
                SPDLOG_WARN("[CloudShadows] Bound DFLight shader uses class instances; replacement disabled for that binding");
            }
            return false;
        }
        return *shader != nullptr;
    }

    bool GetStaticBoundSkyShaders(
        ID3D11DeviceContext* context,
        ID3D11VertexShader** vertexShader,
        ID3D11PixelShader** pixelShader) noexcept
    {
        if (!context || !vertexShader || !pixelShader)
            return false;
        *vertexShader = nullptr;
        *pixelShader = nullptr;

        std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
        UINT vertexClassCount = static_cast<UINT>(instances.size());
        context->VSGetShader(vertexShader, instances.data(), &vertexClassCount);
        for (UINT i = 0; i < (std::min)(vertexClassCount,
                 static_cast<UINT>(instances.size())); ++i) {
            if (instances[i])
                instances[i]->Release();
        }

        instances = {};
        UINT pixelClassCount = static_cast<UINT>(instances.size());
        context->PSGetShader(pixelShader, instances.data(), &pixelClassCount);
        for (UINT i = 0; i < (std::min)(pixelClassCount,
                 static_cast<UINT>(instances.size())); ++i) {
            if (instances[i])
                instances[i]->Release();
        }

        if (!*vertexShader || !*pixelShader || vertexClassCount != 0 ||
            pixelClassCount != 0) {
            if (*vertexShader) {
                (*vertexShader)->Release();
                *vertexShader = nullptr;
            }
            if (*pixelShader) {
                (*pixelShader)->Release();
                *pixelShader = nullptr;
            }
            return false;
        }
        return true;
    }

    struct StockSkyShaderHashes
    {
        uint64_t vertex{};
        uint64_t pixel{};
        uint64_t compatiblePixel{};
    };

    [[nodiscard]] StockSkyShaderHashes ExpectedStockSkyShaderHashes(
        FO4CS::F4SECompat::RuntimeTarget target,
        uint32_t skyTechnique) noexcept
    {
        using FO4CS::F4SECompat::RuntimeTarget;
        const bool flat = target == RuntimeTarget::kLegacy ||
            target == RuntimeTarget::kAE;
        const bool vr = target == RuntimeTarget::kVR;
        if (!flat && !vr)
            return {};

        switch (skyTechnique) {
        case CloudShadows::kSkyTechniqueClouds:
            return flat
                ? StockSkyShaderHashes{
                      0x86C6EA0FF7F00137ULL, 0x8E88AA0367139E3DULL }
                : StockSkyShaderHashes{
                      0xE0DCCF1AA3930CDBULL, 0x0219856733164CB0ULL,
                      0x6D2B9D973AF2EC0DULL };
        case CloudShadows::kSkyTechniqueCloudsLerp:
            return flat
                ? StockSkyShaderHashes{
                      0x65B9ECF2BD55648AULL, 0xD2C1A25885A8C5D8ULL }
                : StockSkyShaderHashes{
                      0x4D3AEFC3CC2C5351ULL, 0x7C98FE0C571E1916ULL,
                      0x9D6DBBA74E764A0CULL };
        case CloudShadows::kSkyTechniqueCloudsFade:
            // CloudsFade deliberately aliases the Clouds vertex shader in all
            // three audited stock packages; its pixel formula is distinct.
            return flat
                ? StockSkyShaderHashes{
                      0x86C6EA0FF7F00137ULL, 0xA4C32FA7398E1DDBULL }
                : StockSkyShaderHashes{
                      0xE0DCCF1AA3930CDBULL, 0x9672C2620203FD20ULL,
                      0x585F89F70474752CULL };
        default:
            return {};
        }
    }

    [[nodiscard]] bool AuthenticateStockSkyShaders(
        uint32_t skyTechnique,
        ID3D11VertexShader* vertexShader,
        ID3D11PixelShader* pixelShader,
        uint64_t* observedVertexHash = nullptr,
        uint64_t* observedPixelHash = nullptr) noexcept
    {
        const uint64_t vertexHash =
            SIE::DFLightPatcher::LookupShaderHash(vertexShader);
        const uint64_t pixelHash =
            SIE::DFLightPatcher::LookupShaderHash(pixelShader);
        if (observedVertexHash)
            *observedVertexHash = vertexHash;
        if (observedPixelHash)
            *observedPixelHash = pixelHash;

        const auto expected = ExpectedStockSkyShaderHashes(
            FO4CS::RuntimeAPI::GetSingleton().Target(), skyTechnique);
        // These exact FO4VR Deferred 0.5.0-dev cache variants preserve native
        // cloud UV/alpha and both outputs. Their only executable difference
        // is equivalent CB12 motion-vector indexing (see compatibility tests).
        return expected.vertex != 0 && expected.pixel != 0 &&
            vertexHash == expected.vertex && (pixelHash == expected.pixel ||
                (expected.compatiblePixel != 0 && pixelHash == expected.compatiblePixel));
    }

    static SwapState BeginSwap(ID3D11DeviceContext* ctx)
    {
        SwapState s;
        // Code detours can see driver entry points shared by deferred
        // contexts. All cloud work is intentionally restricted to the one
        // captured immediate context whose BeginTechnique state we own.
        if (g_internalDrawDepth != 0 || !IsCapturedImmediateContext(ctx))
            return s;

        // A native opacity cube must have been published before any DFLight
        // state inspection or full-screen compute work is useful.  This is a
        // deliberately cheap hot-path gate: startup, interiors and rejected
        // native captures remain identical to vanilla lighting.
        if (CloudShadows::g_worldCloudActiveLayers.load(
                std::memory_order_acquire) == 0 ||
            CloudShadows::g_worldCloudCommittedEpoch.load(
                std::memory_order_acquire) == 0) {
            return s;
        }

        if (!CloudShadows::g_dfLightPatcher.IsReady() ||
            !CloudShadows::g_cloudShadowSRV) {
            return s;
        }
        // Gate on the active DFLight technique AND the swap-map lookup. Both
        // are required: the game SHARES PixelShader objects between FXPs
        // (observed live: a swap-map PS bound during DFPrepass/kind=0 draws —
        // 2.6M mis-hits in 20s when the gate was removed, silently swapping
        // patched DFLight shaders into G-buffer passes). The kind global IS
        // coherent here: draw telemetry shows every draw flows through the
        // single immediate context, same thread as BeginTechnique.
        auto& phase = g_techniqueState;
        if (phase.kind != TechniqueKind::kDFLight || !phase.sunShaderValidated)
            return s;

        if (!GetStaticBoundPixelShader(ctx, &s.vanillaPS))
            return s;

        // BeginTechnique already authenticated and retained this exact pair.
        // Verify the live binding, then reuse it without another private-data
        // lookup/lifecycle mutex on each participating draw.
        if (s.vanillaPS != phase.dfLightVanillaPS.Get() ||
            !phase.dfLightPatchedPS) {
            // Only log misses that occur during an active DFLight technique —
            // a DFLight miss is a real bug (unregistered PS); misses outside
            // DFLight are expected (other shaders, UI, sky).
            if (phase.kind == TechniqueKind::kDFLight) {
                uint32_t m = ++g_dfLightMissDuringPhase;
                static std::mutex dfUniqueMu;
                static std::unordered_set<ID3D11PixelShader*> dfUnique;
                bool newUnique = false;
                {
                    std::lock_guard<std::mutex> lk(dfUniqueMu);
                    newUnique = dfUnique.insert(s.vanillaPS).second;
                }
                if (newUnique || m <= 5 || (m % 500) == 0) {
                    SPDLOG_INFO("[CloudShadows] DFLight miss #{} ps={} newUnique={}",
                        m, (void*)s.vanillaPS, newUnique);
                }
            }
            return s;
        }
        s.patchedPS = phase.dfLightPatchedPS;

        // HIT — got a DFLight PS that matches one we registered.
        {
            uint32_t h = ++g_dfLightSwapHits;
            if (h <= 5 || (h % 500) == 0) {
                SPDLOG_INFO("[CloudShadows] DFLight swap HIT #{} ps={} kind={}",
                    h, (void*)s.vanillaPS, static_cast<int>(phase.kind));
            }
        }

        // Reconstruct for this exact draw. DFLight can reuse a shader object
        // across techniques/views while changing RTV, viewport, t3/s3, or the
        // contents/ranges of b2 and b12. Pointer identity cannot detect a
        // dynamic-buffer update, so reusing an earlier mask is not a valid
        // optimization. Prepass validates the complete current binding set
        // and secondary/reflection draws therefore fail neutral.
        if (!CloudShadows::Prepass() ||
            !CloudShadows::g_shadowMaskValid.load(std::memory_order_acquire))
            return s;

        ctx->PSGetShaderResources(47, 1, &s.prevT47);
        ID3D11ShaderResourceView* cloudSRV = CloudShadows::g_cloudShadowSRV;
        ctx->PSSetShader(s.patchedPS.Get(), nullptr, 0);
        ctx->PSSetShaderResources(47, 1, &cloudSRV);
        s.swapped = true;
        // Evidence is not published here: binding a replacement does not prove
        // the game draw was submitted. Each detour records it only after the
        // original driver entry returns.
        s.shaderHash = CloudShadows::g_dfLightPatcher.LookupPSHash(s.vanillaPS);
        s.pixelDescriptor = phase.dfLightPixelDescriptor;
        return s;
    }

    static void RecordSubmittedSwap(const SwapState& s) noexcept
    {
        if (!s.swapped)
            return;
        g_lastActualDfLightShaderHash.store(
            s.shaderHash, std::memory_order_relaxed);
        g_lastActualDfLightPixelDescriptor.store(
            s.pixelDescriptor, std::memory_order_relaxed);
        const uint64_t submitted = g_actualDfLightSwapHits.fetch_add(
            1, std::memory_order_release) + 1;
        if (submitted == 1 || submitted == 100 ||
            (submitted % 5000) == 0) {
            SPDLOG_INFO(
                "[CloudShadows] SUBMITTED DFLight swap #{} vanillaPS={} "
                "patchedPS={} hash=0x{:016X} descriptor=0x{:08X}",
                submitted, static_cast<void*>(s.vanillaPS),
                static_cast<void*>(s.patchedPS.Get()), s.shaderHash,
                s.pixelDescriptor);
        }
    }

    static void EndSwap(ID3D11DeviceContext* ctx, SwapState& s)
    {
        if (s.swapped) {
            ctx->PSSetShader(s.vanillaPS, nullptr, 0);
            ctx->PSSetShaderResources(47, 1, &s.prevT47);
        }
        if (s.prevT47) s.prevT47->Release();
        if (s.vanillaPS) s.vanillaPS->Release();
    }

    // Engine technique classification anchors live Sky capture and the exact
    // DFLight draw phase used by the driver-entry detours below.
    // Verified flat ABI: this + VS/HS/DS/PS descriptors.  Fallout 4 VR adds a
    // sixth shader-selection output pointer.  The pointer is not used by our
    // observer, but VR's original function requires it; dropping it leaves the
    // original reading an uninitialised argument in its prologue.  Keep distinct
    // detours so OG/AE and VR both retain their exact native ABI.
    //
    // The only object fields observed here are the
    // stable BSShader type/name slots at 0x18 and 0x110 (0x188 on 1.11.240), read through an opaque
    // pointer so no CommonLib engine type enters this translation unit.
    using BeginTechniqueFlat_t = bool(WINAPI*)(
        void*, uint32_t, uint32_t, uint32_t, uint32_t);
    using BeginTechniqueVR_t = bool(WINAPI*)(
        void*, uint32_t, uint32_t, uint32_t, uint32_t, void*);
    static BeginTechniqueFlat_t s_original_BeginTechniqueFlat = nullptr;
    static BeginTechniqueVR_t s_original_BeginTechniqueVR = nullptr;
    // Reviewed VR ABI: graphics, VS, HS, DS, PS engine wrappers. The wrapper
    // descriptor is at +0 and the native D3D shader is at +8. The engine has
    // already consumed these live arguments when the observer runs.
    using SetShadersVR_t = void(WINAPI*)(void*, void*, void*, void*, void*);
    static SetShadersVR_t s_original_SetShadersVR = nullptr;

    struct ShaderIdentity
    {
        std::int32_t type{ -1 };
        std::array<char, 64> fxpFilename{};
        std::uint8_t fxpFilenameLength{ 0 };

        [[nodiscard]] bool HasFilename() const noexcept
        {
            return fxpFilenameLength != 0;
        }

        [[nodiscard]] std::string_view Filename() const noexcept
        {
            return { fxpFilename.data(), fxpFilenameLength };
        }
    };

    struct ShaderIdentityCacheEntry
    {
        void* shader{};
        std::uintptr_t vtable{};
        ShaderIdentity identity{};
    };

    [[nodiscard]] std::uintptr_t ReadShaderVtable(void* shader) noexcept
    {
        if (!shader)
            return 0;
#if defined(_MSC_VER)
        __try {
            return *reinterpret_cast<const std::uintptr_t*>(shader);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
#else
        return *reinterpret_cast<const std::uintptr_t*>(shader);
#endif
    }

    [[nodiscard]] ShaderIdentity ReadShaderIdentity(void* shader) noexcept
    {
        ShaderIdentity result{};
        if (!shader)
            return result;
        // BeginTechnique repeatedly visits a small process-lifetime set of
        // BSShader objects. Cache their immutable class/name identity so VR can
        // classify DFPrepass/DFLight/DFComposite without a VirtualQuery on every
        // technique. Matching the vtable as well as the object address prevents
        // a recycled allocation from inheriting a stale identity.
        static thread_local std::array<ShaderIdentityCacheEntry, 32> cache{};
        static thread_local std::uint32_t nextCacheEntry = 0;
        const auto vtable = ReadShaderVtable(shader);
        if (vtable != 0) {
            for (const auto& entry : cache) {
                if (entry.shader == shader && entry.vtable == vtable)
                    return entry.identity;
            }
        }
        auto& runtime = FO4CS::RuntimeAPI::GetSingleton();
        const auto address = reinterpret_cast<std::uintptr_t>(shader);
        // BSShader layout (Combined Ghidra project, BSShader::BSShader):
        //   OG 1.10.163 / VR: shaderType at 0x18, fxpFilename at 0x110.
        //   1.11.240: three CRITICAL_SECTIONs were added after the vtables,
        //   moving fxpFilename to 0x188 (shaderType stays at 0x18). Reading
        //   0x110 there returns the 0xDEADBEEF scatter-table sentinel.
        const bool aeLayout = runtime.Target() == FO4CS::F4SECompat::RuntimeTarget::kAE;
        const std::size_t filenameOffset = aeLayout ? 0x188u : 0x110u;
        if (!runtime.ValidateMemory(
                address, filenameOffset + sizeof(const char*),
                FO4CS::AddressKind::kReadable)) {
            return result;
        }
        std::memcpy(&result.type,
            reinterpret_cast<const std::byte*>(shader) + 0x18,
            sizeof(result.type));
        const char* filename = nullptr;
        std::memcpy(&filename,
            reinterpret_cast<const std::byte*>(shader) + filenameOffset,
            sizeof(filename));
        const auto filenameAddress = reinterpret_cast<std::uintptr_t>(filename);
        if (!filename || !runtime.ValidateMemory(
                filenameAddress, result.fxpFilename.size(),
                FO4CS::AddressKind::kReadable)) {
            return result;
        }
        std::memcpy(
            result.fxpFilename.data(), filename,
            result.fxpFilename.size());
        const auto* terminator = static_cast<const char*>(std::memchr(
            result.fxpFilename.data(), '\0', result.fxpFilename.size()));
        if (!terminator || terminator == result.fxpFilename.data()) {
            result.fxpFilename.fill('\0');
            return result;
        }
        result.fxpFilenameLength = static_cast<std::uint8_t>(
            terminator - result.fxpFilename.data());
        if (vtable != 0) {
            auto& entry = cache[nextCacheEntry++ % cache.size()];
            entry.shader = shader;
            entry.vtable = vtable;
            entry.identity = result;
        }
        return result;
    }

    void CloseCaptureEpochForLighting() noexcept
    {
        auto& phase = g_techniqueState;
        if (!phase.captureEpochStarted)
            return;
        phase.captureEpochStarted = false;
    }

    bool OpenCaptureEpoch(ID3D11DeviceContext* context) noexcept
    {
        // FO4 1.10.163 draws the main Sky into the exact kMainTemp resource and
        // subresource with a full-output viewport. Reflection and secondary Sky
        // passes must never publish an empty generation which Present could commit.
        if (!CloudShadows::IsMainSkyWorldView(context))
            return false;
        auto& phase = g_techniqueState;
        if (phase.captureEpochStarted)
            return true;
        phase.captureEpoch = g_captureEpochCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        phase.captureEpochStarted = true;
        CloudShadows::g_worldCloudPendingEpoch.store(phase.captureEpoch, std::memory_order_release);
        return true;
    }

    void MaintainRendererBindings() noexcept
    {
        FO4CS::RendererLifetime::Scope rendererLifetime;
        auto* rd = CloudShadows::GetRendererData();
        if (!rd || !rd->device || !rd->context)
            return;

        auto* device = reinterpret_cast<ID3D11Device*>(rd->device);
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(rd->context);
        std::uintptr_t* deviceVTable = nullptr;
        std::uintptr_t* contextVTable = nullptr;
        if (!ReadValidatedComVTable(device, 15, &deviceVTable) ||
            !ReadValidatedComVTable(context, 50, &contextVTable)) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreateVertexShaderHookUnavailable,
                false);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreatePixelShaderHookUnavailable,
                false);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kDrawHookUnavailable, false);
            SPDLOG_ERROR(
                "[CloudShadows] RendererData published an invalid D3D11 "
                "device/context COM span; bindings rejected");
            return;
        }
        const bool deviceChanged = g_d3dDevice && g_d3dDevice != device;
        if (deviceChanged) {
            SPDLOG_WARN("[CloudShadows] Renderer device changed (old={} new={}); rebuilding device-owned state",
                (void*)g_d3dDevice, (void*)device);
            // Shader private-data attachments are owned by the old D3D
            // device. Never arm a native capture for the replacement device
            // until that device has created and attached its own authenticated
            // stock-cloud MRT variants.
            FO4CS::NativeSkyCube::SetCaptureConsumerReady(false);
            g_nativeCloudMrtShaderCount.store(0, std::memory_order_release);
            g_nativePrimaryClearHookReady.store(
                false, std::memory_order_release);
            Overlay::Shutdown();
            CloudShadows::ReleaseDeviceResources();
            g_createVSHookInstalled.store(false, std::memory_order_release);
            g_createPSHookInstalled.store(false, std::memory_order_release);
            g_techniqueState = {};
        }

        const bool contextChanged = g_d3dContext != context;
        {
            std::lock_guard bindingLock(g_rendererBindingMutex);
            if (deviceChanged) {
                if (g_ownedRendererSwapChain)
                    FO4CS::RendererLifetime::Retire(
                        g_ownedRendererSwapChain);
                g_ownedRendererSwapChain.Reset();
                g_swapChain = nullptr;
                g_publishedRendererSwapChain.store(
                    nullptr, std::memory_order_release);
            }
            if (g_ownedRendererDevice.Get() != device) {
                if (g_ownedRendererDevice)
                    FO4CS::RendererLifetime::Retire(g_ownedRendererDevice);
                g_ownedRendererDevice = device;
            }
            if (g_ownedRendererContext.Get() != context) {
                if (g_ownedRendererContext)
                    FO4CS::RendererLifetime::Retire(
                        g_ownedRendererContext);
                g_ownedRendererContext = context;
            }
            g_d3dDevice = g_ownedRendererDevice.Get();
            g_d3dContext = g_ownedRendererContext.Get();
            g_publishedRendererDevice.store(
                g_d3dDevice, std::memory_order_release);
            g_publishedRendererContext.store(
                g_d3dContext, std::memory_order_release);
        }
        if (contextChanged) {
            CloudShadows::InvalidateShadowMaskState();
            SPDLOG_INFO("[CloudShadows] Main immediate context captured/updated: {}", (void*)context);
        }

        if (rd->renderWindow[0].swapChain) {
            InstallPresentHook(
                reinterpret_cast<IDXGISwapChain*>(rd->renderWindow[0].swapChain),
                "RendererData maintenance", true);
        }

        g_createVSHookInstalled.store(
            InstallCreateVertexShaderHook(device), std::memory_order_release);
        g_createPSHookInstalled.store(
            InstallCreatePixelShaderHook(device), std::memory_order_release);

        if (!CloudShadows::g_initialized)
            CloudShadows::Initialize();
        CloudShadows::VerifyDrawHookIntegrity();
    }

    void ObserveBeginTechniqueResult(
        void* shader, uint32_t vertexDescriptor, uint32_t hullDescriptor,
        uint32_t domainDescriptor, uint32_t pixelDescriptor,
        bool result)
    {
        FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::Technique);
        FO4CS::RendererLifetime::Scope rendererLifetime(CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire));
        (void)hullDescriptor;
        (void)domainDescriptor;

        // F10 OFF is a true master bypass. Avoid shader-identity VirtualQuery,
        // D3D state inspection, hook maintenance and phase tracking until the
        // Present-boundary key poll enables Cloud Shadows again.
        if (!CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire)) {
            g_techniqueState = {};
            g_worldRenderPhaseActive = false;
            return;
        }

        static std::atomic<uint32_t> btTotal = 0;
        const uint32_t total = ++btTotal;
        auto& phase = g_techniqueState;
        phase = {};

        // Bootstrap immediately, then poll renderer ownership at a bounded
        // cadence. GetRendererData validates relocated memory, so doing it for
        // every one of Fallout's hundreds of techniques per frame caused
        // thousands of VirtualQuery calls per second while the feature was ON.
        if (total <= 10 || !g_d3dContext || (total % 2048u) == 0u)
            MaintainRendererBindings();

        const bool vrTarget =
            FO4CS::RuntimeAPI::GetSingleton().Target() ==
            FO4CS::F4SECompat::RuntimeTarget::kVR;
        if (!result) {
            // Failed selections used to disappear from the VR diagnosis. Keep
            // a bounded record without accepting any failed native binding.
            if (vrTarget && ReadShaderIdentity(shader).Filename() == "Sky") {
                static std::array<std::atomic_uint32_t, 9> failedSky{};
                const auto technique = vertexDescriptor & 0xFFu;
                if (technique < failedSky.size()) {
                    const auto count = failedSky[technique].fetch_add(
                        1, std::memory_order_relaxed) + 1;
                    if (count <= 2) {
                        SPDLOG_INFO("[CloudShadows] VR Sky native selection failed: "
                            "technique={} vDesc=0x{:08X} pDesc=0x{:08X} thread={}",
                            technique, vertexDescriptor, pixelDescriptor,
                            GetCurrentThreadId());
                    }
                }
            }
            return;
        }
        const bool coarseDirectionalSun =
            IsDirectionalSunDescriptor(pixelDescriptor);
        const bool committedCloudField =
            CloudShadows::g_worldCloudActiveLayers.load(
                std::memory_order_acquire) != 0 &&
            CloudShadows::g_worldCloudCommittedEpoch.load(
                std::memory_order_acquire) != 0;
        const bool nativeCaptureActive =
            FO4CS::NativeSkyCube::IsCaptureActive();
        const uint32_t possibleSkyTechnique = vertexDescriptor & 0xFFu;
        const bool possibleSkyDescriptor =
            possibleSkyTechnique >= CloudShadows::kSkyTechniqueTexture &&
            possibleSkyTechnique <= CloudShadows::kSkyTechniqueCloudsFade;

        // Both reviewed runtimes share the coarse direct-sun descriptor bits,
        // but that gate cannot identify VR's DFPrepass/DFComposite world-phase
        // boundaries. Classify exact VR FXP names broadly; shader identities
        // are cached so this does not reintroduce VirtualQuery per technique.
        if (!nativeCaptureActive && !vrTarget && !coarseDirectionalSun &&
            !possibleSkyDescriptor) {
            return;
        }

        const auto shaderIdentity = ReadShaderIdentity(shader);
        if (total == 1 || total == 100 || (total % 10000) == 0) {
            const char* fxp = shaderIdentity.HasFilename() ?
                shaderIdentity.fxpFilename.data() : "<invalid>";
            SPDLOG_INFO("[CloudShadows] BeginTechnique #{} fxp={} type={}",
                total, fxp, shaderIdentity.type);
        }

        if (!shaderIdentity.HasFilename())
            return;

        const std::string_view fxp = shaderIdentity.Filename();
        const bool isDFPrepass = fxp == "DFPrepass";
        const bool isDFLight = fxp == "DFLight" ||
            (vrTarget && fxp == "DFLightVR");
        const bool isDFComposite = fxp == "DFComposite";
        const bool privateCapture = nativeCaptureActive ||
            FO4CS::NativeSkyCube::IsReflectionUpdateActive();

        // All three deferred passes share the generic Lighting shader type.
        // Exact FXP identity is the reviewed world-phase contract used by the
        // working VR renderer: DFPrepass opens the phase, DFComposite closes
        // it, and Present provides a fail-safe frame boundary.
        if (vrTarget && !privateCapture) {
            if (isDFPrepass)
                g_worldRenderPhaseActive = true;
            else if (isDFComposite)
                g_worldRenderPhaseActive = false;
        }
        if (vrTarget && !nativeCaptureActive &&
            !isDFPrepass && !isDFLight && !isDFComposite && fxp != "Sky") {
            return;
        }

        if (isDFPrepass) {
            CloseCaptureEpochForLighting();
            phase.kind = TechniqueKind::kDFPrepass;
        } else if (isDFLight) {
            CloseCaptureEpochForLighting();
            phase.kind = TechniqueKind::kDFLight;
			phase.dfLightPixelDescriptor = pixelDescriptor;

            ID3D11PixelShader* boundPS = nullptr;
            // Flat retains its descriptor gate. VR admits the exact DFLight
            // phase here; AcquireDescriptor then requires an attested full-DXBC
            // sunlight signature before returning any replacement shader.
            const bool possibleDirectionalSun =
                IsDirectionalSunDescriptor(pixelDescriptor);
            const bool mainWorldPhase =
                !vrTarget || g_worldRenderPhaseActive;
            // Geometry bootstrap observes its origin at the authenticated
            // main-Sky draw. BeginTechnique is too early: reflection hosts
            // can advertise a directional technique while posAdjust is zero,
            // repeatedly invalidating the field before any main draw occurs.
            const bool onMainImmediateContext =
                possibleDirectionalSun && mainWorldPhase &&
                committedCloudField && !privateCapture &&
                IsCapturedImmediateContext(g_d3dContext);
            if (onMainImmediateContext)
                GetStaticBoundPixelShader(g_d3dContext, &boundPS);
            ComPtr<ID3D11PixelShader> patched;
            if (onMainImmediateContext && boundPS) {
                patched.Attach(
                    CloudShadows::g_dfLightPatcher.AcquireDescriptor(
                        pixelDescriptor, boundPS));
            }
            phase.sunShaderValidated = patched != nullptr;
            if (phase.sunShaderValidated) {
                phase.dfLightVanillaPS.Attach(boundPS);
                boundPS = nullptr;
                phase.dfLightPatchedPS = std::move(patched);
            }

            static std::atomic<uint32_t> dfLight{ 0 };
            const uint32_t n = ++dfLight;
            if (n <= 10 || (n % 500) == 0) {
                SPDLOG_INFO("[CloudShadows] BeginTechnique DFLight #{} pxlDesc=0x{:08X} boundPS={} strictSun={}",
                    n, pixelDescriptor,
                    static_cast<void*>(phase.sunShaderValidated
                        ? phase.dfLightVanillaPS.Get()
                        : boundPS),
                    phase.sunShaderValidated);
            }

            if (boundPS)
                boundPS->Release();
        } else if (isDFComposite) {
            phase.kind = TechniqueKind::kDFComposite;
        } else if (fxp == "Sky") {
            // DFComposite can be absent on menu/weather paths. The first Sky
            // draw then opens exactly one capture epoch; later cloud layers
            // accumulate into that same pending world-tile set. Main-view
            // ownership is decided at Draw time because FO4 has not reliably
            // installed the final Sky render target at BeginTechnique time.
            // The native reflection pass records each cloud layer's dome-to-UV
            // mapping. The ordinary player Sky pass is also authenticated so
            // that the resolver can apply that same layer's live TexCoordOff
            // every frame without replaying its geometry.
            const bool mainSkyViewAtBegin = false;
            // Fallout 4 encodes BSSky's technique selector in the vertex
            // descriptor. The pixel descriptor is not the authoritative Sky
            // technique ID (unlike DFLight's pixel-shader classification).
            const uint32_t skyTechnique = vertexDescriptor & 0xFFu;
            // Exact Fallout 4 1.10.163 Sky map: 4 is the generic Texture
            // permutation; the cloud field is 5 Clouds, 6 CloudsLerp, and
            // 7 CloudsFade. Texture must never advertise or capture a layer.
            const bool cloudTechnique =
                CloudShadows::IsCloudTechnique(skyTechnique);
            phase.kind = TechniqueKind::kSky;
            phase.skyTechnique = skyTechnique;
            ID3D11VertexShader* boundVertexShader = nullptr;
            ID3D11PixelShader* boundPixelShader = nullptr;
            const bool staticShaders =
                cloudTechnique &&
                IsCapturedImmediateContext(g_d3dContext) &&
                GetStaticBoundSkyShaders(
                    g_d3dContext, &boundVertexShader, &boundPixelShader);
            uint64_t observedVertexHash = 0;
            uint64_t observedPixelHash = 0;
            if (staticShaders) {
                phase.skyShadersAuthenticated = AuthenticateStockSkyShaders(
                    skyTechnique, boundVertexShader, boundPixelShader,
                    &observedVertexHash, &observedPixelHash);
                phase.skyVertexShader.Attach(boundVertexShader);
                phase.skyPixelShader.Attach(boundPixelShader);
            }
            if (cloudTechnique) {
                CloudShadows::g_lastSkyTechnique.store(
                    skyTechnique, std::memory_order_relaxed);
            }
            const uint32_t skyBegin =
                CloudShadows::g_skyDrawsSeen.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            // Loading-screen sky can consume all of the initial samples.
            // Record each real cloud technique independently as it first
            // appears, so periodic background draws cannot hide its state.
            static std::array<std::atomic_uint32_t, 9> skyTechniques{};
            const auto techniqueCount = skyTechnique < skyTechniques.size()
                ? skyTechniques[skyTechnique].fetch_add(1, std::memory_order_relaxed) + 1
                : 0;
            if (skyBegin <= 24 || (skyBegin % 2000) == 0 ||
                (techniqueCount != 0 && techniqueCount <= 2)) {
                SPDLOG_INFO(
                    "[CloudShadows] Sky BeginTechnique #{} technique={} "
                    "vDesc=0x{:08X} pDesc=0x{:08X} mainViewAtBegin={} "
                    "staticShaders={} authenticated={} vsHash=0x{:016X} "
                    "psHash=0x{:016X} tracked={} cloudTechnique={} thread={}",
                    skyBegin, skyTechnique, vertexDescriptor, pixelDescriptor,
                    mainSkyViewAtBegin, staticShaders,
                    phase.skyShadersAuthenticated, observedVertexHash,
                    observedPixelHash,
                    phase.kind == TechniqueKind::kSky,
                    cloudTechnique, GetCurrentThreadId());
            }
        }

    }

    bool WINAPI Detour_BeginTechniqueFlat(
        void* shader, uint32_t vertexDescriptor, uint32_t hullDescriptor,
        uint32_t domainDescriptor, uint32_t pixelDescriptor)
    {
        const bool result = s_original_BeginTechniqueFlat(
            shader, vertexDescriptor, hullDescriptor,
            domainDescriptor, pixelDescriptor);
        ObserveBeginTechniqueResult(
            shader, vertexDescriptor, hullDescriptor, domainDescriptor,
            pixelDescriptor, result);
        return result;
    }

    bool WINAPI Detour_BeginTechniqueVR(
        void* shader, uint32_t vertexDescriptor, uint32_t hullDescriptor,
        uint32_t domainDescriptor, uint32_t pixelDescriptor,
        void* outputStruct)
    {
        ++g_nativeBeginTechniqueDepth;
        const bool result = s_original_BeginTechniqueVR(
            shader, vertexDescriptor, hullDescriptor,
            domainDescriptor, pixelDescriptor, outputStruct);
        --g_nativeBeginTechniqueDepth;
        ObserveBeginTechniqueResult(
            shader, vertexDescriptor, hullDescriptor, domainDescriptor,
            pixelDescriptor, result);
        return result;
    }

    void WINAPI Detour_SetShadersVR(
        void* graphics, void* vertex, void* hull, void* domain, void* pixel)
    {
        s_original_SetShadersVR(graphics, vertex, hull, domain, pixel);
        // BeginTechnique's post-observer already handles ordinary bindings.
        // Cached batches bypass it entirely, so their actual bindings must
        // replace (including clear) the previous draw classification here.
        if (g_nativeBeginTechniqueDepth != 0 || g_internalDrawDepth != 0)
            return;
        const bool enabled = CloudShadows::g_shadowsEnabled.load(
            std::memory_order_acquire);
        FO4CS::RendererLifetime::Scope rendererLifetime(enabled);
        auto& phase = g_techniqueState;
        phase = {};
        if (!enabled) {
            g_worldRenderPhaseActive = false;
            return;
        }
        if (!vertex || !pixel || !IsCapturedImmediateContext(g_d3dContext))
            return;

        const auto pixelDescriptor =
            FO4CS::EngineAPI::detail::ReadField<uint32_t>(pixel);
        // Cached CloudsFade can reuse the Clouds VS wrapper. Its PS descriptor
        // and exact VS/PS bytecode pair identify the actual cloud formula.
        const auto skyTechnique = pixelDescriptor;
        if (CloudShadows::IsCloudTechnique(skyTechnique)) {
            ComPtr<ID3D11VertexShader> boundVS;
            ComPtr<ID3D11PixelShader> boundPS;
            const auto expected = ExpectedStockSkyShaderHashes(
                FO4CS::F4SECompat::RuntimeTarget::kVR, skyTechnique);
            // Descriptor numbers alone also occur in unrelated FXPs. A known
            // cloud VS establishes the candidate; a replaced/unknown cloud PS
            // must then reject the frame rather than publish partial coverage.
            const auto nativeVS = FO4CS::EngineAPI::detail::ReadField<
                ID3D11VertexShader*>(vertex, 8);
            if (SIE::DFLightPatcher::LookupShaderHash(nativeVS) != expected.vertex) {
                return;
            }
            phase.kind = TechniqueKind::kSky;
            phase.skyTechnique = skyTechnique;
            phase.skyShadersAuthenticated = !hull && !domain &&
                GetStaticBoundSkyShaders(g_d3dContext,
                    boundVS.GetAddressOf(), boundPS.GetAddressOf()) &&
                AuthenticateStockSkyShaders(
                    skyTechnique, boundVS.Get(), boundPS.Get());
            phase.skyVertexShader = std::move(boundVS);
            phase.skyPixelShader = std::move(boundPS);
            CloudShadows::g_lastSkyTechnique.store(
                skyTechnique, std::memory_order_relaxed);
            const auto count = CloudShadows::g_skyDrawsSeen.fetch_add(
                1, std::memory_order_relaxed) + 1;
            static std::array<std::atomic_uint32_t, 3> firstCloudBindings{};
            if (firstCloudBindings[skyTechnique - 5].fetch_add(
                    1, std::memory_order_relaxed) < 2) {
                SPDLOG_INFO("[CloudShadows] VR cached Sky binding #{} "
                    "technique={} authenticated={}", count, skyTechnique,
                    phase.skyShadersAuthenticated);
            }
            return;
        }
        // DFPrepass setup can run on a different thread from cached sunlight
        // submission. Its TLS phase is therefore not an authority here. The
        // exact bound sun shader identifies the candidate; Prepass at Draw
        // still requires the actual main sunlight targets, full stereo viewport,
        // main depth and native sun/camera buffers before changing lighting.
        if (!IsDirectionalSunDescriptor(pixelDescriptor) ||
            CloudShadows::g_worldCloudActiveLayers.load(
                std::memory_order_acquire) == 0 ||
            CloudShadows::g_worldCloudCommittedEpoch.load(
                std::memory_order_acquire) == 0 ||
            FO4CS::NativeSkyCube::IsCaptureActive() ||
            FO4CS::NativeSkyCube::IsReflectionUpdateActive()) {
            return;
        }
        ComPtr<ID3D11PixelShader> boundPS;
        if (!GetStaticBoundPixelShader(g_d3dContext, boundPS.GetAddressOf()))
            return;
        ComPtr<ID3D11PixelShader> patched;
        patched.Attach(CloudShadows::g_dfLightPatcher.AcquireDescriptor(
            pixelDescriptor, boundPS.Get()));
        if (!patched)
            return;
        CloseCaptureEpochForLighting();
        phase.kind = TechniqueKind::kDFLight;
        phase.dfLightPixelDescriptor = pixelDescriptor;
        phase.sunShaderValidated = true;
        phase.dfLightVanillaPS = std::move(boundPS);
        phase.dfLightPatchedPS = std::move(patched);
        static std::atomic_uint32_t firstSunBindings{};
        if (firstSunBindings.fetch_add(1, std::memory_order_relaxed) < 2) {
            SPDLOG_INFO("[CloudShadows] VR cached sunlight binding "
                "pxlDesc=0x{:08X} strictSun=true setupWorldPhase={}",
                pixelDescriptor, g_worldRenderPhaseActive);
        }
    }

	std::mutex g_beginTechniqueHookMutex;
	std::atomic_bool g_beginTechniqueHookInstalled{ false };
	std::atomic<std::uint32_t> g_acceptedHostCapabilities{ 0u };

	std::uint32_t BridgeSetHostActive(
		std::uint32_t hostCapabilities) noexcept
	{
		using namespace FO4CloudShadowsMenuBridge;
		std::scoped_lock lock(g_beginTechniqueHookMutex);

		const std::uint32_t existing =
			g_acceptedHostCapabilities.load(std::memory_order_acquire);
		std::uint32_t accepted = existing;
		if ((hostCapabilities & kHostForwardsBeginTechnique) != 0u &&
			!g_beginTechniqueHookInstalled.load(std::memory_order_acquire)) {
			accepted |= kHostForwardsBeginTechnique;
		}
		if ((hostCapabilities & kHostOwnsMenu) != 0u)
			accepted |= kHostOwnsMenu;

		// Both capabilities are one-way process-lifetime claims. In particular,
		// never detach a live engine detour underneath another plugin's chain.
		if ((accepted & kHostOwnsMenu) != 0u)
			Overlay::SetExternalHostActive(true);
		g_acceptedHostCapabilities.store(accepted, std::memory_order_release);
		const std::uint32_t newlyAccepted = accepted & ~existing;
		if (newlyAccepted != 0u) {
			SPDLOG_INFO(
				"[CloudShadows][MenuBridge] accepted host capabilities 0x{:X} "
				"(cumulative=0x{:X})",
				newlyAccepted, accepted);
		}
		if ((hostCapabilities & kHostForwardsBeginTechnique) != 0u &&
			(accepted & kHostForwardsBeginTechnique) == 0u) {
			SPDLOG_WARN(
				"[CloudShadows][MenuBridge] host BeginTechnique forwarding arrived "
				"after the standalone detour; keeping the standalone observer");
		}
		return accepted;
	}

	bool BridgeGetSettings(
		FO4CloudShadowsMenuBridge::SettingsV1* output,
		std::uint32_t outputSize) noexcept
	{
		using FO4CloudShadowsMenuBridge::SettingsV1;
		if (!output || outputSize < sizeof(SettingsV1) ||
			!FO4CS::RuntimeAPI::GetSingleton().ValidateMemory(
				reinterpret_cast<std::uintptr_t>(output), sizeof(SettingsV1),
				FO4CS::AddressKind::kWritable)) {
			return false;
		}

		if (IsRenderThread())
			RefreshBridgeSettingsSnapshotOnRenderThread();
		std::lock_guard lock(g_bridgeSettings.mutex);
		if (!g_bridgeSettings.snapshotInitialized) {
			g_bridgeSettings.snapshot = MakeBridgeSettingsSnapshot(
				CloudShadows::Settings{}, true);
			g_bridgeSettings.snapshotInitialized = true;
		}
		std::memcpy(output, &g_bridgeSettings.snapshot, sizeof(SettingsV1));
		return true;
	}

	bool BridgeApplySettings(
		const FO4CloudShadowsMenuBridge::SettingsV1* input,
		std::uint32_t inputSize) noexcept
	{
		using FO4CloudShadowsMenuBridge::SettingsV1;
		if (!input || inputSize < sizeof(SettingsV1) ||
			!FO4CS::RuntimeAPI::GetSingleton().ValidateMemory(
				reinterpret_cast<std::uintptr_t>(input), sizeof(SettingsV1),
				FO4CS::AddressKind::kReadable)) {
			return false;
		}
		SettingsV1 request{};
		std::memcpy(&request, input, sizeof(request));
		if (request.structSize < sizeof(SettingsV1) ||
			request.abiVersion != FO4CloudShadowsMenuBridge::kAbiVersion)
			return false;

		CloudShadows::Settings next{
			.Opacity = request.opacity,
			.LayerHeightStep = request.layerHeightStep,
			.LayerScaleMultiplier = request.layerScaleMultiplier,
			.VerticalOpticalDepth = request.verticalOpticalDepth,
			.SunAngularRadius = request.sunAngularRadius,
			.MaxOpticalSlant = request.maxOpticalSlant,
			.BaseMipBias = request.baseMipBias,
			.SunFadeStart = request.sunFadeStart,
			.SunFadeEnd = request.sunFadeEnd,
			.CloudHeight = request.cloudHeight,
			.WorldTileSize = request.worldTileSize,
			.DebugMode = request.debugMode,
			.MaxLayers = request.maxLayers,
		};
		CloudShadows::ValidateSettings(next);
		const bool enabled = request.enabled != 0u;
		if (IsRenderThread()) {
			const bool wasEnabled = CloudShadows::g_shadowsEnabled.load(
				std::memory_order_acquire);
			CloudShadows::g_settings = next;
			CloudShadows::g_shadowsEnabled.store(
				enabled, std::memory_order_release);
			if (wasEnabled != enabled)
				CloudShadows::InvalidateWorldCloudCaptureForToggle();
			RefreshBridgeSettingsSnapshotOnRenderThread();
		} else {
			std::lock_guard lock(g_bridgeSettings.mutex);
			g_bridgeSettings.pending = next;
			g_bridgeSettings.pendingEnabled = enabled;
			g_bridgeSettings.hasPending = true;
			g_bridgeSettings.snapshot = MakeBridgeSettingsSnapshot(next, enabled);
			g_bridgeSettings.snapshotInitialized = true;
		}
		return true;
	}

	void BridgeRestoreDefaults() noexcept
	{
		const CloudShadows::Settings defaults{};
		if (IsRenderThread()) {
			FO4CS::GodraysIntegration::SetCloudOcclusionEnabled(false);
			const bool wasEnabled = CloudShadows::g_shadowsEnabled.load(
				std::memory_order_acquire);
			CloudShadows::g_settings = defaults;
			CloudShadows::g_shadowsEnabled.store(true, std::memory_order_release);
			if (!wasEnabled)
				CloudShadows::InvalidateWorldCloudCaptureForToggle();
			RefreshBridgeSettingsSnapshotOnRenderThread();
		} else {
			std::lock_guard lock(g_bridgeSettings.mutex);
			g_bridgeSettings.pending = defaults;
			g_bridgeSettings.pendingEnabled = true;
			g_bridgeSettings.pendingResetGodrayOcclusion = true;
			g_bridgeSettings.hasPending = true;
			g_bridgeSettings.snapshot = MakeBridgeSettingsSnapshot(defaults, true);
			g_bridgeSettings.snapshotInitialized = true;
		}
		CloudShadows::g_singleCloudIsolationEnabled.store(
			false, std::memory_order_release);
		CloudShadows::g_singleCloudRadiusDegrees.store(
			12.0f, std::memory_order_relaxed);
	}

	bool BridgeLoadSettings() noexcept
	{
		if (!IsRenderThread()) {
			SPDLOG_ERROR(
				"[CloudShadows][MenuBridge] LoadSettings rejected off the render thread");
			return false;
		}
		const bool loaded = CloudShadows::LoadSettings();
		RefreshBridgeSettingsSnapshotOnRenderThread();
		return loaded;
	}

	void BridgeSaveSettings() noexcept
	{
		if (!IsRenderThread()) {
			SPDLOG_ERROR(
				"[CloudShadows][MenuBridge] SaveSettings rejected off the render thread");
			return;
		}
		CloudShadows::SaveSettings();
	}

	bool BridgeGetDiagnostics(
		FO4CloudShadowsMenuBridge::DiagnosticsV1* output,
		std::uint32_t outputSize) noexcept
	{
		using FO4CloudShadowsMenuBridge::DiagnosticsV1;
		if (!output || outputSize < sizeof(DiagnosticsV1) ||
			!FO4CS::RuntimeAPI::GetSingleton().ValidateMemory(
				reinterpret_cast<std::uintptr_t>(output), sizeof(DiagnosticsV1),
				FO4CS::AddressKind::kWritable)) {
			return false;
		}

		*output = DiagnosticsV1{
			.structSize = sizeof(DiagnosticsV1),
			.abiVersion = FO4CloudShadowsMenuBridge::kAbiVersion,
			.worldCloudReady = CloudShadows::g_worldCloudReady.load(std::memory_order_acquire) ? 1u : 0u,
			.shadowMaskValid = CloudShadows::g_lastCompletedShadowMaskValid.load(std::memory_order_acquire) ? 1u : 0u,
			.activeLayers = CloudShadows::g_worldCloudActiveLayers.load(std::memory_order_acquire),
			.skyDrawsSeen = CloudShadows::g_skyDrawsSeen.load(std::memory_order_relaxed),
			.cloudDrawCount = CloudShadows::g_cloudDrawCount.load(std::memory_order_relaxed),
			.lastSkyTechnique = CloudShadows::g_lastSkyTechnique.load(std::memory_order_relaxed),
			.tileCaptures = CloudShadows::g_reRenderCount.load(std::memory_order_relaxed),
			.replacementDraws = CloudShadows::g_worldCloudReplacementDraws.load(std::memory_order_relaxed),
			.captureOverflow = CloudShadows::g_worldCloudCaptureOverflow.load(std::memory_order_relaxed),
			.reserved = g_hookUnavailableMask.load(std::memory_order_acquire),
			.pendingEpoch = CloudShadows::g_worldCloudPendingEpoch.load(std::memory_order_relaxed),
			.committedEpoch = CloudShadows::g_worldCloudCommittedEpoch.load(std::memory_order_relaxed),
			.anchorX = CloudShadows::g_projectionAnchorX.load(std::memory_order_relaxed),
			.anchorY = CloudShadows::g_projectionAnchorY.load(std::memory_order_relaxed),
			.anchorZ = CloudShadows::g_projectionAnchorZ.load(std::memory_order_relaxed),
			.reservedFloat = 0.0f,
		};
		return true;
	}

	void BridgeObserveBeginTechnique(
		void* shader,
		std::uint32_t vertexDescriptor,
		std::uint32_t hullDescriptor,
		std::uint32_t domainDescriptor,
		std::uint32_t pixelDescriptor,
		void* outputStruct,
		std::uint32_t succeeded) noexcept
	{
		(void)outputStruct;
		if ((g_acceptedHostCapabilities.load(std::memory_order_acquire) &
			FO4CloudShadowsMenuBridge::kHostForwardsBeginTechnique) == 0u) {
			return;
		}
		try {
			ObserveBeginTechniqueResult(
				shader, vertexDescriptor,
				hullDescriptor, domainDescriptor, pixelDescriptor,
				succeeded != 0u);
		} catch (const std::exception& error) {
			g_techniqueState = {};
			SPDLOG_ERROR(
				"[CloudShadows][MenuBridge] BeginTechnique observer failed: {}",
				error.what());
		} catch (...) {
			g_techniqueState = {};
			SPDLOG_ERROR(
				"[CloudShadows][MenuBridge] BeginTechnique observer failed with an unknown exception");
		}
	}

	const FO4CloudShadowsMenuBridge::ApiV1 g_menuBridgeApi{
		.structSize = sizeof(FO4CloudShadowsMenuBridge::ApiV1),
		.abiVersion = FO4CloudShadowsMenuBridge::kAbiVersion,
		.pluginVersionMajor = 1u,
		.pluginVersionMinor = 0u,
		.pluginVersionPatch = 0u,
		.capabilities = FO4CloudShadowsMenuBridge::kApiCapabilities,
		.setHostActive = &BridgeSetHostActive,
		.getSettings = &BridgeGetSettings,
		.applySettings = &BridgeApplySettings,
		.restoreDefaults = &BridgeRestoreDefaults,
		.loadSettings = &BridgeLoadSettings,
		.saveSettings = &BridgeSaveSettings,
		.getDiagnostics = &BridgeGetDiagnostics,
		.observeBeginTechnique = &BridgeObserveBeginTechnique,
	};
}

namespace CloudShadows
{
    // Draw interception via Detours CODE patches, not vtable writes.
    //
    // Diagnosis 2026-06-11: the immediate context's vtable lives INSIDE the
    // context object (ctx+8, heap) and the D3D11 runtime/NVIDIA driver
    // continuously re-patches its entries to point at driver fast-path
    // functions (observed: our vtable writes overwritten within seconds,
    // every time, forever — no draw thunk ever fired in any session).
    // Patching the code of those driver entry points instead catches every
    // draw from every context and cannot be routed around.
    using PFN_DrawIndexed = void(WINAPI*)(ID3D11DeviceContext*, UINT, UINT, INT);
    using PFN_Draw = void(WINAPI*)(ID3D11DeviceContext*, UINT, UINT);
    using PFN_DrawIndexedInstanced = void(WINAPI*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    using PFN_DrawInstanced = void(WINAPI*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
    using PFN_ClearRenderTargetView = void(WINAPI*)(
        ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);

    // Driver entry points may change after device removal/recreation. Each
    // unique entry gets an immutable process-lifetime trampoline; no old
    // detour is ever retargeted while another context can still execute it.
    constexpr std::size_t kMaximumDrawDetourRoutes = 16;

    template <class TFunction>
    struct DrawDetourRoutes
    {
        std::array<TFunction, kMaximumDrawDetourRoutes> originals{};
        std::array<std::uintptr_t, kMaximumDrawDetourRoutes> targets{};
        std::size_t count{};
    };

    DrawDetourRoutes<PFN_DrawIndexed> s_drawIndexedRoutes;
    DrawDetourRoutes<PFN_Draw> s_drawRoutes;
    DrawDetourRoutes<PFN_DrawIndexedInstanced> s_drawIndexedInstancedRoutes;
    DrawDetourRoutes<PFN_DrawInstanced> s_drawInstancedRoutes;
    DrawDetourRoutes<PFN_ClearRenderTargetView> s_clearRenderTargetRoutes;
    std::mutex s_drawHookMutex;

    thread_local PFN_DrawIndexed s_downstreamDrawIndexed = nullptr;
    thread_local PFN_Draw s_downstreamDraw = nullptr;
    thread_local PFN_DrawIndexedInstanced s_downstreamDrawIndexedInstanced = nullptr;
    thread_local PFN_DrawInstanced s_downstreamDrawInstanced = nullptr;
    thread_local bool g_downstreamDrawInvoked = false;

    template <class TFunction>
    class ScopedDownstreamRoute
    {
    public:
        ScopedDownstreamRoute(TFunction& slot, TFunction value) noexcept :
            slot_(slot), previous_(slot)
        {
            slot_ = value;
        }
        ~ScopedDownstreamRoute() { slot_ = previous_; }
        ScopedDownstreamRoute(const ScopedDownstreamRoute&) = delete;
        ScopedDownstreamRoute& operator=(const ScopedDownstreamRoute&) = delete;

    private:
        TFunction& slot_;
        TFunction previous_;
    };

    uint64_t HashLayerIdentityValue(uint64_t hash, uint64_t value) noexcept
    {
        for (uint32_t byte = 0; byte < 8; ++byte) {
            hash ^= static_cast<uint8_t>(value >> (byte * 8));
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    uint64_t BuildStableLayerId(
        ID3D11DeviceContext* ctx,
        const CloudShadows::CloudDrawCommand& command) noexcept
    {
        uint64_t hash = 0xcbf29ce484222325ULL;

        ID3D11InputLayout* inputLayout = nullptr;
        ctx->IAGetInputLayout(&inputLayout);
        hash = HashLayerIdentityValue(hash, reinterpret_cast<uintptr_t>(inputLayout));

        // Stock Sky geometry is interleaved in IA slot 0. Hashing all 32 IA
        // slots made the logical cloud identity depend on stale, unused driver
        // bindings and could churn slices between otherwise identical frames.
        ID3D11Buffer* vertexBuffer = nullptr;
        UINT vertexStride = 0;
        UINT vertexOffset = 0;
        ctx->IAGetVertexBuffers(0, 1, &vertexBuffer, &vertexStride, &vertexOffset);
        hash = HashLayerIdentityValue(hash, reinterpret_cast<uintptr_t>(vertexBuffer));
        hash = HashLayerIdentityValue(hash, vertexStride);
        hash = HashLayerIdentityValue(hash, vertexOffset);

        ID3D11Buffer* indexBuffer = nullptr;
        DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
        UINT indexOffset = 0;
        const bool indexedDraw =
            command.kind == CloudShadows::CloudDrawKind::DrawIndexed ||
            command.kind == CloudShadows::CloudDrawKind::DrawIndexedInstanced;
        // The IA index binding is ignored by non-indexed draws and may contain
        // stale driver state. Hash a fixed null marker for those kinds so it
        // cannot churn an otherwise stable logical layer.
        if (indexedDraw)
            ctx->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        hash = HashLayerIdentityValue(hash, reinterpret_cast<uintptr_t>(indexBuffer));
        hash = HashLayerIdentityValue(hash, static_cast<uint32_t>(indexFormat));
        hash = HashLayerIdentityValue(hash, indexOffset);

        D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ctx->IAGetPrimitiveTopology(&topology);
        hash = HashLayerIdentityValue(hash, static_cast<uint32_t>(topology));

        // Canonicalise the graphics call to its geometry range. Fallout VR can
        // submit the player Sky as a stereo-instanced draw while the natural
        // reflection face is necessarily single-view; InstanceCount,
        // StartInstanceLocation and the Draw-vs-DrawInstanced enum therefore
        // are not properties of the logical cloud layer.  Hashing them made
        // the live layer fail to match its captured mapping.  The Sky
        // technique and t0/t1 textures are also intentionally excluded because
        // they change during weather lerps while the layer remains the same.
        const bool instancedDraw =
            command.kind == CloudShadows::CloudDrawKind::DrawIndexedInstanced ||
            command.kind == CloudShadows::CloudDrawKind::DrawInstanced;
        hash = HashLayerIdentityValue(hash, indexedDraw ? 1u : 0u);
        hash = HashLayerIdentityValue(hash, command.a);
        hash = HashLayerIdentityValue(
            hash, instancedDraw ? command.c : command.b);
        if (indexedDraw) {
            hash = HashLayerIdentityValue(
                hash, static_cast<uint32_t>(command.d));
        }

        if (inputLayout) inputLayout->Release();
        if (vertexBuffer) vertexBuffer->Release();
        if (indexBuffer) indexBuffer->Release();
        return hash;
    }

    static void InvokeOriginalWorldCloudDraw(
        ID3D11DeviceContext* ctx,
        const CloudShadows::CloudDrawCommand& command)
    {
        ScopedInternalDraw guard;
        // This is the detour's downstream trampoline, not an additional draw.
        // Preserve every vanilla argument, including VR instance count.
        switch (command.kind) {
        case CloudShadows::CloudDrawKind::DrawIndexed:
            if (s_downstreamDrawIndexed) {
                g_downstreamDrawInvoked = true;
                s_downstreamDrawIndexed(ctx, command.a, command.b, command.d);
            }
            break;
        case CloudShadows::CloudDrawKind::Draw:
            if (s_downstreamDraw) {
                g_downstreamDrawInvoked = true;
                s_downstreamDraw(ctx, command.a, command.b);
            }
            break;
        case CloudShadows::CloudDrawKind::DrawIndexedInstanced:
            if (s_downstreamDrawIndexedInstanced) {
                g_downstreamDrawInvoked = true;
                s_downstreamDrawIndexedInstanced(ctx, command.a, command.b,
                    command.c, command.d, command.e);
            }
            break;
        case CloudShadows::CloudDrawKind::DrawInstanced:
            if (s_downstreamDrawInstanced) {
                g_downstreamDrawInvoked = true;
                s_downstreamDrawInstanced(ctx, command.a, command.b,
                    command.c, command.e);
            }
            break;
        }
    }

    bool ProcessNativeCloudDraw(
        ID3D11DeviceContext* context,
        const CloudShadows::CloudDrawCommand& command,
        CloudShadows::CloudDrawReissue downstreamDraw) noexcept
    {
        if (!context || !downstreamDraw ||
            !FO4CS::NativeSkyCube::IsCaptureActive()) {
            return false;
        }

        const uint32_t face =
            FO4CS::NativeSkyCube::ResolveActiveFace(context);
        const auto failCapture = [&]() noexcept {
            MarkNativeCaptureFaceFailed(face);
            return false;
        };
        if (face >= CloudShadows::kWorldCloudCubeFaceCount ||
            !EnsureNativeCaptureFacePrepared(context, face)) {
            return failCapture();
        }
        g_nativeCaptureCloudCandidateFaceMask |= 1u << face;

        auto& phase = g_techniqueState;
        if (phase.kind != TechniqueKind::kSky ||
            !CloudShadows::IsCloudTechnique(phase.skyTechnique) ||
            !phase.skyShadersAuthenticated ||
            !phase.skyVertexShader || !phase.skyPixelShader) {
            return failCapture();
        }

        ComPtr<ID3D11VertexShader> boundVertexShader;
        ComPtr<ID3D11PixelShader> boundPixelShader;
        ID3D11VertexShader* rawVertexShader = nullptr;
        ID3D11PixelShader* rawPixelShader = nullptr;
        if (!GetStaticBoundSkyShaders(
                context, &rawVertexShader, &rawPixelShader)) {
            return failCapture();
        }
        boundVertexShader.Attach(rawVertexShader);
        boundPixelShader.Attach(rawPixelShader);
        if (boundVertexShader.Get() != phase.skyVertexShader.Get() ||
            boundPixelShader.Get() != phase.skyPixelShader.Get()) {
            return failCapture();
        }
        // BeginTechnique authenticated these exact immutable shader objects.
        // Pointer identity above proves that the draw still owns that pair;
        // repeating both private-data hash lookups for every cloud layer only
        // adds COM/driver traffic inside the already expensive cube pass.

        ComPtr<ID3D11PixelShader> mappingPixelShader;
        mappingPixelShader.Attach(AcquireNativeCloudMrtShader(
            boundPixelShader.Get()));
        const FO4CS::CloudMotionResolver::CaptureLayerFace mappingLayer{
            .stableLayerId = command.stableLayerId,
            .faceIndex = face,
            .technique = static_cast<
                FO4CS::CloudMotionResolver::CloudTechnique>(
                    phase.skyTechnique)
        };
        FO4CS::CloudMotionResolver::CaptureFaceTarget mappingTarget;
        if (!mappingPixelShader || command.stableLayerId == 0 ||
            !FO4CS::CloudMotionResolver::AcquireLayerMappingTarget(
                context, mappingLayer, mappingTarget) ||
            !mappingTarget.mappingRtv) {
            return failCapture();
        }

        std::array<ID3D11RenderTargetView*,
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rawTargets{};
        ID3D11DepthStencilView* rawDepthStencil = nullptr;
        context->OMGetRenderTargets(
            static_cast<UINT>(rawTargets.size()), rawTargets.data(),
            &rawDepthStencil);
        std::array<ComPtr<ID3D11RenderTargetView>,
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> originalTargets;
        for (size_t index = 0; index < rawTargets.size(); ++index)
            originalTargets[index].Attach(rawTargets[index]);
        ComPtr<ID3D11DepthStencilView> originalDepthStencil;
        originalDepthStencil.Attach(rawDepthStencil);
        if (!originalTargets[0] || originalTargets[1])
            return failCapture();

        ID3D11BlendState* rawBlendState = nullptr;
        std::array<float, 4> blendFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
        UINT sampleMask = UINT_MAX;
        context->OMGetBlendState(
            &rawBlendState, blendFactor.data(), &sampleMask);
        ComPtr<ID3D11BlendState> originalBlendState;
        originalBlendState.Attach(rawBlendState);
        ID3D11BlendState* mappingBlendState =
            CloudShadows::GetNativeWorldCloudCaptureBlendState(
                originalBlendState.Get());
        if (!mappingBlendState)
            return failCapture();

        std::array<ID3D11RenderTargetView*,
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> captureTargets{};
        for (size_t index = 0; index < originalTargets.size(); ++index)
            captureTargets[index] = originalTargets[index].Get();
        // Piggyback the naturally scheduled cloud draw. The authenticated MRT
        // shader preserves its stock Target0 result and copies visible-cloud
        // alpha to private Target1, so Fallout's colour cube remains exact.
        captureTargets[CloudShadows::kNativeWorldCloudOpacityTargetSlot] =
            mappingTarget.mappingRtv.Get();

        context->OMSetRenderTargets(
            static_cast<UINT>(captureTargets.size()),
            captureTargets.data(), originalDepthStencil.Get());
        context->OMSetBlendState(
            mappingBlendState, blendFactor.data(), sampleMask);
        context->PSSetShader(mappingPixelShader.Get(), nullptr, 0);

        // Exactly one downstream vanilla draw executes for this submission.
        const bool priorDownstreamDrawInvoked = g_downstreamDrawInvoked;
        g_downstreamDrawInvoked = false;
        downstreamDraw(context, command);
        const bool drawInvoked = g_downstreamDrawInvoked;
        g_downstreamDrawInvoked = priorDownstreamDrawInvoked;

        context->PSSetShader(boundPixelShader.Get(), nullptr, 0);
        context->OMSetBlendState(
            originalBlendState.Get(), blendFactor.data(), sampleMask);
        std::array<ID3D11RenderTargetView*,
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> restoreTargets{};
        for (size_t index = 0; index < originalTargets.size(); ++index)
            restoreTargets[index] = originalTargets[index].Get();
        context->OMSetRenderTargets(
            static_cast<UINT>(restoreTargets.size()),
            restoreTargets.data(), originalDepthStencil.Get());

        const bool mappingCompleted =
            FO4CS::CloudMotionResolver::CompleteLayerMapping(
                mappingLayer, drawInvoked);
        if (!drawInvoked || !mappingCompleted) {
            MarkNativeCaptureFaceFailed(face);
            // `true` means the detour must not issue a fallback draw. Once the
            // downstream trampoline has submitted vanilla geometry, capture
            // bookkeeping failure can reject only this refresh; replaying the
            // draw would corrupt RT0 and violate passive exactly-once capture.
            return drawInvoked;
        }

        FO4CS::NativeSkyCube::MarkCloudDrawCaptured(context);
        // Compatibility telemetry names predate passive MRT capture. These
        // counters now count piggybacked cloud submissions, not extra draws.
        CloudShadows::g_reRenderCount.fetch_add(
            1, std::memory_order_relaxed);
        CloudShadows::g_worldCloudReplacementDraws.fetch_add(
            1, std::memory_order_relaxed);
        const uint32_t drawOrdinal =
            CloudShadows::g_cloudDrawCount.fetch_add(
                1, std::memory_order_relaxed) + 1;
        if (drawOrdinal <= 12 || (drawOrdinal % 2000) == 0) {
            SPDLOG_INFO(
                "[CloudShadows] Passive cloud MRT draw #{} "
                "technique={} face={} serial={}",
                drawOrdinal, phase.skyTechnique, face,
                FO4CS::NativeSkyCube::ActiveCaptureSerial());
        }
        return true;
    }

    bool ProcessCloudDraw(
        ID3D11DeviceContext* ctx,
        CloudShadows::CloudDrawCommand command) noexcept
    {
        FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::CloudDrawValidation);
        if (g_internalDrawDepth != 0 || !IsCapturedImmediateContext(ctx))
            return false;
        auto& phase = g_techniqueState;
        if (phase.kind != TechniqueKind::kSky) {
            return false;
        }

        // F10/the menu is a true master bypass. Natural reflection rendering
        // remains untouched; only the private MRT attachment is disabled.
        if (!CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire))
            return false;

        const bool nativeCapture =
            FO4CS::NativeSkyCube::IsCaptureActive();
        if (!nativeCapture && !CloudShadows::IsMainSkyWorldView(ctx)) {
            if (CloudShadows::IsCloudTechnique(phase.skyTechnique)) {
                static std::atomic_uint32_t mainSkyRejections{};
                const auto count = mainSkyRejections.fetch_add(
                    1, std::memory_order_relaxed) + 1;
                if (count <= 4 || (count & (count - 1)) == 0) {
                    CloudShadows::LogMainSkyWorldViewRejection(
                        ctx, phase.skyTechnique, count);
                }
            }
            return false;
        }

        command.skyTechnique = phase.skyTechnique;
        command.authentication = phase.skyShadersAuthenticated
            ? CloudShadows::CloudDrawAuthentication::kStockVisibleCloudShaders
            : CloudShadows::CloudDrawAuthentication::kUnauthenticated;
        if (CloudShadows::IsCloudTechnique(phase.skyTechnique))
            command.stableLayerId = BuildStableLayerId(ctx, command);

        if (!nativeCapture && CloudShadows::IsCloudTechnique(phase.skyTechnique)) {
            // BeginTechnique alone cannot attest a later draw: another hook
            // may have replaced either shader in between. Reject its cloud
            // data while still submitting the visible draw exactly once.
            ComPtr<ID3D11VertexShader> vertexShader;
            ComPtr<ID3D11PixelShader> pixelShader;
            if (!GetStaticBoundSkyShaders(ctx, vertexShader.GetAddressOf(),
                    pixelShader.GetAddressOf()) ||
                vertexShader.Get() != phase.skyVertexShader.Get() ||
                pixelShader.Get() != phase.skyPixelShader.Get()) {
                command.authentication =
                    CloudShadows::CloudDrawAuthentication::kUnauthenticated;
            }
        }

        // Natural reflection Sky is the sole opacity producer. A successful
        // path invokes the one original draw with stock RT0 plus private RT1.
        // Rejection returns false so the caller executes the vanilla draw with
        // untouched state; no failure path is allowed to suppress rendering.
        cpuTiming.Stop();
        if (nativeCapture)
            return ProcessNativeCloudDraw(
                ctx, command, &InvokeOriginalWorldCloudDraw);

        // Submit the visible Sky once. Private low-resolution draws then blend
        // complete cloud opacity, including overlapping surfaces in one mesh.
        return CloudShadows::ProcessWorldCloudDraw(
            ctx, command, &InvokeOriginalWorldCloudDraw);
    }

    template <std::size_t Route>
    static void WINAPI Detour_DrawIndexed(
        ID3D11DeviceContext* This, UINT IndexCount,
        UINT StartIndexLocation, INT BaseVertexLocation)
    {
        FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::DrawHook);
        const auto original = s_drawIndexedRoutes.originals[Route];
        if (!original)
            return;
        FO4CS::RendererLifetime::Scope rendererLifetime(
            ((CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire) &&
                 (g_techniqueState.kind == TechniqueKind::kSky ||
                  g_techniqueState.kind == TechniqueKind::kDFLight ||
                  FO4CS::GodraysIntegration::IsCloudOcclusionEnabled())) ||
                FO4CS::NativeSkyCube::IsCaptureActive()) &&
            This == g_publishedRendererContext.load(std::memory_order_acquire));
        ObserveNativeCubeDraw(This);
        if (!CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire)) {
            original(This, IndexCount, StartIndexLocation, BaseVertexLocation);
            return;
        }
        if (g_internalDrawDepth != 0) {
            original(This, IndexCount, StartIndexLocation, BaseVertexLocation);
            return;
        }
        auto godraySwap = FO4CS::GodraysIntegration::BeginDraw(This);
        if (godraySwap.swapped) {
            original(This, IndexCount, StartIndexLocation, BaseVertexLocation);
            FO4CS::GodraysIntegration::EndDraw(
                This, godraySwap, true);
            return;
        }
        const auto& phase = g_techniqueState;
        const bool nativePassiveCapture =
            FO4CS::NativeSkyCube::IsCaptureActive() &&
            IsCapturedImmediateContext(This);
        const bool nativeCloudDraw =
            nativePassiveCapture &&
            phase.kind == TechniqueKind::kSky &&
            CloudShadows::IsCloudTechnique(phase.skyTechnique);
        if (nativePassiveCapture) {
            bool captured = false;
            if (nativeCloudDraw) {
                ScopedDownstreamRoute route(
                    s_downstreamDrawIndexed, original);
                captured = ProcessCloudDraw(This, {
                    CloudShadows::CloudDrawKind::DrawIndexed,
                    IndexCount, StartIndexLocation, 0,
                    BaseVertexLocation, 0 });
            }
            // ProcessCloudDraw invokes the downstream draw exactly once only
            // on successful MRT setup. Every other path remains vanilla.
            if (!captured)
                original(This, IndexCount, StartIndexLocation,
                    BaseVertexLocation);
            return;
        }
        if (phase.kind == TechniqueKind::kSky &&
            IsCapturedImmediateContext(This)) {
            ScopedDownstreamRoute route(s_downstreamDrawIndexed, original);
            if (!ProcessCloudDraw(This, {
                    CloudShadows::CloudDrawKind::DrawIndexed,
                    IndexCount, StartIndexLocation, 0,
                    BaseVertexLocation, 0 })) {
                original(This, IndexCount, StartIndexLocation,
                    BaseVertexLocation);
            }
            return;
        }
        const bool cloudLitDraw =
            phase.kind == TechniqueKind::kDFLight &&
            phase.sunShaderValidated;
        if (!cloudLitDraw || !IsCapturedImmediateContext(This)) {
            original(This, IndexCount, StartIndexLocation, BaseVertexLocation);
            return;
        }
        if (cloudLitDraw) {
            auto swap = BeginSwap(This);
            original(This, IndexCount, StartIndexLocation, BaseVertexLocation);
            RecordSubmittedSwap(swap);
            EndSwap(This, swap);
            return;
        }
        original(This, IndexCount, StartIndexLocation, BaseVertexLocation);
    }

    template <std::size_t Route>
    static void WINAPI Detour_Draw(
        ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation)
    {
        FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::DrawHook);
        const auto original = s_drawRoutes.originals[Route];
        if (!original)
            return;
        FO4CS::RendererLifetime::Scope rendererLifetime(
            ((CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire) &&
                 (g_techniqueState.kind == TechniqueKind::kSky ||
                  g_techniqueState.kind == TechniqueKind::kDFLight ||
                  FO4CS::GodraysIntegration::IsCloudOcclusionEnabled())) ||
                FO4CS::NativeSkyCube::IsCaptureActive()) &&
            This == g_publishedRendererContext.load(std::memory_order_acquire));
        ObserveNativeCubeDraw(This);
        if (!CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire)) {
            original(This, VertexCount, StartVertexLocation);
            return;
        }
        if (g_internalDrawDepth != 0) {
            original(This, VertexCount, StartVertexLocation);
            return;
        }
        auto godraySwap = FO4CS::GodraysIntegration::BeginDraw(This);
        if (godraySwap.swapped) {
            original(This, VertexCount, StartVertexLocation);
            FO4CS::GodraysIntegration::EndDraw(
                This, godraySwap, true);
            return;
        }
        const auto& phase = g_techniqueState;
        const bool nativePassiveCapture =
            FO4CS::NativeSkyCube::IsCaptureActive() &&
            IsCapturedImmediateContext(This);
        const bool nativeCloudDraw =
            nativePassiveCapture &&
            phase.kind == TechniqueKind::kSky &&
            CloudShadows::IsCloudTechnique(phase.skyTechnique);
        if (nativePassiveCapture) {
            bool captured = false;
            if (nativeCloudDraw) {
                ScopedDownstreamRoute route(s_downstreamDraw, original);
                captured = ProcessCloudDraw(This, {
                    CloudShadows::CloudDrawKind::Draw,
                    VertexCount, StartVertexLocation, 0, 0, 0 });
            }
            if (!captured)
                original(This, VertexCount, StartVertexLocation);
            return;
        }
        if (phase.kind == TechniqueKind::kSky &&
            IsCapturedImmediateContext(This)) {
            ScopedDownstreamRoute route(s_downstreamDraw, original);
            if (!ProcessCloudDraw(This, {
                    CloudShadows::CloudDrawKind::Draw,
                    VertexCount, StartVertexLocation, 0, 0, 0 })) {
                original(This, VertexCount, StartVertexLocation);
            }
            return;
        }
        const bool cloudLitDraw =
            phase.kind == TechniqueKind::kDFLight &&
            phase.sunShaderValidated;
        if (!cloudLitDraw || !IsCapturedImmediateContext(This)) {
            original(This, VertexCount, StartVertexLocation);
            return;
        }
        if (cloudLitDraw) {
            auto swap = BeginSwap(This);
            original(This, VertexCount, StartVertexLocation);
            RecordSubmittedSwap(swap);
            EndSwap(This, swap);
            return;
        }
        original(This, VertexCount, StartVertexLocation);
    }

    template <std::size_t Route>
    static void WINAPI Detour_DrawIndexedInstanced(
        ID3D11DeviceContext* This, UINT IndexCountPerInstance,
        UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
        UINT StartInstanceLocation)
    {
        FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::DrawHook);
        const auto original = s_drawIndexedInstancedRoutes.originals[Route];
        if (!original)
            return;
        FO4CS::RendererLifetime::Scope rendererLifetime(
            ((CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire) &&
                 (g_techniqueState.kind == TechniqueKind::kSky ||
                  g_techniqueState.kind == TechniqueKind::kDFLight ||
                  FO4CS::GodraysIntegration::IsCloudOcclusionEnabled())) ||
                FO4CS::NativeSkyCube::IsCaptureActive()) &&
            This == g_publishedRendererContext.load(std::memory_order_acquire));
        ObserveNativeCubeDraw(This);
        if (!CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire)) {
            original(This, IndexCountPerInstance, InstanceCount,
                StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            return;
        }
        if (g_internalDrawDepth != 0) {
            original(This, IndexCountPerInstance, InstanceCount,
                StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            return;
        }
        auto godraySwap = FO4CS::GodraysIntegration::BeginDraw(This);
        if (godraySwap.swapped) {
            original(This, IndexCountPerInstance, InstanceCount,
                StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            FO4CS::GodraysIntegration::EndDraw(
                This, godraySwap, true);
            return;
        }
        const auto& phase = g_techniqueState;
        const bool nativePassiveCapture =
            FO4CS::NativeSkyCube::IsCaptureActive() &&
            IsCapturedImmediateContext(This);
        const bool nativeCloudDraw =
            nativePassiveCapture &&
            phase.kind == TechniqueKind::kSky &&
            CloudShadows::IsCloudTechnique(phase.skyTechnique);
        if (nativePassiveCapture) {
            bool captured = false;
            if (nativeCloudDraw) {
                ScopedDownstreamRoute route(
                    s_downstreamDrawIndexedInstanced, original);
                captured = ProcessCloudDraw(This, {
                    CloudShadows::CloudDrawKind::DrawIndexedInstanced,
                    IndexCountPerInstance, InstanceCount,
                    StartIndexLocation, BaseVertexLocation,
                    StartInstanceLocation });
            }
            if (!captured) {
                original(This, IndexCountPerInstance, InstanceCount,
                    StartIndexLocation, BaseVertexLocation,
                    StartInstanceLocation);
            }
            return;
        }
        if (phase.kind == TechniqueKind::kSky &&
            IsCapturedImmediateContext(This)) {
            ScopedDownstreamRoute route(
                s_downstreamDrawIndexedInstanced, original);
            if (!ProcessCloudDraw(This, {
                    CloudShadows::CloudDrawKind::DrawIndexedInstanced,
                    IndexCountPerInstance, InstanceCount,
                    StartIndexLocation, BaseVertexLocation,
                    StartInstanceLocation })) {
                original(This, IndexCountPerInstance, InstanceCount,
                    StartIndexLocation, BaseVertexLocation,
                    StartInstanceLocation);
            }
            return;
        }
        const bool cloudLitDraw =
            phase.kind == TechniqueKind::kDFLight &&
            phase.sunShaderValidated;
        if (!cloudLitDraw || !IsCapturedImmediateContext(This)) {
            original(This, IndexCountPerInstance, InstanceCount,
                StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            return;
        }
        if (cloudLitDraw) {
            auto swap = BeginSwap(This);
            original(This, IndexCountPerInstance, InstanceCount,
                StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
            RecordSubmittedSwap(swap);
            EndSwap(This, swap);
            return;
        }
        original(This, IndexCountPerInstance, InstanceCount,
            StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
    }

    template <std::size_t Route>
    static void WINAPI Detour_DrawInstanced(
        ID3D11DeviceContext* This, UINT VertexCountPerInstance,
        UINT InstanceCount, UINT StartVertexLocation,
        UINT StartInstanceLocation)
    {
        FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::DrawHook);
        const auto original = s_drawInstancedRoutes.originals[Route];
        if (!original)
            return;
        FO4CS::RendererLifetime::Scope rendererLifetime(
            ((CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire) &&
                 (g_techniqueState.kind == TechniqueKind::kSky ||
                  g_techniqueState.kind == TechniqueKind::kDFLight ||
                  FO4CS::GodraysIntegration::IsCloudOcclusionEnabled())) ||
                FO4CS::NativeSkyCube::IsCaptureActive()) &&
            This == g_publishedRendererContext.load(std::memory_order_acquire));
        ObserveNativeCubeDraw(This);
        if (!CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire)) {
            original(This, VertexCountPerInstance, InstanceCount,
                StartVertexLocation, StartInstanceLocation);
            return;
        }
        if (g_internalDrawDepth != 0) {
            original(This, VertexCountPerInstance, InstanceCount,
                StartVertexLocation, StartInstanceLocation);
            return;
        }
        auto godraySwap = FO4CS::GodraysIntegration::BeginDraw(This);
        if (godraySwap.swapped) {
            original(This, VertexCountPerInstance, InstanceCount,
                StartVertexLocation, StartInstanceLocation);
            FO4CS::GodraysIntegration::EndDraw(
                This, godraySwap, true);
            return;
        }
        const auto& phase = g_techniqueState;
        const bool nativePassiveCapture =
            FO4CS::NativeSkyCube::IsCaptureActive() &&
            IsCapturedImmediateContext(This);
        const bool nativeCloudDraw =
            nativePassiveCapture &&
            phase.kind == TechniqueKind::kSky &&
            CloudShadows::IsCloudTechnique(phase.skyTechnique);
        if (nativePassiveCapture) {
            bool captured = false;
            if (nativeCloudDraw) {
                ScopedDownstreamRoute route(
                    s_downstreamDrawInstanced, original);
                captured = ProcessCloudDraw(This, {
                    CloudShadows::CloudDrawKind::DrawInstanced,
                    VertexCountPerInstance, InstanceCount,
                    StartVertexLocation, 0, StartInstanceLocation });
            }
            if (!captured) {
                original(This, VertexCountPerInstance, InstanceCount,
                    StartVertexLocation, StartInstanceLocation);
            }
            return;
        }
        if (phase.kind == TechniqueKind::kSky &&
            IsCapturedImmediateContext(This)) {
            ScopedDownstreamRoute route(s_downstreamDrawInstanced, original);
            if (!ProcessCloudDraw(This, {
                    CloudShadows::CloudDrawKind::DrawInstanced,
                    VertexCountPerInstance, InstanceCount,
                    StartVertexLocation, 0, StartInstanceLocation })) {
                original(This, VertexCountPerInstance, InstanceCount,
                    StartVertexLocation, StartInstanceLocation);
            }
            return;
        }
        const bool cloudLitDraw =
            phase.kind == TechniqueKind::kDFLight &&
            phase.sunShaderValidated;
        if (!cloudLitDraw || !IsCapturedImmediateContext(This)) {
            original(This, VertexCountPerInstance, InstanceCount,
                StartVertexLocation, StartInstanceLocation);
            return;
        }
        if (cloudLitDraw) {
            auto swap = BeginSwap(This);
            original(This, VertexCountPerInstance, InstanceCount,
                StartVertexLocation, StartInstanceLocation);
            RecordSubmittedSwap(swap);
            EndSwap(This, swap);
            return;
        }
        original(This, VertexCountPerInstance, InstanceCount,
            StartVertexLocation, StartInstanceLocation);
    }

    template <std::size_t Route>
    static void WINAPI Detour_ClearRenderTargetView(
        ID3D11DeviceContext* This,
        ID3D11RenderTargetView* RenderTargetView,
        const FLOAT ColorRGBA[4])
    {
        const auto original = s_clearRenderTargetRoutes.originals[Route];
        if (!original)
            return;
        // Passive capture never owns or suppresses Fallout's colour-cube clear.
        original(This, RenderTargetView, ColorRGBA);
    }

    template <std::size_t... Routes>
    [[nodiscard]] constexpr auto MakeDrawIndexedDetours(
        std::index_sequence<Routes...>) noexcept
    {
        return std::array<PFN_DrawIndexed, sizeof...(Routes)>{
            &Detour_DrawIndexed<Routes>...
        };
    }

    template <std::size_t... Routes>
    [[nodiscard]] constexpr auto MakeDrawDetours(
        std::index_sequence<Routes...>) noexcept
    {
        return std::array<PFN_Draw, sizeof...(Routes)>{
            &Detour_Draw<Routes>...
        };
    }

    template <std::size_t... Routes>
    [[nodiscard]] constexpr auto MakeDrawIndexedInstancedDetours(
        std::index_sequence<Routes...>) noexcept
    {
        return std::array<PFN_DrawIndexedInstanced, sizeof...(Routes)>{
            &Detour_DrawIndexedInstanced<Routes>...
        };
    }

    template <std::size_t... Routes>
    [[nodiscard]] constexpr auto MakeDrawInstancedDetours(
        std::index_sequence<Routes...>) noexcept
    {
        return std::array<PFN_DrawInstanced, sizeof...(Routes)>{
            &Detour_DrawInstanced<Routes>...
        };
    }

    template <std::size_t... Routes>
    [[nodiscard]] constexpr auto MakeClearRenderTargetDetours(
        std::index_sequence<Routes...>) noexcept
    {
        return std::array<PFN_ClearRenderTargetView, sizeof...(Routes)>{
            &Detour_ClearRenderTargetView<Routes>...
        };
    }

    constexpr auto s_drawIndexedDetours = MakeDrawIndexedDetours(
        std::make_index_sequence<kMaximumDrawDetourRoutes>{});
    constexpr auto s_drawDetours = MakeDrawDetours(
        std::make_index_sequence<kMaximumDrawDetourRoutes>{});
    constexpr auto s_drawIndexedInstancedDetours =
        MakeDrawIndexedInstancedDetours(
            std::make_index_sequence<kMaximumDrawDetourRoutes>{});
    constexpr auto s_drawInstancedDetours = MakeDrawInstancedDetours(
        std::make_index_sequence<kMaximumDrawDetourRoutes>{});
    constexpr auto s_clearRenderTargetDetours =
        MakeClearRenderTargetDetours(
            std::make_index_sequence<kMaximumDrawDetourRoutes>{});

    template <class TFunction>
    [[nodiscard]] bool ReserveDrawRoute(
        DrawDetourRoutes<TFunction>& routes,
        std::uintptr_t target,
        std::size_t& route,
        bool& added) noexcept
    {
        for (std::size_t index = 0; index < routes.count; ++index) {
            if (routes.targets[index] == target) {
                route = index;
                added = false;
                return true;
            }
        }
        if (routes.count >= routes.targets.size())
            return false;
        route = routes.count++;
        routes.targets[route] = target;
        routes.originals[route] = reinterpret_cast<TFunction>(target);
        added = true;
        return true;
    }

    template <class TFunction>
    void RollBackDrawRoute(
        DrawDetourRoutes<TFunction>& routes,
        std::size_t route,
        bool added) noexcept
    {
        if (!added)
            return;
        // A verification pass reserves at most one tail entry per method.
        if (routes.count == route + 1)
            --routes.count;
        routes.targets[route] = 0;
        routes.originals[route] = nullptr;
    }

    bool NativeCaptureRoutesCurrent() noexcept
    {
        FO4CS::RendererLifetime::Scope rendererLifetime;
        auto* context = g_d3dContext;
        if (!IsCapturedImmediateContext(context) ||
            !g_nativePrimaryClearHookReady.load(
                std::memory_order_acquire)) {
            return false;
        }

        auto* vtable = *reinterpret_cast<std::uintptr_t**>(context);
        if (!vtable)
            return false;
        const std::array<std::uintptr_t, 5> current{
            std::atomic_ref<std::uintptr_t>(vtable[12]).load(
                std::memory_order_acquire),
            std::atomic_ref<std::uintptr_t>(vtable[13]).load(
                std::memory_order_acquire),
            std::atomic_ref<std::uintptr_t>(vtable[20]).load(
                std::memory_order_acquire),
            std::atomic_ref<std::uintptr_t>(vtable[21]).load(
                std::memory_order_acquire),
            std::atomic_ref<std::uintptr_t>(vtable[50]).load(
                std::memory_order_acquire)
        };

        std::lock_guard lock(s_drawHookMutex);
        const auto routed = [](const auto& routes,
                                std::uintptr_t target) noexcept {
            for (std::size_t index = 0; index < routes.count; ++index) {
                if (routes.targets[index] == target)
                    return true;
            }
            return false;
        };
        return routed(s_drawIndexedRoutes, current[0]) &&
            routed(s_drawRoutes, current[1]) &&
            routed(s_drawIndexedInstancedRoutes, current[2]) &&
            routed(s_drawInstancedRoutes, current[3]) &&
            routed(s_clearRenderTargetRoutes, current[4]);
    }

    void VerifyDrawHookIntegrity() noexcept
    {
        FO4CS::RendererLifetime::Scope rendererLifetime;
        std::lock_guard hookLock(s_drawHookMutex);

        auto* ctx = g_d3dContext;
        if (!IsCapturedImmediateContext(ctx))
            return;
        std::uintptr_t* vtbl = nullptr;
        if (!ReadValidatedComVTable(ctx, 50, &vtbl)) {
            g_nativePrimaryClearHookReady.store(
                false, std::memory_order_release);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kDrawHookUnavailable, false);
            return;
        }
        const std::array<std::uintptr_t, 5> targets{
            vtbl[12], vtbl[13], vtbl[20], vtbl[21], vtbl[50]
        };

        auto executable = [](std::uintptr_t address) noexcept {
            if (!address)
                return false;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                return false;
            }
            const DWORD protection = mbi.Protect & 0xFFu;
            return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        };
        for (auto target : targets) {
            if (!executable(target)) {
                g_nativePrimaryClearHookReady.store(
                    false, std::memory_order_release);
                SetHookAvailable(
                    FO4CloudShadowsMenuBridge::kDrawHookUnavailable, false);
                SPDLOG_ERROR("[CloudShadows] Refusing draw detour: non-executable vtable target 0x{:016X}",
                    static_cast<uint64_t>(target));
                return;
            }
        }

        std::size_t drawIndexedRoute = 0;
        std::size_t drawRoute = 0;
        std::size_t drawIndexedInstancedRoute = 0;
        std::size_t drawInstancedRoute = 0;
        std::size_t clearRenderTargetRoute = 0;
        bool addDrawIndexed = false;
        bool addDraw = false;
        bool addDrawIndexedInstanced = false;
        bool addDrawInstanced = false;
        bool addClearRenderTarget = false;
        const bool capacityReady =
            ReserveDrawRoute(s_drawIndexedRoutes, targets[0],
                drawIndexedRoute, addDrawIndexed) &&
            ReserveDrawRoute(s_drawRoutes, targets[1],
                drawRoute, addDraw) &&
            ReserveDrawRoute(s_drawIndexedInstancedRoutes, targets[2],
                drawIndexedInstancedRoute, addDrawIndexedInstanced) &&
            ReserveDrawRoute(s_drawInstancedRoutes, targets[3],
                drawInstancedRoute, addDrawInstanced) &&
            ReserveDrawRoute(s_clearRenderTargetRoutes, targets[4],
                clearRenderTargetRoute, addClearRenderTarget);
        if (!capacityReady) {
            RollBackDrawRoute(
                s_drawIndexedRoutes, drawIndexedRoute, addDrawIndexed);
            RollBackDrawRoute(s_drawRoutes, drawRoute, addDraw);
            RollBackDrawRoute(s_drawIndexedInstancedRoutes,
                drawIndexedInstancedRoute, addDrawIndexedInstanced);
            RollBackDrawRoute(
                s_drawInstancedRoutes, drawInstancedRoute, addDrawInstanced);
            RollBackDrawRoute(
                s_clearRenderTargetRoutes, clearRenderTargetRoute,
                addClearRenderTarget);
            g_nativePrimaryClearHookReady.store(
                false, std::memory_order_release);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kDrawHookUnavailable, false);
            SPDLOG_CRITICAL(
                "[CloudShadows] Draw-detour route capacity exhausted after {} "
                "device generations",
                kMaximumDrawDetourRoutes);
            return;
        }

        if (!addDrawIndexed && !addDraw && !addDrawIndexedInstanced &&
            !addDrawInstanced && !addClearRenderTarget) {
            g_nativePrimaryClearHookReady.store(
                true, std::memory_order_release);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kDrawHookUnavailable, true);
            return;
        }

        DetourThreadEnlistment threads;
        LONG err = DetourTransactionBegin();
        if (err == NO_ERROR) err = threads.EnlistProcessThreads();
        if (err == NO_ERROR && addDrawIndexed) {
            err = DetourAttach(
                reinterpret_cast<PVOID*>(
                    &s_drawIndexedRoutes.originals[drawIndexedRoute]),
                reinterpret_cast<PVOID>(
                    s_drawIndexedDetours[drawIndexedRoute]));
        }
        if (err == NO_ERROR && addDraw) {
            err = DetourAttach(
                reinterpret_cast<PVOID*>(
                    &s_drawRoutes.originals[drawRoute]),
                reinterpret_cast<PVOID>(s_drawDetours[drawRoute]));
        }
        if (err == NO_ERROR && addDrawIndexedInstanced) {
            err = DetourAttach(
                reinterpret_cast<PVOID*>(
                    &s_drawIndexedInstancedRoutes.originals[
                        drawIndexedInstancedRoute]),
                reinterpret_cast<PVOID>(
                    s_drawIndexedInstancedDetours[
                        drawIndexedInstancedRoute]));
        }
        if (err == NO_ERROR && addDrawInstanced) {
            err = DetourAttach(
                reinterpret_cast<PVOID*>(
                    &s_drawInstancedRoutes.originals[drawInstancedRoute]),
                reinterpret_cast<PVOID>(
                    s_drawInstancedDetours[drawInstancedRoute]));
        }
        if (err == NO_ERROR && addClearRenderTarget) {
            err = DetourAttach(
                reinterpret_cast<PVOID*>(
                    &s_clearRenderTargetRoutes.originals[
                        clearRenderTargetRoute]),
                reinterpret_cast<PVOID>(
                    s_clearRenderTargetDetours[clearRenderTargetRoute]));
        }
        if (err != NO_ERROR) {
            DetourTransactionAbort();
            RollBackDrawRoute(
                s_drawIndexedRoutes, drawIndexedRoute, addDrawIndexed);
            RollBackDrawRoute(s_drawRoutes, drawRoute, addDraw);
            RollBackDrawRoute(s_drawIndexedInstancedRoutes,
                drawIndexedInstancedRoute, addDrawIndexedInstanced);
            RollBackDrawRoute(
                s_drawInstancedRoutes, drawInstancedRoute, addDrawInstanced);
            RollBackDrawRoute(
                s_clearRenderTargetRoutes, clearRenderTargetRoute,
                addClearRenderTarget);
            g_nativePrimaryClearHookReady.store(
                false, std::memory_order_release);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kDrawHookUnavailable, false);
            SPDLOG_ERROR("[CloudShadows] Draw-detour attach failed: {}", err);
            return;
        }
        err = DetourTransactionCommit();
        if (err != NO_ERROR) {
            RollBackDrawRoute(
                s_drawIndexedRoutes, drawIndexedRoute, addDrawIndexed);
            RollBackDrawRoute(s_drawRoutes, drawRoute, addDraw);
            RollBackDrawRoute(s_drawIndexedInstancedRoutes,
                drawIndexedInstancedRoute, addDrawIndexedInstanced);
            RollBackDrawRoute(
                s_drawInstancedRoutes, drawInstancedRoute, addDrawInstanced);
            RollBackDrawRoute(
                s_clearRenderTargetRoutes, clearRenderTargetRoute,
                addClearRenderTarget);
            g_nativePrimaryClearHookReady.store(
                false, std::memory_order_release);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kDrawHookUnavailable, false);
            SPDLOG_ERROR(
                "[CloudShadows] Draw-detour commit failed: {}", err);
            return;
        }
        g_nativePrimaryClearHookReady.store(
            true, std::memory_order_release);
        SetHookAvailable(
            FO4CloudShadowsMenuBridge::kDrawHookUnavailable, true);
        SPDLOG_INFO(
            "[CloudShadows] Draw detour generation installed: "
            "DrawIndexed=0x{:016X}[{}] Draw=0x{:016X}[{}] "
            "DII=0x{:016X}[{}] DI=0x{:016X}[{}] "
            "ClearRTV=0x{:016X}[{}]",
            static_cast<uint64_t>(targets[0]), drawIndexedRoute,
            static_cast<uint64_t>(targets[1]), drawRoute,
            static_cast<uint64_t>(targets[2]), drawIndexedInstancedRoute,
            static_cast<uint64_t>(targets[3]), drawInstancedRoute,
            static_cast<uint64_t>(targets[4]), clearRenderTargetRoute);
    }

}

namespace
{


    void TryInstallBeginTechniqueHook()
    {
		std::scoped_lock lock(g_beginTechniqueHookMutex);
        const bool vrAbi = FO4CS::RuntimeAPI::GetSingleton().Target() ==
            FO4CS::F4SECompat::RuntimeTarget::kVR;
        // A host may forward BeginTechnique, but that cannot forward VR's
        // cached batches. This observer is required in either integration.
        static bool vrBindingHookInstalled = false;
        if (vrAbi && !vrBindingHookInstalled) {
            const auto address = FO4CS::EngineAPI::GetVRSetShadersAddress();
            if (!address || !IsExecutableMainModuleAddress(address)) {
                SetHookAvailable(
                    FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable, false);
                SPDLOG_ERROR("[CloudShadows] VR shader-binding address unavailable");
                return;
            }
            s_original_SetShadersVR = reinterpret_cast<SetShadersVR_t>(address);
            DetourThreadEnlistment threads;
            LONG error = DetourTransactionBegin();
            if (error == NO_ERROR) error = threads.EnlistProcessThreads();
            if (error == NO_ERROR) {
                error = DetourAttach(
                    reinterpret_cast<PVOID*>(&s_original_SetShadersVR),
                    reinterpret_cast<PVOID>(&Detour_SetShadersVR));
            }
            if (error != NO_ERROR) {
                DetourTransactionAbort();
            } else {
                error = DetourTransactionCommit();
            }
            if (error != NO_ERROR) {
                SetHookAvailable(
                    FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable, false);
                SPDLOG_ERROR("[CloudShadows] VR shader-binding detour failed: {}", error);
                return;
            }
            vrBindingHookInstalled = true;
            SPDLOG_INFO("[CloudShadows] VR BSGraphics::SetShaders detour "
                "installed @ 0x{:X}", address);
        }
        if (g_beginTechniqueHookInstalled.load(std::memory_order_acquire)) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable,
                true);
            return;
        }
		if ((g_acceptedHostCapabilities.load(std::memory_order_acquire) &
			FO4CloudShadowsMenuBridge::kHostForwardsBeginTechnique) != 0u) {
			SetHookAvailable(
				FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable,
				true);
			SPDLOG_INFO(
				"[CloudShadows][MenuBridge] compatible host forwards BeginTechnique; "
				"standalone engine detour skipped");
			return;
		}
        // Reviewed OG/AE Address Library IDs plus the exact VR RVA all resolve
        // through the project-owned runtime bridge.
        auto addr = FO4CS::EngineAPI::GetBeginTechniqueAddress();
        if (!addr) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable,
                false);
            SPDLOG_WARN("[CloudShadows] BeginTechnique address unresolved — DFLight descriptor-swap disabled");
            return;
        }
        if (!IsExecutableMainModuleAddress(addr)) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable,
                false);
            SPDLOG_CRITICAL("[CloudShadows] Refusing BeginTechnique detour: 0x{:016X} is not in an executable Fallout4.exe section",
                static_cast<uint64_t>(addr));
            return;
        }
        if (vrAbi) {
            s_original_BeginTechniqueVR =
                reinterpret_cast<BeginTechniqueVR_t>(addr);
        } else {
            s_original_BeginTechniqueFlat =
                reinterpret_cast<BeginTechniqueFlat_t>(addr);
        }
        DetourThreadEnlistment threads;
        LONG err = DetourTransactionBegin();
        if (err == NO_ERROR) err = threads.EnlistProcessThreads();
        if (err == NO_ERROR) {
            if (vrAbi) {
                err = DetourAttach(
                    reinterpret_cast<PVOID*>(&s_original_BeginTechniqueVR),
                    reinterpret_cast<PVOID>(&Detour_BeginTechniqueVR));
            } else {
                err = DetourAttach(
                    reinterpret_cast<PVOID*>(&s_original_BeginTechniqueFlat),
                    reinterpret_cast<PVOID>(&Detour_BeginTechniqueFlat));
            }
        }
        if (err != NO_ERROR) {
            DetourTransactionAbort();
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable,
                false);
            SPDLOG_ERROR("[CloudShadows] DetourAttach BeginTechnique failed: {}", err);
            return;
        }
        err = DetourTransactionCommit();
        if (err != NO_ERROR) {
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable,
                false);
            SPDLOG_ERROR("[CloudShadows] DetourAttach BeginTechnique failed: {}", err);
            return;
        }
		g_beginTechniqueHookInstalled.store(true, std::memory_order_release);
        SetHookAvailable(
            FO4CloudShadowsMenuBridge::kBeginTechniqueHookUnavailable, true);
        SPDLOG_INFO(
            "[CloudShadows] BSShader::BeginTechnique detour installed @ 0x{:X} ABI={}",
            addr, vrAbi ? "VR-6" : "flat-5");
    }

    // -----------------------------------------------------------------------
    // Prologue-patch hook on d3d11.dll!D3D11CreateDeviceAndSwapChain using MS
    // Detours. This intercepts the function in d3d11.dll itself, so ALL
    // callers go through our hook regardless of IAT / GetProcAddress path.
    //
    // We install both shader-creation vtable hooks the instant a device is
    // created. Patcher initialization stays on the renderer lifecycle thread;
    // shaders created before it are retained and rematched by immutable hash.
    // -----------------------------------------------------------------------
    using PFN_D3D11CreateDeviceAndSwapChain = HRESULT(WINAPI*)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
        UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    static PFN_D3D11CreateDeviceAndSwapChain s_origCreateDeviceAndSwapChain = nullptr;
    static std::atomic<bool> s_createDeviceHooksInstalled{ false };

    void CaptureCreatedD3DObjects(
        ID3D11Device* device,
        ID3D11DeviceContext* immediateContext) noexcept
    {
        if (!device)
            return;

        // Install identity capture on every created device. RendererData later
        // chooses the actual game device; this also covers a device recreated
        // after removal before its first shader is created.
        g_createVSHookInstalled.store(
            InstallCreateVertexShaderHook(device), std::memory_order_release);
        g_createPSHookInstalled.store(
            InstallCreatePixelShaderHook(device), std::memory_order_release);

        // RendererData/Present publishes the authoritative device/context on
        // the render thread. This callback may run on an auxiliary device-
        // creation thread, so it deliberately never mutates the shared raw
        // renderer pointers.
        SPDLOG_INFO(
            "[CloudShadows] Shader capture armed on created D3D11 device {} "
            "(immediateContext={})",
            static_cast<void*>(device),
            static_cast<void*>(immediateContext));

        // Patching initializes from the render/F4SE lifecycle path. Keeping it
        // off detached threads makes device teardown and shader-map lifetime
        // deterministic; all pre-init bytecode remains available for rematch.
    }

    HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChain(
        IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
        UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
        UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
        IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
        D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
    {
        SPDLOG_INFO("[CloudShadows] hk_D3D11CreateDeviceAndSwapChain ENTRY ppDevice={} vsInstalled={} psInstalled={}",
            (void*)ppDevice,
            g_createVSHookInstalled.load(std::memory_order_acquire),
            g_createPSHookInstalled.load(std::memory_order_acquire));

        HRESULT ret = s_origCreateDeviceAndSwapChain(
            pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
            SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
            ppImmediateContext);

        SPDLOG_INFO("[CloudShadows] hk_D3D11CreateDeviceAndSwapChain RETURN hr=0x{:08X} device={} ctx={} swap={}",
            (uint32_t)ret,
            (void*)(ppDevice ? *ppDevice : nullptr),
            (void*)(ppImmediateContext ? *ppImmediateContext : nullptr),
            (void*)(ppSwapChain ? *ppSwapChain : nullptr));

        if (SUCCEEDED(ret)) {
            CaptureCreatedD3DObjects(
                ppDevice ? *ppDevice : nullptr,
                ppImmediateContext ? *ppImmediateContext : nullptr);

            // Install Present on the *real* swap chain the game actually
            // renders through. F4SE-message-time installation would hook a
            // stale first swap chain; the game may create a second
            // swap chain (different vtable) and render through that.
            if (ppSwapChain && *ppSwapChain)
                InstallPresentHook(
                    *ppSwapChain, "CreateDeviceAndSwapChain", false);
        }
        return ret;
    }

    // Secondary hook on D3D11CreateDevice (no swapchain variant). Some code
    // paths call this overload instead.
    using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(
        IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
        UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    static PFN_D3D11CreateDevice s_origCreateDevice = nullptr;

    HRESULT WINAPI hk_D3D11CreateDevice(
        IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
        UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
        UINT SDKVersion, ID3D11Device** ppDevice,
        D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext)
    {
        SPDLOG_INFO("[CloudShadows] hk_D3D11CreateDevice ENTRY ppDevice={} vsInstalled={} psInstalled={}",
            (void*)ppDevice,
            g_createVSHookInstalled.load(std::memory_order_acquire),
            g_createPSHookInstalled.load(std::memory_order_acquire));

        HRESULT ret = s_origCreateDevice(
            pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
            SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);

        if (SUCCEEDED(ret)) {
            CaptureCreatedD3DObjects(
                ppDevice ? *ppDevice : nullptr,
                ppImmediateContext ? *ppImmediateContext : nullptr);
        }
        return ret;
    }

    [[nodiscard]] bool InstallD3DCreateDeviceHook() noexcept
    {
        HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
        if (!d3d11) {
            d3d11 = LoadLibraryA("d3d11.dll");
        }
        if (!d3d11) {
            SPDLOG_ERROR("[CloudShadows] Failed to load d3d11.dll");
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable,
                false);
            return false;
        }
        auto proc = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
            GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
        if (!proc) {
            SPDLOG_ERROR("[CloudShadows] GetProcAddress(D3D11CreateDeviceAndSwapChain) failed");
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable,
                false);
            return false;
        }
        s_origCreateDeviceAndSwapChain = proc;

        auto procCd = reinterpret_cast<PFN_D3D11CreateDevice>(
            GetProcAddress(d3d11, "D3D11CreateDevice"));
        if (!procCd) {
            SPDLOG_ERROR("[CloudShadows] GetProcAddress(D3D11CreateDevice) failed");
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable,
                false);
            return false;
        }
        s_origCreateDevice = procCd;

        DetourThreadEnlistment threads;
        LONG err = DetourTransactionBegin();
        if (err == NO_ERROR) err = threads.EnlistProcessThreads();
        if (err == NO_ERROR) {
            err = DetourAttach(reinterpret_cast<PVOID*>(&s_origCreateDeviceAndSwapChain),
                reinterpret_cast<PVOID>(&hk_D3D11CreateDeviceAndSwapChain));
        }
        if (err == NO_ERROR && procCd) {
            err = DetourAttach(reinterpret_cast<PVOID*>(&s_origCreateDevice),
                reinterpret_cast<PVOID>(&hk_D3D11CreateDevice));
        }
        if (err != NO_ERROR) {
            DetourTransactionAbort();
            SPDLOG_ERROR("[CloudShadows] DetourAttach D3D11 create-device failed: {}", err);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable,
                false);
            return false;
        }
        err = DetourTransactionCommit();
        if (err != NO_ERROR) {
            SPDLOG_ERROR("[CloudShadows] DetourAttach D3D11 create-device failed: {}", err);
            SetHookAvailable(
                FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable,
                false);
            return false;
        }
        SetHookAvailable(
            FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable, true);
        s_createDeviceHooksInstalled.store(true, std::memory_order_release);
        SPDLOG_INFO("[CloudShadows] D3D11CreateDevice[AndSwapChain] detours installed (swapchain={}, direct={})",
            (void*)proc, (void*)procCd);
        return true;
    }

    [[nodiscard]] bool UninstallD3DCreateDeviceHook() noexcept
    {
        if (!s_createDeviceHooksInstalled.load(std::memory_order_acquire))
            return true;
        DetourThreadEnlistment threads;
        LONG err = DetourTransactionBegin();
        if (err == NO_ERROR)
            err = threads.EnlistProcessThreads();
        if (err == NO_ERROR) {
            err = DetourDetach(
                reinterpret_cast<PVOID*>(&s_origCreateDeviceAndSwapChain),
                reinterpret_cast<PVOID>(&hk_D3D11CreateDeviceAndSwapChain));
        }
        if (err == NO_ERROR) {
            err = DetourDetach(
                reinterpret_cast<PVOID*>(&s_origCreateDevice),
                reinterpret_cast<PVOID>(&hk_D3D11CreateDevice));
        }
        if (err != NO_ERROR) {
            DetourTransactionAbort();
            SPDLOG_CRITICAL(
                "[CloudShadows] Unable to roll back D3D11 create-device "
                "detours after load failure: {}", err);
            return false;
        }
        err = DetourTransactionCommit();
        if (err != NO_ERROR) {
            SPDLOG_CRITICAL(
                "[CloudShadows] Unable to commit D3D11 create-device detour "
                "rollback after load failure: {}", err);
            return false;
        }
        s_createDeviceHooksInstalled.store(false, std::memory_order_release);
        SetHookAvailable(
            FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable, false);
        return true;
    }

    // Drive DFLight patcher initialization from F4SE lifecycle messages.
    // Device-level hooks are installed from the intercepted D3D11 creation
    // exports before the target creates its stock pixel shaders.
    void TryInstallHooks()
    {
        (void)FO4CS::GodraysIntegration::TryInstall(
            FO4CS::RuntimeAPI::GetSingleton().Target());
        if (!CloudShadows::g_dfLightPatcher.IsInitialized()) {
            CloudShadows::InitializeDFLightPatcher();
        }
    }

    void TryInstallNativeSkyCubeHooks() noexcept
    {
        FO4CS::NativeSkyCube::SetFaceLifecycleCallback(
            &OnNativeSkyCubeFaceLifecycle);
        if (!FO4CS::NativeSkyCube::Install()) {
            SPDLOG_ERROR(
                "[CloudShadows] Native sky-cubemap producer unavailable; "
                "visible rendering remains vanilla and cloud shadows stay "
                "fail-neutral");
        }
    }

    void FO4CS_F4SEAPI OnMessage(FO4CS::F4SECompat::Message* a_msg)
    {
        if (!a_msg)
            return;
        SPDLOG_INFO("[CloudShadows] Message received: type={}", (int)a_msg->type);
        // The timed capture watcher needs the actual load boundary, not the
        // next periodic render-log flush. Only lifecycle events flush here.
        if (a_msg->type == FO4CS::F4SECompat::kPreLoadGame ||
            a_msg->type == FO4CS::F4SECompat::kPostLoadGame) {
            if (auto* logger = spdlog::default_logger_raw()) logger->flush();
        }

        switch (a_msg->type) {
        case FO4CS::F4SECompat::kPostPostLoad:
            SPDLOG_INFO("[CloudShadows] kPostPostLoad - trying hooks");
			{
			TryInstallNativeSkyCubeHooks();
			bool hostActivationPending = false;
			if (GetModuleHandleW(L"RealisticReflections.dll")) {
				using namespace FO4CloudShadowsMenuBridge;
				HandshakeV1 handshake{
					.structSize = sizeof(HandshakeV1),
					.magic = kHandshakeMagic,
					.requestedAbiVersion = kAbiVersion,
					.hostCapabilities = 0u,
					.api = &g_menuBridgeApi,
				};
				auto messaging =
					FO4CS::RuntimeAPI::GetSingleton().Messaging();
				const bool delivered = messaging && messaging.Dispatch(
					kHandshakeMessage, &handshake, sizeof(handshake),
					"RealisticReflections");
				const bool accepted = delivered &&
					(handshake.hostCapabilities & kRequiredHostCapabilities) ==
						kRequiredHostCapabilities &&
					Overlay::IsExternalHostActive();
				if (accepted) {
					SPDLOG_INFO(
						"[CloudShadows][MenuBridge] Realistic Reflections accepted ABI {} "
						"and owns the unified F11 menu",
						kAbiVersion);
				} else if (delivered &&
					(handshake.hostCapabilities & kHostActivationPending) != 0u) {
					hostActivationPending = true;
					SPDLOG_INFO(
						"[CloudShadows][MenuBridge] Realistic Reflections validated ABI {}; "
						"standalone ownership retained until its render-thread menu is ready",
						kAbiVersion);
				} else {
					SPDLOG_WARN(
						"[CloudShadows][MenuBridge] RealisticReflections.dll is present "
						"but did not accept ABI {}; retaining the standalone menu",
						kAbiVersion);
				}
			}
			if (hostActivationPending) {
				SPDLOG_INFO(
					"[CloudShadows][MenuBridge] deferring standalone BeginTechnique "
					"until kInputLoaded while the compatible host finishes hook setup");
			} else {
				TryInstallBeginTechniqueHook();
			}
            // The world-tile path captures live t0/t1/b2 directly from the
            // successful Sky draw. The old raw-address weather/texture hooks
            // are intentionally not installed; they are unnecessary and were
            // a source of unsafe cross-thread raw-SRV publication.
            TryInstallHooks();
			}
            break;
        case FO4CS::F4SECompat::kInputLoaded:
        case FO4CS::F4SECompat::kGameDataReady:
			if (a_msg->type == FO4CS::F4SECompat::kGameDataReady)
				FO4CS::NativeSkyCube::SetLoadBlocked(false);
			TryInstallBeginTechniqueHook();
            TryInstallHooks();
            break;
        case FO4CS::F4SECompat::kPreLoadGame:
            FO4CS::NativeSkyCube::SetLoadBlocked(true);
            FO4CS::NativeSkyCube::RequestWorldReset();
            CloudShadows::RequestWorldCloudReset();
            TryInstallHooks();
            break;
        case FO4CS::F4SECompat::kPostLoadGame:
        case FO4CS::F4SECompat::kNewGame:
            FO4CS::NativeSkyCube::RequestWorldReset();
            CloudShadows::RequestWorldCloudReset();
            FO4CS::NativeSkyCube::SetLoadBlocked(false);
            TryInstallHooks();
            break;
        default:
            break;
        }
	}
}

extern "C" DLLEXPORT bool FO4CS_F4SEAPI F4SEPlugin_Load(
    const void* rawF4SEInterface)
{
    auto& runtime = FO4CS::RuntimeAPI::GetSingleton();
    if (!runtime.Initialize(rawF4SEInterface)) {
        const auto detail = runtime.LastError();
        OutputDebugStringA("FO4CloudShadows: runtime initialization rejected: ");
        OutputDebugStringA(detail.c_str());
        OutputDebugStringA("\n");
        return false;
    }

    const auto host = runtime.Host();
    const bool loggerReady = InitializePluginLogger(host.target);
    if (!loggerReady) {
        OutputDebugStringA(
            "FO4CloudShadows: file logger initialization failed\n");
    }
    SPDLOG_INFO(
        "FO4CloudShadows v{} build={} loading universal target {} "
        "(actual={}.{}.{}.{}, reported={}.{}.{}.{}, F4SE={}.{}.{}.{})",
        FO4CLOUDSHADOWS_VERSION_STR, FO4CS_BUILD_ID, RuntimeName(host.target),
        host.executableVersion.major, host.executableVersion.minor,
        host.executableVersion.patch, host.executableVersion.build,
        host.reportedRuntimeVersion.major, host.reportedRuntimeVersion.minor,
        host.reportedRuntimeVersion.patch, host.reportedRuntimeVersion.build,
        host.f4seVersion.major, host.f4seVersion.minor,
        host.f4seVersion.patch, host.f4seVersion.build);

    SPDLOG_INFO("[CloudShadows] {}", FO4CS::BuildFeatures::kMarker);
    FO4CS::McmSettings::Initialize();

    // MUTUAL EXCLUSION: CommunityShaders.dll and DynamicReflections.dll are
    // sibling CS-derived plugins that hook the SAME pipeline (DFLight/DFComposite
    // PS patching, BeginTechnique, D3D vtables). Running two at once corrupts
    // rendering (proven in-world 2026-06-20: whole scene darkened). F4SE loads
    // plugins alphabetically, so both sort before us and are already mapped if
    // active — self-disable loudly rather than fight them.
    for (const wchar_t* rival : { L"CommunityShaders.dll", L"DynamicReflections.dll" }) {
        if (GetModuleHandleW(rival)) {
            SPDLOG_ERROR("FO4CloudShadows: {} is loaded — two CS-derived plugins "
                         "corrupt the deferred pipeline. SELF-DISABLING (no hooks installed). "
                         "Deactivate the other plugin to use FO4CloudShadows.",
                std::filesystem::path(rival).string());
            return true;  // stay resident but inert
        }
    }

    // ENB draws its own cloud shadows. With them active at startup this plugin
    // stays resident but inert (no hooks, no per-frame cost) so ENB provides the
    // effect; with them disabled it runs normally and hooks ENB's Present (see
    // IsGameDirectoryGraphicsProxy). Decided once per launch from the
    // enbseries.ini ENB reads at startup (EnbCompat.h).
    if (const auto enb = FO4CS::EnbCompat::Detect(); enb.present) {
        const auto enbVersion = FO4CS::EnbCompat::VersionText(enb.version);
        auto enbConfig = FO4CS::EnbCompat::ToUtf8(enb.configPath.wstring());
        if (enbConfig.empty())
            enbConfig = "enbseries.ini";
        if (enb.cloudShadowsActive) {
            SPDLOG_INFO(
                "[CloudShadows] ENB {} detected with its cloud shadows active ({}: "
                "[GLOBAL] UseEffect and [EFFECT] EnableCloudShadows are not false). "
                "Cloud Shadows stays inactive (no hooks installed) so ENB renders the "
                "cloud shadows. Set EnableCloudShadows=false in ENB and restart the "
                "game to use this mod instead.",
                enbVersion, enbConfig);
            if (auto* logger = spdlog::default_logger_raw())
                logger->flush();
            return true;  // stay resident but inert
        }
        SPDLOG_INFO(
            "[CloudShadows] ENB {} detected with its cloud shadows disabled ({}); "
            "Cloud Shadows runs normally",
            enbVersion, enbConfig);
    }

    auto messaging = runtime.Messaging();
    if (!messaging) {
        SPDLOG_CRITICAL(
            "[CloudShadows] F4SE messaging interface unavailable; "
            "refusing partial hook initialization");
        return false;
    }

    // Detour d3d11.dll!D3D11CreateDeviceAndSwapChain at the function prologue
    // so the shader-creation vtable hooks are installed the instant the device
    // is created — before the game issues CreateVertexShader/CreatePixelShader.
    // We use MS Detours (prologue patch) rather than IAT patching because
    // FO4 appears to resolve this symbol in a way that bypasses Fallout4.exe's
    // IAT (GetProcAddress or a wrapper DLL).
    if (!InstallD3DCreateDeviceHook()) {
        SPDLOG_CRITICAL(
            "[CloudShadows] Required D3D11 device-creation interception is unavailable; "
            "refusing to load an inert partial plugin");
        return false;
    }

    if (!messaging.RegisterListener(&OnMessage)) {
        SPDLOG_CRITICAL(
            "[CloudShadows] F4SE messaging listener registration failed; "
            "rolling back D3D11 interception");
        if (UninstallD3DCreateDeviceHook())
            return false;
        // A failed detour rollback makes unloading unsafe because patched
        // d3d11 entry points still reference this module. Stay resident and
        // explicitly unavailable rather than returning a dangling callback.
        SPDLOG_CRITICAL(
            "[CloudShadows] Detour rollback also failed; remaining resident "
            "in hard-unavailable fail-safe state");
        SetHookAvailable(
            FO4CloudShadowsMenuBridge::kCreateDeviceHookUnavailable, false);
        return true;
    }

    // The device-creation detour precedes BSShader::LoadShaders on the exact
    // supported runtime, so no separate polling fallback is needed.

    (void)FO4CS::GodraysIntegration::TryInstall(host.target);

#if FO4CS_ENABLE_DEVELOPER_TOOLS
    SPDLOG_INFO("[CloudShadows][FPS] Manual FPS logger v1 available in flat: "
                "F10 toggles shadows and arms 10s ON/OFF averages after 3s settling");
#endif
    SPDLOG_INFO("[CloudShadows] Plugin load complete");
    return true;
}

extern "C" DLLEXPORT constinit FO4CS::F4SECompat::PluginVersionData
F4SEPlugin_Version = []() constexpr {
    FO4CS::F4SECompat::PluginVersionData v{};
    v.SetPluginName("FO4CloudShadows");
    v.SetPluginVersion({ 1, 0, 0, 0 });
    v.UsesSignatureScanning(false);
    v.UsesAddressLibraryNG(true);
    v.UsesAddressLibraryAE(true);
    v.HasNoStructureUse(false);
    v.IsLayoutDependentNG(true);
    v.IsLayoutDependentAE(true);
    v.SetCompatibleVersions({
        FO4CS::F4SECompat::kLegacyRuntime,
        FO4CS::F4SECompat::kAERuntime,
        FO4CS::F4SECompat::kVRRuntime,
        // Official F4SEVR 0.6.21 reports this proxy. Query/Load still prove
        // the actual main image is exactly Fallout4VR.exe 1.2.72.
        FO4CS::F4SECompat::kF4SEVRProxyRuntime
    });
    v.SetMinimumF4SEVersion(FO4CS::F4SECompat::kF4SEVR0621);
    return v;
}();

extern "C" DLLEXPORT bool FO4CS_F4SEAPI F4SEPlugin_Query(
    const void* rawF4SEInterface,
    FO4CS::F4SECompat::PluginInfo* pluginInfo)
{
    if (!rawF4SEInterface || !pluginInfo)
        return false;
    pluginInfo->infoVersion = FO4CS::F4SECompat::PluginInfo::kVersion;
    pluginInfo->name = F4SEPlugin_Version.pluginName;
    pluginInfo->version = F4SEPlugin_Version.pluginVersion;
    FO4CS::RuntimeHost host;
    std::string error;
    return FO4CS::RuntimeAPI::ProbeHost(
        rawF4SEInterface, host, error);
}
