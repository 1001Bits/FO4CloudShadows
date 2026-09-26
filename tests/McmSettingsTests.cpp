// SPDX-License-Identifier: GPL-3.0-only
#include "McmSettingsFile.h"
#include "GodraysIntegration.h"
#include "EnbCompat.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <set>

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
        Files::Write(defaults, { true, 2.0f, 10000.0f });
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
            development.debugView = 1;
            development.isolateCloud = true;
            development.isolationRadius = 20.0f;
            Require(!Files::LegacyMigration(development).has_value(),
                "development options are never migrated from the legacy JSON");
        }
        {
            // Development Menu options round-trip, are bounded, and a
            // malformed debug view is rejected instead of half-applied.
            const auto developmentFile = root / L"development.ini";
            Files::Values development;
            development.hotkeys = true;
            development.debugView = 3;
            development.isolateCloud = true;
            development.isolationRadius = 18.5f;
            Files::Write(developmentFile, development);
            Require(Files::Read(defaults, developmentFile) == Files::Values{
                    true, 2.0f, 10000.0f, true, 3, true, 18.5f },
                "development options round trip");
            Files::Values bounded = development;
            bounded.debugView = 9;
            bounded.isolationRadius = 90.0f;
            Require(Files::Validate(bounded).debugView == 4 &&
                    Files::Validate(bounded).isolationRadius == 30.0f,
                "development options are bounded");
            Require(WritePrivateProfileStringW(L"CloudShadows", L"iDebugView", L"7",
                developmentFile.c_str()), "write invalid debug view");
            bool rejectedView = false;
            try { (void)Files::Read(defaults, developmentFile); } catch (...) { rejectedView = true; }
            Require(rejectedView, "debug view must be 0 to 4");
            auto viewChanged = development;
            viewChanged.debugView = 1;
            const auto viewOnly = Files::Difference(development, viewChanged, false);
            Require(viewOnly.debugView && !viewOnly.hotkeys && !viewOnly.opacity &&
                    !viewOnly.isolateCloud && !viewOnly.isolationRadius,
                "a debug view change applies alone");
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
        const std::set<std::string> developmentControls{ "bHotkeys:CloudShadows",
            "iDebugView:CloudShadows", "bIsolateCloud:CloudShadows",
            "fIsolationRadius:CloudShadows" };
        std::set<std::string> expectedControls{ "bEnabled:CloudShadows", "fOpacity:CloudShadows" };
        expectedControls.insert(developmentControls.begin(), developmentControls.end());
        Require(controls == expectedControls,
            "MCM exposes the player controls and the Development Menu options "
            "(cloud height is not user-adjustable)");
        Require(canonical.hotkeys == false, "hotkeys are off by default");
        // Development options stay hidden until their controlling switch is on.
        for (const auto& row : menu.at("content")) {
            if (row.contains("id")) {
                const auto id = row.at("id").get<std::string>();
                const bool development = developmentControls.contains(id);
                if (id == "bHotkeys:CloudShadows")
                    Require(row.value("groupControl", 0) == 1,
                        "the Development Menu switch controls its options' visibility");
                else if (development)
                    Require(row.value("groupCondition", 0) == 1,
                        "development options are hidden until the Development Menu is on");
            }
        }
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
