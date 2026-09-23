#include "CpuStageProfiler.h"
#include "ResidentPageValidation.h"
#include "RuntimeAPI.h"

#include <Windows.h>
#include <winver.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <fstream>
#include <limits>
#include <system_error>

#pragma comment(lib, "Version.lib")

namespace
{
    using FO4CS::F4SECompat::Version;

    static_assert(std::endian::native == std::endian::little);

    constexpr std::uint64_t kMaximumAddressCount = 10'000'000;

    void AssignError(std::string& destination, std::string_view value) noexcept
    {
        try {
            destination.assign(value);
        } catch (...) {
            destination.clear();
        }
    }

    [[nodiscard]] std::wstring VersionFilenamePart(Version version)
    {
        return std::to_wstring(version.major) + L"-" +
            std::to_wstring(version.minor) + L"-" +
            std::to_wstring(version.patch) + L"-" +
            std::to_wstring(version.build);
    }

    [[nodiscard]] bool ReadExecutableVersion(
        const std::filesystem::path& path,
        Version& version,
        std::string& error)
    {
        DWORD ignored = 0;
        const DWORD byteCount = GetFileVersionInfoSizeW(
            path.c_str(), &ignored);
        if (byteCount == 0) {
            error = "main executable has no readable file-version resource";
            return false;
        }

        std::vector<std::byte> bytes(byteCount);
        if (!GetFileVersionInfoW(
                path.c_str(), 0, byteCount, bytes.data())) {
            error = "GetFileVersionInfoW failed for the main executable";
            return false;
        }

        VS_FIXEDFILEINFO* fixedInfo = nullptr;
        UINT fixedInfoSize = 0;
        if (!VerQueryValueW(
                bytes.data(), L"\\",
                reinterpret_cast<void**>(&fixedInfo),
                &fixedInfoSize) ||
            !fixedInfo || fixedInfoSize < sizeof(VS_FIXEDFILEINFO) ||
            fixedInfo->dwSignature != 0xFEEF04BDu) {
            error = "main executable file-version resource is malformed";
            return false;
        }

        version = {
            HIWORD(fixedInfo->dwFileVersionMS),
            LOWORD(fixedInfo->dwFileVersionMS),
            HIWORD(fixedInfo->dwFileVersionLS),
            LOWORD(fixedInfo->dwFileVersionLS)
        };
        return true;
    }

    [[nodiscard]] bool ReadMainImage(
        std::filesystem::path& path,
        std::uintptr_t& base,
        std::size_t& imageSize,
        std::string& error)
    {
        const HMODULE mainModule = GetModuleHandleW(nullptr);
        if (!mainModule) {
            error = "GetModuleHandleW(nullptr) failed";
            return false;
        }

        std::wstring pathBuffer(32768, L'\0');
        const DWORD pathLength = GetModuleFileNameW(
            mainModule, pathBuffer.data(),
            static_cast<DWORD>(pathBuffer.size()));
        if (pathLength == 0 || pathLength >= pathBuffer.size()) {
            error = "GetModuleFileNameW failed or truncated the main path";
            return false;
        }
        pathBuffer.resize(pathLength);
        path = std::filesystem::path(pathBuffer);

        const auto* bytes = reinterpret_cast<const std::byte*>(mainModule);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            error = "main executable has an invalid DOS header";
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            bytes + static_cast<std::size_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->OptionalHeader.SizeOfImage == 0) {
            error = "main executable has an invalid PE64 header";
            return false;
        }

        base = reinterpret_cast<std::uintptr_t>(mainModule);
        imageSize = nt->OptionalHeader.SizeOfImage;
        return true;
    }

    [[nodiscard]] std::string_view StripCarriageReturn(
        std::string_view value) noexcept
    {
        if (!value.empty() && value.back() == '\r')
            value.remove_suffix(1);
        return value;
    }

    [[nodiscard]] bool SplitCSVLine(
        std::string_view line,
        std::string_view& first,
        std::string_view& second) noexcept
    {
        line = StripCarriageReturn(line);
        const auto comma = line.find(',');
        if (comma == std::string_view::npos ||
            line.find(',', comma + 1) != std::string_view::npos) {
            return false;
        }
        first = line.substr(0, comma);
        second = line.substr(comma + 1);
        return !first.empty() && !second.empty();
    }

    template <class Integer>
    [[nodiscard]] bool ParseInteger(
        std::string_view text,
        int base,
        Integer& value) noexcept
    {
        value = 0;
        const auto result = std::from_chars(
            text.data(), text.data() + text.size(), value, base);
        return result.ec == std::errc{} &&
            result.ptr == text.data() + text.size();
    }

    [[nodiscard]] bool IsReadableProtection(DWORD protection) noexcept
    {
        switch (protection & 0xFFu) {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool IsWritableProtection(DWORD protection) noexcept
    {
        switch (protection & 0xFFu) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool IsExecutableProtection(DWORD protection) noexcept
    {
        switch (protection & 0xFFu) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }
}

namespace FO4CS
{
    RuntimeAPI& RuntimeAPI::GetSingleton() noexcept
    {
        static RuntimeAPI singleton;
        return singleton;
    }

    bool RuntimeAPI::ProbeHost(
        const void* rawF4SEInterface,
        RuntimeHost& host,
        std::string& error) noexcept
    {
        host = {};
        error.clear();
        try {
            F4SECompat::Interface f4se;
            if (!f4se.Normalize(rawF4SEInterface)) {
                error = "F4SE load/query interface is null or incomplete";
                return false;
            }

            if (!ReadMainImage(
                    host.executablePath,
                    host.moduleBase,
                    host.imageSize,
                    error)) {
                return false;
            }
            if (!ReadExecutableVersion(
                    host.executablePath,
                    host.executableVersion,
                    error)) {
                return false;
            }

            host.reportedRuntimeVersion = f4se.RuntimeVersion();
            host.f4seVersion = f4se.F4SEVersion();
            host.target = F4SECompat::ClassifyHost(
                f4se,
                host.executablePath.filename().wstring(),
                host.executableVersion);
            if (host.target ==
                F4SECompat::RuntimeTarget::kUnsupported) {
                error =
                    "unsupported executable/F4SE combination; expected "
                    "Fallout4.exe 1.10.163 or 1.11.240, or "
                    "Fallout4VR.exe 1.2.72 with F4SEVR 0.6.21";
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            AssignError(error, exception.what());
            return false;
        } catch (...) {
            AssignError(error, "unknown exception while probing runtime host");
            return false;
        }
    }

    bool RuntimeAPI::Initialize(const void* rawF4SEInterface) noexcept
    {
        if (initialized_.load(std::memory_order_acquire))
            return true;

        RuntimeHost host;
        std::string error;
        F4SECompat::Interface f4se;
        if (!ProbeHost(rawF4SEInterface, host, error) ||
            !f4se.Normalize(rawF4SEInterface)) {
            std::scoped_lock lock(mutex_);
            SetErrorLocked(error.empty() ?
                "unable to normalize F4SE interface" : std::move(error));
            return false;
        }

        try {
            std::scoped_lock lock(mutex_);
            if (initialized_.load(std::memory_order_relaxed))
                return true;
            f4se_ = f4se;
            host_ = std::move(host);
            addressDatabaseAttempted_ = false;
            addressDatabasePath_.clear();
            mappings_.clear();
            lastError_.clear();
            initialized_.store(true, std::memory_order_release);
            return true;
        } catch (...) {
            std::scoped_lock lock(mutex_);
            initialized_.store(false, std::memory_order_relaxed);
            SetErrorLocked("unable to retain normalized runtime state");
            return false;
        }
    }

    bool RuntimeAPI::IsInitialized() const noexcept
    {
        return initialized_.load(std::memory_order_acquire);
    }

    RuntimeHost RuntimeAPI::Host() const
    {
        std::scoped_lock lock(mutex_);
        return host_;
    }

    F4SECompat::RuntimeTarget RuntimeAPI::Target() const noexcept
    {
        if (!initialized_.load(std::memory_order_acquire))
            return F4SECompat::RuntimeTarget::kUnsupported;
        return host_.target;
    }

    F4SECompat::Interface RuntimeAPI::F4SE() const noexcept
    {
        if (!initialized_.load(std::memory_order_acquire))
            return {};
        return f4se_;
    }

    F4SECompat::MessagingInterface RuntimeAPI::Messaging() const noexcept
    {
        if (!initialized_.load(std::memory_order_acquire))
            return {};
        return f4se_.GetMessagingInterface();
    }

    std::uintptr_t RuntimeAPI::ResolveID(
        std::uint64_t id,
        AddressKind kind) noexcept
    {
        try {
            std::scoped_lock lock(mutex_);
            if (!initialized_.load(std::memory_order_acquire)) {
                SetErrorLocked("runtime API is not initialized");
                return 0;
            }
            if (!LoadAddressDatabaseLocked())
                return 0;

            const Mapping key{ id, 0 };
            const auto found = std::lower_bound(
                mappings_.begin(), mappings_.end(), key);
            if (found == mappings_.end() || found->id != id) {
                SetErrorLocked(
                    "Address Library does not contain exact ID " +
                    std::to_string(id));
                return 0;
            }
            if (found->offset == 0 ||
                found->offset >= host_.imageSize ||
                found->offset >
                    (std::numeric_limits<std::uintptr_t>::max)() -
                        host_.moduleBase) {
                SetErrorLocked(
                    "Address Library ID resolves outside the main image");
                return 0;
            }

            const auto address = host_.moduleBase +
                static_cast<std::uintptr_t>(found->offset);
            if (!ValidateAddress(address, kind)) {
                SetErrorLocked(
                    "Address Library ID failed PE/page validation");
                return 0;
            }
            return address;
        } catch (const std::exception& exception) {
            std::scoped_lock lock(mutex_);
            SetErrorLocked(exception.what());
            return 0;
        } catch (...) {
            std::scoped_lock lock(mutex_);
            SetErrorLocked("unknown exception resolving Address Library ID");
            return 0;
        }
    }

    std::uintptr_t RuntimeAPI::ResolveRVA(
        RuntimeRVA rvas,
        AddressKind kind) const noexcept
    {
        if (!initialized_.load(std::memory_order_acquire))
            return 0;
        const auto rva = rvas.Select(host_.target);
        if (rva == 0 || rva >= host_.imageSize ||
            rva > (std::numeric_limits<std::uintptr_t>::max)() -
                host_.moduleBase) {
            return 0;
        }
        const auto address = host_.moduleBase + rva;
        return ValidateAddress(address, kind) ? address : 0;
    }

    bool RuntimeAPI::ValidateAddress(
        std::uintptr_t address,
        AddressKind kind) const noexcept
    {
        if (!initialized_.load(std::memory_order_acquire) ||
            host_.moduleBase == 0 || host_.imageSize == 0 ||
            address < host_.moduleBase ||
            address - host_.moduleBase >= host_.imageSize) {
            return false;
        }

        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(address),
                &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            memory.AllocationBase !=
                reinterpret_cast<void*>(host_.moduleBase) ||
            (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }

        switch (kind) {
        case AddressKind::kImage:
            return true;
        case AddressKind::kReadable:
            return IsReadableProtection(memory.Protect);
        case AddressKind::kWritable:
            return IsWritableProtection(memory.Protect);
        case AddressKind::kExecutable:
            return IsExecutableProtection(memory.Protect);
        default:
            return false;
        }
    }

    bool RuntimeAPI::ValidateMemory(
        std::uintptr_t address,
        std::size_t byteCount,
        AddressKind kind) const noexcept
    {
        FO4CS::CpuProfile::Scope cpuTiming(FO4CS::CpuProfile::Stage::MemoryValidation);
        if (!initialized_.load(std::memory_order_acquire) ||
            address == 0 || byteCount == 0 ||
            byteCount >
                (std::numeric_limits<std::uintptr_t>::max)() - address) {
            return false;
        }

        const auto end = address + byteCount;
        if (kind == AddressKind::kReadable) {
            const auto resident = ResidentPages::Check(address, byteCount);
            if (resident != ResidentPages::Result::Unknown)
                return resident == ResidentPages::Result::Readable;
        }
        auto cursor = address;
        while (cursor < end) {
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(
                    reinterpret_cast<const void*>(cursor),
                    &memory, sizeof(memory)) != sizeof(memory) ||
                memory.State != MEM_COMMIT ||
                (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                return false;
            }

            bool protectionMatches = false;
            switch (kind) {
            case AddressKind::kImage:
            case AddressKind::kReadable:
                protectionMatches = IsReadableProtection(memory.Protect);
                break;
            case AddressKind::kWritable:
                protectionMatches = IsWritableProtection(memory.Protect);
                break;
            case AddressKind::kExecutable:
                protectionMatches = IsExecutableProtection(memory.Protect);
                break;
            default:
                return false;
            }
            if (!protectionMatches)
                return false;

            const auto regionBase = reinterpret_cast<std::uintptr_t>(
                memory.BaseAddress);
            if (memory.RegionSize == 0 || cursor < regionBase ||
                memory.RegionSize >
                    (std::numeric_limits<std::uintptr_t>::max)() -
                        regionBase) {
                return false;
            }
            const auto regionEnd = regionBase + memory.RegionSize;
            if (regionEnd <= cursor)
                return false;
            cursor = (std::min)(end, regionEnd);
        }
        return true;
    }

    std::filesystem::path RuntimeAPI::AddressDatabasePath() const
    {
        std::scoped_lock lock(mutex_);
        return addressDatabasePath_;
    }

    std::string RuntimeAPI::LastError() const
    {
        std::scoped_lock lock(mutex_);
        return lastError_;
    }

    bool RuntimeAPI::LoadAddressDatabaseLocked() noexcept
    {
        if (addressDatabaseAttempted_)
            return !mappings_.empty();
        addressDatabaseAttempted_ = true;

        try {
            const bool vr = host_.target ==
                F4SECompat::RuntimeTarget::kVR;
            addressDatabasePath_ =
                host_.executablePath.parent_path() /
                L"Data" / L"F4SE" / L"Plugins" /
                (L"version-" +
                 VersionFilenamePart(host_.executableVersion) +
                 (vr ? L".csv" : L".bin"));
            return vr ?
                LoadVRDatabaseLocked(addressDatabasePath_) :
                LoadFlatDatabaseLocked(addressDatabasePath_);
        } catch (const std::exception& exception) {
            SetErrorLocked(exception.what());
            mappings_.clear();
            return false;
        } catch (...) {
            SetErrorLocked("unknown exception loading Address Library");
            mappings_.clear();
            return false;
        }
    }

    bool RuntimeAPI::LoadFlatDatabaseLocked(
        const std::filesystem::path& path) noexcept
    {
        try {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                SetErrorLocked(
                    "unable to open flat Address Library database");
                return false;
            }
            const auto end = input.tellg();
            if (end < static_cast<std::streamoff>(sizeof(std::uint64_t))) {
                SetErrorLocked("flat Address Library database is truncated");
                return false;
            }
            const auto fileSize = static_cast<std::uint64_t>(end);
            input.seekg(0, std::ios::beg);

            std::uint64_t count = 0;
            input.read(reinterpret_cast<char*>(&count), sizeof(count));
            if (!input || count == 0 || count > kMaximumAddressCount ||
                count > ((std::numeric_limits<std::uint64_t>::max)() -
                    sizeof(count)) / sizeof(Mapping) ||
                sizeof(count) + count * sizeof(Mapping) != fileSize) {
                SetErrorLocked(
                    "flat Address Library count/size contract is invalid");
                return false;
            }

            mappings_.resize(static_cast<std::size_t>(count));
            input.read(
                reinterpret_cast<char*>(mappings_.data()),
                static_cast<std::streamsize>(
                    mappings_.size() * sizeof(Mapping)));
            if (!input) {
                SetErrorLocked("flat Address Library mapping read failed");
                mappings_.clear();
                return false;
            }

            std::sort(mappings_.begin(), mappings_.end());
            for (std::size_t index = 0; index < mappings_.size(); ++index) {
                if (mappings_[index].offset == 0 ||
                    mappings_[index].offset >= host_.imageSize ||
                    (index != 0 &&
                     mappings_[index - 1].id == mappings_[index].id)) {
                    SetErrorLocked(
                        "flat Address Library contains an invalid mapping");
                    mappings_.clear();
                    return false;
                }
            }
            return true;
        } catch (const std::exception& exception) {
            SetErrorLocked(exception.what());
            mappings_.clear();
            return false;
        } catch (...) {
            SetErrorLocked(
                "unknown exception reading flat Address Library");
            mappings_.clear();
            return false;
        }
    }

    bool RuntimeAPI::LoadVRDatabaseLocked(
        const std::filesystem::path& path) noexcept
    {
        try {
            std::ifstream input(path);
            if (!input) {
                SetErrorLocked("unable to open VR Address Library CSV");
                return false;
            }

            std::string line;
            if (!std::getline(input, line) ||
                StripCarriageReturn(line) != "id,offset") {
                SetErrorLocked("VR Address Library CSV header is invalid");
                return false;
            }

            std::string_view first;
            std::string_view second;
            std::uint64_t expectedCount = 0;
            if (!std::getline(input, line) ||
                !SplitCSVLine(line, first, second) ||
                !ParseInteger(first, 10, expectedCount) ||
                expectedCount == 0 ||
                expectedCount > kMaximumAddressCount) {
                SetErrorLocked(
                    "VR Address Library CSV metadata is invalid");
                return false;
            }
            // The second metadata cell is the address-library release (for
            // example 1.13.1), not the game runtime.  Its presence was checked
            // by SplitCSVLine; the exact executable version remains our gate.

            mappings_.clear();
            mappings_.reserve(static_cast<std::size_t>(expectedCount));
            for (std::uint64_t index = 0; index < expectedCount; ++index) {
                if (!std::getline(input, line) ||
                    !SplitCSVLine(line, first, second)) {
                    SetErrorLocked(
                        "VR Address Library CSV ended before its count");
                    mappings_.clear();
                    return false;
                }

                Mapping mapping{};
                if (!ParseInteger(first, 10, mapping.id) ||
                    !ParseInteger(second, 16, mapping.offset) ||
                    mapping.offset == 0 ||
                    mapping.offset >= host_.imageSize) {
                    SetErrorLocked(
                        "VR Address Library CSV contains an invalid mapping");
                    mappings_.clear();
                    return false;
                }
                mappings_.push_back(mapping);
            }
            while (std::getline(input, line)) {
                if (!StripCarriageReturn(line).empty()) {
                    SetErrorLocked(
                        "VR Address Library CSV exceeds its declared count");
                    mappings_.clear();
                    return false;
                }
            }

            std::sort(mappings_.begin(), mappings_.end());
            for (std::size_t index = 1; index < mappings_.size(); ++index) {
                if (mappings_[index - 1].id == mappings_[index].id) {
                    SetErrorLocked(
                        "VR Address Library CSV contains duplicate IDs");
                    mappings_.clear();
                    return false;
                }
            }
            return true;
        } catch (const std::exception& exception) {
            SetErrorLocked(exception.what());
            mappings_.clear();
            return false;
        } catch (...) {
            SetErrorLocked("unknown exception reading VR Address Library CSV");
            mappings_.clear();
            return false;
        }
    }

    void RuntimeAPI::SetErrorLocked(std::string value) noexcept
    {
        try {
            lastError_ = std::move(value);
        } catch (...) {
            lastError_.clear();
        }
    }
}
