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

file(GLOB_RECURSE PUNTO_ADAPTER_PAYLOADS
    "${PUNTO_STAGE_DIR}/*punto-gtk-editor-adapter*")
if(PUNTO_ADAPTER_PAYLOADS)
    message(FATAL_ERROR "default cmake --install shipped editor adapter payloads")
endif()

file(STRINGS "${PUNTO_STAGE_DIR}/usr/lib/systemd/user/punto-session-bridge.service"
    PUNTO_HELPER_HOME_POLICY REGEX "^ProtectHome=")
if(NOT PUNTO_HELPER_HOME_POLICY STREQUAL "ProtectHome=read-only")
    message(FATAL_ERROR "helper must read the user bus without writable home access")
endif()

set(PUNTO_EXTENSION_DIR
    "${PUNTO_STAGE_DIR}/usr/share/gnome-shell/extensions/punto-input-source@antonshalin76")
foreach(extension_file IN ITEMS
        compat.js dbusAdapter.js extension.js inputSourceBridge.js
        punto-dbus-v1.xml metadata.json)
    if(NOT EXISTS "${PUNTO_EXTENSION_DIR}/${extension_file}" OR
       IS_SYMLINK "${PUNTO_EXTENSION_DIR}/${extension_file}")
        message(FATAL_ERROR
            "cmake --install missed GNOME compatibility file: ${extension_file}")
    endif()
endforeach()

file(REMOVE_RECURSE "${PUNTO_STAGE_DIR}")
