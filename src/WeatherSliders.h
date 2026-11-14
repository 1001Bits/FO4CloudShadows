// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "McmSettingsFile.h"
#include "PluginWeatherScan.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The 12 MCM weather sliders also cover weathers from DLC and weather mods. A
// weather without an opacity of its own follows the slider its editor ID names
// (Fog, Mist, Rain, Rad...), else the slider for its classification: pleasant
// Clear, cloudy Overcast, rainy Rain, snow Radstorm (the game flags its
// radstorms as snow). A slider is shown only when a weather the winning
// climates and regions list follows it.
namespace FO4CS::WeatherSliders
{
    inline constexpr std::string_view kClear = "02B52A";
    inline constexpr std::string_view kClearestSkies = "1D670E";
    inline constexpr std::string_view kOvercast = "1C8556";
    inline constexpr std::string_view kFoggy = "1C3473";
    inline constexpr std::string_view kMisty = "1CC186";
    inline constexpr std::string_view kMistyRain = "1CD096";
    inline constexpr std::string_view kRain = "1CA7E4";
    inline constexpr std::string_view kRadstorm = "1C3D5E";
    inline constexpr std::string_view kGlowingSeaOvercast = "0F1033";
    inline constexpr std::string_view kMarshOvercast = "10F781";
    inline constexpr std::string_view kGoodneighbor = "115C64";
    inline constexpr std::string_view kSanctuary = "12A18E";

    inline constexpr std::uint8_t kPleasant = 1, kCloudy = 2, kRainy = 4, kSnow = 8;

    [[nodiscard]] inline std::optional<std::string_view> VanillaSlider(std::string_view key) noexcept
    {
        for (const auto& weather : McmSettingsFile::kVanillaWeathers)
            if (weather.key == key)
                return weather.key;
        return std::nullopt;
    }

    // Lower-case words of an editor ID: "DLC03_RadMistWeather" gives dlc, 03,
    // rad, mist, weather; "CommonwealthGSRadstorm" gives commonwealth, gs, radstorm.
    [[nodiscard]] inline std::vector<std::string> Words(std::string_view name)
    {
        const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
        const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
        const auto digit = [](char c) { return c >= '0' && c <= '9'; };
        std::vector<std::string> words;
        std::string word;
        const auto flush = [&] {
            if (!word.empty())
                words.push_back(std::move(word));
            word.clear();
        };
        for (std::size_t i = 0; i < name.size(); ++i) {
            const char c = name[i];
            if (!upper(c) && !lower(c) && !digit(c)) {
                flush();
                continue;
            }
            if (!word.empty()) {
                const char previous = name[i - 1];
                const bool nextLower = i + 1 < name.size() && lower(name[i + 1]);
                if (digit(c) != digit(previous) ||                       // 03 | Rad
                    (upper(c) && lower(previous)) ||                     // Rad | Mist
                    (upper(c) && upper(previous) && nextLower))          // GS | Radstorm
                    flush();
            }
            word.push_back(upper(c) ? static_cast<char>(c - 'A' + 'a') : c);
        }
        flush();
        return words;
    }

    // FX lighting and interior weathers are listed by regions but never cast
    // cloud shadows.
    [[nodiscard]] inline bool LightingOnly(std::string_view editorId)
    {
        std::string name(editorId);
        for (auto& c : name)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return name.starts_with("fx") || name.find("interior") != std::string::npos;
    }

    // The slider a weather follows when it has no opacity of its own.
    [[nodiscard]] inline std::optional<std::string_view> SliderFor(std::string_view key,
        std::string_view editorId, std::uint8_t kind)
    {
        if (const auto vanilla = VanillaSlider(key))
            return vanilla;
        const auto words = Words(editorId);
        const auto has = [&](std::initializer_list<std::string_view> prefixes) {
            for (const auto& word : words)
                for (const auto prefix : prefixes)
                    if (word.starts_with(prefix))
                        return true;
            return false;
        };
        const auto is = [&](std::string_view exact) {
            for (const auto& word : words)
                if (word == exact)
                    return true;
            return false;
        };
        bool radstorm = false;
        for (const auto& word : words)
            if (word.starts_with("rad") && !word.starts_with("radio") && !word.starts_with("radiant") &&
                !word.starts_with("radius") && !word.starts_with("radial"))
                radstorm = true;
        if (radstorm) return kRadstorm;
        if (is("gs") || has({ "glowing" })) return kGlowingSeaOvercast;
        if (has({ "marsh", "swamp", "bog" })) return kMarshOvercast;
        if (has({ "goodneighbo" })) return kGoodneighbor;
        if (has({ "sanctuary", "prewar" })) return kSanctuary;
        if (has({ "mist" }) && ((kind & kRainy) || has({ "rain" }))) return kMistyRain;
        if (has({ "mist" })) return kMisty;
        if (has({ "fog" })) return kFoggy;
        if (has({ "rain", "storm", "drizzle", "shower", "thunder" })) return kRain;
        if (has({ "clearest" })) return kClearestSkies;
        if (has({ "clear", "sunny", "fair" })) return kClear;
        if (has({ "overcast", "cloud", "dust", "gray", "grey" })) return kOvercast;
        if (kind & kRainy) return kRain;
        if (kind & kSnow) return kRadstorm;
        if (kind & kCloudy) return kOvercast;
        if (kind & kPleasant) return kClear;
        return std::nullopt;
    }

    struct PluginScan
    {
        std::string plugin;
        PluginWeatherScan::Result scan;
    };

    struct Plan
    {
        struct Known
        {
            std::string editorId, plugin;  // the defining plugin names a weather
            std::size_t order{};
            std::uint8_t kind{};           // from the winning override
        };
        std::map<std::string, Known> weathers;        // every weather record
        std::set<std::string> used;                   // listed by a winning climate or region
        std::map<std::string, std::string> sliderOf;  // weather key -> slider it follows
        std::set<std::string> shown;                  // sliders followed by a used weather
        bool listsFound{};                            // false: nothing to hide sliders by
    };

    // `scans` in load order, so later plugins' records override earlier ones.
    [[nodiscard]] inline Plan BuildPlan(const std::vector<PluginScan>& scans)
    {
        Plan plan;
        std::map<std::pair<bool, std::string>, std::vector<std::string>> lists;
        for (std::size_t i = 0; i < scans.size(); ++i) {
            const auto& [plugin, scan] = scans[i];
            for (const auto& weather : scan.weathers) {
                const auto key = PluginWeatherScan::KeyFor(scan, plugin, weather.id);
                if (!key)
                    continue;
                auto [it, inserted] = plan.weathers.try_emplace(*key,
                    Plan::Known{ weather.editorId, plugin, i, weather.kind });
                if (!inserted) {
                    it->second.kind = weather.kind;
                    if (!weather.editorId.empty())
                        it->second.editorId = weather.editorId;
                }
            }
            for (const auto& list : scan.lists) {
                const auto key = PluginWeatherScan::KeyFor(scan, plugin, list.id);
                if (!key)
                    continue;
                auto& listed = lists[{ list.region, *key }];
                listed.clear();
                for (const auto id : list.weathers)
                    if (const auto weather = PluginWeatherScan::KeyFor(scan, plugin, id))
                        listed.push_back(*weather);
            }
        }
        for (const auto& [record, listed] : lists) {
            for (const auto& weather : listed) {
                plan.listsFound = true;
                const auto known = plan.weathers.find(weather);
                if (known == plan.weathers.end() || !LightingOnly(known->second.editorId))
                    plan.used.insert(weather);
            }
        }
        for (const auto& [key, known] : plan.weathers)
            if (const auto slider = SliderFor(key, known.editorId, known.kind))
                plan.sliderOf.emplace(key, std::string(*slider));
        for (const auto& weather : plan.used) {
            const auto known = plan.weathers.find(weather);
            const auto slider = known != plan.weathers.end()
                ? SliderFor(weather, known->second.editorId, known->second.kind)
                : VanillaSlider(weather);
            if (slider)
                plan.shown.emplace(*slider);
        }
        return plan;
    }

    // Whether each of the 12 sliders is shown. With no climate or region lists
    // read at all, every slider stays visible.
    [[nodiscard]] inline std::map<std::string, bool> Visibility(const Plan& plan)
    {
        std::map<std::string, bool> visible;
        for (const auto& weather : McmSettingsFile::kVanillaWeathers)
            visible[std::string(weather.key)] = !plan.listsFound || plan.shown.contains(std::string(weather.key));
        return visible;
    }
}
