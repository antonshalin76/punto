if(NOT DEFINED PUNTO_BUILD_DIR OR NOT DEFINED PUNTO_STAGE_DIR)
    message(FATAL_ERROR "PUNTO_BUILD_DIR and PUNTO_STAGE_DIR are required")
endif()

file(REMOVE_RECURSE "${PUNTO_STAGE_DIR}")
file(MAKE_DIRECTORY "${PUNTO_STAGE_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${PUNTO_STAGE_DIR}"
            "${CMAKE_COMMAND}" --install "${PUNTO_BUILD_DIR}" --prefix /usr
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "isolated cmake --install failed (${install_result})\n"
        "${install_stdout}\n${install_stderr}")
endif()

if(EXISTS "${PUNTO_STAGE_DIR}/usr/share/punto-switcher/sounds")
    message(FATAL_ERROR "cmake --install shipped inactive sound payloads")
endif()

file(REMOVE_RECURSE "${PUNTO_STAGE_DIR}")
