// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <wrl/client.h>

namespace FO4CS::RendererLifetime
{
    namespace Detail
    {
        inline std::shared_mutex callbacks;
        inline std::mutex retiredMutex;
        inline std::vector<Microsoft::WRL::ComPtr<IUnknown>> retired;
        inline std::atomic<bool> pending{ false };
        inline thread_local unsigned depth = 0;
    }

    inline void Collect() noexcept
    {
        if (Detail::depth != 0 || !Detail::pending.load(std::memory_order_acquire))
            return;
        std::vector<Microsoft::WRL::ComPtr<IUnknown>> released;
        {
            // New callbacks cannot acquire an old published pointer while
            // this boundary separates retirement from subsequent readers.
            std::unique_lock boundary(Detail::callbacks, std::try_to_lock);
            if (!boundary.owns_lock()) return;
            std::lock_guard lock(Detail::retiredMutex);
            released.swap(Detail::retired);
            Detail::pending.store(false, std::memory_order_release);
        }
        // COM destruction can enter another hook. Release outside both locks.
    }

    class Scope
    {
    public:
        explicit Scope(bool enabled = true) : enabled_(enabled)
        {
            if (enabled_) {
                if (Detail::depth == 0) Detail::callbacks.lock_shared();
                ++Detail::depth;
            }
        }
        ~Scope()
        {
            if (enabled_ && --Detail::depth == 0) {
                Detail::callbacks.unlock_shared();
                Collect();
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        bool enabled_;
    };

    template <class T>
    void Retire(const Microsoft::WRL::ComPtr<T>& object)
    {
        if (!object) return;
        std::lock_guard lock(Detail::retiredMutex);
        Detail::retired.emplace_back(object);
        Detail::pending.store(true, std::memory_order_release);
    }
}
