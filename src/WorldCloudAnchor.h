#pragma once

#include <array>
#include <cmath>

namespace FO4CS
{
    // World-owned, not camera-owned. Only the authenticated deferred-light
    // view may confirm it; provisional Sky/loading/reflection views cannot.
    class WorldCloudAnchor
    {
    public:
        using Point = std::array<float, 3>;
        enum class Confirmation { kRejected, kUnchanged, kEstablished };

        Confirmation ConfirmMainView(const Point& camera) noexcept
        {
            for (const auto value : camera)
                if (!std::isfinite(value)) return Confirmation::kRejected;
            if (confirmed_) return Confirmation::kUnchanged;
            point_ = camera;
            confirmed_ = true;
            return Confirmation::kEstablished;
        }

        void Reset() noexcept { confirmed_ = false; point_ = {}; }
        bool IsConfirmed() const noexcept { return confirmed_; }
        const Point& Position() const noexcept { return point_; }

    private:
        bool confirmed_{};
        Point point_{};
    };
}
