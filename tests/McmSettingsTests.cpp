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
        Files::Write(user, { false, 3.5f, 45000.0f, 1 });
        const auto saved = Files::Read(defaults, user);
        Require(saved == Files::Values{ false, 3.5f, 45000.0f, 1 }, "F11 save round trip including capture method");
        Require(GetPrivateProfileIntW(L"Future", L"Keep", 0, user.c_str()) == 123, "save preserves other sections");
        bool refused = false;
        try { Files::Write(user, {}, false); } catch (...) { refused = true; }
        Require(refused && Files::Read(defaults, user) == saved, "migration cannot overwrite an existing MCM preference");

        const auto unchanged = Files::Difference(saved, saved, false);
        Require(!unchanged.Any(), "polling unchanged preferences must preserve live F10/unsaved F11 state");
        auto changed = saved;
        changed.cloudHeight = 50000;
        const auto heightOnly = Files::Difference(saved, changed, false);
        Require(heightOnly.cloudHeight && !heightOnly.enabled && !heightOnly.opacity && !heightOnly.captureMethod,
            "height edit must not revert F10, opacity or method");
        auto methodChanged = saved;
        methodChanged.captureMethod = 0;
        const auto methodOnly = Files::Difference(saved, methodChanged, false);
        Require(methodOnly.captureMethod && !methodOnly.enabled && !methodOnly.opacity && !methodOnly.cloudHeight,
            "method edit applies alone");
        Require(WritePrivateProfileStringW(L"CloudShadows", L"iCaptureMethod", L"2", user.c_str()), "write invalid method");
        {
            bool rejectedMethod = false;
            try { (void)Files::Read(defaults, user); } catch (...) { rejectedMethod = true; }
            Require(rejectedMethod, "capture method must be 0 or 1");
        }
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
            legacy.at("CaptureMethod") == canonical.captureMethod &&
            legacy.at("Hotkeys") == canonical.hotkeys,
            "legacy fallback agrees with release defaults");
        std::ifstream menuInput(project / "config/MCM/FO4CloudShadows/config.json");
        const auto menu = nlohmann::json::parse(menuInput);
        Require(menu.at("modName") == "FO4CloudShadows", "MCM folder identity");
        std::set<std::string> controls;
        for (const auto& row : menu.at("content")) if (row.contains("id")) {
            Require(controls.insert(row.at("id").get<std::string>()).second, "unique MCM setting ids");
        }
        Require(controls == std::set<std::string>{ "bEnabled:CloudShadows", "fOpacity:CloudShadows",
            "iCaptureMethod:CloudShadows", "bHotkeys:CloudShadows" },
            "MCM exposes only the four release controls (cloud height is not user-adjustable)");
        Require(canonical.hotkeys == false, "hotkeys are off by default");
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
            Require(Enb::CloudShadowsActiveAtStartup(writeIni(L"enb-missing-key.ini",
                "[GLOBAL]\r\nUseEffect=true\r\n[EFFECT]\r\nEnableBloom=false\r\n")),
                "a missing EnableCloudShadows follows ENB's stock preset (on)");
            Require(Enb::CloudShadowsActiveAtStartup(root / L"no-such-enbseries.ini"),
                "a missing enbseries.ini follows ENB's stock preset (on)");
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
