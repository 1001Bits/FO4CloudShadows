
# Source-level release contract, run by the validation-build target. It checks
# only files this repository contains; packaging-only inputs (licences, notices,
# release scripts) are verified by the release package when present.
if(NOT DEFINED PROJECT_ROOT OR NOT DEFINED EXPECTED_VERSION)
    message(FATAL_ERROR "PROJECT_ROOT and EXPECTED_VERSION are required")
endif()

function(require_file relative_path)
    if(NOT EXISTS "${PROJECT_ROOT}/${relative_path}")
        message(FATAL_ERROR "Required release file is missing: ${relative_path}")
    endif()
endfunction()

function(require_match relative_path regex description)
    require_file("${relative_path}")
    file(READ "${PROJECT_ROOT}/${relative_path}" _content)
    if(NOT _content MATCHES "${regex}")
        message(FATAL_ERROR "${relative_path}: ${description}")
    endif()
endfunction()

function(reject_match relative_path regex description)
    require_file("${relative_path}")
    file(READ "${PROJECT_ROOT}/${relative_path}" _content)
    if(_content MATCHES "${regex}")
        message(FATAL_ERROR "${relative_path}: ${description}")
    endif()
endfunction()

string(REPLACE "." "\\." _regex_version "${EXPECTED_VERSION}")

require_match("CMakeLists.txt"
    "project\\(FO4CloudShadows VERSION ${_regex_version}"
    "project version does not equal ${EXPECTED_VERSION}")
require_match("CMakeLists.txt" "[/]W4[ \t]+[/]WX"
    "project warnings are not configured as errors")
require_match("CMakeLists.txt"
    "option\\(FO4CS_ENABLE_DEVELOPER_TOOLS[^)]*OFF\\)"
    "developer tools must default to OFF")
require_match("CMakeLists.txt"
    "option\\(FO4CS_ENABLE_PRIVATE_PROFILING[^)]*OFF\\)"
    "private profiling must default to OFF")

foreach(_shader IN ITEMS
        FO4CloudShadowScreenCS.hlsl WorldCloudTileVS.hlsl WorldCloudTilePS.hlsl
        WorldCloudSkyVS.hlsl WorldCloudSkyPS.hlsl)
    require_file("shaders/CloudShadows/${_shader}")
endforeach()
require_file("shaders/Features/CloudShadows.ini")
require_file("assets/Fonts/Jost/OFL.txt")

# The Development Menu (F11 and every hotkey) is off for players by default.
require_match("config/MCM/FO4CloudShadows/settings.ini" "bHotkeys=0"
    "the Development Menu must default to off in MCM")
require_match("config/CloudShadows.json" "\"Hotkeys\": false"
    "the legacy settings default must keep hotkeys off")
require_match("config/MCM/FO4CloudShadows/config.json" "\"Development Menu\""
    "MCM must expose the Development Menu section")

# The settings JSON is shipped with default values only.
require_match("config/CloudShadows.json" "\"ProjectionModelVersion\": 4"
    "settings schema version changed without a migration")

# The natural water-reflection cubemap producer was removed in 1.0.x; its
# ClickCubeMap hook must not return.
reject_match("src/Plugin.cpp" "ClickCubeMap"
    "the removed native cubemap producer is referenced again")

message(STATUS "Release contract satisfied for ${EXPECTED_VERSION}")
