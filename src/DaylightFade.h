// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FO4CS::DaylightFade
{
    // The Fallout 4 sun never drops below the horizon: Sun::Update computes
    // Z = |fSunXExtreme| - |X|, so at sunset the disc reaches the horizon and
    // retraces its arc, hidden, all night while the moon lights the world.
    // Only the clock can tell 21:00 from 15:00, so the same elevation cannot
    // decide this. With "Shadows at night" off, shadows follow the current climate's own sun times: full
    // strength by day, fading out as the disc reaches the horizon at the
    // middle of the climate's sunset and back in after the middle of sunrise.
    struct ClimateHours
    {
        float sunriseBegin{};
        float sunriseEnd{};
        float sunsetBegin{};
        float sunsetEnd{};
    };

    // TESClimate stores each sun time as an unsigned count of ten game minutes.
    [[nodiscard]] constexpr float HoursFromClimateCount(std::uint8_t count) noexcept
    {
        return static_cast<float>(count) / 6.0f;
    }

    // Long enough at any timescale that the change cannot be seen as a step.
    inline constexpr float kFadeHours = 1.0f;

    // 1 by day, 0 at night. A window that is not a window (a corrupt or
    // unauthored climate) returns 1: a bad read must not switch shadows off.
    [[nodiscard]] inline float Factor(float hour, const ClimateHours& climate) noexcept
    {
        const float sunrise = (climate.sunriseBegin + climate.sunriseEnd) * 0.5f;
        const float sunset = (climate.sunsetBegin + climate.sunsetEnd) * 0.5f;
        if (!std::isfinite(hour) || !std::isfinite(sunrise) || !std::isfinite(sunset) ||
            !(sunset > sunrise))
            return 1.0f;
        const float fade = (std::min)(kFadeHours, (sunset - sunrise) * 0.5f);
        const float morning = std::clamp((hour - sunrise) / fade, 0.0f, 1.0f);
        const float evening = std::clamp((sunset - hour) / fade, 0.0f, 1.0f);
        const float t = (std::min)(morning, evening);
        return t * t * (3.0f - 2.0f * t);
    }
}
