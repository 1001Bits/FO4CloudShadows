
cmake_minimum_required(VERSION 3.25)

project(FO4CloudShadows VERSION 1.0.0 LANGUAGES CXX)

set(FO4CS_RELEASE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
if(NOT PROJECT_VERSION_PATCH EQUAL 0)
    string(APPEND FO4CS_RELEASE_VERSION ".${PROJECT_VERSION_PATCH}")
endif()

include(CTest)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "FO4CloudShadows is a Windows/MSVC F4SE plugin")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# Exception paths release COM owners and restore scoped render state. Do not
# depend on a developer machine's CMAKE_CXX_FLAGS to enable C++ unwinding.
add_compile_options(/EHsc)

enable_language(RC)

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

option(FO4CS_BUILD_TESTS "Build focused developer contract tests" ON)
option(FO4CS_ENABLE_PRIVATE_PROFILING "Compile the private-desktop benchmark controller and CPU scopes" OFF)
option(FO4CS_ENABLE_DEVELOPER_TOOLS "Expose diagnostics, experimental capture and godray cloud occlusion" OFF)
find_program(FO4CS_POWERSHELL_EXECUTABLE NAMES pwsh.exe pwsh powershell.exe REQUIRED)
find_package(Git REQUIRED)
get_filename_component(_fo4cs_msvc_tools "${CMAKE_LINKER}" DIRECTORY)
find_program(FO4CS_DUMPBIN_EXECUTABLE NAMES dumpbin.exe dumpbin
    HINTS "${_fo4cs_msvc_tools}" REQUIRED)
get_filename_component(_fo4cs_toolchain_dir "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
get_filename_component(_fo4cs_vcpkg_root "${_fo4cs_toolchain_dir}/../.." ABSOLUTE)
set(_fo4cs_downloads_default "${_fo4cs_vcpkg_root}/downloads")
if(DEFINED ENV{VCPKG_DOWNLOADS} AND NOT "$ENV{VCPKG_DOWNLOADS}" STREQUAL "")
    set(_fo4cs_downloads_default "$ENV{VCPKG_DOWNLOADS}")
endif()
set(FO4CS_VCPKG_DOWNLOADS_ROOT "${_fo4cs_downloads_default}" CACHE PATH
    "Source archive cache required for candidate/source packaging")
set(FO4CS_GAME_PATH "" CACHE PATH
    "Optional Fallout 4 OG/AE/VR directory used only by the explicit deploy target")

# There is one source of truth for every runtime asset.  The repository Data/
# tree is only a convenience mirror; normal builds stage directly from these
# canonical files and the DLL is compiled with their hashes.
set(FO4CS_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/CloudShadows")
set(FO4CS_RUNTIME_SHADERS
    "${FO4CS_SHADER_SOURCE_DIR}/FO4CloudShadowScreenCS.hlsl"
    "${FO4CS_SHADER_SOURCE_DIR}/WorldCloudTileVS.hlsl"
    "${FO4CS_SHADER_SOURCE_DIR}/WorldCloudTilePS.hlsl"
    "${FO4CS_SHADER_SOURCE_DIR}/WorldCloudSkyVS.hlsl"
    "${FO4CS_SHADER_SOURCE_DIR}/WorldCloudSkyPS.hlsl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${FO4CS_RUNTIME_SHADERS} "${CMAKE_CURRENT_SOURCE_DIR}/config/CloudShadows.json")
set(FO4CS_FEATURE_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Features/CloudShadows.ini")
set(FO4CS_CONFIG_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/config/CloudShadows.json")
set(FO4CS_FONT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/Fonts/Jost")
set(FO4CS_FONT_FILES
    "${FO4CS_FONT_SOURCE_DIR}/Jost-Light.ttf"
    "${FO4CS_FONT_SOURCE_DIR}/Jost-Regular.ttf"
    "${FO4CS_FONT_SOURCE_DIR}/OFL.txt")
foreach(_fo4cs_required IN LISTS FO4CS_RUNTIME_SHADERS FO4CS_FONT_FILES)
    if(NOT EXISTS "${_fo4cs_required}")
        message(FATAL_ERROR "Required runtime asset is missing: ${_fo4cs_required}")
    endif()
endforeach()
foreach(_fo4cs_required IN ITEMS "${FO4CS_FEATURE_SOURCE}" "${FO4CS_CONFIG_SOURCE}")
    if(NOT EXISTS "${_fo4cs_required}")
        message(FATAL_ERROR "Required runtime asset is missing: ${_fo4cs_required}")
    endif()
endforeach()

file(SHA256 "${FO4CS_SHADER_SOURCE_DIR}/FO4CloudShadowScreenCS.hlsl"
    FO4CS_SCREEN_SHADER_SHA256)
file(SHA256 "${FO4CS_SHADER_SOURCE_DIR}/WorldCloudTileVS.hlsl"
    FO4CS_TILE_VS_SHA256)
file(SHA256 "${FO4CS_SHADER_SOURCE_DIR}/WorldCloudTilePS.hlsl"
    FO4CS_TILE_PS_SHA256)
file(SHA256 "${FO4CS_SHADER_SOURCE_DIR}/WorldCloudSkyVS.hlsl"
    FO4CS_SKY_VS_SHA256)
file(SHA256 "${FO4CS_SHADER_SOURCE_DIR}/WorldCloudSkyPS.hlsl"
    FO4CS_SKY_PS_SHA256)
file(SHA256 "${FO4CS_CONFIG_SOURCE}" FO4CS_CONFIG_DEFAULT_SHA256)

# The vcpkg Detours port does not provide a package-config target. Resolve its
# Release and Debug archives explicitly so a universal Release plugin can never
# link the Debug runtime by accident.
if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR
        "Configure FO4CloudShadows through the vcpkg toolchain so Detours can be resolved")
endif()

set(_fo4cs_vcpkg_triplet_root
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_path(FO4CS_DETOURS_INCLUDE_DIR NAMES detours.h
    PATHS "${_fo4cs_vcpkg_triplet_root}/include/detours"
    NO_DEFAULT_PATH NO_CACHE)
find_library(FO4CS_DETOURS_RELEASE_LIBRARY NAMES detours
    PATHS "${_fo4cs_vcpkg_triplet_root}/lib"
    NO_DEFAULT_PATH NO_CACHE)
find_library(FO4CS_DETOURS_DEBUG_LIBRARY NAMES detours
    PATHS "${_fo4cs_vcpkg_triplet_root}/debug/lib"
    NO_DEFAULT_PATH NO_CACHE)
if(NOT FO4CS_DETOURS_INCLUDE_DIR OR
   NOT FO4CS_DETOURS_RELEASE_LIBRARY OR
   NOT FO4CS_DETOURS_DEBUG_LIBRARY)
    message(FATAL_ERROR
        "Detours was not found for VCPKG_TARGET_TRIPLET='${VCPKG_TARGET_TRIPLET}'")
endif()

add_library(Detours::Detours UNKNOWN IMPORTED)
set_target_properties(Detours::Detours PROPERTIES
    IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
    IMPORTED_LOCATION_DEBUG "${FO4CS_DETOURS_DEBUG_LIBRARY}"
    IMPORTED_LOCATION_RELEASE "${FO4CS_DETOURS_RELEASE_LIBRARY}"
    IMPORTED_LOCATION_RELWITHDEBINFO "${FO4CS_DETOURS_RELEASE_LIBRARY}"
    IMPORTED_LOCATION_MINSIZEREL "${FO4CS_DETOURS_RELEASE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${FO4CS_DETOURS_INCLUDE_DIR}")

add_library(imgui_fo4cs STATIC
    extern/imgui/imgui.cpp
    extern/imgui/imgui_draw.cpp
    extern/imgui/imgui_tables.cpp
    extern/imgui/imgui_widgets.cpp
    extern/imgui/backends/imgui_impl_dx11.cpp
    extern/imgui/backends/imgui_impl_win32.cpp)
target_include_directories(imgui_fo4cs SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/backends")
target_compile_features(imgui_fo4cs PRIVATE cxx_std_17)
target_compile_options(imgui_fo4cs PRIVATE /W0 /WX-)

file(GLOB_RECURSE FO4CS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
# A source/configuration fingerprint distinguishes developer builds that
# share a release version. Input changes automatically reconfigure this ID.
file(GLOB_RECURSE _fo4cs_identity_inputs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*")
list(APPEND _fo4cs_identity_inputs ${FO4CS_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json")
list(SORT _fo4cs_identity_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_fo4cs_identity_inputs})
set(_fo4cs_identity "${PROJECT_VERSION}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}|${CMAKE_CXX_FLAGS}|${CMAKE_CXX_FLAGS_RELEASE}|${VCPKG_TARGET_TRIPLET}|${CMAKE_MSVC_RUNTIME_LIBRARY}")
string(APPEND _fo4cs_identity "|private-profiling:${FO4CS_ENABLE_PRIVATE_PROFILING}")
string(APPEND _fo4cs_identity "|developer-tools:${FO4CS_ENABLE_DEVELOPER_TOOLS}")
foreach(_input IN LISTS _fo4cs_identity_inputs)
    file(SHA256 "${_input}" _hash)
    file(RELATIVE_PATH _relative "${CMAKE_CURRENT_SOURCE_DIR}" "${_input}")
    string(APPEND _fo4cs_identity "|${_relative}:${_hash}")
endforeach()
string(SHA256 FO4CS_BUILD_FINGERPRINT "${_fo4cs_identity}")
string(SUBSTRING "${FO4CS_BUILD_FINGERPRINT}" 0 16 FO4CS_BUILD_ID)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/FO4CloudShadows.build-id.txt"
    "${PROJECT_VERSION}-${FO4CS_BUILD_ID}\n${FO4CS_BUILD_FINGERPRINT}\n")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FO4CloudShadows.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/FO4CloudShadows.rc" @ONLY)

# RuntimeAPI selects OG 1.10.163, AE 1.11.240, or VR 1.2.72 at load time. This
# is deliberately one physical binary with no compile-time runtime split.
add_library(FO4CloudShadows SHARED
    ${FO4CS_SOURCES}
    "${CMAKE_CURRENT_BINARY_DIR}/FO4CloudShadows.rc")
target_compile_features(FO4CloudShadows PRIVATE cxx_std_23)
target_compile_definitions(FO4CloudShadows PRIVATE
    FALLOUT4
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WINDOWS
    UNICODE
    _UNICODE
    FO4CLOUDSHADOWS_VERSION_STR="${PROJECT_VERSION}"
    FO4CS_RELEASE_VERSION_STR="${FO4CS_RELEASE_VERSION}"
    FO4CS_BUILD_ID="${FO4CS_BUILD_ID}"
    FO4CS_ENABLE_PRIVATE_PROFILING=$<BOOL:${FO4CS_ENABLE_PRIVATE_PROFILING}>
    FO4CS_ENABLE_DEVELOPER_TOOLS=$<BOOL:${FO4CS_ENABLE_DEVELOPER_TOOLS}>
    FO4CS_SCREEN_SHADER_SHA256="${FO4CS_SCREEN_SHADER_SHA256}"
    FO4CS_TILE_VS_SHA256="${FO4CS_TILE_VS_SHA256}"
    FO4CS_TILE_PS_SHA256="${FO4CS_TILE_PS_SHA256}"
    FO4CS_SKY_VS_SHA256="${FO4CS_SKY_VS_SHA256}"
    FO4CS_SKY_PS_SHA256="${FO4CS_SKY_PS_SHA256}"
    FO4CS_CONFIG_DEFAULT_SHA256="${FO4CS_CONFIG_DEFAULT_SHA256}"
    $<$<CONFIG:Debug>:DEBUG>
    _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING)
target_precompile_headers(FO4CloudShadows PRIVATE src/PCH.h)
target_include_directories(FO4CloudShadows PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FO4CloudShadows PRIVATE
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    Detours::Detours
    imgui_fo4cs
    d3d11.lib
    d3dcompiler.lib
    dxgi.lib
    bcrypt.lib
    shell32.lib ole32.lib
    version.lib)
target_compile_options(FO4CloudShadows PRIVATE
    /MP4 /W4 /WX /utf-8 /permissive- /Zc:alignedNew /Zc:auto
    /Zc:__cplusplus /Zc:forScope /Zc:noexceptTypes /Zc:preprocessor
    /Zc:threadSafeInit
    "$<$<CONFIG:Debug>:/fp:strict;/ZI;/Od;/Gy>"
    "$<$<CONFIG:Release>:/Zi;/fp:precise;/GL;/O2;/Ob2;/Oi;/Ot>")
target_link_options(FO4CloudShadows PRIVATE
    /WX
    "$<$<CONFIG:Debug>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
    "$<$<CONFIG:Release>:/LTCG;/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>")
set_target_properties(FO4CloudShadows PROPERTIES
    OUTPUT_NAME "FO4CloudShadows"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")

if(BUILD_TESTING AND FO4CS_BUILD_TESTS)
    add_executable(FO4CloudShadowsMcmSettingsTests EXCLUDE_FROM_ALL
        tests/McmSettingsTests.cpp src/GodraysIntegration.cpp)
    target_include_directories(FO4CloudShadowsMcmSettingsTests PRIVATE src)
    target_compile_definitions(FO4CloudShadowsMcmSettingsTests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN FO4CS_ENABLE_DEVELOPER_TOOLS=0 FO4CS_ENABLE_PRIVATE_PROFILING=0)
    target_compile_options(FO4CloudShadowsMcmSettingsTests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_link_libraries(FO4CloudShadowsMcmSettingsTests PRIVATE nlohmann_json::nlohmann_json)
    set_target_properties(FO4CloudShadowsMcmSettingsTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
    add_test(NAME mcm-release-settings-contract
        COMMAND "$<TARGET_FILE:FO4CloudShadowsMcmSettingsTests>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(mcm-release-settings-contract PROPERTIES LABELS core TIMEOUT 30)
    add_executable(FO4CloudShadowsWorldAnchorTests EXCLUDE_FROM_ALL
        tests/WorldCloudAnchorTests.cpp)
    target_include_directories(FO4CloudShadowsWorldAnchorTests PRIVATE src)
    target_compile_options(FO4CloudShadowsWorldAnchorTests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(FO4CloudShadowsWorldAnchorTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
    add_test(NAME world-cloud-anchor-contract COMMAND "$<TARGET_FILE:FO4CloudShadowsWorldAnchorTests>")
    set_tests_properties(world-cloud-anchor-contract PROPERTIES LABELS core TIMEOUT 30)

    add_executable(FO4CloudShadowsFrameRateTests EXCLUDE_FROM_ALL
        tests/FrameRateWindowTests.cpp)
    target_include_directories(FO4CloudShadowsFrameRateTests PRIVATE src)
    target_compile_options(FO4CloudShadowsFrameRateTests PRIVATE
        /W4 /WX /utf-8 /permissive-)
    set_target_properties(FO4CloudShadowsFrameRateTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
    add_test(NAME manual-fps-contract COMMAND "$<TARGET_FILE:FO4CloudShadowsFrameRateTests>")
    set_tests_properties(manual-fps-contract PROPERTIES LABELS core TIMEOUT 30)

    add_executable(FO4CloudShadowsShaderToolsTests EXCLUDE_FROM_ALL
        tests/ShaderToolsTests.cpp
		src/RuntimeAPI.cpp
        src/ShaderTools/DXBCPatcher.cpp
        src/ShaderTools/DFLightPatcher.cpp)
    target_compile_features(FO4CloudShadowsShaderToolsTests PRIVATE cxx_std_23)
    target_compile_definitions(FO4CloudShadowsShaderToolsTests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_precompile_headers(FO4CloudShadowsShaderToolsTests PRIVATE
        tests/ShaderToolsTestPCH.h)
    target_include_directories(FO4CloudShadowsShaderToolsTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(FO4CloudShadowsShaderToolsTests PRIVATE
		spdlog::spdlog d3d11.lib d3dcompiler.lib dxgi.lib version.lib)
    target_compile_options(FO4CloudShadowsShaderToolsTests PRIVATE
        /W4 /WX /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor
        /Zc:threadSafeInit)
    set_target_properties(FO4CloudShadowsShaderToolsTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
    add_test(NAME shadertools-contract
        COMMAND "$<TARGET_FILE:FO4CloudShadowsShaderToolsTests>")
    set_tests_properties(shadertools-contract PROPERTIES LABELS core TIMEOUT 60)

    add_executable(FO4CloudShadowsSkyCloudCompatibilityTests EXCLUDE_FROM_ALL
        tests/SkyCloudCompatibilityTests.cpp)
    target_compile_features(FO4CloudShadowsSkyCloudCompatibilityTests PRIVATE cxx_std_23)
    target_compile_definitions(FO4CloudShadowsSkyCloudCompatibilityTests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(FO4CloudShadowsSkyCloudCompatibilityTests PRIVATE
        d3d11.lib d3dcompiler.lib)
    target_compile_options(FO4CloudShadowsSkyCloudCompatibilityTests PRIVATE
        /W4 /WX /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
    set_target_properties(FO4CloudShadowsSkyCloudCompatibilityTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
    add_test(NAME sky-cloud-compatibility-contract
        COMMAND "$<TARGET_FILE:FO4CloudShadowsSkyCloudCompatibilityTests>")
    set_tests_properties(sky-cloud-compatibility-contract PROPERTIES
        LABELS core TIMEOUT 60)

    add_executable(FO4CloudShadowsRendererTests EXCLUDE_FROM_ALL
        tests/CloudRendererTests.cpp
        src/CloudGeometryCapture.cpp src/CloudCubePreview.cpp src/GodrayCloudShader.cpp)
    target_compile_features(FO4CloudShadowsRendererTests PRIVATE cxx_std_23)
    target_compile_definitions(FO4CloudShadowsRendererTests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(FO4CloudShadowsRendererTests PRIVATE src)
    target_link_libraries(FO4CloudShadowsRendererTests PRIVATE
        spdlog::spdlog d3d11.lib d3dcompiler.lib dxgi.lib)
    target_compile_options(FO4CloudShadowsRendererTests PRIVATE
        /W4 /WX /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
    set_target_properties(FO4CloudShadowsRendererTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
    add_test(NAME cloud-renderer-contract
        COMMAND "$<TARGET_FILE:FO4CloudShadowsRendererTests>"
            "${FO4CS_SHADER_SOURCE_DIR}")
    set_tests_properties(cloud-renderer-contract PROPERTIES LABELS core TIMEOUT 120)
    add_executable(FO4CloudShadowsGodraySettingsTests EXCLUDE_FROM_ALL
        tests/GodraySettingsTests.cpp src/GodrayGameSettings.cpp)
    target_compile_features(FO4CloudShadowsGodraySettingsTests PRIVATE cxx_std_23)
    target_compile_definitions(FO4CloudShadowsGodraySettingsTests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN FO4CS_SETTINGS_TESTING)
    target_include_directories(FO4CloudShadowsGodraySettingsTests PRIVATE src)
    target_link_libraries(FO4CloudShadowsGodraySettingsTests PRIVATE shell32.lib ole32.lib)
    target_compile_options(FO4CloudShadowsGodraySettingsTests PRIVATE
        /W4 /WX /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
    set_target_properties(FO4CloudShadowsGodraySettingsTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
    add_test(NAME godray-settings-contract
        COMMAND "$<TARGET_FILE:FO4CloudShadowsGodraySettingsTests>")
    set_tests_properties(godray-settings-contract PROPERTIES LABELS core TIMEOUT 30)
    add_executable(FO4CloudShadowsRuntimeLifecycleTests EXCLUDE_FROM_ALL
        tests/RuntimeLifecycleTests.cpp)
    target_compile_features(FO4CloudShadowsRuntimeLifecycleTests PRIVATE cxx_std_23)
    target_compile_definitions(FO4CloudShadowsRuntimeLifecycleTests PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(FO4CloudShadowsRuntimeLifecycleTests PRIVATE src)
    target_compile_options(FO4CloudShadowsRuntimeLifecycleTests PRIVATE
        /W4 /WX /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
    set_target_properties(FO4CloudShadowsRuntimeLifecycleTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
    add_test(NAME runtime-lifecycle-contract
        COMMAND "$<TARGET_FILE:FO4CloudShadowsRuntimeLifecycleTests>")
    set_tests_properties(runtime-lifecycle-contract PROPERTIES LABELS core TIMEOUT 30)
    foreach(_profile_test IN ITEMS CpuStageProfiler ResidentPageValidation)
        add_executable(FO4CloudShadows${_profile_test}Tests EXCLUDE_FROM_ALL
            tests/${_profile_test}Tests.cpp)
        target_include_directories(FO4CloudShadows${_profile_test}Tests PRIVATE src)
        target_compile_definitions(FO4CloudShadows${_profile_test}Tests PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN FO4CS_ENABLE_PRIVATE_PROFILING=1)
        target_compile_options(FO4CloudShadows${_profile_test}Tests PRIVATE /W4 /WX /utf-8 /permissive-)
        set_target_properties(FO4CloudShadows${_profile_test}Tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
        add_test(NAME ${_profile_test}-contract
            COMMAND "$<TARGET_FILE:FO4CloudShadows${_profile_test}Tests>")
        set_tests_properties(${_profile_test}-contract PROPERTIES LABELS core TIMEOUT 30)
    endforeach()
    add_custom_target(fo4cs-tests DEPENDS
        FO4CloudShadowsMcmSettingsTests
        FO4CloudShadowsWorldAnchorTests
        FO4CloudShadowsFrameRateTests
        FO4CloudShadowsShaderToolsTests FO4CloudShadowsSkyCloudCompatibilityTests
        FO4CloudShadowsRendererTests FO4CloudShadowsGodraySettingsTests
        FO4CloudShadowsRuntimeLifecycleTests
        FO4CloudShadowsCpuStageProfilerTests FO4CloudShadowsResidentPageValidationTests)
    add_custom_target(validation-build DEPENDS FO4CloudShadows fo4cs-tests)
    add_custom_target(release-contract-check
        COMMAND "${CMAKE_COMMAND}" "-DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DEXPECTED_VERSION=${PROJECT_VERSION}" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyReleaseContract.cmake"
        VERBATIM)
    add_dependencies(validation-build release-contract-check)
    # The binary and acceptance-protocol contracts and the release package are
    # driven by the release tooling in scripts/, which is not part of every
    # source checkout. They are registered only where that tooling exists.
    set(_fo4cs_scripts "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
    if(EXISTS "${_fo4cs_scripts}/verify_built_binary.ps1")
        add_test(NAME binary-contract
            COMMAND "${FO4CS_POWERSHELL_EXECUTABLE}" -NoProfile -File
                "${_fo4cs_scripts}/verify_built_binary.ps1"
                -BinaryPath "$<TARGET_FILE:FO4CloudShadows>"
                -DumpbinPath "${FO4CS_DUMPBIN_EXECUTABLE}"
                -ExpectedVersion "${PROJECT_VERSION}.0")
        set_tests_properties(binary-contract PROPERTIES LABELS core TIMEOUT 60)
        if(NOT FO4CS_ENABLE_DEVELOPER_TOOLS AND NOT FO4CS_ENABLE_PRIVATE_PROFILING)
            add_test(NAME release-features-contract
                COMMAND "${FO4CS_POWERSHELL_EXECUTABLE}" -NoProfile -File
                    "${_fo4cs_scripts}/verify_built_binary.ps1"
                    -BinaryPath "$<TARGET_FILE:FO4CloudShadows>"
                    -DumpbinPath "${FO4CS_DUMPBIN_EXECUTABLE}"
                    -ExpectedVersion "${PROJECT_VERSION}.0" -RequireReleaseFeatures)
            set_tests_properties(release-features-contract PROPERTIES LABELS core TIMEOUT 60)
        endif()
    endif()
    if(EXISTS "${_fo4cs_scripts}/run_ingame_acceptance.ps1")
        add_test(NAME acceptance-protocol-contract
            COMMAND "${FO4CS_POWERSHELL_EXECUTABLE}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/AcceptanceProtocolTests.ps1")
        set_tests_properties(acceptance-protocol-contract PROPERTIES LABELS core TIMEOUT 30)
    endif()
endif()

if(BUILD_TESTING AND FO4CS_BUILD_TESTS AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_candidate.ps1")
    add_custom_target(release-package
        COMMAND "${CMAKE_COMMAND}" -DACTIVE_CONFIG=$<CONFIG>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RequireReleaseConfig.cmake"
        COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CMAKE_CURRENT_BINARY_DIR}"
            -C Release -L core --output-on-failure --no-tests=error
        COMMAND "${FO4CS_POWERSHELL_EXECUTABLE}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/package_candidate.ps1"
            -ProjectRoot "${CMAKE_CURRENT_SOURCE_DIR}"
            -BuildRoot "${CMAKE_CURRENT_BINARY_DIR}"
            -Version "${PROJECT_VERSION}"
            -CMakeExecutable "${CMAKE_COMMAND}"
            -GitExecutable "${GIT_EXECUTABLE}"
            -DumpbinPath "${FO4CS_DUMPBIN_EXECUTABLE}"
            -VcpkgRoot "${_fo4cs_vcpkg_root}"
            -VcpkgInstalledRoot "${VCPKG_INSTALLED_DIR}"
            -VcpkgDownloadsRoot "${FO4CS_VCPKG_DOWNLOADS_ROOT}"
            -VcpkgTriplet "${VCPKG_TARGET_TRIPLET}"
        DEPENDS validation-build
        COMMENT "Testing and packaging a candidate with verified Corresponding Source"
        VERBATIM)
endif()

install(TARGETS FO4CloudShadows
    RUNTIME DESTINATION "Data/F4SE/Plugins" COMPONENT Runtime)
install(FILES "$<TARGET_PDB_FILE:FO4CloudShadows>"
    DESTINATION "Data/F4SE/Plugins" COMPONENT Symbols OPTIONAL)
install(FILES ${FO4CS_RUNTIME_SHADERS}
    DESTINATION "Data/Shaders/CloudShadows" COMPONENT Runtime)
install(FILES "${FO4CS_FEATURE_SOURCE}" "${FO4CS_CONFIG_SOURCE}"
    DESTINATION "Data/Shaders/Features" COMPONENT Runtime)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/config/MCM/"
    DESTINATION "Data/MCM/Config" COMPONENT Runtime
    FILES_MATCHING PATTERN "*.json" PATTERN "*.ini")
install(FILES ${FO4CS_FONT_FILES}
    DESTINATION "Data/Interface/FO4CloudShadows/Fonts/Jost" COMPONENT Runtime)
# Licence texts and documentation ship with release packages built from the
# full project tree; a source-only checkout installs the runtime alone.
foreach(_fo4cs_document IN ITEMS LICENSE EXCEPTIONS.md THIRD_PARTY_NOTICES.md
        SOURCE_DELIVERY.md README.md)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_fo4cs_document}")
        install(FILES "${_fo4cs_document}" DESTINATION "." COMPONENT Runtime)
    endif()
endforeach()
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/licenses")
    install(DIRECTORY licenses/ DESTINATION licenses COMPONENT Runtime)
endif()
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs")
    install(DIRECTORY docs/ DESTINATION docs COMPONENT Runtime
        FILES_MATCHING PATTERN "*.md")
endif()

# Fast developer staging never depends on shader/corpus tests.  It does depend
# on the DLL, and always takes HLSL directly from the canonical shaders/ tree.
set(FO4CS_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/stage")
add_custom_target(stage
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${FO4CS_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_CURRENT_BINARY_DIR}"
        --config "$<CONFIG>" --prefix "${FO4CS_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}"
        -DSTAGE_ROOT=${FO4CS_STAGE_DIR}
        -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRuntimeAssets.cmake"
    DEPENDS FO4CloudShadows
    COMMENT "Staging the DLL and canonical runtime assets"
    VERBATIM)

add_custom_target(sync-runtime-assets
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Data/Shaders/CloudShadows"
        "${CMAKE_CURRENT_SOURCE_DIR}/Data/Shaders/Features"
        "${CMAKE_CURRENT_SOURCE_DIR}/Data/Interface/FO4CloudShadows/Fonts/Jost"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${FO4CS_RUNTIME_SHADERS}
        "${CMAKE_CURRENT_SOURCE_DIR}/Data/Shaders/CloudShadows"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${FO4CS_FEATURE_SOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Data/Shaders/Features/CloudShadows.ini"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${FO4CS_CONFIG_SOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/Data/Shaders/Features/CloudShadows.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        ${FO4CS_FONT_FILES}
        "${CMAKE_CURRENT_SOURCE_DIR}/Data/Interface/FO4CloudShadows/Fonts/Jost"
    COMMAND "${CMAKE_COMMAND}"
        -DSTAGE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DEXPECTED_VERSION=${PROJECT_VERSION}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRuntimeAssets.cmake"
    COMMENT "Synchronizing and verifying the canonical Data convenience mirror"
    VERBATIM)

if(FO4CS_GAME_PATH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/deploy_stage.ps1")
    find_program(FO4CS_POWERSHELL_EXECUTABLE
        NAMES pwsh.exe pwsh powershell.exe powershell REQUIRED)
    add_custom_target(deploy
        COMMAND "${FO4CS_POWERSHELL_EXECUTABLE}" -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/deploy_stage.ps1"
            -StageDataPath "${FO4CS_STAGE_DIR}/Data"
            -FO4Path "${FO4CS_GAME_PATH}"
        DEPENDS stage
        COMMENT "Explicitly deploying the staged universal plugin"
        VERBATIM)
endif()
