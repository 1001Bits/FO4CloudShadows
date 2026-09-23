if(NOT DEFINED ACTIVE_CONFIG OR NOT ACTIVE_CONFIG STREQUAL "Release")
    message(FATAL_ERROR
        "This target is release-only. Re-run it with '--config Release'. "
        "Active configuration: '${ACTIVE_CONFIG}'")
endif()
