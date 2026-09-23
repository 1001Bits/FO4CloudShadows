
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
    config/CloudShadows.json config/vcpkg-source-lock.json
    config/MCM/FO4CloudShadows/config.json config/MCM/FO4CloudShadows/settings.ini
    src/BuildFeatures.h src/McmSettings.cpp tests/McmSettingsTests.cpp
    src/CloudFramePublication.h src/RendererLifetime.h src/GodrayCloudShader.cpp
    endif()
endfunction()

function(require_before relative_path earlier later description)
    require_file("${relative_path}")
    file(READ "${PROJECT_ROOT}/${relative_path}" _content)
    string(FIND "${_content}" "${earlier}" _earlier_index)
    string(FIND "${_content}" "${later}" _later_index)
    if(_earlier_index EQUAL -1 OR _later_index EQUAL -1 OR
require_text("CMakeLists.txt" "FO4CS_BUILD_FINGERPRINT")
require_text("CMakeLists.txt" "option(FO4CS_ENABLE_DEVELOPER_TOOLS")
require_text("scripts/package_candidate.ps1" "-RequireReleaseFeatures")
require_text("src/Plugin.cpp" "FO4CS_BUILD_ID")
    endif()
endfunction()

string(REPLACE "." "-" _ini_version "${EXPECTED_VERSION}")
string(REPLACE "." "\\." _regex_version "${EXPECTED_VERSION}")

require_match("CMakeLists.txt"
    "project\\(FO4CloudShadows VERSION ${_regex_version}"
    "project version does not equal ${EXPECTED_VERSION}")
require_match("CMakeLists.txt"
    "eaa917736be34c4853302beea222c65641ec662e"
    "CommonLibF4 revision is not pinned")
require_match("CMakeLists.txt"
    "dfd28a41a832108c4de1367608bd42e1ba477212"
    "commonlib-shared revision is not pinned")
require_match("CMakeLists.txt" "FO4CS_ALLOW_UNPINNED_COMMONLIBF4"
    "development override/release rejection plumbing is missing")
require_match("CMakeLists.txt" "[/]W4[ \\t]+[/]WX"
    "project warnings are not configured as errors")
require_match("CMakeLists.txt" "[/]Ges[ \\t]+[/]WX[ \\t]+[/]O3"
    "release shader verification is not strict optimized FXC")
reject_match("CMakeLists.txt" "[/]W4[ \\t]+[/]WX[-:]|[/]WX:NO"
    "project build still disables warnings-as-errors")

require_match("vcpkg.json"
    "\"version-string\"[ \\t]*:[ \\t]*\"${_regex_version}\""
    "manifest version does not equal ${EXPECTED_VERSION}")
require_match("vcpkg.json"
    "\"license\"[ \\t]*:[ \\t]*\"GPL-3\\.0-only\""
    "manifest must describe the GPLv3-covered project")
foreach(_unused_dependency IN ITEMS fmt rapidcsv rsm-mmio)
    reject_match("vcpkg.json" "\"${_unused_dependency}\""
        "unused/transitive direct dependency '${_unused_dependency}' is still declared")
endforeach()
file(READ "${PROJECT_ROOT}/vcpkg.json" _vcpkg_manifest)
string(REGEX MATCH
    "\"builtin-baseline\"[ \\t]*:[ \\t]*\"([0-9a-f]+)\""
    _baseline_match "${_vcpkg_manifest}")
set(_vcpkg_baseline "${CMAKE_MATCH_1}")
string(LENGTH "${_vcpkg_baseline}" _baseline_length)
if(NOT _baseline_match OR NOT _baseline_length EQUAL 40)
    message(FATAL_ERROR "vcpkg.json: dependencies are not pinned to a 40-character baseline")
endif()

require_file("config/vcpkg-source-lock.json")
file(READ "${PROJECT_ROOT}/config/vcpkg-source-lock.json" _vcpkg_source_lock)
string(JSON _source_lock_type ERROR_VARIABLE _source_lock_error
    TYPE "${_vcpkg_source_lock}")
if(NOT _source_lock_error STREQUAL "NOTFOUND" OR
   NOT _source_lock_type STREQUAL "OBJECT")
    message(FATAL_ERROR "config/vcpkg-source-lock.json is not a valid JSON object")
endif()
string(JSON _source_lock_schema GET "${_vcpkg_source_lock}" SchemaVersion)
string(JSON _source_lock_baseline GET "${_vcpkg_source_lock}" VcpkgBaseline)
string(JSON _source_lock_triplet GET "${_vcpkg_source_lock}" Triplet)
string(JSON _source_lock_triplet_hash GET
    "${_vcpkg_source_lock}" TripletSHA256)
string(LENGTH "${_source_lock_triplet_hash}" _source_lock_triplet_hash_length)
if(NOT _source_lock_schema EQUAL 1 OR
   NOT _source_lock_baseline STREQUAL _vcpkg_baseline OR
   NOT _source_lock_triplet STREQUAL "x64-windows-static-md" OR
   NOT _source_lock_triplet_hash_length EQUAL 64 OR
   NOT _source_lock_triplet_hash MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
        "config/vcpkg-source-lock.json does not match the manifest baseline/triplet")
endif()
string(JSON _source_package_count LENGTH "${_vcpkg_source_lock}" Packages)
set(_source_package_names)
math(EXPR _source_package_last "${_source_package_count} - 1")
foreach(_index RANGE 0 ${_source_package_last})
    string(JSON _source_package_name GET
        "${_vcpkg_source_lock}" Packages ${_index} Name)
    string(JSON _source_archive_hash GET
        "${_vcpkg_source_lock}" Packages ${_index} ArchiveSHA256)
    string(LENGTH "${_source_archive_hash}" _source_archive_hash_length)
    if(NOT _source_archive_hash_length EQUAL 64 OR
       NOT _source_archive_hash MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "config/vcpkg-source-lock.json has invalid '${_source_package_name}' hashes")
    endif()
    list(APPEND _source_package_names "${_source_package_name}")
endforeach()
    cloud-renderer-contract godray-settings-contract runtime-lifecycle-contract
    binary-contract acceptance-protocol-contract mcm-release-settings-contract)
    require_text("CMakeLists.txt" "NAME ${suite}")
    message(FATAL_ERROR
        "The vcpkg Corresponding Source lock must cover exactly '${_expected_source_packages}'")
endif()

set(_runtime_shader_names
    FO4CloudShadowScreenCS.hlsl
    WorldCloudTileVS.hlsl
    WorldCloudTilePS.hlsl
    WorldCloudSkyVS.hlsl
    WorldCloudSkyPS.hlsl)
foreach(_shader IN LISTS _runtime_shader_names)
    require_match("shaders/CloudShadows/${_shader}"
        "SPDX-License-Identifier:[ \\t]*GPL-3\\.0-or-later"
        "runtime shader has no GPL-3.0-or-later provenance header")
endforeach()
file(GLOB _canonical_hlsl LIST_DIRECTORIES FALSE
    "${PROJECT_ROOT}/shaders/CloudShadows/*.hlsl")
list(LENGTH _canonical_hlsl _canonical_hlsl_count)
if(NOT _canonical_hlsl_count EQUAL 5)
    message(FATAL_ERROR
        "shaders/CloudShadows must contain exactly five HLSL files; found ${_canonical_hlsl_count}")
endif()

require_match("shaders/Features/CloudShadows.ini"
    "Version[ \\t]*=[ \\t]*${_ini_version}"
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
/*@@UNRECOVERED@@*/
    "feature version does not equal ${_ini_version}")
require_match("src/Plugin.cpp"
	"pluginVersionMajor = 2u,[\r\n\t ]*\\.pluginVersionMinor = 0u,[\r\n\t ]*\\.pluginVersionPatch = 6u"
	"menu-bridge API does not advertise Cloud Shadows 2.0.6")
require_match("src/Plugin.cpp"
    "g_skyDrawsSeen\\.fetch_add"
    "Sky Begin diagnostics do not count pre-gate Sky observations")
require_match("src/Overlay.cpp" "Sky Begin calls"
    "overlay does not expose the pre-gate Sky observation counter")
require_match("src/Overlay.cpp" "Cloud draw candidates"
    "overlay mislabels pre-contract cloud draw candidates as captures")
require_match("src/Plugin.cpp"
    "phase\\.kind = TechniqueKind::kSky"
    "BeginTechnique does not track Sky state independently of view ownership")
require_match("src/Plugin.cpp"
    "The actual draw state is authoritative for main-view ownership"
    "main-view ownership is not deferred to the actual Sky draw")
require_match("src/CloudShadows.cpp"
    "kMainTempRenderTargetIndex[ \t]*=[ \t]*4"
    "Fallout 4 main Sky target is not pinned to RendererData index 4")
require_match("src/CloudShadows.cpp"
    "kMainRenderTargetIndex[ \t]*=[ \t]*3"
    "Fallout 4 main DFLight target is not pinned to RendererData index 3")
require_match("src/CloudShadows.cpp"
    "IsMainSkyRenderTargetView\\(ID3D11RenderTargetView\\* view\\) noexcept"
    "the exact kMainTemp RTV/subresource identity gate is missing")
require_match("src/Plugin.cpp"
    "if \\(!CloudShadows::IsMainSkyWorldView\\(context\\)\\)"
    "Sky epoch advertisement does not use the dedicated kMainTemp view gate")
require_match("src/WorldClouds.cpp"
    "if \\(!IsMainSkyWorldView\\(context\\)\\)"
    "world-cloud capture/replacement does not recheck the kMainTemp view gate")
require_match("src/WorldClouds.cpp"
    "viewports\\[0\\]\\.TopLeftX == 0\\.0f[\r\n\t ]*&&[\r\n\t ]*viewports\\[0\\]\\.TopLeftY == 0\\.0f[\r\n\t ]*&&[\r\n\t ]*viewports\\[0\\]\\.Width == static_cast<float>\\(primaryWidth\\)[\r\n\t ]*&&[\r\n\t ]*viewports\\[0\\]\\.Height == static_cast<float>\\(primaryHeight\\)"
    "main Sky ownership is not restricted to the full output viewport")
reject_match("src/Plugin.cpp"
    "if \\(!CloudShadows::IsMainWorldView\\(context\\)\\)"
    "Sky epoch advertisement still uses the DFLight kMain view gate")
reject_match("src/CloudShadows.cpp"
    "std::size\\(rendererData->renderTargets\\)"
    "temporary all-render-target diagnostic scan remains enabled")
require_match("src/Plugin.cpp" "if \\(!OpenCaptureEpoch\\(ctx\\)\\)"
    "Sky draw does not enforce the strict main-Sky-view gate at draw time")
require_match("src/Plugin.cpp"
    "CloudShadows::IsCloudTechnique\\(phase\\.skyTechnique\\)"
    "draw capture does not enforce Fallout 4 cloud techniques 5/6/7")
require_before("src/Plugin.cpp"
    "if (!OpenCaptureEpoch(ctx))"
    "CloudShadows::IsCloudTechnique(phase.skyTechnique)"
    "main Sky frames do not advertise an epoch before layer authentication")
require_match("src/Plugin.cpp"
    "Shader identity authorizes capture/replacement, not frame[\r\n\t ]+// advertisement"
    "shader identity and frame-advertisement responsibilities are conflated")
reject_match("src/Plugin.cpp"
    "phase\\.kind = TechniqueKind::kDFComposite;[\r\n\t ]+OpenCaptureEpoch"
    "DFComposite opens an epoch before Fallout commits its draw state")
require_match("src/Plugin.cpp"
    "CommitWorldCloudFrameAtPresent\\(g_d3dContext\\)"
    "Present does not use its camera-neutral publication path")
reject_match("src/Plugin.cpp"
    "CloudShadows::CommitWorldCloudFrame\\(g_d3dContext\\)"
    "Present still invokes render-camera synchronization")
require_match("src/CloudShadows.h"
    "std::atomic<uint64_t> g_shadowMaskSuccessStamp"
    "the per-frame successful-dispatch stamp is missing")
require_match("src/CloudShadows.cpp"
    "bool Prepass\\(\\)[\r\n\t ]+\\{[\r\n\t ]+g_shadowMaskValid\\.store\\(false"
    "Prepass no longer clears its per-attempt BeginSwap validity")
require_match("src/CloudShadows.cpp" "enum class PrepassRejectReason"
    "shadow-prepass rejection diagnostics are not grouped")
require_match("src/CloudShadows.cpp" "Shadow prepass reject group={}"
    "shadow-prepass rejection diagnostics are not emitted")
require_match("src/CloudShadows.cpp"
    "ordinal <= 4u \\|\\| \\(ordinal & \\(ordinal - 1u\\)\\) == 0u"
    "shadow-prepass rejection diagnostics are not logarithmically bounded")
require_match("src/CloudShadows.cpp" "Shadow prepass dispatch #{}"
    "successful shadow-mask dispatch evidence is missing")
file(READ "${PROJECT_ROOT}/src/CloudShadows.cpp" _cloud_shadow_source)
string(REGEX MATCHALL "Shadow prepass reject group="
    _prepass_reject_log_sites "${_cloud_shadow_source}")
list(LENGTH _prepass_reject_log_sites _prepass_reject_log_site_count)
if(NOT _prepass_reject_log_site_count EQUAL 1)
    message(FATAL_ERROR
        "src/CloudShadows.cpp: shadow-prepass rejections must have exactly one bounded log site")
endif()
require_before("src/CloudShadows.cpp"
    "context->Dispatch("
    "const uint64_t encodedEpoch = dispatchEpoch + 1u;"
    "the successful-dispatch stamp is published before compute dispatch")
require_match("src/CloudShadows.cpp"
    "const uint64_t encodedEpoch = dispatchEpoch \\+ 1u;[\r\n\t ]+g_shadowMaskSuccessStamp\\.store"
    "a successful compute dispatch does not publish its frame stamp")
require_match("src/Plugin.cpp"
    "g_shadowMaskSuccessStamp\\.exchange\\([\r\n\t ]+0"
    "Present does not consume the frame-level successful-dispatch stamp")
reject_match("src/Plugin.cpp"
    "g_shadowMaskValid\\.exchange"
    "Present still lets a later rejected Prepass erase an earlier successful dispatch")
require_match("src/Plugin.cpp"
    "const bool deviceHealthy = g_d3dDevice &&[\r\n\t ]+SUCCEEDED\\(g_d3dDevice->GetDeviceRemovedReason\\(\\)\\)"
    "Present does not verify device health before reporting a completed mask")
require_match("src/Plugin.cpp"
    "result == DXGI_ERROR_DEVICE_REMOVED \\|\\|[\r\n\t ]+result == DXGI_ERROR_DEVICE_RESET"
    "Present failure does not invalidate shadow-mask state")
require_match("src/CloudShadows.cpp"
    "g_cloudShadowUAV = newUAV\\.Detach\\(\\);[\r\n\t ]+InvalidateShadowMaskState\\(\\);"
    "screen-mask replacement does not invalidate prior mask state")
require_match("src/CloudShadows.cpp"
    "void ReleaseDeviceResources\\(\\) noexcept[\r\n\t ]+\\{[\r\n\t ]+g_initialized = false;[\r\n\t ]+InvalidateShadowMaskState\\(\\);"
    "device-resource release does not invalidate prior mask state")
require_match("src/WorldClouds.cpp"
    "g_worldCloudReady\\.store\\(false, std::memory_order_release\\);[\r\n\t ]+InvalidateShadowMaskState\\(\\);"
    "world-cloud resource recreation does not invalidate prior mask state")
require_match("src/WorldClouds.cpp"
    "void RequestWorldCloudReset\\(\\) noexcept[\r\n\t ]+\\{[\r\n\t ]+s_resetRequested\\.store\\(true, std::memory_order_release\\);[\r\n\t ]+InvalidateShadowMaskState\\(\\);"
    "an explicit world reset does not invalidate prior mask state")
reject_match("src/WorldClouds.cpp"
    "g_shadowMaskValid\\.store"
    "a world/resource reset bypasses the complete mask invalidation helper")
require_match("src/Plugin.cpp"
    "g_d3dContext = context;[\r\n\t ]+CloudShadows::InvalidateShadowMaskState\\(\\);"
    "renderer context replacement does not invalidate prior mask state")
require_match("src/WorldClouds.cpp"
    "void CommitWorldCloudFrameAtPresent\\(ID3D11DeviceContext\\* context\\) noexcept"
    "camera-neutral Present publication is missing")
require_match("src/WorldClouds.cpp"
    "A reset blocks stale publication and remains[\r\n\t ]+// pending for the next qualified main draw"
    "Present can consume a reset without an authoritative render camera")
require_match("src/WorldClouds.cpp"
    "if \\(s_resetRequested\\.load\\(std::memory_order_acquire\\)\\)[\r\n\t ]+return;[\r\n\t ]+PublishAdvertisedWorldCloudFrame\\(context\\);"
    "Present does not executable-gate publication on an unconsumed reset")
require_match("src/WorldClouds.cpp"
    "!IsCloudTechnique\\(skyTechnique\\)"
    "world-cloud capture contract does not enforce techniques 5/6/7")
require_match("src/WorldClouds.cpp"
    "!IsCloudTechnique\\(command\\.skyTechnique\\)"
    "world-cloud draw admission bypasses the shared technique classifier")
require_match("src/WorldClouds.cpp" "skyTechnique == kSkyTechniqueCloudsLerp"
    "CloudsLerp does not require its Fallout 4 t1/s1 binding")
require_match("src/WorldClouds.cpp" "enum class CaptureRejectReason"
    "capture rejection diagnostics are not grouped")
require_match("src/WorldClouds.cpp" "camera-unavailable"
    "camera acquisition failures are absent from capture rejection diagnostics")
require_match("src/WorldClouds.cpp"
    "ordinal > 4u && \\(ordinal & \\(ordinal - 1u\\)\\) != 0u"
    "capture rejection diagnostics are not logarithmically bounded")
require_match("src/WorldClouds.cpp"
    "This defensive revalidation is intentionally silent"
    "the defensive tile validator can regress to duplicate rejection logging")
file(READ "${PROJECT_ROOT}/src/WorldClouds.cpp" _world_cloud_source)
string(REGEX MATCHALL "World-cloud capture reject group="
    _capture_reject_log_sites "${_world_cloud_source}")
list(LENGTH _capture_reject_log_sites _capture_reject_log_site_count)
if(NOT _capture_reject_log_site_count EQUAL 1)
    message(FATAL_ERROR
        "src/WorldClouds.cpp: capture rejections must have exactly one bounded log site")
endif()
require_match("shaders/CloudShadows/WorldCloudTilePS.hlsl"
    "skyTechnique > 5\\.5 && skyTechnique < 6\\.5"
    "tile capture does not apply lerp to Fallout 4 CloudsLerp technique 6")
require_match("shaders/CloudShadows/WorldCloudTilePS.hlsl"
    "skyTechnique > 6\\.5"
    "tile capture does not apply fade to Fallout 4 CloudsFade technique 7")
require_match("docs/RELEASE_VALIDATION.md"
    "Sky BeginTechnique \\.\\.\\. technique=5 \\.\\.\\. staticShaders=true"
    "runtime matrix does not require settled Fallout 4 technique-5 acceptance")
require_match("docs/RELEASE_VALIDATION.md"
    "WORLD CLOUD DRAW \\.\\.\\. technique=5"
    "runtime matrix does not require a settled technique-5 captured draw")
require_match("docs/RELEASE_VALIDATION.md"
    "Technique 4 \\(`Texture`\\) must never be accepted"
    "runtime matrix does not reject Sky Texture technique 4")

require_match("docs/RELEASE_VALIDATION.md"
    "kMainTemp"
    "runtime matrix does not document the dedicated main Sky target")
require_match("docs/RELEASE_VALIDATION.md"
    "RendererData::renderTargets\\[4\\]"
    "runtime matrix does not pin the main Sky target to RendererData index 4")
require_match("docs/RELEASE_VALIDATION.md"
    "DFLight to[\r\n\t ]+dispatch epoch E, Sky to capture E\\+1"
    "runtime validation incorrectly assumes dispatch and post-Present epochs match")
require_match("docs/RELEASE_VALIDATION.md"
    "World-cloud capture reject group=\\.\\.\\."
    "runtime validation does not require bounded capture-rejection evidence")

require_file("config/CloudShadows.json")
file(READ "${PROJECT_ROOT}/config/CloudShadows.json" _settings_json)
string(JSON _settings_type ERROR_VARIABLE _settings_error TYPE "${_settings_json}")
if(NOT _settings_error STREQUAL "NOTFOUND" OR NOT _settings_type STREQUAL "OBJECT")
    message(FATAL_ERROR "config/CloudShadows.json is not a valid JSON object")
endif()
set(_expected_settings
    BaseMipBias CloudHeight DebugMode Enabled LayerHeightStep LayerScaleMultiplier
    MaxLayers MaxOpticalSlant Opacity ProjectionModelVersion SunAngularRadius
    SunFadeEnd SunFadeStart VerticalOpticalDepth WorldTileSize)
string(JSON _settings_length LENGTH "${_settings_json}")
set(_actual_settings)
math(EXPR _settings_last "${_settings_length} - 1")
foreach(_index RANGE 0 ${_settings_last})
    string(JSON _key MEMBER "${_settings_json}" ${_index})
    list(APPEND _actual_settings "${_key}")
endforeach()
list(SORT _expected_settings)
list(SORT _actual_settings)
if(NOT _actual_settings STREQUAL _expected_settings)
    message(FATAL_ERROR
        "config/CloudShadows.json has stale/missing keys. Expected '${_expected_settings}', "
        "found '${_actual_settings}'")
endif()
string(JSON _projection_version GET "${_settings_json}" ProjectionModelVersion)
string(JSON _max_layers GET "${_settings_json}" MaxLayers)
if(NOT _projection_version EQUAL 4 OR NOT _max_layers EQUAL 16)
    message(FATAL_ERROR "The default settings do not target projection v4 / 16 layers")
endif()

foreach(_file IN ITEMS
        LICENSE
        EXCEPTIONS.md
        SOURCE_DELIVERY.md
        THIRD_PARTY_NOTICES.md
        assets/Fonts/Jost/Jost-Light.ttf
        assets/Fonts/Jost/Jost-Regular.ttf
        assets/Fonts/Jost/OFL.txt
        licenses/CommonLibF4-MIT.txt
        licenses/commonlib-shared-GPL-3.0.txt
        licenses/commonlib-shared-EXCEPTIONS.txt
        licenses/Community-Shaders-GPL-3.0.txt
        licenses/Dynamic-Reflections-GPL-3.0.txt
        licenses/Dynamic-Reflections-EXCEPTIONS.txt
        licenses/Detours-MIT.txt
        licenses/Dear-ImGui-MIT.txt
        licenses/spdlog-MIT.txt
        licenses/fmt-MIT.txt
        licenses/nlohmann-json-MIT.txt
        licenses/Jost-OFL-1.1.txt
        cmake/VerifySourceSnapshot.cmake
        tests/ShaderToolsTestPCH.h
        tests/ShaderToolsTests.cpp)
    require_file("${_file}")
endforeach()
require_match("LICENSE" "GNU GENERAL PUBLIC LICENSE"
    "root project license is not GPLv3")
require_match("THIRD_PARTY_NOTICES.md"
    "d1ccd465cb38b81baad90973953be1a95e025644"
    "Community Shaders provenance revision is absent")
require_match("THIRD_PARTY_NOTICES.md"
    "3c35b6927d26f1d819f4fa7e827d7b3c9b9f2cb7"
    "Dynamic Reflections provenance revision is absent")

require_match("CMakePresets.json"
    "\"AUTO_DEPLOY\"[ \\t\\r\\n]*:[ \\t\\r\\n]*false"
    "safe presets must explicitly disable deployment")
require_match("CMakePresets.json"
    "\"value\"[ \\t]*:[ \\t]*\"x64\""
    "Visual Studio presets do not select x64")
require_match("CMakePresets.json"
    "\"strategy\"[ \\t]*:[ \\t]*\"set\""
    "Visual Studio presets do not make CMake enforce their architecture")
reject_match("CMakePresets.json" "\"strategy\"[ \\t]*:[ \\t]*\"external\""
    "CLI presets delegate architecture selection instead of enforcing x64")
require_match("CMakePresets.json"
    "\"name\"[ \\t]*:[ \\t]*\"release-package\""
    "release-package build preset is missing")
reject_match("CMakePresets.json" "\"packagePresets\""
    "direct CPack presets bypass release-package dependencies")
require_match(".gitignore" "Generated source-tree convenience mirror"
    "Data mirror ignore comment is stale")
foreach(_shader_tool IN ITEMS
        DFLightPatcher.cpp DFLightPatcher.h DXBCPatch.h DXBCPatcher.cpp DXBCPatcher.h)
    require_match("src/ShaderTools/${_shader_tool}"
        "SPDX-License-Identifier:[ \\t]*GPL-3\\.0-only"
        "derived ShaderTools file has no GPL-3.0-only provenance header")
endforeach()
require_match("src/CloudShadows.cpp"
    "constexpr std::int64_t kProjectionModelVersion[ \t]*=[ \t]*4"
    "the shared world-projection settings schema constant is not v4")
require_match("src/CloudShadows.cpp"
    "is_number_integer\\(\\).*version->get<std::int64_t>\\(\\)[ \t\r\n]*!=[ \t\r\n]*kProjectionModelVersion"
    "LoadSettings does not require the exact shared projection schema version")
require_match("src/CloudShadows.cpp"
    "j\\[\"ProjectionModelVersion\"\\][ \t]*=[ \t]*kProjectionModelVersion"
    "SaveSettings does not emit the shared projection schema version")
require_match("src/CloudShadows.cpp"
    "j\\[\"Enabled\"\\][ \t]*=[ \t]*g_shadowsEnabled"
    "SaveSettings does not persist the master enable state")
require_match("src/CloudShadowsMenuBridge.h"
    "kAbiVersion[ \t]*=[ \t]*2u"
    "the unified-menu protocol must use ABI 2")
require_match("src/CloudShadowsMenuBridge.h"
    "using SetHostActiveFn[ \t]*=[ \t]*std::uint32_t"
    "the ABI 2 cumulative capability callback is missing")
require_match("src/Overlay.cpp"
    "if \\(IsExternalHostActive\\(\\)\\)"
    "hosted Draw does not gate the standalone overlay")
require_before("src/Overlay.cpp"
    "if (IsExternalHostActive()) {"
    "if (!EnsureInit(sc))"
    "hosted Draw must exit before standalone ImGui initialization")
require_match("src/Overlay.cpp"
    "std::recursive_mutex g_imguiLifetimeMutex"
    "standalone ImGui backend/frame lifetime is not serialized")
require_match("src/Overlay.cpp"
    "std::atomic<HWND> g_hwnd"
    "cross-thread standalone window ownership is not atomic")
require_match("src/Overlay.cpp"
    "std::atomic<WNDPROC> g_originalWndProc"
    "WndProc forwarding target is not safe across recreated windows")
require_match("src/Overlay.cpp"
    "void Shutdown\\(\\) noexcept[\r\n\t ]+\\{[\r\n\t ]+std::scoped_lock imguiLock\\(g_imguiLifetimeMutex\\)"
    "Shutdown can race an ImGui frame or Win32 backend callback")
require_match("src/Overlay.cpp"
    "void Draw\\(IDXGISwapChain\\* sc\\)[\r\n\t ]+\\{[\r\n\t ]+std::scoped_lock imguiLock\\(g_imguiLifetimeMutex\\)"
    "Draw does not hold the ImGui lifetime lock for the complete frame")
require_before("src/Overlay.cpp"
    "g_originalWndProc.load(std::memory_order_acquire)"
    "const bool visibleAtEntry"
    "MenuWndProc does not snapshot its forwarding chain before blocking")
require_match("src/Overlay.cpp"
    "if \\(msg == WM_NCDESTROY && g_wndProcInstalled"
    "destroyed output windows do not retire their exact WndProc ownership")
require_match("src/Overlay.cpp"
    "g_windowRetired = true"
    "destroyed output windows do not defer backend teardown to Present")
require_match("src/Overlay.cpp"
    "HWND g_retiredHwnd"
    "retired output-window identity is not preserved across teardown")
require_match("src/Overlay.cpp"
    "desc\\.OutputWindow != initializedHwnd"
    "same-device swap-chain window replacement is not detected")
require_match("src/Overlay.cpp"
    "Retirement intentionally survives Shutdown"
    "Shutdown can immediately re-adopt an HWND whose NCDESTROY is unwinding")
require_before("src/Overlay.cpp"
    "if (isRetiredOutput || outputChanged)"
    "if (!EnsureInit(sc))"
    "Draw does not retire stale ImGui backends before reinitialization")
require_match("src/Overlay.cpp"
    "HWND g_wndProcHwnd"
    "WndProc ownership is not tracked per output window")
reject_match("src/Overlay.cpp"
    "g_originalWndProc\\.store\\(nullptr"
    "Shutdown can erase the forwarding target of an in-flight WndProc")
require_match("src/Overlay.cpp"
    "Once visibility is published, serialize every request"
    "ClipCursor requests are not linearized with menu visibility transitions")
require_match("src/CloudShadows.cpp"
    "if \\(!Overlay::IsExternalHostActive\\(\\)\\)"
    "hosted mode does not yield F10 ownership")
require_match("src/Plugin.cpp"
    "deferring standalone BeginTechnique"
    "the standalone BeginTechnique hook is not deferred during host setup")
require_match("src/Plugin.cpp"
    "case F4SE::MessagingInterface::kInputLoaded:"
    "the deferred BeginTechnique fallback lifecycle is missing")
reject_match("src/Plugin.cpp"
    "DetourDetach\\([^;]*s_original_BeginTechnique"
    "menu handoff must never detach a live cross-plugin BeginTechnique chain")
require_match("scripts/deploy_stage.ps1" "Preserving existing CloudShadows\\.json"
    "direct deploy does not preserve user settings")
require_match("scripts/build_deploy_launch.ps1"
    "--build[ \\t]+--preset[ \\t]+[$]BuildPreset[ \\t]+--target[ \\t]+release-package"
    "packaging helper assumes a preset-named build directory")
require_match("scripts/prepare_runtime.ps1" "SchemaVersion[ \\t]*=[ \\t]*1"
    "runtime corpus marker schema is missing")
require_match("scripts/run_corpus_test.ps1" "FO4CS_REQUIRE_CORPUS_TEST"
    "required/optional corpus-test wrapper is missing")
require_match("scripts/package_source.ps1" "SOURCE-MANIFEST\\.sha256"
    "Corresponding Source packager/manifest is missing")
require_match("scripts/package_source.ps1" "VerifySourceSnapshot\\.cmake"
    "Corresponding Source staging does not self-verify its build snapshot")
require_match("scripts/vcpkg_source_bundle.ps1" "vcpkg_abi_info\\.txt"
    "static vcpkg Corresponding Source verifier is missing")
require_match("scripts/verify_source_package.ps1" "source archive is stale"
    "Corresponding Source freshness verifier is missing")
require_match("SOURCE_DELIVERY.md" "third-party/vcpkg"
    "Corresponding Source notice omits statically linked vcpkg source")
require_match("CMakeLists.txt" "add_custom_target\\(release-source-package"
    "release-source-package target is missing")
require_match("CMakeLists.txt"
    "add_custom_target\\(release-corpus-tests"
    "required exhaustive corpus target is missing")
require_match("CMakeLists.txt"
    "DEPENDS release-corpus-tests"
    "binary release-package does not depend on exhaustive corpus proof")
require_match("CMakeLists.txt"
    "add_executable\\(FO4CloudShadowsShaderToolsTests"
    "standalone production ShaderTools test target is missing")
require_match("cmake/FO4CloudShadows.rc.in" "FILEVERSION.*0"
    "four-part VERSIONINFO resource is missing")
require_match("cmake/CPackReleaseGate.cmake.in" "string\\(JSON"
    "release corpus evidence is not structurally parsed")
require_match("cmake/CPackReleaseGate.cmake.in"
    "string\\(SHA256 _current_corpus_hash"
    "release gate does not recompute DFLight corpus evidence")
require_match("cmake/CPackReleaseGate.cmake.in" "TestExecutableSHA256"
    "release gate does not authenticate exhaustive ShaderTools evidence")
require_match("cmake/CPackReleaseGate.cmake.in" "BinarySHA256"
    "release gate does not authenticate the tested Release DLL")

# Presets and helpers must remain portable. Runtime paths are supplied through
# cache variables, environment variables, or explicit parameters.
reject_match("CMakePresets.json" "[A-Za-z]:[/\\\\]"
    "contains a machine-specific absolute path")
file(GLOB _scripts "${PROJECT_ROOT}/scripts/*.ps1")
foreach(_script IN LISTS _scripts)
    file(RELATIVE_PATH _relative "${PROJECT_ROOT}" "${_script}")
    reject_match("${_relative}" "C:[/\\\\](Development|Games)[/\\\\]"
        "contains a machine-specific project or game path")
endforeach()

foreach(_release_text IN ITEMS README.md docs/BUILDING.md docs/RELEASE_VALIDATION.md)
    reject_match("${_release_text}"
        "technique[s]?[ \\t]+4[/,][ \\t]*5[/,][ \\t]*6|Interface[/\\\\]CommunityShaders[/\\\\]Fonts"
        "contains a stale technique 4/5/6 or project-owned CommunityShaders font claim")
endforeach()

message(STATUS "FO4CloudShadows ${EXPECTED_VERSION} release contract passed")
