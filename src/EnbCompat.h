// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Windows.h>

#include <array>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>

// ENB draws its own cloud shadows. This plugin stays inert when ENB's cloud
// shadows are active at startup, so ENB provides the effect without this
// plugin costing any performance, and runs normally (hooking ENB's Present)
// when ENB is present with them disabled.
//
// The decision is made once at plugin load, before any hook exists. ENB's SDK
// only guarantees parameters after its ENBCallback_PostLoad and advises calling
// ENBGetParameter from its callbacks, which is too late and outside our control,
// so the source of truth is the enbseries.ini that ENB itself reads at startup,
// located next to the ENB DLL.
namespace FO4CS::EnbCompat
{
    enum class Setting : unsigned char
    {
        kTrue,
        kFalse,
        kMissing
    };

    // Reads an ENB boolean the way ENB writes it: "true"/"false", case-insensitive;
    // "1"/"0" are accepted as well.
    [[nodiscard]] inline Setting ReadBool(
        const std::filesystem::path& ini,
        const wchar_t* section,
        const wchar_t* key) noexcept
    {
        std::array<wchar_t, 64> buffer{};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", buffer.data(),
            static_cast<DWORD>(buffer.size()), ini.c_str());
        if (!length || length >= buffer.size() - 1)
            return Setting::kMissing;
        std::size_t begin = 0;
        std::size_t end = length;
        while (begin < end && std::iswspace(buffer[begin]))
            ++begin;
        while (end > begin && std::iswspace(buffer[end - 1]))
            --end;
        const auto equals = [&](const wchar_t* text) noexcept {
            const std::size_t size = std::wcslen(text);
            return end - begin == size &&
                _wcsnicmp(buffer.data() + begin, text, size) == 0;
        };
        if (equals(L"true") || equals(L"1"))
            return Setting::kTrue;
        if (equals(L"false") || equals(L"0"))
            return Setting::kFalse;
        return Setting::kMissing;
    }

    // True when the preset explicitly enables ENB's cloud shadows at startup:
    // [EFFECT] EnableCloudShadows is true and [GLOBAL] UseEffect is not false.
    // A preset without the key (or without an enbseries.ini) is not treated
    // as ENB-owned: staying inert there left players with no cloud shadows at
    // all. The caller logs how to resolve doubled shadows in that case.
    [[nodiscard]] inline bool CloudShadowsActiveAtStartup(
        const std::filesystem::path& enbseriesIni) noexcept
    {
        std::error_code error;
        if (!std::filesystem::exists(enbseriesIni, error) || error)
            return false;
        return ReadBool(enbseriesIni, L"GLOBAL", L"UseEffect") != Setting::kFalse &&
            ReadBool(enbseriesIni, L"EFFECT", L"EnableCloudShadows") == Setting::kTrue;
    }

    // True when the preset states EnableCloudShadows explicitly (either way).
    [[nodiscard]] inline bool CloudShadowsSettingPresent(
        const std::filesystem::path& enbseriesIni) noexcept
    {
        std::error_code error;
        return std::filesystem::exists(enbseriesIni, error) && !error &&
            ReadBool(enbseriesIni, L"EFFECT", L"EnableCloudShadows") != Setting::kMissing;
    }

    struct Detection
    {
        bool present{};
        bool cloudShadowsActive{};
        bool cloudShadowsSettingPresent{};
        long version{};
        std::filesystem::path configPath;
    };

    // ENB identifies itself through its SDK exports, exactly as ENB's own
    // ExamplePlugin does. Only ENBGetVersion, a constant getter, is called.
    [[nodiscard]] inline Detection Detect() noexcept
    {
        Detection result{};
        try {
            for (const wchar_t* name : { L"d3d11.dll", L"dxgi.dll" }) {
                const HMODULE module = GetModuleHandleW(name);
                if (!module || !GetProcAddress(module, "ENBGetSDKVersion"))
                    continue;
                result.present = true;
                using GetVersion_t = long (*)();
                if (const auto getVersion = reinterpret_cast<GetVersion_t>(
                        GetProcAddress(module, "ENBGetVersion"))) {
                    result.version = getVersion();
                }
                std::wstring path(1024, wchar_t{});
                const DWORD length = GetModuleFileNameW(
                    module, path.data(), static_cast<DWORD>(path.size()));
                if (length && length < path.size()) {
                    path.resize(length);
                    result.configPath =
                        std::filesystem::path(path).parent_path() / L"enbseries.ini";
                }
                result.cloudShadowsActive = !result.configPath.empty() &&
                    CloudShadowsActiveAtStartup(result.configPath);
                result.cloudShadowsSettingPresent = !result.configPath.empty() &&
                    CloudShadowsSettingPresent(result.configPath);
                return result;
            }
        } catch (...) {
            result.cloudShadowsActive = false;
        }
        return result;
    }

    // UTF-8 for logging without std::filesystem::path::string(), whose ANSI
    // code-page conversion throws for characters the code page cannot represent.
    [[nodiscard]] inline std::string ToUtf8(const std::wstring& text) noexcept
    {
        try {
            if (text.empty())
                return {};
            const int size = WideCharToMultiByte(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                return {};
            std::string output(static_cast<std::size_t>(size), char{});
            WideCharToMultiByte(
                CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                output.data(), size, nullptr, nullptr);
            return output;
        } catch (...) {
            return {};
        }
    }

    // ENBGetVersion reports 501 for 0.501.
    [[nodiscard]] inline std::string VersionText(long version) noexcept
    {
        try {
            if (version <= 0)
                return "(unknown version)";
            std::string minor = std::to_string(version % 1000);
            while (minor.size() < 3)
                minor.insert(minor.begin(), '0');
            return std::to_string(version / 1000) + "." + minor;
        } catch (...) {
            return {};
        }
    }
}
