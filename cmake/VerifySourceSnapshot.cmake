if(NOT DEFINED PROJECT_ROOT)
    message(FATAL_ERROR "PROJECT_ROOT is required")
endif()

file(REAL_PATH "${PROJECT_ROOT}" _project_root)
set(_manifest_path "${_project_root}/SOURCE-MANIFEST.sha256")
if(NOT EXISTS "${_manifest_path}")
    message(FATAL_ERROR "The packaged source snapshot has no SOURCE-MANIFEST.sha256")
endif()

file(STRINGS "${_manifest_path}" _manifest_lines ENCODING UTF-8)
if(NOT _manifest_lines)
    message(FATAL_ERROR "The packaged source snapshot manifest is empty")
endif()

set(_saw_commonlib FALSE)
set(_saw_commonlib_shared FALSE)
set(_seen_paths)
set(_commonlib_manifest_paths)
string(TOLOWER "${_project_root}/" _project_prefix)
foreach(_line IN LISTS _manifest_lines)
    if(NOT _line MATCHES "^([0-9a-f]+)  (.+)$")
        message(FATAL_ERROR "Malformed source-snapshot manifest line: ${_line}")
    endif()
    set(_expected_hash "${CMAKE_MATCH_1}")
    set(_relative_path "${CMAKE_MATCH_2}")
    string(LENGTH "${_expected_hash}" _hash_length)
    string(FIND "${_relative_path}" "\\" _backslash_index)
    string(REPLACE "/" ";" _relative_path_components "${_relative_path}")
    if(NOT _hash_length EQUAL 64 OR
       IS_ABSOLUTE "${_relative_path}" OR
       ".." IN_LIST _relative_path_components OR
       NOT _backslash_index EQUAL -1)
        message(FATAL_ERROR "Unsafe source-snapshot manifest entry: ${_line}")
    endif()
    if(_relative_path IN_LIST _seen_paths)
        message(FATAL_ERROR "Duplicate source-snapshot path: ${_relative_path}")
    endif()
    list(APPEND _seen_paths "${_relative_path}")

    string(FIND "${_relative_path}" "extern/CommonLibF4/" _commonlib_prefix_index)
    if(_commonlib_prefix_index EQUAL 0)
        list(APPEND _commonlib_manifest_paths "${_relative_path}")
        set(_source_file "${_project_root}/${_relative_path}")
        if(NOT EXISTS "${_source_file}" OR IS_DIRECTORY "${_source_file}")
            message(FATAL_ERROR "Packaged CommonLib source is missing: ${_relative_path}")
        endif()
        file(REAL_PATH "${_source_file}" _source_real_path)
        string(TOLOWER "${_source_real_path}" _source_real_path_compare)
        string(FIND "${_source_real_path_compare}" "${_project_prefix}" _prefix_index)
        if(NOT _prefix_index EQUAL 0)
            message(FATAL_ERROR "Packaged CommonLib source escapes its root: ${_relative_path}")
        endif()
        file(SHA256 "${_source_real_path}" _actual_hash)
        string(TOLOWER "${_actual_hash}" _actual_hash)
        if(NOT _actual_hash STREQUAL _expected_hash)
            message(FATAL_ERROR "Packaged CommonLib source is stale: ${_relative_path}")
        endif()

        if(_relative_path STREQUAL "extern/CommonLibF4/xmake.lua" OR
           _relative_path STREQUAL "extern/CommonLibF4/CMakeLists.txt")
            set(_saw_commonlib TRUE)
        elseif(_relative_path STREQUAL
               "extern/CommonLibF4/lib/commonlib-shared/xmake.lua" OR
               _relative_path STREQUAL
               "extern/CommonLibF4/lib/commonlib-shared/CMakeLists.txt")
            set(_saw_commonlib_shared TRUE)
        endif()
    endif()
endforeach()

# The CommonLib adapter compiles recursively discovered source files. An extra
# unmanifested file must therefore invalidate the snapshot instead of silently
# entering the build.
file(GLOB_RECURSE _actual_commonlib_files LIST_DIRECTORIES FALSE
    "${_project_root}/extern/CommonLibF4/*")
foreach(_actual_file IN LISTS _actual_commonlib_files)
    file(RELATIVE_PATH _actual_relative "${_project_root}" "${_actual_file}")
    string(REPLACE "\\" "/" _actual_relative "${_actual_relative}")
    if(NOT _actual_relative IN_LIST _commonlib_manifest_paths)
        message(FATAL_ERROR
            "Packaged CommonLib source has an unmanifested input: ${_actual_relative}")
    endif()
endforeach()

if(NOT _saw_commonlib OR NOT _saw_commonlib_shared)
    message(FATAL_ERROR
        "The packaged source snapshot does not contain both CommonLib source trees")
endif()

list(LENGTH _commonlib_manifest_paths _source_input_count)
message(STATUS
    "Verified packaged CommonLib source snapshot (${_source_input_count} inputs)")
