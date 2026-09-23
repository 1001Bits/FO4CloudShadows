if(NOT DEFINED VERIFY_DIR)
    message(FATAL_ERROR "VERIFY_DIR was not provided")
endif()

foreach(_name IN ITEMS
        FO4CloudShadowScreenCS.cso
        WorldCloudTileVS.cso
        WorldCloudTilePS.cso
        WorldCloudSkyVS.cso
        WorldCloudSkyPS.cso)
    set(_path "${VERIFY_DIR}/${_name}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Verified shader bytecode is missing: ${_path}")
    endif()
    file(SIZE "${_path}" _size)
    if(_size LESS 32)
        message(FATAL_ERROR "Verified shader bytecode is unexpectedly small: ${_path}")
    endif()
endforeach()

message(STATUS "All strict FXC outputs exist and are non-empty")
