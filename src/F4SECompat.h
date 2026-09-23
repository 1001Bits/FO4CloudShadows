#pragma once

// Minimal, project-owned view of the stable public F4SE plugin ABI.  This file
// intentionally has no CommonLibF4/REL/REX dependency, so the same plugin
// binary can normalize the OG, AE, and legacy F4SEVR load interfaces before
// selecting any runtime-specific engine addresses.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <type_traits>

#if defined(_MSC_VER)
#define FO4CS_F4SEAPI __cdecl
#else
#define FO4CS_F4SEAPI
#endif

namespace FO4CS::F4SECompat
{
    struct Version
    {
        std::uint16_t major{};
        std::uint16_t minor{};
        std::uint16_t patch{};
        std::uint16_t build{};

        [[nodiscard]] static constexpr Version Unpack(
            std::uint32_t packed) noexcept
        {
            return {
                static_cast<std::uint16_t>((packed >> 24u) & 0x0FFu),
                static_cast<std::uint16_t>((packed >> 16u) & 0x0FFu),
                static_cast<std::uint16_t>((packed >> 4u) & 0xFFFu),
                static_cast<std::uint16_t>(packed & 0x00Fu)
            };
        }

        [[nodiscard]] constexpr std::uint32_t Pack() const noexcept
        {
            return
                (static_cast<std::uint32_t>(major) & 0x0FFu) << 24u |
                (static_cast<std::uint32_t>(minor) & 0x0FFu) << 16u |
                (static_cast<std::uint32_t>(patch) & 0xFFFu) << 4u |
                (static_cast<std::uint32_t>(build) & 0x00Fu);
        }

        [[nodiscard]] constexpr auto operator<=>(
            const Version&) const noexcept = default;
    };

    inline constexpr Version kLegacyRuntime{ 1, 10, 163, 0 };
    inline constexpr Version kAERuntime{ 1, 11, 240, 0 };
    inline constexpr Version kVRRuntime{ 1, 2, 72, 0 };
    inline constexpr Version kF4SEVRProxyRuntime{ 1, 10, 138, 0 };
    inline constexpr Version kF4SEVR0621{ 0, 6, 21, 0 };
    inline constexpr Version kF4SEWithPluginInfo{ 0, 6, 22, 0 };
    inline constexpr Version kF4SEWithSaveFolderName{ 0, 7, 1, 0 };

    using PluginHandle = std::uint32_t;
    inline constexpr PluginHandle kInvalidPluginHandle =
        static_cast<PluginHandle>(-1);

    struct PluginInfo
    {
        inline static constexpr std::uint32_t kVersion = 1;

        std::uint32_t infoVersion{};
        const char* name{};
        std::uint32_t version{};
    };
    static_assert(offsetof(PluginInfo, name) == 8);
    static_assert(sizeof(PluginInfo) == 24);

    using QueryInterfaceFn = void*(FO4CS_F4SEAPI*)(std::uint32_t);
    using GetPluginHandleFn = PluginHandle(FO4CS_F4SEAPI*)();
    using GetReleaseIndexFn = std::uint32_t(FO4CS_F4SEAPI*)();
    using GetPluginInfoFn = const void*(FO4CS_F4SEAPI*)(const char*);
    using GetSaveFolderNameFn = const char*(FO4CS_F4SEAPI*)();

    // Official F4SEVR 0.6.21 exposes exactly this prefix.  In particular, it
    // does not allocate storage for GetPluginInfo or GetSaveFolderName.
    struct RawInterfacePrefix
    {
        std::uint32_t f4seVersion;
        std::uint32_t runtimeVersion;
        std::uint32_t editorVersion;
        std::uint32_t isEditor;
        QueryInterfaceFn queryInterface;
        GetPluginHandleFn getPluginHandle;
        GetReleaseIndexFn getReleaseIndex;
    };
    static_assert(std::is_standard_layout_v<RawInterfacePrefix>);
    static_assert(sizeof(RawInterfacePrefix) == 40);

    struct RawExtendedInterface
    {
        RawInterfacePrefix prefix;
        GetPluginInfoFn getPluginInfo;
        GetSaveFolderNameFn getSaveFolderName;
    };
    static_assert(std::is_standard_layout_v<RawExtendedInterface>);
    static_assert(offsetof(RawExtendedInterface, getPluginInfo) == 40);
    static_assert(offsetof(RawExtendedInterface, getSaveFolderName) == 48);
    static_assert(sizeof(RawExtendedInterface) == 56);

    enum class InterfaceID : std::uint32_t
    {
        kInvalid = 0,
        kMessaging,
        kScaleform,
        kPapyrus,
        kSerialization,
        kTask,
        kObject,
        kTrampoline
    };

    struct Message
    {
        const char* sender;
        std::uint32_t type;
        std::uint32_t dataLength;
        void* data;
    };
    static_assert(sizeof(Message) == 24);

    enum MessageType : std::uint32_t
    {
        kPostLoad = 0,
        kPostPostLoad,
        kPreLoadGame,
        kPostLoadGame,
        kPreSaveGame,
        kPostSaveGame,
        kDeleteGame,
        kInputLoaded,
        kNewGame,
        kGameLoaded,
        kGameDataReady
    };

    using MessageHandler = void(FO4CS_F4SEAPI*)(Message*);
    using RegisterListenerFn = bool(FO4CS_F4SEAPI*)(
        PluginHandle, const char*, void*);
    using DispatchMessageFn = bool(FO4CS_F4SEAPI*)(
        PluginHandle, std::uint32_t, void*, std::uint32_t, const char*);
    using GetEventDispatcherFn = void*(FO4CS_F4SEAPI*)(std::uint32_t);

    struct RawMessagingInterface
    {
        std::uint32_t interfaceVersion;
        RegisterListenerFn registerListener;
        DispatchMessageFn dispatch;
        GetEventDispatcherFn getEventDispatcher;
    };
    static_assert(offsetof(RawMessagingInterface, registerListener) == 8);
    static_assert(sizeof(RawMessagingInterface) == 32);

    class MessagingInterface
    {
    public:
        constexpr MessagingInterface() noexcept = default;

        constexpr MessagingInterface(
            const RawMessagingInterface* raw,
            PluginHandle pluginHandle) noexcept :
            raw_(raw), pluginHandle_(pluginHandle)
        {}

        [[nodiscard]] constexpr explicit operator bool() const noexcept
        {
            return raw_ && raw_->registerListener;
        }

        [[nodiscard]] bool RegisterListener(
            MessageHandler handler,
            const char* sender = "F4SE") const noexcept
        {
            return raw_ && raw_->registerListener && handler &&
                raw_->registerListener(
                    pluginHandle_, sender, reinterpret_cast<void*>(handler));
        }

        [[nodiscard]] bool Dispatch(
            std::uint32_t type,
            void* data = nullptr,
            std::uint32_t dataLength = 0,
            const char* receiver = nullptr) const noexcept
        {
            return raw_ && raw_->dispatch &&
                raw_->dispatch(
                    pluginHandle_, type, data, dataLength, receiver);
        }

        [[nodiscard]] constexpr std::uint32_t Version() const noexcept
        {
            return raw_ ? raw_->interfaceVersion : 0;
        }

    private:
        const RawMessagingInterface* raw_{};
        PluginHandle pluginHandle_{ kInvalidPluginHandle };
    };

    // Owns a safe copy of the interface fields.  Normalize() first copies the
    // prefix which every supported extender exposes, then reads extension
    // fields only when the extender version contract says they exist.  This is
    // the key to supporting F4SEVR 0.6.21 without an out-of-bounds read.
    class Interface
    {
    public:
        [[nodiscard]] bool Normalize(const void* rawInterface) noexcept
        {
            *this = {};
            if (!rawInterface)
                return false;

            std::memcpy(&prefix_, rawInterface, sizeof(prefix_));
            const auto f4seVersion = F4SEVersion();
            const auto* bytes = static_cast<const std::byte*>(rawInterface);

            if (f4seVersion >= kF4SEWithPluginInfo) {
                std::memcpy(
                    &getPluginInfo_,
                    bytes + offsetof(RawExtendedInterface, getPluginInfo),
                    sizeof(getPluginInfo_));
            }
            if (f4seVersion >= kF4SEWithSaveFolderName) {
                std::memcpy(
                    &getSaveFolderName_,
                    bytes + offsetof(
                        RawExtendedInterface, getSaveFolderName),
                    sizeof(getSaveFolderName_));
            }

            return prefix_.queryInterface && prefix_.getPluginHandle &&
                prefix_.getReleaseIndex;
        }

        [[nodiscard]] constexpr Version F4SEVersion() const noexcept
        {
            return Version::Unpack(prefix_.f4seVersion);
        }

        [[nodiscard]] constexpr Version RuntimeVersion() const noexcept
        {
            return Version::Unpack(prefix_.runtimeVersion);
        }

        [[nodiscard]] constexpr Version EditorVersion() const noexcept
        {
            return Version::Unpack(prefix_.editorVersion);
        }

        [[nodiscard]] constexpr bool IsEditor() const noexcept
        {
            return prefix_.isEditor != 0;
        }

        [[nodiscard]] PluginHandle GetPluginHandle() const noexcept
        {
            return prefix_.getPluginHandle ?
                prefix_.getPluginHandle() : kInvalidPluginHandle;
        }

        [[nodiscard]] std::uint32_t GetReleaseIndex() const noexcept
        {
            return prefix_.getReleaseIndex ? prefix_.getReleaseIndex() : 0;
        }

        [[nodiscard]] void* QueryInterface(InterfaceID id) const noexcept
        {
            return prefix_.queryInterface ?
                prefix_.queryInterface(static_cast<std::uint32_t>(id)) :
                nullptr;
        }

        [[nodiscard]] MessagingInterface GetMessagingInterface() const noexcept
        {
            return {
                static_cast<const RawMessagingInterface*>(
                    QueryInterface(InterfaceID::kMessaging)),
                GetPluginHandle()
            };
        }

        [[nodiscard]] const void* GetPluginInfo(
            const char* pluginName) const noexcept
        {
            return getPluginInfo_ && pluginName ?
                getPluginInfo_(pluginName) : nullptr;
        }

        [[nodiscard]] const char* GetSaveFolderName() const noexcept
        {
            return getSaveFolderName_ ? getSaveFolderName_() : nullptr;
        }

    private:
        RawInterfacePrefix prefix_{};
        GetPluginInfoFn getPluginInfo_{};
        GetSaveFolderNameFn getSaveFolderName_{};
    };

    enum class RuntimeTarget : std::uint8_t
    {
        kUnsupported,
        kLegacy,
        kAE,
        kVR
    };

    [[nodiscard]] constexpr wchar_t AsciiLower(wchar_t value) noexcept
    {
        return value >= L'A' && value <= L'Z' ?
            static_cast<wchar_t>(value + (L'a' - L'A')) : value;
    }

    [[nodiscard]] constexpr bool EqualsIgnoreCase(
        std::wstring_view left,
        std::wstring_view right) noexcept
    {
        if (left.size() != right.size())
            return false;
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (AsciiLower(left[index]) != AsciiLower(right[index]))
                return false;
        }
        return true;
    }

    // Pure host policy.  The caller supplies the independently read main-image
    // filename/version; a reported 1.10.138 runtime is accepted only for the
    // exact Fallout4VR.exe 1.2.72 + F4SEVR 0.6.21 combination.
    [[nodiscard]] constexpr RuntimeTarget ClassifyHost(
        const Interface& f4se,
        std::wstring_view executableName,
        Version executableVersion) noexcept
    {
        if (f4se.IsEditor())
            return RuntimeTarget::kUnsupported;

        if (EqualsIgnoreCase(executableName, L"Fallout4VR.exe")) {
            if (executableVersion == kVRRuntime &&
                f4se.F4SEVersion() == kF4SEVR0621 &&
                (f4se.RuntimeVersion() == kVRRuntime ||
                 f4se.RuntimeVersion() == kF4SEVRProxyRuntime)) {
                return RuntimeTarget::kVR;
            }
            return RuntimeTarget::kUnsupported;
        }

        if (!EqualsIgnoreCase(executableName, L"Fallout4.exe") ||
            f4se.F4SEVersion() < kF4SEWithPluginInfo ||
            f4se.RuntimeVersion() != executableVersion) {
            return RuntimeTarget::kUnsupported;
        }
        if (executableVersion == kLegacyRuntime)
            return RuntimeTarget::kLegacy;
        if (executableVersion == kAERuntime)
            return RuntimeTarget::kAE;
        return RuntimeTarget::kUnsupported;
    }

    struct PluginVersionData
    {
        inline static constexpr std::uint32_t kVersion = 1;

        constexpr void SetPluginVersion(Version value) noexcept
        {
            pluginVersion = value.Pack();
        }

        constexpr void SetPluginName(std::string_view value) noexcept
        {
            SetText(pluginName, value);
        }

        constexpr void SetAuthorName(std::string_view value) noexcept
        {
            SetText(author, value);
        }

        constexpr void UsesSignatureScanning(bool value) noexcept
        {
            SetBit(addressIndependence, 1u << 0u, value);
        }

        constexpr void UsesAddressLibraryNG(bool value) noexcept
        {
            SetBit(addressIndependence, 1u << 1u, value);
        }

        constexpr void UsesAddressLibraryAE(bool value) noexcept
        {
            SetBit(addressIndependence, 1u << 2u, value);
        }

        constexpr void HasNoStructureUse(bool value) noexcept
        {
            SetBit(structureIndependence, 1u << 0u, value);
        }

        constexpr void IsLayoutDependentNG(bool value) noexcept
        {
            SetBit(structureIndependence, 1u << 1u, value);
        }

        constexpr void IsLayoutDependentAE(bool value) noexcept
        {
            SetBit(structureIndependence, 1u << 2u, value);
        }

        constexpr void SetCompatibleVersions(
            std::initializer_list<Version> values) noexcept
        {
            std::size_t index = 0;
            for (const auto value : values) {
                if (index + 1 >= compatibleVersions.size())
                    break;
                compatibleVersions[index++] = value.Pack();
            }
            if (index < compatibleVersions.size())
                compatibleVersions[index] = 0;
        }

        constexpr void SetMinimumF4SEVersion(Version value) noexcept
        {
            f4seMinimum = value.Pack();
        }

        const std::uint32_t dataVersion{ kVersion };
        std::uint32_t pluginVersion{};
        char pluginName[256]{};
        char author[256]{};
        std::uint32_t addressIndependence{};
        std::uint32_t structureIndependence{};
        std::array<std::uint32_t, 16> compatibleVersions{};
        std::uint32_t f4seMinimum{};
        const std::uint32_t reservedNonBreaking{};
        const std::uint32_t reservedBreaking{};
        std::uint8_t reserved[512]{};

    private:
        template <std::size_t Size>
        static constexpr void SetText(
            char (&destination)[Size],
            std::string_view value) noexcept
        {
            for (auto& character : destination)
                character = '\0';
            const auto count = value.size() < Size - 1 ?
                value.size() : Size - 1;
            for (std::size_t index = 0; index < count; ++index)
                destination[index] = value[index];
        }

        static constexpr void SetBit(
            std::uint32_t& field,
            std::uint32_t bit,
            bool value) noexcept
        {
            if (value)
                field |= bit;
            else
                field &= ~bit;
        }
    };
    static_assert(offsetof(PluginVersionData, pluginName) == 8);
    static_assert(offsetof(PluginVersionData, compatibleVersions) == 528);
    static_assert(sizeof(PluginVersionData) == 1116);
}
