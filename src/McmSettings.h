// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "McmSettingsFile.h"

#include <cstdint>

namespace FO4CS::McmSettings
{
    // Initializes paths and migrates an existing version-4 JSON once, before
    // MCM's plugin-load settings scan. Never replaces an existing user INI.
    void Initialize() noexcept;
    bool Available() noexcept;
    // Render-thread operations. False preserves the complete live snapshot.
    bool Load(bool force) noexcept;
    // Queues the live values on the background writer; fields changed only
    // in the file since it was last applied are kept.
    bool Save() noexcept;
    // File-only access for menus that run off the render thread (F4SE Menu
    // Framework). They never touch live state: a written file is applied by
    // the render thread's next Load poll, exactly like an MCM change.
    bool ReadSaved(McmSettingsFile::Values& values) noexcept;
    // Queues a write and returns its ticket (0 when it cannot be queued).
    std::uint64_t WriteSaved(const McmSettingsFile::Values& values) noexcept;
    // True once the write for `ticket` (or a later one) has failed.
    bool SaveFailed(std::uint64_t ticket) noexcept;
}
