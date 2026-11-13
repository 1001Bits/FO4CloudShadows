// SPDX-License-Identifier: GPL-3.0-only
#include "McmSettingsFile.h"
#include "GodraysIntegration.h"
#include "EnbCompat.h"
#include "PluginWeatherScan.h"
#include "WeatherOpacity.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace Files = FO4CS::McmSettingsFile;
namespace fs = std::filesystem;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
    const auto root = fs::temp_directory_path() / (L"FO4CS-mcm-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directory(root);
    const auto defaults = root / L"defaults.ini";
    const auto user = root / L"user.ini";
    int result = 0;
    try {
        Require(argc == 2, "project path required");
        Require(Files::Read(defaults, user) == Files::Values{}, "missing files use release defaults");
        Files::Write(defaults, Files::Values{});
        Require(WritePrivateProfileStringW(L"CloudShadows", L"fOpacity", L"1.25", user.c_str()), "write partial MCM override");
        const auto partial = Files::Read(defaults, user);
        Require(partial == Files::Values{ true, 1.25f, 10000.0f }, "MCM overrides inherit missing defaults");
        Require(WritePrivateProfileStringW(L"Future", L"Keep", L"123", user.c_str()), "add future key");
        Require(WritePrivateProfileStringW(L"CloudShadows", L"iCaptureMethod", L"1", user.c_str()), "write obsolete method key");
        Files::Write(user, { false, 3.5f, 45000.0f });
        const auto saved = Files::Read(defaults, user);
        Require(saved == Files::Values{ false, 3.5f, 45000.0f }, "F11 save round trip");
        Require(GetPrivateProfileIntW(L"CloudShadows", L"iCaptureMethod", -1, user.c_str()) == -1,
            "saving drops the obsolete capture-method key");
        Require(GetPrivateProfileIntW(L"Future", L"Keep", 0, user.c_str()) == 123, "save preserves other sections");
        bool refused = false;
        try { Files::Write(user, {}, false); } catch (...) { refused = true; }
        Require(refused && Files::Read(defaults, user) == saved, "migration cannot overwrite an existing MCM preference");

        {
            // F11 save merges: fields changed in game win, others keep the
            // file (an MCM/Menu Framework edit not yet polled survives).
            const Files::Values baseline{ true, 2.0f, 10000.0f, false };
            Files::Values live = baseline;
            live.opacity = 3.0f;
            Files::Values file = baseline;
            file.enabled = false;
            file.hotkeys = true;
            const auto merged = Files::Merge(file, live, baseline);
            Require(merged == Files::Values{ false, 3.0f, 10000.0f, true },
                "F11 save keeps an unpolled external edit and its own change");
            Require(Files::Merge(file, live, std::nullopt) == live,
                "without an applied baseline the live values are saved whole");
            // A pristine shipped JSON never becomes a user INI; real changes do.
            Require(!Files::LegacyMigration(Files::Values{}).has_value(),
                "default legacy JSON is not migrated");
            Files::Values legacy;
            legacy.opacity = 3.0f;
            legacy.hotkeys = true;
            const auto migrated = Files::LegacyMigration(legacy);
            Require(migrated && migrated->opacity == 3.0f && !migrated->hotkeys,
                "a changed legacy preference migrates; the development switch never does");
            Files::Values development;
            development.hotkeys = true;
            Require(!Files::LegacyMigration(development).has_value(),
                "the development switch alone does not trigger legacy migration");
        }
        {
            // Older releases exposed diagnostic controls in MCM. Their saved
            // values must neither enable an invisible view nor reject the
            // remaining player preferences, even when those old values are bad.
            const auto developmentFile = root / L"development.ini";
            Files::Values development;
            development.hotkeys = true;
            Files::Write(developmentFile, development);
            for (const auto* value : { L"1", L"3", L"7", L"invalid" }) {
                for (const auto* key : { L"iDebugView", L"bIsolateCloud", L"fIsolationRadius" })
                    Require(WritePrivateProfileStringW(L"CloudShadows", key, value,
                        developmentFile.c_str()), "write obsolete diagnostic preference");
                Require(Files::Read(defaults, developmentFile) == development,
                    "obsolete diagnostics do not affect player settings");
                Files::Write(developmentFile, development);
                for (const auto* key : { L"iDebugView", L"bIsolateCloud", L"fIsolationRadius" })
                    Require(GetPrivateProfileIntW(L"CloudShadows", key, -1,
                        developmentFile.c_str()) == -1,
                        "saving removes obsolete diagnostic preferences");
            }
            auto switchOff = development;
            switchOff.hotkeys = false;
            const auto switchOnly = Files::Difference(development, switchOff, false);
            Require(switchOnly.hotkeys && !switchOnly.enabled && !switchOnly.opacity && !switchOnly.cloudHeight,
                "development switch changes independently of player settings");
            fs::remove(developmentFile);
        }

        const auto unchanged = Files::Difference(saved, saved, false);
        Require(!unchanged.Any(), "polling unchanged preferences must preserve live F10/unsaved F11 state");
        auto changed = saved;
        changed.cloudHeight = 50000;
        const auto heightOnly = Files::Difference(saved, changed, false);
        Require(heightOnly.cloudHeight && !heightOnly.enabled && !heightOnly.opacity,
            "height edit must not revert F10 or opacity");
        Require(WritePrivateProfileStringW(L"CloudShadows", L"iCaptureMethod", L"2", user.c_str()), "write obsolete method key");
        Require(Files::Read(defaults, user) == saved, "an old capture-method key is ignored");
        Files::Write(user, saved);
        Require(Files::Difference(saved, saved, true).enabled, "explicit reload restores saved master preference");
        Require(Files::Difference(std::nullopt, saved, false).enabled, "first load adopts preferences");

        for (const auto* malformed : { L"nan", L"inf", L"1.5junk", L"", L"1,5" }) {
            Require(WritePrivateProfileStringW(L"CloudShadows", L"fOpacity", malformed, user.c_str()), "write malformed value");
            bool rejected = false;
            try { (void)Files::Read(defaults, user); } catch (...) { rejected = true; }
            Require(rejected, "reject malformed/nonfinite values without publishing a partial snapshot");
        }
        Files::Write(user, { true, 8.0f, 100.0f });
        Require(Files::Read(defaults, user) == Files::Values{ true, 4.0f, 10000.0f }, "settings are bounded");
        const HANDLE locked = CreateFileW(user.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "lock user settings");
        bool readRejected = false;
        try { (void)Files::Read(defaults, user); } catch (...) { readRejected = true; }
        CloseHandle(locked);
        Require(readRejected, "sharing failure must not be mistaken for default settings");
        fs::remove(user);
        Require(Files::Read(defaults, user) == Files::Values{}, "deleting overrides restores defaults");

        {
            Require(Files::CanonicalWeatherKey("Fallout4.esm|1ca7e4") == "1CA7E4" &&
                Files::CanonicalWeatherKey("0x2b52a") == "02B52A" &&
                Files::CanonicalWeatherKey("0300ABCD") == "0300ABCD" &&
                Files::CanonicalWeatherKey(" DLCCoast.esm | abcd ") == "dlccoast.esm|00ABCD" &&
                !Files::CanonicalWeatherKey("DLCCoast.esm|1000000") &&
                !Files::CanonicalWeatherKey("|12") && !Files::CanonicalWeatherKey("xyz"),
                "weather keys canonicalize and reject malformed IDs");
            {
                std::ofstream output(user, std::ios::binary);
                output << "[CloudShadows]\r\nbWeatherOpacity=1\r\n[Weather]\r\n"
                    "1ca7e4|25.0\r\nf2B52A=0.75\r\nDLCCoast.esm|abcd|0.5\r\n"
                    "; kept comment\r\nnot a pairing\r\nf0F1033=oops\r\n";
            }
            // The shipped defaults' [Weather] values only feed MCM's display.
            Require(WritePrivateProfileStringW(L"Weather", L"f1C8556", L"3.0", defaults.c_str()),
                "write a default-layer weather");
            const auto read = Files::Read(defaults, user);
            const Files::WeatherOpacities expected{
                { "1CA7E4", 4.0f }, { "02B52A", 0.75f }, { "dlccoast.esm|00ABCD", 0.5f } };
            Require(read.weatherOpacity && read.weathers == expected,
                "hand-written and MCM weather lines pair; malformed lines and defaults do not");
            {
                Require(read.waterShadows, "water shadows stay on when the user file does not mention them");
                auto off = read;
                off.waterShadows = false;
                Require(Files::Difference(read, off, false).waterShadows &&
                    !Files::Difference(read, read, false).Any(),
                    "turning water shadows off is seen as a change, and nothing else is");
                Require(!Files::Merge(read, off, read).waterShadows && Files::Merge(off, read, read).waterShadows == false,
                    "an in-game water switch edit wins, and otherwise the saved value is kept");
                const auto waterFile = user.parent_path() / L"water-switch.ini";
                Files::Write(waterFile, off);
                Require(!Files::Read(defaults, waterFile).waterShadows, "the water switch survives a save");
                Files::Write(waterFile, read);
                Require(Files::Read(defaults, waterFile).waterShadows, "the water switch turns back on");
                Require(WritePrivateProfileStringW(L"CloudShadows", L"bWaterShadows", L"2", waterFile.c_str()),
                    "write an invalid water switch");
                bool rejected = false;
                try {
                    (void)Files::Read(defaults, waterFile);
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                Require(rejected, "a water switch other than 0 or 1 is rejected");
                auto dark = read;
                dark.nightShadows = false;
                Require(Files::Difference(read, dark, false).nightShadows, "turning night shadows off is seen as a change");
                Require(!Files::Merge(read, dark, read).nightShadows, "an in-game night switch edit wins");
                Files::Write(waterFile, dark);
                Require(!Files::Read(defaults, waterFile).nightShadows, "the night switch survives a save");
                // Current weather slider. Only its own paths write the value, so a
                // stale copy elsewhere can never read back as the player moving it.
                auto moved = read;
                moved.currentWeather = 0.25f;
                Files::Write(waterFile, moved);
                Require(Files::Read(defaults, waterFile).currentWeather == Files::Read(defaults, user).currentWeather,
                    "ordinary saves do not write the Current weather slider");
                Files::Write(waterFile, moved, true, true, true);
                Require(Files::Read(defaults, waterFile).currentWeather == 0.25f, "the slider's own path writes it");
                Files::Write(waterFile, read);
                Require(Files::Read(defaults, waterFile).currentWeather == 0.25f, "a later ordinary save leaves it alone");
                Require(Files::Merge(read, moved, read).currentWeather == 0.25f, "merging keeps a slider update");
                Files::Values paired = read;
                paired.opacity = 0.5f;
                paired.weathers["vivid weathers - fo4.esp|004C68"] = 3.0f;
                Require(Files::OpacityFor(paired, "vivid weathers - fo4.esp|004C68") == 3.0f &&
                    Files::OpacityFor(paired, "02B52A") == paired.weathers.at("02B52A") &&
                    Files::OpacityFor(paired, "vivid weathers - fo4.esp|004C67") == 0.5f,
                    "the slider shows a weather's pairing, or Shadow opacity without one");
                Require(!Files::SliderMoved(1.0f, 1.0004f) && Files::SliderMoved(1.0f, 0.95f),
                    "a slider step counts as moving it, float formatting does not");
                Files::Values wild = read;
                wild.currentWeather = 9.0f;
                Require(Files::Validate(wild).currentWeather == 4.0f, "the slider stays on the opacity scale");
                std::filesystem::remove(waterFile);
            }
            Files::Write(user, read);
            Require(Files::ReadWeatherLines(user).front() == "1ca7e4|25.0",
                "an unchanged weather set keeps hand-written lines as written");
            auto edited = read;
            edited.weathers["1CA7E4"] = 0.25f;
            edited.weathers.erase("02B52A");
            Files::Write(user, edited);
            Require(Files::Read(defaults, user) == edited, "edited weather pairings round trip");
            Require(GetPrivateProfileIntW(L"Weather", L"f2B52A", -1, user.c_str()) == -1,
                "a removed pairing leaves no line behind");
            const auto lines = Files::ReadWeatherLines(user);
            Require(std::find(lines.begin(), lines.end(), "f1CA7E4=0.25") != lines.end() &&
                std::find(lines.begin(), lines.end(), "1ca7e4|25.0") == lines.end(),
                "edited pairings are written in MCM's key=value form");

            auto file = edited;
            file.weathers["0F1033"] = 2.0f;   // saved through MCM meanwhile
            auto live = edited;
            live.weathers["1CA7E4"] = 1.5f;   // changed in game
            const auto merged = Files::Merge(file, live, edited);
            Require(merged.weathers.at("0F1033") == 2.0f && merged.weathers.at("1CA7E4") == 1.5f,
                "weather pairings merge per weather");
            Require(Files::Difference(edited, merged, false).weathers &&
                !Files::Difference(edited, merged, false).opacity, "weather edits are a weather change only");
            WritePrivateProfileStringW(L"Weather", nullptr, nullptr, defaults.c_str());
            fs::remove(user);
        }

        {
            // A synthetic plugin: one master, a new and an overriding weather,
            // a compressed record, a climate and an XXXX-sized region list.
            const auto u32 = [](std::uint32_t value) { return std::string(reinterpret_cast<const char*>(&value), 4); };
            const auto sub = [](std::string_view type, std::string_view data) {
                const auto size = static_cast<std::uint16_t>(data.size());
                return std::string(type) + std::string(reinterpret_cast<const char*>(&size), 2) + std::string(data);
            };
            const auto record = [&](std::string_view type, std::uint32_t formID, std::string data,
                    std::uint32_t flags = 0) {
                return std::string(type) + u32(static_cast<std::uint32_t>(data.size())) + u32(flags) +
                    u32(formID) + std::string(8, '\0') + data;
            };
            const auto group = [&](std::string_view label, const std::string& records) {
                return "GRUP" + u32(static_cast<std::uint32_t>(24 + records.size())) + std::string(label) +
                    std::string(12, '\0') + records;
            };
            const auto entry = [&](std::uint32_t weather) { return u32(weather) + u32(30) + u32(0); };
            std::string plugin = record("TES4", 0, sub("HEDR", std::string(12, '\0')) +
                sub("MAST", std::string("Fallout4.esm\0", 13)) + sub("DATA", std::string(8, '\0')));
            plugin += group("GMST", record("GMST", 0x01000900, sub("EDID", std::string("fIgnored\0", 9))));
            plugin += group("WTHR",
                record("WTHR", 0x01000801, sub("EDID", std::string("MyStormyWeather\0", 16))) +
                record("WTHR", 0x0002B52A, sub("EDID", std::string("CommonwealthClear\0", 18))) +
                record("WTHR", 0x01000802, std::string(16, 'z'), 0x00040000));
            plugin += group("CLMT", record("CLMT", 0x01000810,
                sub("EDID", std::string("MyClimate\0", 10)) + sub("WLST", entry(0x01000801) + entry(0x0002B52A))));
            plugin += group("REGN", record("REGN", 0x01000820,
                sub("XXXX", u32(12)) + "RDWT" + std::string(2, '\0') + entry(0x01000803)));
            std::istringstream input(plugin);
            const auto scan = FO4CS::PluginWeatherScan::Scan(input);
            Require(scan && scan->masters == std::vector<std::string>{ "Fallout4.esm" } &&
                scan->weathers.size() == 2 && scan->weathers[0].second == "MyStormyWeather" &&
                scan->compressedSkipped == 1 &&
                scan->references == std::vector<std::uint32_t>{ 0x01000801, 0x0002B52A, 0x01000803 },
                "plugin scan reads masters, weather names and climate/region weather lists");
            Require(FO4CS::PluginWeatherScan::KeyFor(*scan, "MyWeathers.esp", 0x01000801) == "myweathers.esp|000801" &&
                FO4CS::PluginWeatherScan::KeyFor(*scan, "MyWeathers.esp", 0x0002B52A) == "02B52A",
                "file form IDs map to plugin-local and Fallout4.esm keys");
            std::istringstream notPlugin(std::string("not a plugin file at all, just text"));
            Require(!FO4CS::PluginWeatherScan::Scan(notPlugin), "a non-plugin file is rejected");

            namespace Weather = FO4CS::WeatherOpacity;
            Require(Weather::TransitionWeight(0.0f) == 0.0f && Weather::TransitionWeight(1.0f) == 1.0f &&
                Weather::TransitionWeight(0.5f) == 0.5f && Weather::TransitionWeight(0.1f) < 0.1f,
                "weather transitions ease in and out");
            Require(std::abs(Weather::Fade(1.0f, 0.0f, Weather::kFadeSeconds) - std::exp(-1.0f)) < 1e-5f &&
                Weather::Fade(0.3f, 2.0f, 0.0f) == 0.3f,
                "the weather fade approaches its target with the configured time constant");
            float stepped = 1.0f;
            for (int frame = 0; frame < 600; ++frame)
                stepped = Weather::Fade(stepped, 0.0f, 1.0f / 60.0f);
            Require(std::abs(stepped - Weather::Fade(1.0f, 0.0f, 10.0f)) < 1e-3f,
                "the fade is independent of frame rate");
        }

        const fs::path project = argv[1];
        const auto canonical = Files::Read(project / "config/MCM/FO4CloudShadows/settings.ini", user);
        Require(canonical == Files::Values{}, "shipped MCM and native defaults agree");
        std::ifstream legacyInput(project / "config/CloudShadows.json");
        const auto legacy = nlohmann::json::parse(legacyInput);
        Require(legacy.at("Enabled") == canonical.enabled && legacy.at("Opacity") == canonical.opacity &&
            legacy.at("CloudHeight") == canonical.cloudHeight && legacy.at("GodrayCloudOcclusion") == false &&
            !legacy.contains("CaptureMethod") &&
            legacy.at("Hotkeys") == canonical.hotkeys,
            "legacy fallback agrees with release defaults");
        std::ifstream menuInput(project / "config/MCM/FO4CloudShadows/config.json");
        const auto menu = nlohmann::json::parse(menuInput);
        Require(menu.at("modName") == "FO4CloudShadows", "MCM folder identity");
        std::set<std::string> controls;
        for (const auto& row : menu.at("content")) if (row.contains("id")) {
            Require(controls.insert(row.at("id").get<std::string>()).second, "unique MCM setting ids");
        }
        std::set<std::string> expectedControls{
            "bEnabled:CloudShadows", "fOpacity:CloudShadows", "bHotkeys:CloudShadows",
            "bWeatherOpacity:CloudShadows", "bWaterShadows:CloudShadows", "bNightShadows:CloudShadows",
            "fCurrentWeather:CloudShadows" };
        for (const auto& weather : Files::kVanillaWeathers)
            expectedControls.insert("f" + std::string(weather.key) + ":Weather");
        Require(controls == expectedControls,
            "MCM exposes the player controls, the vanilla weather sliders and the Development menu switch");
        for (const auto& row : menu.at("content")) if (row.contains("help")) {
            const auto help = row.at("help").get<std::string>();
            Require(!help.empty() && help.size() <= 64 && help.back() == '.' && help.find('\n') == std::string::npos &&
                help.find(". ") == std::string::npos, "every MCM help text is one sentence on one line");
        }
        for (const auto& weather : Files::kVanillaWeathers)
            Require(GetPrivateProfileIntW(L"Weather", (L"f" + std::wstring(weather.key.begin(), weather.key.end())).c_str(),
                -1, (project / "config/MCM/FO4CloudShadows/settings.ini").c_str()) == 1,
                "every MCM weather slider has a shipped display default");
        Require(canonical.hotkeys == false, "hotkeys are off by default");
        Require(canonical.waterShadows, "water shadows are on by default");
        Require(canonical.nightShadows, "shadows at night are on by default");
        FO4CS::GodraysIntegration::SetCloudOcclusionEnabled(true);
        Require(!FO4CS::GodraysIntegration::IsCloudOcclusionEnabled(), "release refuses godray enable even from an old preference");
        Require(!FO4CS::GodraysIntegration::TryInstall(FO4CS::F4SECompat::RuntimeTarget::kLegacy), "release does not install a godray hook");
        Require(!FO4CS::GodraysIntegration::GetDiagnostics().renderVolumeHookInstalled, "release godray diagnostics report unavailable");
        {
            namespace Enb = FO4CS::EnbCompat;
            const auto writeIni = [&](const wchar_t* name, const char* text) {
                const auto path = root / name;
                std::ofstream output(path, std::ios::binary);
                output << text;
                return path;
            };
            Require(Enb::CloudShadowsActiveAtStartup(writeIni(L"enb-on.ini",
                "[GLOBAL]\r\nUseEffect=true\r\n[EFFECT]\r\nEnableCloudShadows=true\r\n")),
                "ENB with its cloud shadows on: this mod stands down");
            Require(!Enb::CloudShadowsActiveAtStartup(writeIni(L"enb-off.ini",
                "[GLOBAL]\r\nUseEffect=true\r\n[EFFECT]\r\nEnableCloudShadows=false\r\n")),
                "ENB with its cloud shadows off: this mod runs");
            Require(!Enb::CloudShadowsActiveAtStartup(writeIni(L"enb-effects-off.ini",
                "[GLOBAL]\r\nUseEffect=false\r\n[EFFECT]\r\nEnableCloudShadows=true\r\n")),
                "ENB effects disabled at startup: this mod runs");
            Require(!Enb::CloudShadowsActiveAtStartup(writeIni(L"enb-case.ini",
                "[GLOBAL]\r\nUseEffect=TRUE\r\n[EFFECT]\r\nEnableCloudShadows=False\r\n")),
                "ENB booleans are case-insensitive");
            const auto missingKey = writeIni(L"enb-missing-key.ini",
                "[GLOBAL]\r\nUseEffect=true\r\n[EFFECT]\r\nEnableBloom=false\r\n");
            Require(!Enb::CloudShadowsActiveAtStartup(missingKey) &&
                !Enb::CloudShadowsSettingPresent(missingKey),
                "a preset without EnableCloudShadows does not make this mod inert");
            Require(!Enb::CloudShadowsActiveAtStartup(root / L"no-such-enbseries.ini"),
                "a missing enbseries.ini does not make this mod inert");
            Require(Enb::CloudShadowsSettingPresent(root / L"enb-off.ini"),
                "an explicit EnableCloudShadows=false is recognised");
            Require(!Enb::Detect().present, "ENB is not loaded in the test process");
            Require(Enb::VersionText(501) == "0.501" && Enb::VersionText(1025) == "1.025",
                "ENB version formatting");
        }
        std::cout << "MCM persistence, release defaults, temporary toggles and disabled godrays passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    // The test owns this uniquely created directory; delete individual files.
    std::error_code ignored;
    for (const auto& file : fs::directory_iterator(root)) fs::remove(file.path(), ignored);
    fs::remove(root, ignored);
    return result;
}
