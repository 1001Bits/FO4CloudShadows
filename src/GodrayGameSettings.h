#pragma once

#include <filesystem>
#include <string>

namespace FO4CS::GodrayGameSettings
{
    struct State
    {
        bool available{ false };
        bool enabled{ false };
    };

    struct EnableResult
    {
        bool succeeded{ false };
        bool changed{ false };
        std::string error;
    };

    // Flat Fallout's normal INI location; Windows' Documents redirection and
    // the process's mod-manager file virtualization remain in effect.
    [[nodiscard]] std::filesystem::path Directory();
    [[nodiscard]] State Read(const std::filesystem::path& directory) noexcept;
    // Enable the two native flags for the next game launch. Back up the original
    // INIs once, atomically replace each, and roll back both if either write fails. Other keys stay
    // untouched. An explicit directory also permits isolated fixture tests.
    [[nodiscard]] EnableResult Enable(const std::filesystem::path& directory) noexcept;
#if defined(FO4CS_SETTINGS_TESTING)
    namespace Testing
    {
        // Test executable only: simulate termination after the sibling file
        // is flushed and closed, before replacing the original.
        extern void (*beforeReplace)();
    }
#endif
}
