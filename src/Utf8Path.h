#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>

namespace FO4CS
{
    // UTF-8 text for logs and UTF-8 APIs. std::filesystem::path::string()
    // converts through the ANSI code page and throws for characters it cannot
    // represent (for example a Cyrillic or CJK folder on a Western-locale
    // Windows). This conversion never throws; an unconvertible path yields "".
    [[nodiscard]] inline std::string Utf8Path(const std::filesystem::path& path) noexcept
    {
        try {
            const auto& wide = path.native();
            if (wide.empty())
                return {};
            const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (length <= 0)
                return {};
            std::string utf8(static_cast<std::size_t>(length), '\0');
            if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                    utf8.data(), length, nullptr, nullptr) != length)
                return {};
            return utf8;
        } catch (...) {
            return {};
        }
    }
}
