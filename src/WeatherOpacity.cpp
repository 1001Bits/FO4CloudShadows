// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "WeatherOpacity.h"
#include "EngineAPI.h"
#include "PluginWeatherScan.h"

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

        struct CatalogState
        {
            std::mutex mutex;
            std::shared_ptr<const std::vector<Entry>> entries;
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

        // Caller holds state.mutex.
        [[nodiscard]] float Resolve(const void* weather, float global)
        {
            auto [it, inserted] = state.resolved.try_emplace(
                weather, std::numeric_limits<float>::quiet_NaN());
            if (inserted) {
                std::uint32_t formID{};
                std::string plugin;
                if (EngineAPI::ReadFormIdentity(weather, formID, plugin)) {
                    for (const auto& key : KeysFor(formID, plugin)) {
                        if (const auto found = state.weathers.find(key); found != state.weathers.end()) {
                            it->second = found->second;
                            break;
                        }
                    }
                }
            }
            return std::isnan(it->second) ? global : it->second;
        }

        // FX lighting and interior weathers are listed by regions but never
        // cast cloud shadows.
        [[nodiscard]] bool Hidden(const std::string& editorId)
        {
            const auto name = Lower(editorId);
            return name.starts_with("fx") || name.find("interior") != std::string::npos;
        }

        [[nodiscard]] std::vector<Entry> Build(const std::vector<std::string>& plugins)
        {
            const auto started = Clock::now();
            std::array<wchar_t, 32768> executable{};
            const auto length = GetModuleFileNameW(nullptr, executable.data(),
                static_cast<DWORD>(executable.size()));
            const auto data = fs::path(std::wstring(executable.data(), length)).parent_path() / L"Data";

            struct Known
            {
                std::string editorId, plugin;
                std::size_t order{};
            };
            std::map<std::string, Known> names;
            std::set<std::string> referenced;
            std::uint32_t compressed = 0, unreadable = 0;
            for (std::size_t i = 0; i < plugins.size(); ++i) {
                std::ifstream input(data / plugins[i], std::ios::binary);
                const auto scan = input ? PluginWeatherScan::Scan(input) : std::nullopt;
                if (!scan) {
                    ++unreadable;
                    continue;
                }
                compressed += scan->compressedSkipped;
                // Masters load first, so the defining plugin names a weather.
                for (const auto& [id, editorId] : scan->weathers)
                    if (const auto key = PluginWeatherScan::KeyFor(*scan, plugins[i], id))
                        names.try_emplace(*key, Known{ editorId, plugins[i], i });
                for (const auto id : scan->references)
                    if (const auto key = PluginWeatherScan::KeyFor(*scan, plugins[i], id))
                        referenced.insert(*key);
            }

            std::vector<Entry> entries;
            std::set<std::string> listed;
            for (const auto& vanilla : McmSettingsFile::kVanillaWeathers) {
                entries.push_back({ std::string(vanilla.key), std::string(vanilla.label), "Fallout4.esm" });
                listed.insert(std::string(vanilla.key));
            }
            std::vector<std::pair<std::size_t, Entry>> others;
            for (const auto& key : referenced) {
                if (listed.contains(key))
                    continue;
                const auto known = names.find(key);
                const bool named = known != names.end();
                if (named && Hidden(known->second.editorId))
                    continue;
                const auto bar = key.find('|');
                const std::string plugin = named ? known->second.plugin
                    : bar == std::string::npos ? "Fallout4.esm" : key.substr(0, bar);
                std::string label = named && !known->second.editorId.empty() ? known->second.editorId
                    : bar == std::string::npos ? key : key.substr(bar + 1);
                if (Lower(plugin) != "fallout4.esm")
                    label += " (" + plugin + ")";
                others.push_back({ named ? known->second.order : plugins.size(), { key, label, plugin } });
            }
            std::sort(others.begin(), others.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : a.second.label < b.second.label;
            });
            for (auto& [order, entry] : others)
                entries.push_back(std::move(entry));
            SPDLOG_INFO("[CloudShadows][Weather] Listed {} weathers from {} plugins in {} ms "
                "({} compressed records skipped, {} plugins unreadable)",
                entries.size(), plugins.size(),
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count(),
                compressed, unreadable);
            return entries;
        }

        [[nodiscard]] std::shared_ptr<const std::vector<Entry>> ReadyCatalog() noexcept
        {
            std::lock_guard lock(catalog.mutex);
            return catalog.entries;
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
            if (catalog.entries || catalog.started)
                return catalog.entries;
            auto plugins = EngineAPI::ReadLoadedPluginNames();
            if (plugins.empty())
                return nullptr;  // game data not loaded yet; asked again later
            catalog.started = true;
            std::thread([plugins = std::move(plugins)] {
                std::shared_ptr<const std::vector<Entry>> built;
                try {
                    built = std::make_shared<const std::vector<Entry>>(Build(plugins));
                } catch (const std::exception& error) {
                    SPDLOG_ERROR("[CloudShadows][Weather] Could not list weathers: {}", error.what());
                    built = std::make_shared<const std::vector<Entry>>();
                }
                std::lock_guard done(catalog.mutex);
                catalog.entries = std::move(built);
            }).detach();
        } catch (...) {
        }
        return nullptr;
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
            if (const auto list = ReadyCatalog()) {
                for (const auto& listed : *list) {
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
