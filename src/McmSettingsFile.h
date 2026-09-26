// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace FO4CS::McmSettingsFile
{
    struct Values
    {
        bool enabled = true;
        float opacity = 2.0f;
        float cloudHeight = 10000.0f;
        // F7/F8/F10/F11 are ignored while false (default for normal players).
        bool hotkeys = false;
        // Development Menu options; they apply only while `hotkeys` is on.
        // 0 off, 1 world checkerboard, 2 raw depth, 3 transmittance,
        // 4 capture analysis.
        int debugView = 0;
        bool isolateCloud = false;
        float isolationRadius = 12.0f;
        bool operator==(const Values&) const = default;
    };

    struct Changes
    {
        bool enabled{}, opacity{}, cloudHeight{}, hotkeys{};
        bool debugView{}, isolateCloud{}, isolationRadius{};
        bool Any() const noexcept
        {
            return enabled || opacity || cloudHeight || hotkeys ||
                debugView || isolateCloud || isolationRadius;
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
            force || !previous || previous->debugView != next.debugView,
            force || !previous || previous->isolateCloud != next.isolateCloud,
            force || !previous || previous->isolationRadius != next.isolationRadius };
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
        if (live.debugView != baseline->debugView) merged.debugView = live.debugView;
        if (live.isolateCloud != baseline->isolateCloud) merged.isolateCloud = live.isolateCloud;
        if (live.isolationRadius != baseline->isolationRadius)
            merged.isolationRadius = live.isolationRadius;
        return merged;
    }

    // Values from a version-4 JSON worth carrying into a new user INI. The
    // release package ships that JSON with default values: migrating it would
    // pin every default into the INI, so future default changes never apply.
    inline std::optional<Values> LegacyMigration(const Values& legacy) noexcept
    {
        Values migrated = legacy;
        migrated.hotkeys = Values{}.hotkeys;
        migrated.debugView = Values{}.debugView;
        migrated.isolateCloud = Values{}.isolateCloud;
        migrated.isolationRadius = Values{}.isolationRadius;
        if (migrated == Values{})
            return std::nullopt;
        return migrated;
    }

    inline Values Validate(Values values) noexcept
    {
        values.opacity = std::isfinite(values.opacity)
            ? std::clamp(values.opacity, 0.0f, 4.0f) : Values{}.opacity;
        values.cloudHeight = std::isfinite(values.cloudHeight)
            ? std::clamp(values.cloudHeight, 10000.0f, 200000.0f) : Values{}.cloudHeight;
        values.debugView = std::clamp(values.debugView, 0, 4);
        values.isolationRadius = std::isfinite(values.isolationRadius)
            ? std::clamp(values.isolationRadius, 2.0f, 30.0f) : Values{}.isolationRadius;
        return values;
    }

    inline void ReadLayer(const std::filesystem::path& path, Values& values)
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
        if (const auto view = read(L"iDebugView")) {
            if (*view != std::floor(*view) || *view < 0.0f || *view > 4.0f)
                throw std::runtime_error("MCM debug view must be 0 to 4");
            values.debugView = static_cast<int>(*view);
        }
        if (const auto isolate = read(L"bIsolateCloud")) {
            if (*isolate != 0.0f && *isolate != 1.0f) throw std::runtime_error("MCM cloud isolation must be 0 or 1");
            values.isolateCloud = *isolate == 1.0f;
        }
        if (const auto radius = read(L"fIsolationRadius")) values.isolationRadius = *radius;
    }

    inline Values Read(const std::filesystem::path& defaults, const std::filesystem::path& user)
    {
        Values values;
        ReadLayer(defaults, values);
        ReadLayer(user, values);
        return Validate(values);
    }

    inline void Write(const std::filesystem::path& path, Values values, bool overwrite = true)
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
            write(L"iDebugView", std::to_wstring(values.debugView).c_str());
            write(L"bIsolateCloud", values.isolateCloud ? L"1" : L"0");
            write(L"fIsolationRadius", decimal(values.isolationRadius).c_str());
            // The capture method is no longer selectable; drop its old key.
            WritePrivateProfileStringW(L"CloudShadows", L"iCaptureMethod", nullptr, temporary.c_str());
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
