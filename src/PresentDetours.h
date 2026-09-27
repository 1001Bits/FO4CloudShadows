// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "DetourThreadEnlistment.h"
#include "DetourTransaction.h"
#include <dxgi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <utility>

namespace FO4CS::PresentDetours
{
    using Function = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
    using Handler = HRESULT(*)(Function, IDXGISwapChain*, UINT, UINT);
    inline constexpr std::size_t kCapacity = 16;

    // Shared across every route, so wrapper -> native and overlay -> wrapper
    // calls cannot publish the same scene twice. Unrelated receivers and TEST
    // calls retain their original behavior without running plugin frame work.
    class CallScope
    {
    public:
        CallScope(IDXGISwapChain* receiver, IDXGISwapChain* authoritative, UINT flags) noexcept :
            accepted_(++depth_ == 1 && receiver && receiver == authoritative &&
                (flags & DXGI_PRESENT_TEST) == 0)
        {}
        ~CallScope() { --depth_; }
        [[nodiscard]] bool Accepted() const noexcept { return accepted_; }
        CallScope(const CallScope&) = delete;
        CallScope& operator=(const CallScope&) = delete;
    private:
        inline static thread_local unsigned depth_{};
        bool accepted_{};
    };

    struct Result
    {
        bool ready{};
        bool installed{};
        bool attempted{};
        LONG error{};
        std::size_t route{};
        Function original{};
    };

    namespace Detail
    {
        struct Route
        {
            Function original{};
            Handler handler{};
            std::uintptr_t entry{};
            std::uintptr_t body{};
        };
        // Published trampolines are never replaced or detached. A wrapper and
        // native DXGI can both remain callable after renderer ownership changes.
        inline std::array<Route, kCapacity> routes{};
        inline std::size_t count{};
        inline std::mutex mutex;
        inline std::uintptr_t failedEntry{};
        inline ULONGLONG retryAfter{};
        inline unsigned failures{};

        template <std::size_t Index>
        HRESULT WINAPI Thunk(IDXGISwapChain* receiver, UINT interval, UINT flags)
        {
            const auto& route = routes[Index];
            return route.handler(route.original, receiver, interval, flags);
        }

        template <std::size_t... Indices>
        constexpr auto MakeThunks(std::index_sequence<Indices...>) noexcept
        {
            return std::array<Function, sizeof...(Indices)>{ &Thunk<Indices>... };
        }
        inline constexpr auto thunks = MakeThunks(std::make_index_sequence<kCapacity>{});

        inline Result Existing(std::size_t index, Handler handler) noexcept
        {
            const auto& route = routes[index];
            if (route.handler != handler)
                return { .error = ERROR_INVALID_PARAMETER };
            return { .ready = true, .route = index, .original = route.original };
        }
    }

    // The caller validates the executable Present entry from the actual COM
    // vtable. No receiver is inferred from another object's private data.
    inline Result Ensure(std::uintptr_t entry, Handler handler)
    {
        if (!entry || !handler)
            return { .error = ERROR_INVALID_PARAMETER };
        std::lock_guard lock(Detail::mutex);
        for (std::size_t index = 0; index < Detail::count; ++index) {
            const auto& route = Detail::routes[index];
            if (entry == route.entry || entry == route.body ||
                entry == reinterpret_cast<std::uintptr_t>(Detail::thunks[index]))
                return Detail::Existing(index, handler);
        }
        // Detours follows import/forwarding jumps. An alias that already leads
        // to one of our thunks must not detour that thunk a second time.
        const auto resolved = reinterpret_cast<std::uintptr_t>(
            DetourCodeFromPointer(reinterpret_cast<void*>(entry), nullptr));
        for (std::size_t index = 0; index < Detail::count; ++index) {
            if (resolved == reinterpret_cast<std::uintptr_t>(Detail::thunks[index]))
                return Detail::Existing(index, handler);
        }
        if (Detail::count == kCapacity)
            return { .error = ERROR_INSUFFICIENT_BUFFER };
        const auto now = GetTickCount64();
        if (entry == Detail::failedEntry && now < Detail::retryAfter)
            return { .error = ERROR_RETRY };

        const auto index = Detail::count;
        auto& route = Detail::routes[index];
        route = { reinterpret_cast<Function>(entry), handler, entry, 0 };
        std::lock_guard transactionLock(DetourTransaction::Mutex());
        LONG error = DetourTransactionBegin();
        const bool transactionBegun = error == NO_ERROR;
        PVOID body = nullptr;
        if (error == NO_ERROR)
            error = DetourAttachEx(reinterpret_cast<PVOID*>(&route.original),
                reinterpret_cast<PVOID>(Detail::thunks[index]), nullptr, &body, nullptr);
        DetourThreadEnlistment threads;
        if (error == NO_ERROR)
            error = threads.EnlistProcessThreads();
        if (error == NO_ERROR)
            error = DetourTransactionCommit();
        else if (transactionBegun)
            DetourTransactionAbort();
        if (error != NO_ERROR) {
            route = {};
            if (Detail::failedEntry != entry)
                Detail::failures = 0;
            Detail::failedEntry = entry;
            ++Detail::failures;
            Detail::retryAfter = now + (std::min)(60000ull,
                1000ull << (std::min)(Detail::failures, 6u));
            return { .attempted = true, .error = error };
        }
        route.body = reinterpret_cast<std::uintptr_t>(body);
        ++Detail::count;
        Detail::failedEntry = 0;
        Detail::retryAfter = 0;
        Detail::failures = 0;
        return { .ready = true, .installed = true, .attempted = true,
            .route = index, .original = route.original };
    }
}
