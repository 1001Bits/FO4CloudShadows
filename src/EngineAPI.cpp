#include "EngineAPI.h"

#include <limits>
#include <cmath>

namespace
{
    using FO4CS::AddressKind;
    using FO4CS::EngineAPI::detail::ReadField;
    using FO4CS::F4SECompat::RuntimeTarget;
    using FO4CS::RuntimeAPI;
    using FO4CS::RuntimeRVA;

    struct ReviewedSymbol
    {
        std::uint64_t legacyID{};
        std::uint64_t aeID{};
        std::uintptr_t vrRva{};
    };

    constexpr ReviewedSymbol kRendererDataSymbol{
        1235449, 2704429, 0x60F3CE8
    };
    constexpr ReviewedSymbol kGraphicsStateSymbol{
        600795, 2704621, 0x65A2AB0
    };
    constexpr ReviewedSymbol kRenderTargetManagerSymbol{
        1508457, 2666735, 0x38AC010
    };
    constexpr ReviewedSymbol kSkySingletonFunctionSymbol{
        484694, 2192448, 0x012FB50
    };
    constexpr ReviewedSymbol kWorldRootCameraFunctionSymbol{
        384264, 2228956, 0x0D87550
    };
    constexpr ReviewedSymbol kPlayerSingletonSymbol{
        // Dear CommonLib's three-runtime VariantID is
        // { OG, 1.10.980 NG, 1.11.x AE }.  The middle ID is absent from the
        // installed 1.11.240 database; 4798212 is the exact AE singleton ID.
        412034, 4798212, 0x5A38518
    };
    constexpr ReviewedSymbol kTESSingletonSymbol{
        1194835, 2698044, 0x5B042C0
    };
    constexpr ReviewedSymbol kBeginTechniqueSymbol{
        1041640, 2318876, 0x2814BE0
    };
    // Fallout4VR 1.2.72 BSGraphics::SetShaders. Both BeginTechnique and the
    // cached render batches (0x28AA8D0/0x28AAF00) submit their wrappers here.
    // No corresponding flat hook is needed or attested.
    constexpr ReviewedSymbol kVRSetShadersSymbol{ 0, 0, 0x1D92C00 };
    // Vanilla cursor mode for the F11 menu. CommonLibF4 VariantIDs
    // (OG, 1.11.x): UIMessageQueue::Singleton {82123, ..., 4796377},
    // UIMessageQueue::AddMessage {1182019, 2284929},
    // BSStringPool::GetEntry_char {507142, 2268729}. No reviewed VR RVAs.
    constexpr ReviewedSymbol kUIMessageQueueSingletonSymbol{ 82123, 4796377, 0 };
    constexpr ReviewedSymbol kUIMessageQueueAddMessageSymbol{ 1182019, 2284929, 0 };
    constexpr ReviewedSymbol kStringPoolGetEntrySymbol{ 507142, 2268729, 0 };
    struct UIMessageQueueObject { std::byte opaque[0x10]{}; };

    constexpr std::size_t kFlatCameraStateOffset = 0x160;
    constexpr std::size_t kFlatPosAdjustOffset = 0x370;
    constexpr std::size_t kFlatPreviousPosAdjustOffset = 0x388;
    constexpr std::size_t kViewDirectionOffset = 0x40;

    constexpr std::size_t kVRCameraStateOffset = 0x160;
    constexpr std::size_t kVRPosAdjustOffset = 0x580;
    constexpr std::size_t kVRPreviousPosAdjustOffset = 0x5B0;

    // Complete flat/AE map span. VR uses +0x13BC/+0x15FC and 144/17 entries;
    // its depth map ends at +0x1640 (native manager at VR RVA 0x1DB93D0).
    constexpr std::size_t kRenderTargetManagerReadableSpan = 0xFB4;

    [[nodiscard]] std::uintptr_t ResolveReviewedSymbol(
        const ReviewedSymbol& symbol,
        AddressKind kind) noexcept
    {
        auto& runtime = RuntimeAPI::GetSingleton();
        switch (runtime.Target()) {
        case RuntimeTarget::kLegacy:
            return symbol.legacyID != 0 ?
                runtime.ResolveID(symbol.legacyID, kind) : 0;
        case RuntimeTarget::kAE:
            return symbol.aeID != 0 ?
                runtime.ResolveID(symbol.aeID, kind) : 0;
        case RuntimeTarget::kVR:
            return symbol.vrRva != 0 ?
                runtime.ResolveRVA(
                    RuntimeRVA{ 0, 0, symbol.vrRva }, kind) :
                0;
        default:
            return 0;
        }
    }

    [[nodiscard]] std::uintptr_t ResolveCachedAddress(
        std::atomic<std::uintptr_t>& cache,
        const ReviewedSymbol& symbol,
        AddressKind kind) noexcept
    {
        auto address = cache.load(std::memory_order_acquire);
        if (address != 0)
            return address;
        address = ResolveReviewedSymbol(symbol, kind);
        if (address != 0)
            cache.store(address, std::memory_order_release);
        return address;
    }

    template <class TObject>
    [[nodiscard]] TObject* ResolveDirectObject(
        std::atomic<std::uintptr_t>& cache,
        const ReviewedSymbol& symbol,
        AddressKind kind = AddressKind::kReadable) noexcept
    {
        const auto address = ResolveCachedAddress(cache, symbol, kind);
        if (address == 0)
            return nullptr;
        auto& runtime = RuntimeAPI::GetSingleton();
        return runtime.ValidateMemory(
                   address, sizeof(TObject), AddressKind::kReadable) ?
            reinterpret_cast<TObject*>(address) : nullptr;
    }

    [[nodiscard]] std::byte* ResolveDirectSpan(
        std::atomic<std::uintptr_t>& cache,
        const ReviewedSymbol& symbol,
        std::size_t readableSpan) noexcept
    {
        const auto address = ResolveCachedAddress(
            cache, symbol, AddressKind::kReadable);
        if (address == 0 || readableSpan == 0)
            return nullptr;
        return RuntimeAPI::GetSingleton().ValidateMemory(
                   address, readableSpan, AddressKind::kReadable) ?
            reinterpret_cast<std::byte*>(address) : nullptr;
    }

    template <class TObject>
    [[nodiscard]] TObject* ResolveIndirectObject(
        std::atomic<std::uintptr_t>& cache,
        const ReviewedSymbol& symbol,
        std::size_t readableSpan = sizeof(TObject)) noexcept
    {
        const auto storage = ResolveCachedAddress(
            cache, symbol, AddressKind::kReadable);
        auto& runtime = RuntimeAPI::GetSingleton();
        if (storage == 0 || !runtime.ValidateMemory(
                storage, sizeof(std::uintptr_t), AddressKind::kReadable))
            return nullptr;

        const auto pointee = ReadField<std::uintptr_t>(
            reinterpret_cast<const void*>(storage));
        if (pointee == 0)
            return nullptr;

        // The relocation slot is image-resident, but renderer/player/TES
        // instances are commonly allocated on the process heap. Requiring the
        // pointee itself to lie inside Fallout4.exe rejects every valid heap
        // singleton and leaves the cloud mask permanently neutral.
        return runtime.ValidateMemory(
            pointee, readableSpan, AddressKind::kReadable) ?
            reinterpret_cast<TObject*>(pointee) :
            nullptr;
    }

    template <class TObject>
    [[nodiscard]] TObject* ValidateReturnedObject(TObject* object) noexcept
    {
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        return object && RuntimeAPI::GetSingleton().ValidateMemory(
                   address, sizeof(TObject), AddressKind::kReadable) ?
            object : nullptr;
    }

    [[nodiscard]] bool ValidateReadableField(
        const void* object,
        std::size_t offset,
        std::size_t size) noexcept
    {
        const auto base = reinterpret_cast<std::uintptr_t>(object);
        if (!base || offset >
                (std::numeric_limits<std::uintptr_t>::max)() - base) {
            return false;
        }
        return RuntimeAPI::GetSingleton().ValidateMemory(
            base + offset, size, AddressKind::kReadable);
    }

    template <class TFunction>
    [[nodiscard]] TFunction ResolveFunction(
        std::atomic<std::uintptr_t>& cache,
        const ReviewedSymbol& symbol) noexcept
    {
        const auto address = ResolveCachedAddress(
            cache, symbol, AddressKind::kExecutable);
        return address != 0 ?
            reinterpret_cast<TFunction>(address) :
            nullptr;
    }

    [[nodiscard]] RuntimeTarget CurrentTarget() noexcept
    {
        return RuntimeAPI::GetSingleton().Target();
    }

    [[nodiscard]] std::size_t CameraPosAdjustOffset() noexcept
    {
        return CurrentTarget() == RuntimeTarget::kVR ?
            kVRPosAdjustOffset :
            kFlatPosAdjustOffset;
    }

    [[nodiscard]] std::size_t CameraPreviousPosAdjustOffset() noexcept
    {
        return CurrentTarget() == RuntimeTarget::kVR ?
            kVRPreviousPosAdjustOffset :
            kFlatPreviousPosAdjustOffset;
    }

    [[nodiscard]] std::size_t CameraViewDirectionStorageOffset() noexcept
    {
        const auto cameraStateOffset = CurrentTarget() == RuntimeTarget::kVR ?
            kVRCameraStateOffset :
            kFlatCameraStateOffset;
        return cameraStateOffset + kViewDirectionOffset;
    }
}

namespace FO4CS::EngineAPI
{
    uint32_t PhysicalRenderTargetCount() noexcept
    {
        return RuntimeAPI::GetSingleton().Target() == RuntimeTarget::kVR ? 145u : 101u;
    }

    uint32_t PhysicalDepthTargetCount() noexcept
    {
        return RuntimeAPI::GetSingleton().Target() == RuntimeTarget::kVR ? 18u : 13u;
    }

    namespace
    {
        std::size_t DepthTargetsOffset() noexcept
        {
            return RuntimeAPI::GetSingleton().Target() == RuntimeTarget::kVR ?
                0x2588u : 0x1D48u;
        }
    }

    const RenderTarget* RendererData::RenderTargetAt(uint32_t index) const noexcept
    {
        if (index >= PhysicalRenderTargetCount())
            return nullptr;
        return reinterpret_cast<const RenderTarget*>(
            reinterpret_cast<const std::byte*>(this) +
            0xA58u + index * sizeof(RenderTarget));
    }

    const DepthStencilTarget* RendererData::DepthTargetAt(uint32_t index) const noexcept
    {
        if (index >= PhysicalDepthTargetCount())
            return nullptr;
        return reinterpret_cast<const DepthStencilTarget*>(
            reinterpret_cast<const std::byte*>(this) +
            DepthTargetsOffset() + index * sizeof(DepthStencilTarget));
    }

    RendererData* GetRendererData() noexcept
    {
        static std::atomic<std::uintptr_t> cache{ 0 };
        return ResolveIndirectObject<RendererData>(
            cache, kRendererDataSymbol,
            DepthTargetsOffset() + PhysicalDepthTargetCount() * sizeof(DepthStencilTarget));
    }

    State* GetGraphicsState() noexcept
    {
        static std::atomic<std::uintptr_t> cache{ 0 };
        return ResolveDirectObject<State>(
            cache, kGraphicsStateSymbol, AddressKind::kReadable);
    }

    std::byte* GetRenderTargetManager() noexcept
    {
        static std::atomic<std::uintptr_t> cache{ 0 };
        return ResolveDirectSpan(
            cache, kRenderTargetManagerSymbol,
            RuntimeAPI::GetSingleton().Target() == RuntimeTarget::kVR ?
                0x1640u : kRenderTargetManagerReadableSpan);
    }

    Sky* GetSky() noexcept
    {
        using GetSky_t = Sky* (*)();
        static std::atomic<std::uintptr_t> cache{ 0 };
        const auto function = ResolveFunction<GetSky_t>(
            cache, kSkySingletonFunctionSymbol);
        return function ? ValidateReturnedObject(function()) : nullptr;
    }

    Camera* GetWorldRootCamera() noexcept
    {
        using GetCamera_t = Camera* (*)();
        static std::atomic<std::uintptr_t> cache{ 0 };
        const auto function = ResolveFunction<GetCamera_t>(
            cache, kWorldRootCameraFunctionSymbol);
        return function ? ValidateReturnedObject(function()) : nullptr;
    }

    PlayerCharacter* GetPlayerCharacter() noexcept
    {
        static std::atomic<std::uintptr_t> cache{ 0 };
        return ResolveIndirectObject<PlayerCharacter>(
            cache, kPlayerSingletonSymbol);
    }

    TES* GetTES() noexcept
    {
        static std::atomic<std::uintptr_t> cache{ 0 };
        return ResolveIndirectObject<TES>(
            cache, kTESSingletonSymbol);
    }

    std::uintptr_t GetBeginTechniqueAddress() noexcept
    {
        static std::atomic<std::uintptr_t> cache{ 0 };
        return ResolveCachedAddress(
            cache, kBeginTechniqueSymbol, AddressKind::kExecutable);
    }

    std::uintptr_t GetVRSetShadersAddress() noexcept
    {
        static std::atomic<std::uintptr_t> cache{ 0 };
        return ResolveCachedAddress(
            cache, kVRSetShadersSymbol, AddressKind::kExecutable);
    }

    DirectX::XMFLOAT3 ReadCameraPosAdjust(const State* state) noexcept
    {
        const auto offset = CameraPosAdjustOffset();
        if (!ValidateReadableField(state, offset, sizeof(DirectX::XMFLOAT3)))
            return {};
        return ReadField<DirectX::XMFLOAT3>(
            state, offset);
    }

    DirectX::XMFLOAT3 ReadCameraPreviousPosAdjust(const State* state) noexcept
    {
        const auto offset = CameraPreviousPosAdjustOffset();
        if (!ValidateReadableField(state, offset, sizeof(DirectX::XMFLOAT3)))
            return {};
        return ReadField<DirectX::XMFLOAT3>(
            state, offset);
    }

    DirectX::XMFLOAT4 ReadCameraViewDirection(const State* state) noexcept
    {
        const auto offset = CameraViewDirectionStorageOffset();
        if (!ValidateReadableField(state, offset, sizeof(DirectX::XMFLOAT4)))
            return {};
        return ReadField<DirectX::XMFLOAT4>(
            state, offset);
    }

    SkyMode ReadSkyMode(const Sky* sky) noexcept
    {
        // VR's Sky tail is 0x10 bytes shorter than the flat layout. The VR
        // exterior update reads mode at +0x35C; +0x36C is a different field.
        const std::size_t offset = RuntimeAPI::GetSingleton().Target() ==
            RuntimeTarget::kVR ? 0x35C : offsetof(Sky, mode);
        if (!ValidateReadableField(
                sky, offset, sizeof(std::uint32_t))) {
            return SkyMode::kNone;
        }
        return static_cast<SkyMode>(
            ReadField<std::uint32_t>(sky, offset));
    }

    bool ReadVisibleSunDirection(const Sky* sky,
        DirectX::XMFLOAT3& direction) noexcept
    {
        direction = {};
        // Reviewed Sky/Sun/NiAVObject layout: Sky.root=08, Sky.sun=80,
        // Sun.sunBase=20, NiAVObject.world.translate=A0. The paired node
        // centres cancel camera translation without a GPU readback.
        if (!ValidateReadableField(sky, 0x08, sizeof(void*)) ||
            !ValidateReadableField(sky, 0x80, sizeof(void*)))
            return false;
        const auto* root = ReadField<void*>(sky, 0x08);
        const auto* sun = ReadField<void*>(sky, 0x80);
        if (!ValidateReadableField(sun, 0x20, sizeof(void*)))
            return false;
        const auto* geometry = ReadField<void*>(sun, 0x20);
        if (!ValidateReadableField(root, 0xA0, sizeof(DirectX::XMFLOAT3)) ||
            !ValidateReadableField(geometry, 0xA0, sizeof(DirectX::XMFLOAT3)))
            return false;
        const auto origin = ReadField<DirectX::XMFLOAT3>(root, 0xA0);
        const auto centre = ReadField<DirectX::XMFLOAT3>(geometry, 0xA0);
        const auto vector = DirectX::XMVectorSubtract(
            DirectX::XMLoadFloat3(&centre), DirectX::XMLoadFloat3(&origin));
        const float lengthSquared = DirectX::XMVectorGetX(
            DirectX::XMVector3LengthSq(vector));
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f)
            return false;
        DirectX::XMStoreFloat3(&direction, DirectX::XMVector3Normalize(vector));
        return true;
    }

    bool ShowGameCursorMenu(bool show) noexcept
    {
        // UIMessageQueue::AddMessage(this, const BSFixedString& menu, UI_MESSAGE_TYPE)
        // with BSFixedString = one interned BSStringPool::Entry pointer.
        using GetEntry_t = void (*)(void** result, const char* text, bool caseSensitive);
        using AddMessage_t = void (*)(void* queue, void* const* fixedString, std::int32_t type);
        constexpr std::int32_t kShow = 1;
        constexpr std::int32_t kHide = 3;
        static std::atomic<std::uintptr_t> queueCache{ 0 };
        static std::atomic<std::uintptr_t> addMessageCache{ 0 };
        static std::atomic<std::uintptr_t> getEntryCache{ 0 };
        static std::atomic<bool> requested{ false };
        static void* entry = nullptr;
        if (!show && !requested.load(std::memory_order_acquire))
            return true; // Never hide a cursor menu this plugin did not request.
        auto* queue = ResolveIndirectObject<UIMessageQueueObject>(
            queueCache, kUIMessageQueueSingletonSymbol);
        const auto addMessage = ResolveFunction<AddMessage_t>(
            addMessageCache, kUIMessageQueueAddMessageSymbol);
        const auto getEntry = ResolveFunction<GetEntry_t>(
            getEntryCache, kStringPoolGetEntrySymbol);
        if (!queue || !addMessage || !getEntry)
            return false;
        if (!entry) {
            getEntry(&entry, "CursorMenu", false);
            if (!entry)
                return false;
        }
        void* fixedString = entry;
        addMessage(queue, &fixedString, show ? kShow : kHide);
        requested.store(show, std::memory_order_release);
        return true;
    }

    bool IsExteriorCell(const TESObjectCELL* cell) noexcept
    {
        constexpr std::uint16_t kInteriorFlag = 1u << 0u;
        if (!ValidateReadableField(
                cell, offsetof(TESObjectCELL, cellFlags),
                sizeof(std::uint16_t))) {
            return false;
        }
        return (ReadField<std::uint16_t>(
                    cell, offsetof(TESObjectCELL, cellFlags)) &
                kInteriorFlag) == 0;
    }
}
