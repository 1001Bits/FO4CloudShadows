// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "WeatherOpacity.h"
#include "EngineAPI.h"
#include "McmSettings.h"
#include "PluginWeatherScan.h"
#include "WeatherSliders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace FO4CS::WeatherOpacity
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct State
        {
            std::mutex mutex;
            bool enabled{};
            McmSettingsFile::WeatherOpacities weathers;
            // Per weather form: its pairing, NaN when it has none.
            std::unordered_map<const void*, float> resolved;
            bool hasValue{};
            float value{};
            float lastGlobal{};
            Clock::time_point last{};
        };

        struct CatalogData
        {
            std::vector<Entry> entries;
            // Weather key -> the vanilla slider it follows without its own pairing.
            std::map<std::string, std::string> sliderOf;
        };

        struct CatalogState
        {
            std::mutex mutex;
            std::shared_ptr<const CatalogData> data;
            bool started{};
        };

        // Never destroyed: render threads and the catalog thread may still
        // run while the process exits.
        State& state = *new State();
        CatalogState& catalog = *new CatalogState();

        [[nodiscard]] std::string Lower(std::string text)
        {
            for (auto& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return text;
        }

        // Settings keys that can name this weather: plugin plus local ID
        // first, then the runtime form ID.
        [[nodiscard]] std::vector<std::string> KeysFor(std::uint32_t formID, const std::string& plugin)
        {
            std::vector<std::string> keys;
            if (!plugin.empty() && Lower(plugin) != "fallout4.esm") {
                const std::uint32_t local = (formID >> 24) == 0xFE ? formID & 0xFFFu : formID & 0xFFFFFFu;
                keys.push_back(McmSettingsFile::FormatWeatherKey(Lower(plugin), local));
            }
            keys.push_back(McmSettingsFile::FormatWeatherKey({}, formID));
            return keys;
        }

        [[nodiscard]] std::shared_ptr<const CatalogData> ReadyCatalog() noexcept
        {
            std::lock_guard lock(catalog.mutex);
            return catalog.data;
        }

        // Caller holds state.mutex (then the catalog's; never the reverse). A
        // weather's own pairing, else the pairing of the slider it follows,
        // else NaN: Shadow opacity.
        [[nodiscard]] float Resolve(const void* weather, float global)
        {
            auto [it, inserted] = state.resolved.try_emplace(
                weather, std::numeric_limits<float>::quiet_NaN());
            if (inserted) {
                std::uint32_t formID{};
                std::string plugin;
                if (EngineAPI::ReadFormIdentity(weather, formID, plugin)) {
                    const auto keys = KeysFor(formID, plugin);
                    for (const auto& key : keys) {
                        if (const auto found = state.weathers.find(key); found != state.weathers.end()) {
                            it->second = found->second;
                            break;
                        }
                    }
                    if (std::isnan(it->second)) {
                        if (const auto data = ReadyCatalog()) {
                            const auto slider = data->sliderOf.find(keys.front());
                            if (slider != data->sliderOf.end()) {
                                const auto found = state.weathers.find(slider->second);
                                if (found != state.weathers.end())
                                    it->second = found->second;
                            }
                        }
                    }
                }
            }
            return std::isnan(it->second) ? global : it->second;
        }

        [[nodiscard]] CatalogData Build(const std::vector<std::string>& plugins,
            std::map<std::string, bool>& visible)
        {
            const auto started = Clock::now();
            std::array<wchar_t, 32768> executable{};
            const auto length = GetModuleFileNameW(nullptr, executable.data(),
                static_cast<DWORD>(executable.size()));
            const auto dataDir = fs::path(std::wstring(executable.data(), length)).parent_path() / L"Data";

            std::vector<WeatherSliders::PluginScan> scans;
            scans.reserve(plugins.size());
            std::uint32_t compressed = 0, unreadable = 0;
            for (const auto& name : plugins) {
                std::ifstream input(dataDir / name, std::ios::binary);
                auto scan = input ? PluginWeatherScan::Scan(input) : std::nullopt;
                if (!scan) {
                    ++unreadable;
                    continue;
                }
                compressed += scan->compressedSkipped;
                scans.push_back({ name, std::move(*scan) });
            }
            const auto plan = WeatherSliders::BuildPlan(scans);
            visible = WeatherSliders::Visibility(plan);

            CatalogData result;
            result.sliderOf = plan.sliderOf;
            std::set<std::string> vanilla;
            std::string shown, hidden;
            for (const auto& weather : McmSettingsFile::kVanillaWeathers) {
                const std::string key(weather.key);
                vanilla.insert(key);
                auto& names = visible.at(key) ? shown : hidden;
                names += (names.empty() ? "" : ", ") + std::string(weather.label);
                if (visible.at(key))
                    result.entries.push_back({ key, std::string(weather.label), "Fallout4.esm" });
            }
            std::vector<std::pair<std::size_t, Entry>> others;
            for (const auto& key : plan.used) {
                if (vanilla.contains(key))
                    continue;
                const auto known = plan.weathers.find(key);
                const bool named = known != plan.weathers.end();
                const auto bar = key.find('|');
                const std::string plugin = named ? known->second.plugin
                    : bar == std::string::npos ? "Fallout4.esm" : key.substr(0, bar);
                std::string label = named && !known->second.editorId.empty() ? known->second.editorId
                    : bar == std::string::npos ? key : key.substr(bar + 1);
                if (Lower(plugin) != "fallout4.esm")
                    label += " (" + plugin + ")";
                others.push_back({ named ? known->second.order : scans.size(), { key, label, plugin } });
            }
            std::sort(others.begin(), others.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : a.second.label < b.second.label;
            });
            for (auto& [order, entry] : others)
                result.entries.push_back(std::move(entry));
            SPDLOG_INFO("[CloudShadows][Weather] Listed {} weathers from {} plugins in {} ms "
                "({} compressed records skipped, {} plugins unreadable)",
                result.entries.size(), plugins.size(),
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count(),
                compressed, unreadable);
            SPDLOG_INFO("[CloudShadows][Weather] {} weathers follow a slider; sliders shown: {}; hidden: {}",
                plan.sliderOf.size(), shown.empty() ? "none" : shown, hidden.empty() ? "none" : hidden);
            return result;
        }
    }

    void Configure(bool enabled, const McmSettingsFile::WeatherOpacities& weathers) noexcept
    {
        try {
            std::lock_guard lock(state.mutex);
            state.enabled = enabled;
            state.weathers = weathers;
            state.resolved.clear();
            state.hasValue = false;
        } catch (...) {
        }
    }

    float Effective(float global) noexcept
    {
        try {
            std::lock_guard lock(state.mutex);
            if (!state.enabled) {
                state.hasValue = false;
                return global;
            }
            const auto now = Clock::now();
            EngineAPI::WeatherState weather;
            const bool known = EngineAPI::ReadWeatherState(EngineAPI::GetSky(), weather);
            float target = global;
            if (known) {
                target = Resolve(weather.current, global);
                if (weather.previous && weather.previous != weather.current)
                    target = std::lerp(Resolve(weather.previous, global), target,
                        TransitionWeight(weather.fraction));
            }
            // A settings change or the first weather after a load applies at
            // once; only changes of weather fade.
            const bool snap = !state.hasValue || !known || global != state.lastGlobal;
            state.value = snap ? target : Fade(state.value, target,
                std::chrono::duration<float>(now - state.last).count());
            state.hasValue = known;
            state.lastGlobal = global;
            state.last = now;
            return state.value;
        } catch (...) {
            return global;
        }
    }

    std::shared_ptr<const std::vector<Entry>> Catalog() noexcept
    {
        try {
            std::lock_guard lock(catalog.mutex);
            if (catalog.data)
                return std::shared_ptr<const std::vector<Entry>>(catalog.data, &catalog.data->entries);
            if (catalog.started)
                return nullptr;
            auto plugins = EngineAPI::ReadLoadedPluginNames();
            if (plugins.empty())
                return nullptr;  // game data not loaded yet; asked again later
            catalog.started = true;
            std::thread([plugins = std::move(plugins)] {
                std::shared_ptr<const CatalogData> built;
                std::map<std::string, bool> visible;
                try {
                    built = std::make_shared<const CatalogData>(Build(plugins, visible));
                } catch (const std::exception& error) {
                    SPDLOG_ERROR("[CloudShadows][Weather] Could not list weathers: {}", error.what());
                    built = std::make_shared<const CatalogData>();
                    visible.clear();
                }
                {
                    std::lock_guard done(catalog.mutex);
                    catalog.data = std::move(built);
                }
                {
                    // Weathers resolved before the catalog existed may follow a slider now.
                    std::lock_guard resolved(state.mutex);
                    state.resolved.clear();
                }
                if (!visible.empty())
                    McmSettings::PublishSliderVisibility(visible);
            }).detach();
        } catch (...) {
        }
        return nullptr;
    }

    std::string SliderFor(const std::string& key) noexcept
    {
        try {
            const auto data = ReadyCatalog();
            if (!data)
                return {};
            const auto found = data->sliderOf.find(key);
            return found != data->sliderOf.end() ? found->second : std::string{};
        } catch (...) {
            return {};
        }
    }

    Entry CurrentWeather() noexcept
    {
        try {
            EngineAPI::WeatherState weather;
            std::uint32_t formID{};
            std::string plugin;
            if (!EngineAPI::ReadWeatherState(EngineAPI::GetSky(), weather) ||
                !EngineAPI::ReadFormIdentity(weather.current, formID, plugin))
                return {};
            Entry entry{ KeysFor(formID, plugin).front(), {}, plugin };
            if (const auto data = ReadyCatalog()) {
                for (const auto& listed : data->entries) {
                    if (listed.key == entry.key) {
                        entry.label = listed.label;
                        break;
                    }
                }
            }
            if (entry.label.empty()) {
                std::array<char, 16> hex{};
                std::snprintf(hex.data(), hex.size(), "%08X", formID);
                entry.label = (plugin.empty() ? std::string("Weather") : plugin) + " " + hex.data();
            }
            return entry;
        } catch (...) {
            return {};
        }
    }
}
