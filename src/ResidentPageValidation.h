// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace FO4CS::ResidentPages
{
    enum class Result { Unknown, Readable, Unreadable };
    // Private profiling can A/B the original region scan in the same process.
    inline std::atomic<bool> enabled{true};
    inline DWORD PageSize() noexcept {
        static const DWORD value=[] { SYSTEM_INFO info{}; GetSystemInfo(&info); return info.dwPageSize; }();
        return value;
    }
    // Only proves readability. Executable/writable/image-identity checks retain
    // VirtualQuery. Unknown/nonresident/large spans also retain that fallback.
    // No protection or pointer cache: every covered page is queried every time.
    inline Result Check(std::uintptr_t address, std::size_t size) noexcept {
        if (!enabled.load(std::memory_order_relaxed)) return Result::Unknown;
        if (!address || !size || size > (std::numeric_limits<std::uintptr_t>::max)()-address)
            return Result::Unreadable;
        const auto page=PageSize();
        if (!page) return Result::Unknown;
        const auto first=address-address%page;
        const auto last=address+size-1;
        const auto count=(last-first)/page+1;
        std::array<PSAPI_WORKING_SET_EX_INFORMATION,64> pages{};
        if (count>pages.size()) return Result::Unknown;
        for (std::size_t i=0;i<count;++i)
            pages[i].VirtualAddress=reinterpret_cast<void*>(first+i*page);
        if (!QueryWorkingSetEx(GetCurrentProcess(),pages.data(),
            static_cast<DWORD>(count*sizeof(pages[0])))) return Result::Unknown;
        for (std::size_t i=0;i<count;++i) {
            const auto attributes=pages[i].VirtualAttributes;
            // Special/locked mappings retain the original MEM_COMMIT check;
            // working-set residency alone is not a proof of allocation state.
            if (!attributes.Valid || attributes.Locked || attributes.LargePage || attributes.Bad)
                return Result::Unknown;
            const DWORD protection=static_cast<DWORD>(attributes.Win32Protection);
            if (protection & (PAGE_GUARD|PAGE_NOACCESS)) return Result::Unreadable;
            switch (protection & 0xffu) {
            case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
            case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
                break;
            default: return Result::Unreadable;
            }
        }
        return Result::Readable;
    }
}
