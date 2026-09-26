#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace FO4CS
{
    // Present may observe a world/load transition but never establishes a
    // world. An established identity of 0 means no authenticated main view has
    // established one yet (startup, after a transition reset, or while shadows
    // are disabled); comparing it with the live world is not a transition and
    // must not reset the already empty field on every Present.
    enum class PresentWorldAction { kReset, kWait, kContinue };

    [[nodiscard]] constexpr PresentWorldAction ClassifyPresentWorld(
        bool explicitReset, std::uintptr_t established, std::uintptr_t current) noexcept
    {
        if (explicitReset || (established != 0 && established != current))
            return PresentWorldAction::kReset;
        return established == 0 ? PresentWorldAction::kWait : PresentWorldAction::kContinue;
    }

    // The captured cube is directional (Fallout's sky dome sits at infinity),
    // so any world-fixed origin projects it correctly near that origin. Far
    // from it, receivers sample near-horizon texels whose ground footprint
    // grows with distance, and receivers above the shell get no shadow at
    // all. Leaving this cylinder around the origin moves the anchor to the
    // gameplay camera; the projection crossfades between the two origins so
    // the ground pattern never jumps. Following the camera continuously would
    // instead make every terrain shadow slide with the player.
    struct ReanchorPolicy
    {
        static constexpr float kHorizontalCloudHeights = 1.5f;
        static constexpr float kVerticalCloudHeights = 0.5f;
        static constexpr std::uint64_t kCrossfadeMilliseconds = 4000;
        static constexpr std::uint64_t kReadbackIntervalMilliseconds = 500;
    };

    [[nodiscard]] inline bool NeedsReanchor(
        const std::array<float, 3>& origin,
        const std::array<float, 3>& camera,
        float cloudHeight) noexcept
    {
        if (!std::isfinite(cloudHeight) || cloudHeight <= 0.0f)
            return false;
        for (unsigned i = 0; i < 3; ++i) {
            if (!std::isfinite(origin[i]) || !std::isfinite(camera[i]))
                return false;
        }
        const float dx = camera[0] - origin[0];
        const float dy = camera[1] - origin[1];
        const float dz = camera[2] - origin[2];
        const float horizontal = ReanchorPolicy::kHorizontalCloudHeights * cloudHeight;
        const float vertical = ReanchorPolicy::kVerticalCloudHeights * cloudHeight;
        return dx * dx + dy * dy > horizontal * horizontal || std::abs(dz) > vertical;
    }

    // Weight of the previous origin `elapsed` milliseconds into a crossfade:
    // one at the switch, zero once complete, smoothstep in between.
    [[nodiscard]] constexpr float CrossfadeWeight(
        std::uint64_t elapsed, std::uint64_t duration) noexcept
    {
        if (duration == 0 || elapsed >= duration)
            return 0.0f;
        const float t = static_cast<float>(elapsed) / static_cast<float>(duration);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }

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

        // Travel re-anchoring (see NeedsReanchor). Only a confirmed anchor can
        // move, and only to a finite main-view camera.
        bool Reanchor(const Point& camera) noexcept
        {
            if (!confirmed_)
                return false;
            for (const auto value : camera)
                if (!std::isfinite(value)) return false;
            point_ = camera;
            return true;
        }

        void Reset() noexcept { confirmed_ = false; point_ = {}; }
        bool IsConfirmed() const noexcept { return confirmed_; }
        const Point& Position() const noexcept { return point_; }

    private:
        bool confirmed_{};
        Point point_{};
    };
}
