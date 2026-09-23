include_guard(GLOBAL)

# CMake adapter for Dear-Modding-FO4/commonlibf4. It mirrors the upstream
# xmake targets without changing the fetched source tree.
function(fo4cs_add_commonlibf4 source_dir)
    if(TARGET CommonLibF4::CommonLibF4)
        return()
    endif()

    set(_shared_dir "${source_dir}/lib/commonlib-shared")
    file(GLOB_RECURSE _shared_sources CONFIGURE_DEPENDS
        "${_shared_dir}/src/*.cpp"
        "${_shared_dir}/include/*.h")
    file(GLOB_RECURSE _f4_sources CONFIGURE_DEPENDS
        "${source_dir}/src/*.cpp"
        "${source_dir}/include/*.h")
    if(NOT _shared_sources OR NOT _f4_sources)
        message(FATAL_ERROR "CommonLibF4 or commonlib-shared contains no sources")
    endif()

    add_library(fo4cs_commonlib_shared STATIC ${_shared_sources})
    target_include_directories(fo4cs_commonlib_shared SYSTEM PUBLIC
        "${_shared_dir}/include")
    target_compile_features(fo4cs_commonlib_shared PUBLIC cxx_std_23)
    target_precompile_headers(fo4cs_commonlib_shared PRIVATE
        "${_shared_dir}/src/REX/PCH.h")
    target_compile_options(fo4cs_commonlib_shared PUBLIC
        /EHsc /permissive- /Zc:preprocessor)
    target_compile_options(fo4cs_commonlib_shared PRIVATE
        /bigobj /utf-8 /wd4200 /wd4201 /wd4324 /we4715)
    target_link_libraries(fo4cs_commonlib_shared PUBLIC
        spdlog::spdlog
        advapi32 bcrypt d3d11 d3dcompiler dbghelp dxgi ole32 shell32 user32
        version ws2_32)

    add_library(fo4cs_commonlibf4 STATIC ${_f4_sources})
    add_library(CommonLibF4::CommonLibF4 ALIAS fo4cs_commonlibf4)
    target_include_directories(fo4cs_commonlibf4 SYSTEM PUBLIC
        "${source_dir}/include")
    target_compile_features(fo4cs_commonlibf4 PUBLIC cxx_std_23)
    target_compile_definitions(fo4cs_commonlibf4 PUBLIC
        COMMONLIB_RUNTIMECOUNT=3 _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_precompile_headers(fo4cs_commonlibf4 PRIVATE
        "${source_dir}/include/F4SE/Impl/PCH.h")
    target_compile_options(fo4cs_commonlibf4 PRIVATE /bigobj /utf-8)
    target_link_libraries(fo4cs_commonlibf4 PUBLIC fo4cs_commonlib_shared)
endfunction()
