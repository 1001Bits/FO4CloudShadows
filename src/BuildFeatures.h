// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifndef FO4CS_ENABLE_DEVELOPER_TOOLS
#define FO4CS_ENABLE_DEVELOPER_TOOLS 0
#endif
#ifndef FO4CS_ENABLE_PRIVATE_PROFILING
#define FO4CS_ENABLE_PRIVATE_PROFILING 0
#endif

#define FO4CS_EXPERIMENTAL_FEATURES (FO4CS_ENABLE_DEVELOPER_TOOLS || FO4CS_ENABLE_PRIVATE_PROFILING)

namespace FO4CS::BuildFeatures
{
    inline constexpr bool kDeveloperTools = FO4CS_ENABLE_DEVELOPER_TOOLS != 0;
    inline constexpr bool kExperimental = FO4CS_EXPERIMENTAL_FEATURES != 0;

    // Kept in the loaded DLL and checked by the packaging gate, independently
    // of a potentially stale CMake cache or a developer's staging directory.
#if FO4CS_ENABLE_DEVELOPER_TOOLS
#define FO4CS_DEVELOPER_MARKER "developer_tools=1;"
#else
#define FO4CS_DEVELOPER_MARKER "developer_tools=0;"
#endif
#if FO4CS_ENABLE_PRIVATE_PROFILING
    inline constexpr char kMarker[] = "FO4CS_BUILD_FEATURES:" FO4CS_DEVELOPER_MARKER "private_profiling=1";
#else
    inline constexpr char kMarker[] = "FO4CS_BUILD_FEATURES:" FO4CS_DEVELOPER_MARKER "private_profiling=0";
#endif
#undef FO4CS_DEVELOPER_MARKER
}
