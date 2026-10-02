// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "McmSettings.h"
#include "McmSettingsFile.h"
#include "ManualFpsLog.h"
#include "CloudShadows.h"
#include "GodraysIntegration.h"
#include "CloudComparison.h"
#include "Utf8Path.h"
#include "WeatherOpacity.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace FO4CS::McmSettings
{
    namespace
    {
        fs::path defaults, user;
        bool available{};
        // Serializes writers of the user INI.
        std::mutex writeMutex;
        std::optional<McmSettingsFile::Values> saved;
        struct Stamp
        {
            fs::file_time_type time{};
            std::uintmax_t size{};
            bool exists{};
            bool operator==(const Stamp&) const = default;
        };
        std::optional<std::array<Stamp, 2>> lastRead;
        // A malformed or locked file is not re-read until one of the files
        // changes again; the poll then costs two stats every two seconds.
        std::optional<std::array<Stamp, 2>> failedRead;
        Stamp Inspect(const fs::path& path)
        {
            if (!fs::exists(path)) return {};
            return { fs::last_write_time(path), fs::file_size(path), true };
        }

        // Saves run on one background thread: a settings write copies the INI,
        // rewrites five keys and replaces it with write-through, which took
        // 5-50 ms on the render thread (F11 close) or the framework's event
        // thread. The newest request replaces an older one still queued.
        struct Request
        {
            McmSettingsFile::Values values{};
            // Present for F11: merge with the file, keeping fields changed
            // elsewhere since `baseline` was applied.
            std::optional<McmSettingsFile::Values> baseline;
            // F11 has no weather controls; its saves never touch them.
            bool includeWeather = true;
        };
        struct Writer
        {
            std::mutex mutex;
            std::condition_variable wake;
            std::optional<Request> pending;
            std::uint64_t queued{};
            std::uint64_t completed{};
            std::uint64_t lastFailed{};
            bool started{};
        };
        // Never destroyed: the detached writer may still reference it while
        // the process exits.
        Writer& writer = *new Writer();

        void WriteNow(const Request& request)
        {
            std::lock_guard lock(writeMutex);
            auto values = request.values;
            if (request.baseline) {
                McmSettingsFile::Values file = values;
                try {
                    file = McmSettingsFile::Read(defaults, user);
                } catch (const std::exception&) {
                    // An unreadable file cannot contribute other edits.
                }
                values = McmSettingsFile::Merge(file, request.values, request.baseline);
            }
            McmSettingsFile::Write(user, values, true, request.includeWeather);
        }

        void WriterLoop() noexcept
        {
            for (;;) {
                Request request;
                std::uint64_t ticket = 0;
                {
                    std::unique_lock lock(writer.mutex);
                    writer.wake.wait(lock, [] { return writer.pending.has_value(); });
                    request = std::move(*writer.pending);
                    writer.pending.reset();
                    ticket = writer.queued;
                }
                bool succeeded = false;
                try {
                    WriteNow(request);
                    succeeded = true;
                } catch (const std::exception& error) {
                    SPDLOG_ERROR("[CloudShadows][MCM] Could not save settings: {}", error.what());
                } catch (...) {
                    SPDLOG_ERROR("[CloudShadows][MCM] Could not save settings");
                }
                std::lock_guard lock(writer.mutex);
                writer.completed = ticket;
                if (!succeeded)
                    writer.lastFailed = ticket;
            }
        }

        std::uint64_t Queue(Request request)
        {
            std::lock_guard lock(writer.mutex);
            if (!writer.started) {
                std::thread(WriterLoop).detach();
                writer.started = true;
            }
            writer.pending = std::move(request);
            const auto ticket = ++writer.queued;
            writer.wake.notify_one();
            return ticket;
        }

        [[nodiscard]] bool WriteInFlight()
        {
            std::lock_guard lock(writer.mutex);
            return writer.completed != writer.queued;
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
                    // The release package ships this JSON with default values;
                    // only a player's real changes become a user INI.
                    if (const auto migrated = McmSettingsFile::LegacyMigration(
                            McmSettingsFile::Validate(values))) {
                        McmSettingsFile::Write(user, *migrated, false);
                        SPDLOG_INFO("[CloudShadows][MCM] Migrated saved JSON preferences; original JSON retained");
                    }
                }
            }
            SPDLOG_INFO("[CloudShadows][MCM] Shared F11/MCM settings: {}", Utf8Path(user));
        } catch (const std::exception& error) {
            SPDLOG_ERROR("[CloudShadows][MCM] Settings initialization: {}", error.what());
        }
    }

    bool Available() noexcept { return available && !BuildFeatures::kExperimental; }

    bool Load(bool force) noexcept
    {
        try {
            // A queued save is about to change the file; reading the old one
            // now would revert the edit being saved for one poll.
            if (!force && WriteInFlight())
                return true;
            const std::array before{ Inspect(defaults), Inspect(user) };
            if (!force && lastRead && *lastRead == before) return true;
            if (!force && failedRead && *failedRead == before) return false;
            McmSettingsFile::Values next;
            try {
                next = McmSettingsFile::Read(defaults, user);
            } catch (...) {
                // Deterministic until a file changes; a torn read (below)
                // retries at the next poll instead.
                failedRead = before;
                throw;
            }
            if (before != std::array{ Inspect(defaults), Inspect(user) })
                throw std::runtime_error("MCM settings changed during read; retrying");
            failedRead.reset();
            const auto changes = McmSettingsFile::Difference(saved, next, force);
            if (changes.enabled || changes.opacity || changes.cloudHeight ||
                changes.weatherOpacity || changes.weathers)
                ManualFpsLog::Stop();
            if (changes.opacity) CloudShadows::g_settings.Opacity = next.opacity;
            if (changes.weatherOpacity || changes.weathers)
                WeatherOpacity::Configure(next.weatherOpacity, next.weathers);
            if (changes.cloudHeight) CloudShadows::g_settings.CloudHeight = next.cloudHeight;
            if (changes.hotkeys)
                CloudShadows::g_hotkeysEnabled.store(next.hotkeys, std::memory_order_release);
            if (changes.enabled && CloudShadows::g_shadowsEnabled.exchange(next.enabled,
                    std::memory_order_acq_rel) != next.enabled)
                CloudShadows::InvalidateWorldCloudCaptureForToggle();
            // Diagnostic views belong to the F-key menu, never saved MCM
            // options. Start neutral and clear them when the menu is disabled;
            // ordinary MCM edits must preserve an active session-only view.
            if (!next.hotkeys || !saved || force) {
                CloudShadows::g_settings.DebugMode = 0.0f;
                CloudShadows::CancelSingleCloudLock();
                CloudShadows::g_singleCloudIsolationEnabled.store(false, std::memory_order_release);
            }
            GodraysIntegration::SetCloudOcclusionEnabled(false);
            saved = next;
            lastRead = before;
            if (changes.Any())
                SPDLOG_INFO("[CloudShadows][MCM] Applied saved preferences: enabled={} opacity={:.2f} cloudHeight={:.0f} hotkeys={} "
                    "weatherOpacity={} weatherPairings={}",
                    next.enabled, next.opacity, next.cloudHeight, next.hotkeys,
                    next.weatherOpacity, next.weathers.size());
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
            // F11 saves player settings; diagnostic views stay session-only.
            auto values = McmSettingsFile::Validate({
                ManualFpsLog::EnabledForSave(CloudShadows::g_shadowsEnabled.load(std::memory_order_acquire)),
                CloudShadows::g_settings.Opacity, CloudShadows::g_settings.CloudHeight,
                CloudShadows::g_hotkeysEnabled.load(std::memory_order_acquire) });
            if (saved) {
                values.weatherOpacity = saved->weatherOpacity;
                values.weathers = saved->weathers;
            }
            // Only fields changed in game since the last applied file win; the
            // writer merges them into whatever the file holds at write time.
            (void)Queue({ values, saved, false });
            saved = values;
            lastRead.reset();
            failedRead.reset();
            SPDLOG_INFO("[CloudShadows][MCM] F11 settings saved; MCM reloads its displayed values at the next game launch");
            return true;
        } catch (const std::exception& error) {
            SPDLOG_ERROR("[CloudShadows][MCM] Could not save settings: {}", error.what());
            return false;
        }
    }

    bool ReadSaved(McmSettingsFile::Values& values) noexcept
    {
        try {
            if (!Available()) return false;
            values = McmSettingsFile::Read(defaults, user);
            return true;
        } catch (const std::exception& error) {
            SPDLOG_ERROR("[CloudShadows][MCM] Could not read settings: {}", error.what());
            return false;
        }
    }

    std::uint64_t WriteSaved(const McmSettingsFile::Values& values) noexcept
    {
        try {
            if (!Available()) return 0;
            return Queue({ McmSettingsFile::Validate(values), std::nullopt });
        } catch (const std::exception& error) {
            SPDLOG_ERROR("[CloudShadows][MCM] Could not queue settings: {}", error.what());
            return 0;
        }
    }

    bool SaveFailed(std::uint64_t ticket) noexcept
    {
        if (ticket == 0)
            return true;
        std::lock_guard lock(writer.mutex);
        return writer.lastFailed >= ticket;
    }
}
