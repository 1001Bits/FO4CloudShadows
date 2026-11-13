// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cwchar>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace FO4CS::McmSettingsFile
{
    // Per-weather Shadow opacity (same 0-4 scale), keyed canonically:
    //   "1CA7E4"               runtime form ID: Fallout4.esm weathers, or any
    //                          weather by its full load-order form ID
    //   "dlccoast.esm|00ABCD"  plugin plus local form ID: DLC and mod weathers,
    //                          stable across load-order changes
    // A weather without a pairing uses Shadow opacity.
    using WeatherOpacities = std::map<std::string, float>;

    inline constexpr std::size_t kMaxWeatherEntries = 1024;

    // The outdoor weathers of Fallout4.esm's climates and regions, listed in
    // MCM (which cannot enumerate forms) and first in the Menu Framework page.
    struct VanillaWeather
    {
        std::string_view key, editorId, label;
    };
    inline constexpr std::array<VanillaWeather, 12> kVanillaWeathers{ {
        { "02B52A", "CommonwealthClear", "Clear" },
        { "1D670E", "CommonwealthClearestSkies", "Clearest skies" },
        { "1C8556", "CommonwealthOvercast", "Overcast" },
        { "1C3473", "CommonwealthFoggy", "Foggy" },
        { "1CC186", "CommonwealthMisty", "Misty" },
        { "1CD096", "CommonwealthMistyRainy", "Misty rain" },
        { "1CA7E4", "CommonwealthRain", "Rain" },
        { "1C3D5E", "CommonwealthGSRadstorm", "Radstorm" },
        { "0F1033", "CommonwealthGSOvercast", "Glowing Sea overcast" },
        { "10F781", "TCommonwealthMarshOvercast", "Marsh overcast" },
        { "115C64", "GoodneighborWeatherBase", "Goodneighbor" },
        { "12A18E", "CommonwealthSanctuaryClear", "Sanctuary (pre-war)" },
    } };

    [[nodiscard]] inline std::string FormatWeatherKey(std::string_view plugin, std::uint32_t id)
    {
        std::array<char, 16> hex{};
        std::snprintf(hex.data(), hex.size(), plugin.empty() && id > 0xFFFFFFu ? "%08X" : "%06X", id);
        return plugin.empty() ? std::string(hex.data()) : std::string(plugin) + "|" + hex.data();
    }

    // "1ca7e4", "Fallout4.esm|1CA7E4" or "DLCCoast.esm|abcd" to canonical form.
    [[nodiscard]] inline std::optional<std::string> CanonicalWeatherKey(std::string_view text)
    {
        const auto trim = [](std::string_view value) {
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
            return value;
        };
        text = trim(text);
        std::string plugin;
        std::string_view hex = text;
        if (const auto bar = text.rfind('|'); bar != std::string_view::npos) {
            const auto name = trim(text.substr(0, bar));
            if (name.empty() || name.size() > 260) return std::nullopt;
            for (const char c : name) {
                if (static_cast<unsigned char>(c) < 0x20 || c == '|' || c == '=' || c == '[' || c == ']')
                    return std::nullopt;
                plugin.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            }
            hex = trim(text.substr(bar + 1));
        }
        if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex.remove_prefix(2);
        std::uint32_t id = 0;
        if (hex.empty() || hex.size() > 8) return std::nullopt;
        const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != hex.data() + hex.size()) return std::nullopt;
        // Fallout4.esm always loads first, so its local IDs are runtime IDs.
        if (plugin == "fallout4.esm") plugin.clear();
        if (!plugin.empty() && id > 0xFFFFFFu) return std::nullopt;
        return FormatWeatherKey(plugin, id);
    }

    // One [Weather] line: "f<key>=<value>" (MCM's typed float setting) or the
    // hand-written "<key>|<value>". Comments, blanks and malformed lines: none.
    [[nodiscard]] inline std::optional<std::pair<std::string, float>> ParseWeatherLine(std::string_view line)
    {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.front() == ';' || line.front() == '#') return std::nullopt;
        std::string_view key, value;
        if (const auto equals = line.find('='); equals != std::string_view::npos) {
            key = line.substr(0, equals);
            if (key.empty() || (key.front() != 'f' && key.front() != 'F')) return std::nullopt;
            key.remove_prefix(1);
            value = line.substr(equals + 1);
        } else if (const auto bar = line.rfind('|'); bar != std::string_view::npos) {
            key = line.substr(0, bar);
            value = line.substr(bar + 1);
        } else {
            return std::nullopt;
        }
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
        float number{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
            !std::isfinite(number))
            return std::nullopt;
        auto canonical = CanonicalWeatherKey(key);
        if (!canonical) return std::nullopt;
        return std::pair{ std::move(*canonical), number };
    }

    struct Values
    {
        bool enabled = true;
        float opacity = 1.0f;
        float cloudHeight = 10000.0f;
        // F10/F11 and FPS testing are off by default for normal players.
        bool hotkeys = false;
        // Weather-specific opacity is off by default; pairings are kept while off.
        bool weatherOpacity = false;
        WeatherOpacities weathers;
        // Clouds also shade water on desktop.
        bool waterShadows = true;
        // Clouds also shade the moonlight. Off fades shadows out at sunset.
        bool nightShadows = true;
        // MCM's Current weather slider, shown for whichever weather is active.
        // A change the plugin did not write itself becomes that weather's pairing.
        float currentWeather = 1.0f;
        bool operator==(const Values&) const = default;
    };

    struct Changes
    {
        bool enabled{}, opacity{}, cloudHeight{}, hotkeys{}, weatherOpacity{}, weathers{}, waterShadows{}, nightShadows{};
        bool Any() const noexcept
        {
            return enabled || opacity || cloudHeight || hotkeys || weatherOpacity || weathers ||
                waterShadows || nightShadows;
        }
    };

    // Compare saved preferences, not the live F10/F11 values. An unchanged
    // file must never undo a temporary toggle or an unsaved slider edit.
    inline Changes Difference(const std::optional<Values>& previous,
        const Values& next, bool force) noexcept
    {
        return { force || !previous || previous->enabled != next.enabled,
            force || !previous || previous->opacity != next.opacity,
            force || !previous || previous->cloudHeight != next.cloudHeight,
            force || !previous || previous->hotkeys != next.hotkeys,
            force || !previous || previous->weatherOpacity != next.weatherOpacity,
            force || !previous || previous->weathers != next.weathers,
            force || !previous || previous->waterShadows != next.waterShadows,
            force || !previous || previous->nightShadows != next.nightShadows };
    }

    // F11 saves what the player changed in game since the file was last
    // applied (`baseline`), and keeps every other field as currently saved,
    // so an MCM or Menu Framework edit not yet polled is never overwritten.
    inline Values Merge(const Values& file, const Values& live,
        const std::optional<Values>& baseline) noexcept
    {
        if (!baseline)
            return live;
        Values merged = file;
        if (live.enabled != baseline->enabled) merged.enabled = live.enabled;
        if (live.opacity != baseline->opacity) merged.opacity = live.opacity;
        if (live.cloudHeight != baseline->cloudHeight) merged.cloudHeight = live.cloudHeight;
        if (live.hotkeys != baseline->hotkeys) merged.hotkeys = live.hotkeys;
        if (live.weatherOpacity != baseline->weatherOpacity) merged.weatherOpacity = live.weatherOpacity;
        if (live.waterShadows != baseline->waterShadows) merged.waterShadows = live.waterShadows;
        if (live.nightShadows != baseline->nightShadows) merged.nightShadows = live.nightShadows;
        if (live.currentWeather != baseline->currentWeather) merged.currentWeather = live.currentWeather;
        // Per weather, so one pairing edited in game keeps the others saved
        // meanwhile through MCM.
        if (live.weathers != baseline->weathers) {
            std::vector<std::string> keys;
            for (const auto& [key, value] : live.weathers) keys.push_back(key);
            for (const auto& [key, value] : baseline->weathers) keys.push_back(key);
            for (const auto& key : keys) {
                const auto now = live.weathers.find(key);
                const auto before = baseline->weathers.find(key);
                const bool present = now != live.weathers.end();
                if (present == (before != baseline->weathers.end()) &&
                    (!present || now->second == before->second))
                    continue;
                if (present) merged.weathers[key] = now->second;
                else merged.weathers.erase(key);
            }
        }
        return merged;
    }

    // Values from a version-4 JSON worth carrying into a new user INI. The
    // release package ships that JSON with default values: migrating it would
    // pin every default into the INI, so future default changes never apply.
    inline std::optional<Values> LegacyMigration(const Values& legacy) noexcept
    {
        Values migrated = legacy;
        migrated.hotkeys = Values{}.hotkeys;
        if (migrated == Values{})
            return std::nullopt;
        return migrated;
    }

    // The opacity a weather uses: its pairing, or Shadow opacity.
    [[nodiscard]] inline float OpacityFor(const Values& values, const std::string& key) noexcept
    {
        const auto found = values.weathers.find(key);
        return found != values.weathers.end() ? found->second : values.opacity;
    }

    // Slider values pass through MCM's own float formatting.
    [[nodiscard]] inline bool SliderMoved(float shown, float value) noexcept
    {
        return std::abs(shown - value) > 0.001f;
    }

    inline Values Validate(Values values) noexcept
    {
        values.currentWeather = std::isfinite(values.currentWeather)
            ? std::clamp(values.currentWeather, 0.0f, 4.0f) : Values{}.currentWeather;
        values.opacity = std::isfinite(values.opacity)
            ? std::clamp(values.opacity, 0.0f, 4.0f) : Values{}.opacity;
        values.cloudHeight = std::isfinite(values.cloudHeight)
            ? std::clamp(values.cloudHeight, 10000.0f, 200000.0f) : Values{}.cloudHeight;
        for (auto it = values.weathers.begin(); it != values.weathers.end();) {
            if (!std::isfinite(it->second) || values.weathers.size() > kMaxWeatherEntries) {
                it = values.weathers.erase(it);
                continue;
            }
            it->second = std::clamp(it->second, 0.0f, 4.0f);
            ++it;
        }
        return values;
    }

    // Raw [Weather] lines of one INI, as Win32 returns them.
    [[nodiscard]] inline std::vector<std::string> ReadWeatherLines(const std::filesystem::path& path)
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD length = GetPrivateProfileSectionW(L"Weather", buffer.data(),
            static_cast<DWORD>(buffer.size()), path.c_str());
        if (length >= buffer.size() - 2) throw std::runtime_error("MCM [Weather] section is too large");
        std::vector<std::string> lines;
        for (const wchar_t* line = buffer.data(); *line; line += std::wcslen(line) + 1) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
            std::string text(size > 0 ? static_cast<std::size_t>(size - 1) : 0, '\0');
            if (size > 1)
                WideCharToMultiByte(CP_UTF8, 0, line, -1, text.data(), size, nullptr, nullptr);
            lines.push_back(std::move(text));
        }
        return lines;
    }

    // Later lines win, as when the same key is written twice.
    [[nodiscard]] inline WeatherOpacities ParseWeathers(const std::vector<std::string>& lines)
    {
        WeatherOpacities weathers;
        for (const auto& line : lines)
            if (auto entry = ParseWeatherLine(line))
                weathers[entry->first] = entry->second;
        return weathers;
    }

    // Only the user layer contributes [Weather] pairings: the shipped defaults
    // give MCM's sliders a displayed value, which must not pin a weather.
    inline void ReadLayer(const std::filesystem::path& path, Values& values, bool readWeathers = false)
    {
        // Distinguish a missing override from an unreadable file. Otherwise a
        // sharing/access failure could silently reset the player's settings.
        const HANDLE probe = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
            throw std::system_error(static_cast<int>(error), std::system_category(), "read MCM settings");
        }
        LARGE_INTEGER size{};
        const bool bounded = GetFileSizeEx(probe, &size) && size.QuadPart > 0 && size.QuadPart <= 65536;
        CloseHandle(probe);
        if (!bounded) throw std::runtime_error("MCM settings file is empty or too large");

        const auto read = [&](const wchar_t* key) -> std::optional<float> {
            std::array<wchar_t, 128> text{};
            const auto length = GetPrivateProfileStringW(L"CloudShadows", key,
                L"__missing__", text.data(), static_cast<DWORD>(text.size()), path.c_str());
            if (std::wstring_view(text.data(), length) == L"__missing__") return std::nullopt;
            if (!length || length >= text.size() - 1) throw std::runtime_error("invalid MCM setting length");
            std::string number;
            for (DWORD i = 0; i < length; ++i) {
                if (text[i] > 127) throw std::runtime_error("invalid MCM numeric setting");
                number.push_back(static_cast<char>(text[i]));
            }
            float value{};
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || !std::isfinite(value))
                throw std::runtime_error("invalid MCM numeric setting");
            return value;
        };
        if (const auto enabled = read(L"bEnabled")) {
            if (*enabled != 0.0f && *enabled != 1.0f) throw std::runtime_error("MCM enabled must be 0 or 1");
            values.enabled = *enabled == 1.0f;
        }
        if (const auto opacity = read(L"fOpacity")) values.opacity = *opacity;
        if (const auto height = read(L"fCloudHeight")) values.cloudHeight = *height;
        if (const auto hotkeys = read(L"bHotkeys")) {
            if (*hotkeys != 0.0f && *hotkeys != 1.0f) throw std::runtime_error("MCM hotkeys must be 0 or 1");
            values.hotkeys = *hotkeys == 1.0f;
        }
        if (const auto weatherOpacity = read(L"bWeatherOpacity")) {
            if (*weatherOpacity != 0.0f && *weatherOpacity != 1.0f)
                throw std::runtime_error("MCM weather opacity switch must be 0 or 1");
            values.weatherOpacity = *weatherOpacity == 1.0f;
        }
        if (const auto waterShadows = read(L"bWaterShadows")) {
            if (*waterShadows != 0.0f && *waterShadows != 1.0f)
                throw std::runtime_error("MCM water shadows switch must be 0 or 1");
            values.waterShadows = *waterShadows == 1.0f;
        }
        if (const auto slider = read(L"fCurrentWeather")) values.currentWeather = *slider;
        if (const auto nightShadows = read(L"bNightShadows")) {
            if (*nightShadows != 0.0f && *nightShadows != 1.0f)
                throw std::runtime_error("MCM night shadows switch must be 0 or 1");
            values.nightShadows = *nightShadows == 1.0f;
        }
        // A malformed hand-written weather line is skipped, not fatal: it must
        // not discard the player's other settings.
        if (readWeathers)
            values.weathers = ParseWeathers(ReadWeatherLines(path));
        // Removed MCM diagnostic keys are intentionally ignored. An old saved
        // debug view must not activate an option the player can no longer see.
    }

    inline Values Read(const std::filesystem::path& defaults, const std::filesystem::path& user)
    {
        Values values;
        ReadLayer(defaults, values);
        ReadLayer(user, values, true);
        return Validate(values);
    }

    // `includeWeather` false leaves bWeatherOpacity, bWaterShadows, bNightShadows
    // and [Weather] as saved: the F11 menu does not edit them. fCurrentWeather is
    // written only on request: any other writer could hold a stale value, which
    // would read back as the player moving the slider.
    inline void Write(const std::filesystem::path& path, Values values, bool overwrite = true,
        bool includeWeather = true, bool includeSlider = false)
    {
        values = Validate(values);
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L".tmp";
        try {
            // Preserve other sections and future keys. Win32's INI API also
            // preserves ANSI/UTF-16 encoding used by MCM and mod managers.
            if (std::filesystem::exists(path))
                std::filesystem::copy_file(path, temporary);
            const auto write = [&](const wchar_t* key, const wchar_t* value) {
                if (!WritePrivateProfileStringW(L"CloudShadows", key, value, temporary.c_str()))
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "write MCM setting");
            };
            write(L"bEnabled", values.enabled ? L"1" : L"0");
            const auto decimal = [](float value) {
                std::array<char, 64> buffer{};
                const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                if (converted.ec != std::errc{}) throw std::runtime_error("format MCM setting");
                return std::wstring(buffer.data(), converted.ptr);
            };
            write(L"fOpacity", decimal(values.opacity).c_str());
            write(L"fCloudHeight", decimal(values.cloudHeight).c_str());
            write(L"bHotkeys", values.hotkeys ? L"1" : L"0");
            if (includeSlider)
                write(L"fCurrentWeather", decimal(values.currentWeather).c_str());
            if (includeWeather) {
                write(L"bWeatherOpacity", values.weatherOpacity ? L"1" : L"0");
                write(L"bWaterShadows", values.waterShadows ? L"1" : L"0");
                write(L"bNightShadows", values.nightShadows ? L"1" : L"0");
            }
            // Rewrite [Weather] only when its pairings change, so hand-written
            // lines stay as written until the player edits a weather in game.
            Values onFile;
            const auto lines = includeWeather ? ReadWeatherLines(temporary) : std::vector<std::string>{};
            onFile.weathers = ParseWeathers(lines);
            if (includeWeather && Validate(std::move(onFile)).weathers != values.weathers) {
                std::wstring block;
                const auto append = [&block](std::string_view utf8) {
                    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                        static_cast<int>(utf8.size()), nullptr, 0);
                    std::wstring wide(static_cast<std::size_t>((std::max)(size, 0)), L'\0');
                    if (size > 0)
                        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
                    block += wide;
                    block.push_back(L'\0');
                };
                // Keep comments and anything else that is not a pairing.
                for (const auto& line : lines)
                    if (!ParseWeatherLine(line)) append(line);
                for (const auto& [key, value] : values.weathers) {
                    std::string line = "f" + key + "=";
                    for (const wchar_t digit : decimal(value))
                        line.push_back(static_cast<char>(digit));  // to_chars output is ASCII
                    append(line);
                }
                block.push_back(L'\0');
                const bool written = block.size() > 1
                    ? WritePrivateProfileSectionW(L"Weather", block.c_str(), temporary.c_str())
                    : WritePrivateProfileStringW(L"Weather", nullptr, nullptr, temporary.c_str());
                if (!written)
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "write MCM weather settings");
            }
            // Remove obsolete controls when saving, preserving unrelated keys.
            for (const auto* key : { L"iCaptureMethod", L"iDebugView",
                    L"bIsolateCloud", L"fIsolationRadius" })
                write(key, nullptr);
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH |
                    (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "replace MCM settings");
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }
}
