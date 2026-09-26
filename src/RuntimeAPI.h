#pragma once

#include "F4SECompat.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace FO4CS
{
    enum class AddressKind : std::uint8_t
    {
        kImage,
        kReadable,
        kWritable,
        kExecutable
    };

    // A zero entry means that runtime has no reviewed fallback and must fail
    // closed.  This is deliberately explicit at every call site: an OG RVA is
    // never reused merely because an Address Library ID is missing on AE/VR.
    struct RuntimeRVA
    {
        std::uintptr_t legacy{};
        std::uintptr_t ae{};
        std::uintptr_t vr{};

        [[nodiscard]] constexpr std::uintptr_t Select(
            F4SECompat::RuntimeTarget target) const noexcept
        {
            switch (target) {
            case F4SECompat::RuntimeTarget::kLegacy:
                return legacy;
            case F4SECompat::RuntimeTarget::kAE:
                return ae;
            case F4SECompat::RuntimeTarget::kVR:
                return vr;
            default:
                return 0;
            }
        }
    };

    struct RuntimeHost
    {
        F4SECompat::RuntimeTarget target{
            F4SECompat::RuntimeTarget::kUnsupported
        };
        F4SECompat::Version executableVersion{};
        F4SECompat::Version reportedRuntimeVersion{};
        F4SECompat::Version f4seVersion{};
        std::filesystem::path executablePath{};
        std::uintptr_t moduleBase{};
        std::size_t imageSize{};
    };

    // Project-local runtime boundary for one physical DLL.  The declarations
    // and Address Library formats are derived from the public F4SE plugin ABI
    // and GPL-3.0 CommonLibF4 implementations (alandtse/CommonLibF4 and
    // Dear-Modding-FO4/commonlibf4); this project is distributed under GPL-3.0.
    // No CommonLib type crosses this interface.
    class RuntimeAPI
    {
    public:
        RuntimeAPI(const RuntimeAPI&) = delete;
        RuntimeAPI(RuntimeAPI&&) = delete;
        RuntimeAPI& operator=(const RuntimeAPI&) = delete;
        RuntimeAPI& operator=(RuntimeAPI&&) = delete;

        [[nodiscard]] static RuntimeAPI& GetSingleton() noexcept;

        // Safe for F4SEPlugin_Query: validates the actual main executable and
        // normalized interface without retaining the loader's pointer or
        // opening an Address Library database.
        [[nodiscard]] static bool ProbeHost(
            const void* rawF4SEInterface,
            RuntimeHost& host,
            std::string& error) noexcept;

        // Called from F4SEPlugin_Load after ProbeHost.  Copies the stable ABI
        // fields and retains no pointer to the raw load-interface object.
        [[nodiscard]] bool Initialize(
            const void* rawF4SEInterface) noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept;
        [[nodiscard]] RuntimeHost Host() const;
        [[nodiscard]] F4SECompat::RuntimeTarget Target() const noexcept;
        [[nodiscard]] F4SECompat::Interface F4SE() const noexcept;
        [[nodiscard]] F4SECompat::MessagingInterface Messaging() const noexcept;

        // Resolves an exact ID from the runtime's version-N-N-N-N.bin (flat)
        // or .csv (VR) database.  ID equality and the resulting PE address are
        // both checked; lookup never falls through to the next sorted ID.
        [[nodiscard]] std::uintptr_t ResolveID(
            std::uint64_t id,
            AddressKind kind = AddressKind::kImage) noexcept;

        // Selects only the RVA explicitly supplied for the active runtime and
        // validates it against the loaded main image and page protection.
        [[nodiscard]] std::uintptr_t ResolveRVA(
            RuntimeRVA rvas,
            AddressKind kind = AddressKind::kImage) const noexcept;

        [[nodiscard]] bool ValidateAddress(
            std::uintptr_t address,
            AddressKind kind) const noexcept;

        // Validates an arbitrary committed virtual-memory span. Unlike
        // ValidateAddress(), this deliberately permits heap allocations; it is
        // used only after an image-resident relocation slot has supplied the
        // pointee. Every page in the requested span must have the requested
        // protection.
        [[nodiscard]] bool ValidateMemory(
            std::uintptr_t address,
            std::size_t byteCount,
            AddressKind kind) const noexcept;

        // Readable spans proven valid are remembered per thread until the
        // next authoritative frame boundary calls this. Engine singletons and
        // shader objects outlive a frame, so one proof per frame replaces
        // roughly a hundred working-set queries.
        static void AdvanceValidationEpoch() noexcept;

        [[nodiscard]] std::filesystem::path AddressDatabasePath() const;
        [[nodiscard]] std::string LastError() const;

    private:
        struct Mapping
        {
            std::uint64_t id{};
            std::uint64_t offset{};

            [[nodiscard]] constexpr bool operator<(
                const Mapping& right) const noexcept
            {
                return id < right.id;
            }
        };
        static_assert(sizeof(Mapping) == 16);

        RuntimeAPI() noexcept = default;

        [[nodiscard]] bool LoadAddressDatabaseLocked() noexcept;
        [[nodiscard]] bool LoadFlatDatabaseLocked(
            const std::filesystem::path& path) noexcept;
        [[nodiscard]] bool LoadVRDatabaseLocked(
            const std::filesystem::path& path) noexcept;
        void SetErrorLocked(std::string value) noexcept;

        mutable std::mutex mutex_{};
        F4SECompat::Interface f4se_{};
        RuntimeHost host_{};
        // Initialize() publishes the immutable host/interface payload exactly
        // once. Readers acquire this flag before accessing either object.
        std::atomic<bool> initialized_{ false };
        bool addressDatabaseAttempted_{};
        std::filesystem::path addressDatabasePath_{};
        std::vector<Mapping> mappings_{};
        std::string lastError_{};
    };
}
