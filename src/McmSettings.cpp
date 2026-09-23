// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "McmSettings.h"
#include "McmSettingsFile.h"
#include "CloudShadows.h"
#include "GodraysIntegration.h"
#include "CloudComparison.h"

namespace FO4CS::McmSettings
{
    namespace
    {
        fs::path defaults, user;
        bool available{};
        std::optional<McmSettingsFile::Values> saved;
        struct Stamp
        {
            fs::file_time_type time{};
            std::uintmax_t size{};
            bool exists{};
            bool operator==(const Stamp&) const = default;
        };
        std::optional<std::array<Stamp, 2>> lastRead;
        Stamp Inspect(const fs::path& path)
        {
            if (!fs::exists(path)) return {};
            return { fs::last_write_time(path), fs::file_size(path), true };
        }
    }

    void Initialize() noexcept
    {
        try {
            if (BuildFeatures::kExperimental) return; // Developer tools retain their JSON controls.
            std::array<wchar_t, 32768> executable{};
            const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            if (!length || length >= executable.size()) return;
            const auto data = fs::path(executable.data()).parent_path() / L"Data";
            defaults = data / L"MCM/Config/FO4CloudShadows/settings.ini";
            user = data / L"MCM/Settings/FO4CloudShadows.ini";
            available = fs::exists(defaults);
            if (!available) return;
            if (!fs::exists(user)) {
                const auto legacy = data / L"Shaders/Features/CloudShadows.json";
                if (fs::exists(legacy)) {
                    std::ifstream input(legacy);
                    const auto json = nlohmann::json::parse(input);
                    if (json.at("ProjectionModelVersion").get<int>() != 4)
                        throw std::runtime_error("legacy projection version is not supported");
                    McmSettingsFile::Values values;
                    values.enabled = json.value("Enabled", values.enabled);
                    values.opacity = json.value("Opacity", values.opacity);
                    values.cloudHeight = json.value("CloudHeight", values.cloudHeight);
                    McmSettingsFile::Write(user, values, false);
                    SPDLOG_INFO("[CloudShadows][MCM] Migrated saved JSON preferences; original JSON retained");
                }
            }
            SPDLOG_INFO("[CloudShadows][MCM] Shared F11/MCM settings: {}", user.string());
        } catch (const std::exception& error) {
            SPDLOG_ERROR("[CloudShadows][MCM] Settings initialization: {}", error.what());
        }
    }

    bool Available() noexcept { return available && !BuildFeatures::kExperimental; }

    bool Load(bool force) noexcept
    {
        try {
            const std::array before{ Inspect(defaults), Inspect(user) };
            if (!force && lastRead && *lastRead == before) return true;
            const auto next = McmSettingsFile::Read(defaults, user);
            if (before != std::array{ Inspect(defaults), Inspect(user) })
                throw std::runtime_error("MCM settings changed during read; retrying");
            const auto changes = McmSettingsFile::Difference(saved, next, force);
            if (changes.opacity) CloudShadows::g_settings.Opacity = next.opacity;
            if (changes.cloudHeight) CloudShadows::g_settings.CloudHeight = next.cloudHeight;
            if (changes.captureMethod)
                CloudComparison::SetMethod(next.captureMethod == 1
                    ? CloudComparison::Method::SunMask : CloudComparison::Method::Cubemap);
            if (changes.hotkeys)
                CloudShadows::g_hotkeysEnabled.store(next.hotkeys, std::memory_order_release);
            if (changes.enabled && CloudShadows::g_shadowsEnabled.exchange(next.enabled,
                    std::memory_order_acq_rel) != next.enabled)
                CloudShadows::InvalidateWorldCloudCaptureForToggle();
            CloudShadows::g_settings.DebugMode = 0;
            GodraysIntegration::SetCloudOcclusionEnabled(false);
            saved = next;
            lastRead = before;
            if (changes.Any())
                SPDLOG_INFO("[CloudShadows][MCM] Applied saved preferences: enabled={} opacity={:.2f} cloudHeight={:.0f} method={} hotkeys={}",
                    next.enabled, next.opacity, next.cloudHeight, CloudComparison::MethodName(), next.hotkeys);
            return true;
        } catch (const std::exception& error) {
            static auto nextWarning = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextWarning) {
                nextWarning = now + std::chrono::seconds(30);
                SPDLOG_ERROR("[CloudShadows][MCM] Keeping live settings: {}", error.what());
            }
            return false;
        }
    }

    bool Save() noexcept
    {
        try {
            const auto values = McmSettingsFile::Validate({
                CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire),
                CloudShadows::g_settings.Opacity, CloudShadows::g_settings.CloudHeight,
                CloudComparison::GetMethod() == CloudComparison::Method::SunMask ? 1 : 0,
                CloudShadows::g_hotkeysEnabled.load(std::memory_order_acquire) });
            McmSettingsFile::Write(user, values);
            saved = values;
            lastRead.reset();
            SPDLOG_INFO("[CloudShadows][MCM] F11 settings saved; MCM reloads its displayed values at the next game launch");
            return true;
        } catch (const std::exception& error) {
            SPDLOG_ERROR("[CloudShadows][MCM] Could not save settings: {}", error.what());
            return false;
        }
    }
}
