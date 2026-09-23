#include "GodrayGameSettings.h"

#include <Windows.h>
#include <ShlObj.h>

#include <array>
#include <atomic>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace FO4CS::GodrayGameSettings
{
#if defined(FO4CS_SETTINGS_TESTING)
    namespace Testing
    {
        void (*beforeReplace)() = nullptr;
    }
#endif
    namespace
    {
        constexpr std::array names{ L"Fallout4.ini", L"Fallout4Prefs.ini" };
        constexpr std::array keys{ "bNvGodraysEnable", "bVolumetricLightingEnable" };

        std::string Bytes(const std::filesystem::path& path)
        {
            if (!std::filesystem::is_regular_file(path) ||
                std::filesystem::file_size(path) > 1024 * 1024)
                throw std::runtime_error("Native godray INI is missing or too large");
            std::ifstream file(path, std::ios::binary);
            if (!file)
                throw std::runtime_error("Cannot read native godray INI");
            std::string result{ std::istreambuf_iterator<char>(file), {} };
            if (file.bad())
                throw std::runtime_error("Failed reading native godray INI");
            return result;
        }

        template<class C>
        bool Space(C c)
        {
            return c == C(' ') || c == C('\t') || c == C('\r') || c == C('\n');
        }

        template<class C>
        std::basic_string_view<C> Trim(std::basic_string_view<C> text)
        {
            while (!text.empty() && Space(text.front())) text.remove_prefix(1);
            while (!text.empty() && Space(text.back())) text.remove_suffix(1);
            return text;
        }

        template<class C>
        bool Equal(std::basic_string_view<C> text, std::string_view expected)
        {
            if (text.size() != expected.size()) return false;
            for (std::size_t i = 0; i < text.size(); ++i) {
                auto a = text[i];
                auto b = expected[i];
                if (a >= C('A') && a <= C('Z')) a += C('a' - 'A');
                if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
                if (a != C(b)) return false;
            }
            return true;
        }

        // Work on lines so comments, unrelated values, BOMs, and line endings
        // survive. Win32's profile writer duplicates [Display] for UTF-8 BOM
        // files, so it cannot safely perform this update on all supported INIs.
        template<class C>
        std::pair<std::basic_string<C>, bool> Edit(
            const std::basic_string<C>& source, std::string_view key, bool enable)
        {
            using View = std::basic_string_view<C>;
            const auto widen = [](std::string_view value) {
                return std::basic_string<C>(value.begin(), value.end());
            };
            const auto newline = source.find(widen("\r\n")) != source.npos ?
                widen("\r\n") : widen("\n");
            std::basic_string<C> output;
            bool inDisplay = false, found = false, allEnabled = true;
            std::size_t insertion = source.npos;
            for (std::size_t start = 0; start < source.size();) {
                const auto lf = source.find(C('\n'), start);
                const auto end = lf == source.npos ? source.size() : lf + 1;
                const View line(source.data() + start, end - start);
                const auto trimmed = Trim(line);
                if (!trimmed.empty() && trimmed.front() == C('[')) {
                    const auto close = trimmed.find(C(']'));
                    inDisplay = close != View::npos &&
                        Equal(Trim(trimmed.substr(1, close - 1)), "Display");
                    output.append(line);
                    if (inDisplay && insertion == source.npos) insertion = output.size();
                } else {
                    const auto equal = line.find(C('='));
                    if (inDisplay && equal != View::npos && Equal(Trim(line.substr(0, equal)), key)) {
                        found = true;
                        std::size_t first = equal + 1, last = line.size();
                        for (std::size_t i = first; i < last; ++i) {
                            if (line[i] == C(';') || line[i] == C('#')) { last = i; break; }
                        }
                        while (last > first && Space(line[last - 1])) --last;
                        while (first < last && Space(line[first])) ++first;
                        const bool enabled = Equal(line.substr(first, last - first), "1");
                        allEnabled &= enabled;
                        if (enable && !enabled) {
                            output.append(line.substr(0, first));
                            output.push_back(C('1'));
                            output.append(line.substr(last));
                        } else output.append(line);
                    } else output.append(line);
                }
                start = end;
            }
            if (enable && !found) {
                if (insertion == source.npos) {
                    if (!output.empty() && output.back() != C('\n')) output += newline;
                    output += widen("[Display]") + newline + widen(key) + widen("=1") + newline;
                } else {
                    auto added = widen(key) + widen("=1") + newline;
                    if (insertion != 0 && output[insertion - 1] != C('\n')) added = newline + added;
                    output.insert(insertion, added);
                }
            }
            return { std::move(output), found && allEnabled };
        }

        std::pair<std::string, bool> EditBytes(
            const std::string& source, std::string_view key, bool enable)
        {
            if (source.starts_with("\xFF\xFE") || source.starts_with("\xFE\xFF")) {
                if (source.size() % 2 != 0) throw std::runtime_error("Truncated UTF-16 INI");
                const bool bigEndian = static_cast<unsigned char>(source[0]) == 0xfe;
                std::u16string decoded;
                for (std::size_t i = 2; i < source.size(); i += 2) {
                    const auto a = static_cast<unsigned char>(source[i]);
                    const auto b = static_cast<unsigned char>(source[i + 1]);
                    decoded.push_back(static_cast<char16_t>(bigEndian ? (a << 8) | b : a | (b << 8)));
                }
                auto [updated, enabled] = Edit(decoded, key, enable);
                std::string result = source.substr(0, 2);
                for (const auto c : updated) {
                    result.push_back(static_cast<char>(bigEndian ? c >> 8 : c & 0xff));
                    result.push_back(static_cast<char>(bigEndian ? c & 0xff : c >> 8));
                }
                return { std::move(result), enabled };
            }
            const std::size_t bom = source.starts_with("\xEF\xBB\xBF") ? 3 : 0;
            auto [updated, enabled] = Edit(source.substr(bom), key, enable);
            return { source.substr(0, bom) + updated, enabled };
        }

        void Write(const std::filesystem::path& path, const std::string& bytes)
        {
            // Keep the destination intact until a complete sibling file has
            // reached disk. A same-volume rename has no truncate/rewrite
            // window. Do not allow a cross-volume copy/delete fallback.
            // The sibling path stays in the same mod-manager namespace.
            static std::atomic<uint64_t> serial{ 0 };
            struct PendingFile
            {
                std::filesystem::path path;
                HANDLE handle{ INVALID_HANDLE_VALUE };
                ~PendingFile()
                {
                    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
                    if (!path.empty()) DeleteFileW(path.c_str());
                }
            } temporary;
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                temporary.path = path.wstring() + L".FO4CS." +
                    std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(serial.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
                temporary.handle = CreateFileW(temporary.path.c_str(),
                    GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (temporary.handle != INVALID_HANDLE_VALUE) break;
                const auto error = GetLastError();
                temporary.path.clear(); // Never remove a file we did not create.
                if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                    break;
            }
            if (temporary.handle == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cannot create temporary native godray INI");
            DWORD written = 0;
            if (bytes.size() > MAXDWORD ||
                !WriteFile(temporary.handle, bytes.data(),
                    static_cast<DWORD>(bytes.size()), &written, nullptr) ||
                written != bytes.size() || !FlushFileBuffers(temporary.handle)) {
                throw std::runtime_error("Cannot flush temporary native godray INI");
            }
            if (!CloseHandle(temporary.handle))
                throw std::runtime_error("Cannot close temporary native godray INI");
            temporary.handle = INVALID_HANDLE_VALUE;
#if defined(FO4CS_SETTINGS_TESTING)
            if (Testing::beforeReplace) Testing::beforeReplace();
#endif
            if (!MoveFileExW(temporary.path.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                throw std::runtime_error("Cannot atomically replace native godray INI");
            }
            temporary.path.clear();
        }

        bool Restore(const std::filesystem::path& path, const std::string& original) noexcept
        {
            try {
                if (Bytes(path) == original)
                    return true;
                Write(path, original);
                return Bytes(path) == original;
            } catch (...) {
                return false;
            }
        }
    }

    std::filesystem::path Directory()
    {
        PWSTR documents = nullptr;
        const HRESULT result = SHGetKnownFolderPath(
            FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents);
        if (FAILED(result) || !documents) {
            CoTaskMemFree(documents);
            throw std::runtime_error("Cannot locate the Fallout INI directory");
        }
        std::filesystem::path path;
        try {
            path = std::filesystem::path(documents) / L"My Games" / L"Fallout4";
        } catch (...) {
            CoTaskMemFree(documents);
            throw;
        }
        CoTaskMemFree(documents);
        return path;
    }

    State Read(const std::filesystem::path& directory) noexcept
    {
        try {
            State state{ true, true };
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto path = directory / names[i];
                if (!std::filesystem::is_regular_file(path))
                    return {};
                state.enabled &= EditBytes(Bytes(path), keys[i], false).second;
            }
            return state;
        } catch (...) {
            return {};
        }
    }

    EnableResult Enable(const std::filesystem::path& directory) noexcept
    {
        std::array<std::filesystem::path, 2> paths;
        std::array<std::string, 2> originals;
        std::array<std::string, 2> updated;
        bool writesStarted = false;
        try {
            // Complete all reads and backups before changing either setting.
            for (std::size_t i = 0; i < names.size(); ++i) {
                paths[i] = directory / names[i];
                originals[i] = Bytes(paths[i]);
                updated[i] = EditBytes(originals[i], keys[i], true).first;
            }
            if (updated == originals) return { true, false, {} };
            for (const auto& path : paths) {
                const auto backup = std::filesystem::path(
                    path.wstring() + L".FO4CloudShadows.godrays.bak");
                if (!std::filesystem::exists(backup) &&
                    !CopyFileW(path.c_str(), backup.c_str(), TRUE))
                    throw std::runtime_error("Cannot back up native godray INI");
                if (!std::filesystem::is_regular_file(backup))
                    throw std::runtime_error("Native godray INI backup is not a file");
            }
            writesStarted = true;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (updated[i] != originals[i]) Write(paths[i], updated[i]);
            }
            if (!Read(directory).enabled)
                throw std::runtime_error("Native godray INI verification failed");
            return { true, true, {} };
        } catch (const std::exception& error) {
            bool restored = true;
            if (writesStarted) {
                for (std::size_t i = 0; i < names.size(); ++i)
                    restored = Restore(paths[i], originals[i]) && restored;
            }
            return { false, false, std::string(error.what()) +
                (restored ? "" : "; rollback failed, original INI backups retained") };
        }
    }
}
