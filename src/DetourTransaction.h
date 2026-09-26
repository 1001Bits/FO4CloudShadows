#pragma once

#include <mutex>

namespace FO4CS::DetourTransaction
{
    // Detours supports one pending transaction per process. Every install
    // path serializes through this lock, so two hooks being installed from
    // different threads cannot make each other fail spuriously.
    [[nodiscard]] inline std::mutex& Mutex() noexcept
    {
        static std::mutex mutex;
        return mutex;
    }
}
