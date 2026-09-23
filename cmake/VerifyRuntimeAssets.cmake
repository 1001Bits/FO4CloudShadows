if(NOT DEFINED STAGE_ROOT OR NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "STAGE_ROOT and SOURCE_ROOT are required")
endif()

file(REAL_PATH "${STAGE_ROOT}" _stage_root)
file(REAL_PATH "${SOURCE_ROOT}" _source_root)
if(NOT IS_DIRECTORY "${_stage_root}" OR NOT IS_DIRECTORY "${_source_root}")
    message(FATAL_ERROR "Runtime-asset roots must be existing directories")
endif()

set(_shader_names
    FO4CloudShadowScreenCS.hlsl
    WorldCloudTileVS.hlsl
    WorldCloudTilePS.hlsl
    WorldCloudSkyVS.hlsl
    WorldCloudSkyPS.hlsl)
set(_manifest_entries "")
foreach(_name IN LISTS _shader_names)
    set(_source "${_source_root}/shaders/CloudShadows/${_name}")
    set(_staged "${_stage_root}/Data/Shaders/CloudShadows/${_name}")
    if(NOT EXISTS "${_source}" OR NOT EXISTS "${_staged}")
        message(FATAL_ERROR "Runtime shader is missing: ${_name}")
    endif()
    file(SHA256 "${_source}" _source_hash)
    file(SHA256 "${_staged}" _staged_hash)
    if(NOT _source_hash STREQUAL _staged_hash)
        message(FATAL_ERROR "Staged runtime shader is stale: ${_name}")
    endif()
    string(APPEND _manifest_entries
        "    \"${_name}\": \"${_source_hash}\",\n")
endforeach()

set(_asset_pairs
    "shaders/Features/CloudShadows.ini|Data/Shaders/Features/CloudShadows.ini"
    "config/CloudShadows.json|Data/Shaders/Features/CloudShadows.json"
    "config/MCM/FO4CloudShadows/config.json|Data/MCM/Config/FO4CloudShadows/config.json"
    "config/MCM/FO4CloudShadows/settings.ini|Data/MCM/Config/FO4CloudShadows/settings.ini"
    "assets/Fonts/Jost/Jost-Light.ttf|Data/Interface/FO4CloudShadows/Fonts/Jost/Jost-Light.ttf"
    "assets/Fonts/Jost/Jost-Regular.ttf|Data/Interface/FO4CloudShadows/Fonts/Jost/Jost-Regular.ttf"
    "assets/Fonts/Jost/OFL.txt|Data/Interface/FO4CloudShadows/Fonts/Jost/OFL.txt")
foreach(_pair IN LISTS _asset_pairs)
    string(REPLACE "|" ";" _parts "${_pair}")
    list(GET _parts 0 _source_relative)
    list(GET _parts 1 _staged_relative)
    set(_source "${_source_root}/${_source_relative}")
    set(_staged "${_stage_root}/${_staged_relative}")
    if(NOT EXISTS "${_source}" OR NOT EXISTS "${_staged}")
        message(FATAL_ERROR "Runtime asset is missing: ${_staged_relative}")
    endif()
    file(SHA256 "${_source}" _source_hash)
    file(SHA256 "${_staged}" _staged_hash)
    if(NOT _source_hash STREQUAL _staged_hash)
        message(FATAL_ERROR "Staged runtime asset is stale: ${_staged_relative}")
    endif()
endforeach()

if(NOT DEFINED EXPECTED_VERSION OR
   NOT EXPECTED_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "EXPECTED_VERSION must be a quoted major.minor.patch value")
endif()
string(REGEX REPLACE ",\n$" "\n" _manifest_entries "${_manifest_entries}")
set(_manifest_directory "${_stage_root}/Data/Shaders/Features")
file(MAKE_DIRECTORY "${_manifest_directory}")
file(WRITE "${_manifest_directory}/FO4CloudShadows.assets.json"
    "{\n"
    "  \"SchemaVersion\": 1,\n"
    "  \"PluginVersion\": \"${EXPECTED_VERSION}\",\n"
    "  \"Shaders\": {\n${_manifest_entries}  }\n"
    "}\n")

message(STATUS "Runtime assets match canonical sources under ${_stage_root}")
