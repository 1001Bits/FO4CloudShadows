// Exercise real INI writes on private temporary files, never the game's files.
#include "GodrayGameSettings.h"
#include <Windows.h>

#include <filesystem>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace Settings = FO4CS::GodrayGameSettings;

void Require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

std::string Read(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), {} };
}

void Write(const fs::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(file.good(), "write test fixture");
}

struct Fixture
{
    fs::path root = fs::temp_directory_path() /
        (L"FO4CS-godray-settings-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    fs::path game = root / "Fallout4.ini";
    fs::path prefs = root / "Fallout4Prefs.ini";
    Fixture() { Require(fs::create_directory(root), "create private fixture directory"); }
    ~Fixture()
    {
        // Only individual fixture files are removed; no recursive deletion.
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(root, error)) {
            SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
            fs::remove(entry.path(), error);
        }
        fs::remove(root, error);
    }
};

std::string Utf16(const std::wstring& text)
{
    return std::string("\xFF\xFE", 2) +
        std::string(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
}

void VerifyInterruptedWrite(const Fixture& files,
    const std::string& originalGame, const std::string& originalPrefs,
    bool afterFirstReplacement = false)
{
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    Require(length != 0 && length < executable.size(), "locate child test executable");
    auto command = L"\"" + std::wstring(executable.data(), length) +
        (afterFirstReplacement ? L"\" --interrupt-second-replace \"" :
            L"\" --interrupt-before-replace \"") + files.root.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process), "start interrupted-write child");
    const auto wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 88);
        WaitForSingleObject(process.hProcess, 1000);
    }
    DWORD status = 0;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require(wait == WAIT_OBJECT_0 && status == 87, "child terminated at the pre-replacement boundary");
    auto expectedGame = originalGame;
    if (afterFirstReplacement)
        expectedGame[expectedGame.find("bNvGodraysEnable=0") + std::string("bNvGodraysEnable=").size()] = '1';
    Require(Read(files.game) == expectedGame && Read(files.prefs) == originalPrefs,
        "process termination leaves each INI complete at either replacement boundary");
    if (afterFirstReplacement) {
        Require(Settings::Enable(files.root).succeeded && Settings::Read(files.root).enabled,
            "an interrupted two-file update recovers on the next enable");
        Write(files.game, originalGame);
        Write(files.prefs, originalPrefs);
    }
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--interrupt-before-replace") {
            Settings::Testing::beforeReplace = [] { ExitProcess(87); };
            (void)Settings::Enable(fs::path(argv[2]));
            return 89;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--interrupt-second-replace") {
            Settings::Testing::beforeReplace = [] {
                static unsigned replacements = 0;
                if (++replacements == 2) ExitProcess(87);
            };
            (void)Settings::Enable(fs::path(argv[2]));
            return 89;
        }
        Fixture files;
        const std::string originalGame =
            "; keep this comment\r\n[Display]\r\nbNvGodraysEnable=0\r\n"
            "fShadowDistance=3000\r\n[Other]\r\nsName=unchanged\r\n";
        const std::string originalPrefs =
            "[Display]\r\nbVolumetricLightingEnable=0\r\niVolumetricLightingQuality=0\r\n";
        Write(files.game, originalGame);
        Write(files.prefs, originalPrefs);
        VerifyInterruptedWrite(files, originalGame, originalPrefs);
        VerifyInterruptedWrite(files, originalGame, originalPrefs, true);
        Require(Settings::Read(files.root).available && !Settings::Read(files.root).enabled,
            "detect existing disabled native settings");
        const auto first = Settings::Enable(files.root);
        Require(first.succeeded && first.changed, "enable both native godray flags");
        Require(Settings::Read(files.root).enabled, "verify both settings through native INI API");
        Require(Read(files.game).find("fShadowDistance=3000") != std::string::npos &&
            Read(files.game).find("; keep this comment") != std::string::npos &&
            Read(files.prefs).find("iVolumetricLightingQuality=0") != std::string::npos,
            "retain comments, shadow range, and selected godray quality");
        const auto backupGame = fs::path(files.game.wstring() + L".FO4CloudShadows.godrays.bak");
        const auto backupPrefs = fs::path(files.prefs.wstring() + L".FO4CloudShadows.godrays.bak");
        Require(Read(backupGame) == originalGame && Read(backupPrefs) == originalPrefs,
            "retain exact original INI bytes in backups");
        const auto gameAfter = Read(files.game);
        const auto prefsAfter = Read(files.prefs);
        const auto again = Settings::Enable(files.root);
        Require(again.succeeded && !again.changed && Read(files.game) == gameAfter &&
            Read(files.prefs) == prefsAfter, "already-enabled settings are not rewritten");

        // Fail the second write and require byte-exact restoration of the first.
        Require(WritePrivateProfileStringW(L"Display", L"bNvGodraysEnable", L"0", files.game.c_str()) &&
            WritePrivateProfileStringW(L"Display", L"bVolumetricLightingEnable", L"0", files.prefs.c_str()),
            "reset native flags");
        const auto beforeFailure = Read(files.game);
        const auto prefsBeforeFailure = Read(files.prefs);
        Require(SetFileAttributesW(files.prefs.c_str(), FILE_ATTRIBUTE_READONLY), "make second INI read-only");
        const auto denied = Settings::Enable(files.root);
        Require(!denied.succeeded && !denied.error.empty(), "report a denied INI update");
        Require(Read(files.game) == beforeFailure && Read(files.prefs) == prefsBeforeFailure,
            "failed second write rolls back both files");
        SetFileAttributesW(files.prefs.c_str(), FILE_ATTRIBUTE_NORMAL);
        HANDLE heldPrefs = CreateFileW(files.prefs.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(heldPrefs != INVALID_HANDLE_VALUE, "hold second INI without delete sharing");
        const auto sharingDenied = Settings::Enable(files.root);
        CloseHandle(heldPrefs);
        Require(!sharingDenied.succeeded && Read(files.game) == beforeFailure &&
            Read(files.prefs) == prefsBeforeFailure,
            "replacement sharing failure preserves destination and rolls back the first INI");
        fs::remove(files.prefs);
        Require(!Settings::Enable(files.root).succeeded && Read(files.game) == beforeFailure &&
            !fs::exists(files.prefs), "missing INI fails without creating or changing game configuration");

        // The native API must preserve a Unicode INI and unrelated Unicode data.
        Write(files.game, Utf16(L"[Display]\r\nbNvGodraysEnable=0\r\nsNote=\u03A9\r\n"));
        Write(files.prefs, Utf16(L"[Display]\r\nbVolumetricLightingEnable=0\r\n"));
        Require(Settings::Enable(files.root).succeeded, "enable Unicode INI files");
        const auto unicode = Read(files.game);
        Require(unicode.starts_with(std::string("\xFF\xFE", 2)) &&
            unicode.find(Utf16(L"sNote=\u03A9").substr(2)) != std::string::npos,
            "preserve UTF-16 encoding and unrelated Unicode values");
        Require(Read(backupGame) == originalGame, "do not overwrite original backups on later enables");
        // A UTF-8 BOM must not make the writer duplicate the first section.
        const std::string utf8 = "\xEF\xBB\xBF[Display]\n"
            "bNvGodraysEnable = 0 ; keep inline comment\nsNote=Caf\xC3\xA9\n";
        Write(files.game, utf8);
        Write(files.prefs, "[Display]\nbVolumetricLightingEnable=0\n");
        Require(Settings::Enable(files.root).succeeded, "enable UTF-8 BOM INIs");
        auto expectedUtf8 = utf8;
        expectedUtf8[expectedUtf8.find("= 0") + 2] = '1';
        Require(Read(files.game) == expectedUtf8, "UTF-8 changes only the requested flag byte");
        Write(files.game, "[General]\nsNote=keep\n[DISPLAY]");
        Write(files.prefs, "[General]\nsNote=keep\n");
        Require(Settings::Enable(files.root).succeeded && Settings::Read(files.root).enabled,
            "insert missing native flags and sections, including a header without final newline");
        Write(files.game, "[Display]\nbNvGodraysEnable=0\n[Display]\nbNvGodraysEnable=1\n");
        Require(Settings::Enable(files.root).succeeded && Read(files.game).find("Enable=0") == std::string::npos,
            "duplicate target flags become consistent without changing section structure");
        std::cout << "PASS: native godray settings atomic replacement, interruption, sharing failure, preservation, backups, rollback, Unicode\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
