// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "McmSettingsFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace FO4CS::WeatherOpacity
{
    // From the applied settings (render thread). Takes effect immediately,
    // without the weather fade, so menu edits show at once.
    void Configure(bool enabled, const McmSettingsFile::WeatherOpacities& weathers) noexcept;

    // Shadow opacity for this frame. `global` unless weather-specific opacity
    // is on; then the current weather's pairing (or `global`), blended across
    // the game's weather transition and faded slowly, so a change of weather
    // is not noticeable as a step.
    [[nodiscard]] float Effective(float global) noexcept;

    // The blend used by Effective. The game's own transition fraction is eased
    // with a smoothstep, so the change starts and ends imperceptibly; on top,
    // the value approaches its target exponentially (63% after kFadeSeconds),
    // which also softens instant weather switches such as a forced weather.
    inline constexpr float kFadeSeconds = 10.0f;

    [[nodiscard]] inline float TransitionWeight(float fraction) noexcept
    {
        const float t = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : 1.0f;
        return t * t * (3.0f - 2.0f * t);
    }

    [[nodiscard]] inline float Fade(float value, float target, float seconds) noexcept
    {
        if (!(seconds > 0.0f) || !std::isfinite(seconds))
            return value;
        return value + (target - value) * (1.0f - std::exp(-seconds / kFadeSeconds));
    }

    struct Entry
    {
        std::string key;      // settings key (McmSettingsFile::WeatherOpacities)
        std::string label;    // friendly name, editor ID or plugin + form ID
        std::string plugin;   // defining plugin file name
    };

    // Weathers used by the loaded climates and regions, the vanilla outdoor
    // weathers whose MCM slider is in use first. Built once on a background
    // thread from the plugin files; null until ready. Call after the game data
    // has loaded. Building it also publishes which MCM sliders are shown.
    [[nodiscard]] std::shared_ptr<const std::vector<Entry>> Catalog() noexcept;

    // The vanilla slider key a weather follows when it has no opacity of its
    // own (see WeatherSliders.h), or empty before the catalog is built.
    [[nodiscard]] std::string SliderFor(const std::string& key) noexcept;

    // The sky's current weather, or an empty key when unavailable.
    [[nodiscard]] Entry CurrentWeather() noexcept;
}
