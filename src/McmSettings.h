// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace FO4CS::McmSettings
{
    // Initializes paths and migrates an existing version-4 JSON once, before
    // MCM's plugin-load settings scan. Never replaces an existing user INI.
    void Initialize() noexcept;
    bool Available() noexcept;
    // Render-thread operations. False preserves the complete live snapshot.
    bool Load(bool force) noexcept;
    bool Save() noexcept;
}
