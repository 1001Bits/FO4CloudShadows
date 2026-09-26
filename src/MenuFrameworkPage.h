// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace FO4CS::MenuFrameworkPage
{
    // Registers the Cloud Shadows settings page with F4SE Menu Framework when
    // that optional flat-game menu host is loaded. Call once at kPostPostLoad.
    void Install() noexcept;
}
