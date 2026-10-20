# =============================================================================
# Punto Switcher для Linux (C++20 версия)
# Высокопроизводительный плагин для interception-tools
# =============================================================================
cmake_minimum_required(VERSION 3.16)

set(PUNTO_VERSION_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
if(NOT EXISTS "${PUNTO_VERSION_PATH}" OR IS_SYMLINK "${PUNTO_VERSION_PATH}")
    message(FATAL_ERROR "VERSION must be a regular non-symlink file")
endif()
file(READ "${PUNTO_VERSION_PATH}" PUNTO_VERSION_CONTENT LIMIT 129)
string(REGEX REPLACE "\n$" "" PUNTO_VERSION "${PUNTO_VERSION_CONTENT}")
if(NOT PUNTO_VERSION_CONTENT STREQUAL "${PUNTO_VERSION}\n" OR
   NOT PUNTO_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain one LF-terminated numeric triplet")
endif()
project(punto VERSION "${PUNTO_VERSION}" LANGUAGES C CXX)
set(PUNTO_POLICY_HASH "0" CACHE STRING "Xorg ingress policy identity")
set(PUNTO_BUILD_ID "0" CACHE STRING "Xorg ingress ensemble build identity")
set(PUNTO_WITH_XORG_RECIPIENT OFF)
if(NOT PUNTO_POLICY_HASH STREQUAL "0" OR NOT PUNTO_BUILD_ID STREQUAL "0")
    if(PUNTO_POLICY_HASH STREQUAL "0" OR PUNTO_BUILD_ID STREQUAL "0")
        message(FATAL_ERROR
            "PUNTO_POLICY_HASH and PUNTO_BUILD_ID must both be zero or both be nonzero")
    endif()
    set(PUNTO_WITH_XORG_RECIPIENT ON)
endif()

# Для clangd/IDE: генерируем compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Стандарты и флаги компиляции
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Keep distro/toolchain Release flags intact; hardening is layered below rather
# than replacing CMAKE_CXX_FLAGS_RELEASE (and therefore dpkg-buildflags).
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")

# Предупреждения
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
    -Werror=unused-result
    -Wconversion
    -Wsign-conversion
    -Wno-unused-parameter
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        $<$<CONFIG:Release>:-fstack-protector-strong>
        $<$<AND:$<CONFIG:Release>,$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>>:-fPIE>
        $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=3>
        $<$<CONFIG:Release>:-D_GLIBCXX_ASSERTIONS>
    )
    add_link_options(
        $<$<AND:$<CONFIG:Release>,$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>>:-pie>
        $<$<CONFIG:Release>:-Wl,-z,relro>
        $<$<CONFIG:Release>:-Wl,-z,now>
        $<$<CONFIG:Release>:-Wl,-z,noexecstack>
    )
endif()

# -----------------------------------------------------------------------------
# Поиск зависимостей
# -----------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(yaml-cpp REQUIRED)

# Reconfiguration can switch between stock and selected native SDK providers.
# FindPkgConfig caches discovery and absolute library paths independently.
get_cmake_property(PUNTO_PKG_CACHE_VARIABLES CACHE_VARIABLES)
foreach(prefix GTK3 X11 X11_XCB XCB)
    unset(${prefix}_FOUND CACHE)
    foreach(variable IN LISTS PUNTO_PKG_CACHE_VARIABLES)
        if(variable MATCHES "^pkgcfg_lib_${prefix}_")
            unset(${variable} CACHE)
        endif()
    endforeach()
endforeach()

# Native session discovery and recoverable X11 transport.
pkg_check_modules(SYSTEMD REQUIRED libsystemd)
pkg_check_modules(GIO REQUIRED gio-2.0 gio-unix-2.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
find_path(XCB_RES_INCLUDE_DIR NAMES xcb/res.h REQUIRED)
find_path(XCB_CORE_INCLUDE_DIR NAMES xcb.h PATH_SUFFIXES xcb REQUIRED)
find_library(XCB_RES_LIBRARY NAMES xcb-res libxcb-res.so.0 REQUIRED)
unset(PUNTO_NATIVE_RECIPIENT_ABI CACHE)
if(PUNTO_WITH_XORG_RECIPIENT)
    pkg_check_modules(X11_XCB QUIET IMPORTED_TARGET x11-xcb)
    if(NOT X11_XCB_FOUND)
        message(FATAL_ERROR
            "Native recipient requires matched patched GTK/Xlib/XCB and x11-xcb development metadata")
    endif()
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES PkgConfig::GTK3 PkgConfig::X11
        PkgConfig::X11_XCB PkgConfig::XCB)
    check_cxx_source_compiles([=[
        #include <gdk/gdkx.h>
        #include <X11/Xlib-xcb.h>
        #include <xcb/xcbext.h>
        int main() {
            auto *volatile enable = &gdk_x11_display_enable_bounded_progress_v3;
            auto *volatile enabled = &gdk_x11_display_bounded_progress_enabled_v3;
            auto *volatile xlib_enable = &XTryBoundedEnable_v3;
            auto *volatile xlib_request = &XTryBoundedRequest_v1;
            auto *volatile xcb_poll = &xcb_bounded_poll_reply_v1;
            return !enable || !enabled || !xlib_enable || !xlib_request || !xcb_poll;
        }
    ]=] PUNTO_NATIVE_RECIPIENT_ABI)
    cmake_pop_check_state()
    if(NOT PUNTO_NATIVE_RECIPIENT_ABI)
        message(FATAL_ERROR "Native recipient requires matched patched GTK/Xlib/XCB ABI")
    endif()
    message(STATUS "GTK native recipient: ENABLED (matched ABI)")
else()
    message(STATUS "GTK native recipient: DISABLED (extensionless GUI)")
endif()
pkg_check_modules(XCB_XKB REQUIRED xcb-xkb)
pkg_check_modules(XCB_XFIXES REQUIRED xcb-xfixes)
pkg_check_modules(XCB_XTEST REQUIRED xcb-xtest)
pkg_check_modules(XCB_XINPUT REQUIRED xcb-xinput)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(XAU REQUIRED xau)

# Hunspell для проверки орфографии с учётом словоформ (падежи, склонения, времена)
pkg_check_modules(HUNSPELL hunspell)
if(HUNSPELL_FOUND)
    add_compile_definitions(HAVE_HUNSPELL)
    message(STATUS "Hunspell found: ${HUNSPELL_VERSION}")
else()
    message(STATUS "Hunspell not found - using hash-based dictionary only")
endif()

# Опционально: ICU для продвинутой работы с Unicode
# find_package(ICU COMPONENTS uc data)

# -----------------------------------------------------------------------------
# Исходные файлы
# -----------------------------------------------------------------------------
set(PUNTO_SOURCES
    src/main.cpp
    src/event_loop.cpp
    src/input_buffer.cpp
    src/config.cpp
    src/logger.cpp
    src/text_processor.cpp
    src/x11_session.cpp
    src/layout_analyzer.cpp
    src/dictionary.cpp
    src/ipc_server.cpp
    src/typo_corrector.cpp
    src/smart_bypass.cpp
    src/word_editor.cpp
    src/desktop_layout_client.cpp
    src/clipboard_manager.cpp
    src/macro_lock.cpp
    src/terminal_detection.cpp
    src/sound_manager.cpp
    src/undo_detector.cpp
    src/ordered_ingress_frame.cpp
    src/xorg_input_transport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_wire.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_control.c
)

set(PUNTO_HEADERS
    include/punto/types.hpp
    include/punto/event_loop.hpp
    include/punto/input_buffer.hpp
    include/punto/config.hpp
    include/punto/control_plane_state.hpp
    include/punto/logger.hpp
    include/punto/text_processor.hpp
    include/punto/x11_session.hpp
    include/punto/scancode_map.hpp
    include/punto/asm_utils.hpp
    include/punto/ngram_data.hpp
    include/punto/layout_analyzer.hpp
    include/punto/dictionary.hpp
    include/punto/ipc_server.hpp
    include/punto/concurrent_queue.hpp
    include/punto/analysis_worker_pool.hpp
    include/punto/typo_corrector.hpp
    include/punto/smart_bypass.hpp
    include/punto/runtime_tuning.hpp
    include/punto/runtime_file.hpp
    include/punto/runtime_health.hpp
    include/punto/sound_manager.hpp
    include/punto/undo_detector.hpp
    include/punto/desktop_layout_client.hpp
    include/punto/ordered_ingress_frame.hpp
    include/punto/xorg_input_transport.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/punto_ingress_wire.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/punto_ingress_stream.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/punto_ingress_control.h
)

# -----------------------------------------------------------------------------
# Основной исполняемый файл
# -----------------------------------------------------------------------------
function(configure_punto_daemon_target target_name)
    target_compile_definitions(${target_name}
        PRIVATE PUNTO_VERSION="${PROJECT_VERSION}"
                PUNTO_POLICY_HASH=${PUNTO_POLICY_HASH}
                PUNTO_BUILD_ID=${PUNTO_BUILD_ID}
    )

    target_include_directories(${target_name}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include
        ${SYSTEMD_INCLUDE_DIRS}
        ${GIO_INCLUDE_DIRS}
        ${JSON_GLIB_INCLUDE_DIRS}
        ${XCB_INCLUDE_DIRS}
        ${XCB_RES_INCLUDE_DIR}
        ${XCB_CORE_INCLUDE_DIR}
        ${XCB_XKB_INCLUDE_DIRS}
        ${XCB_XFIXES_INCLUDE_DIRS}
        ${XCB_XTEST_INCLUDE_DIRS}
        ${XCB_XINPUT_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${XAU_INCLUDE_DIRS}
        ${HUNSPELL_INCLUDE_DIRS}
    )

    target_link_libraries(${target_name}
        PRIVATE
        ${SYSTEMD_LIBRARIES}
        ${GIO_LIBRARIES}
        ${JSON_GLIB_LIBRARIES}
        ${XCB_LIBRARIES}
        ${XCB_RES_LIBRARY}
        ${XCB_XKB_LIBRARIES}
        ${XCB_XFIXES_LIBRARIES}
        ${XCB_XTEST_LIBRARIES}
        ${XCB_XINPUT_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        ${XAU_LIBRARIES}
        ${HUNSPELL_LIBRARIES}
        Threads::Threads
        OpenSSL::Crypto
        yaml-cpp::yaml-cpp
    )
endfunction()

add_executable(punto ${PUNTO_SOURCES} ${PUNTO_HEADERS})
configure_punto_daemon_target(punto)

add_executable(punto-session-bridge
    src/session_bridge.cpp
    src/accessible_context.cpp
    src/session_resolver.cpp)
target_compile_definitions(punto-session-bridge PRIVATE
    PUNTO_PROTOCOL_PATH="/usr/share/punto-switcher/protocol/punto-dbus-v1.xml"
)
target_include_directories(punto-session-bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GIO_INCLUDE_DIRS}
    ${SYSTEMD_INCLUDE_DIRS}
)
target_link_libraries(punto-session-bridge PRIVATE
    ${GIO_LIBRARIES}
    ${SYSTEMD_LIBRARIES}
)

# The editor adapter belongs only to the explicitly selected native SDK.
# Ordinary GUI builds use WordEditor replay without loading editor modules.
if(PUNTO_WITH_XORG_RECIPIENT)
add_library(punto-gtk-editor-adapter MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/../gtk/punto-editor-adapter/adapter.cpp)
set_target_properties(punto-gtk-editor-adapter PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "punto-gtk-editor-adapter"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gtk-3.0/modules")
function(configure_punto_gtk_adapter target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${JSON_GLIB_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE
        PUNTO_WITH_XORG_RECIPIENT=$<BOOL:${PUNTO_WITH_XORG_RECIPIENT}>
        PUNTO_POLICY_HASH=${PUNTO_POLICY_HASH}
        PUNTO_BUILD_ID=${PUNTO_BUILD_ID})
    target_link_libraries(${target} PRIVATE
        PkgConfig::GTK3 PkgConfig::X11 ${JSON_GLIB_LIBRARIES})
    if(PUNTO_WITH_XORG_RECIPIENT)
        target_sources(${target} PRIVATE src/xorg_recipient.cpp)
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include)
        target_link_libraries(${target} PRIVATE PkgConfig::X11_XCB PkgConfig::XCB)
    endif()
    # This module is loaded into independently built GTK applications.  A
    # Debug-only sanitizer runtime dependency would make those applications
    # abort before GTK initialization.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Debug>:-fno-sanitize=address,undefined>)
        target_link_options(${target} PRIVATE -Wl,--no-undefined
            $<$<CONFIG:Debug>:-fno-sanitize=address,undefined>)
    endif()
endfunction()
configure_punto_gtk_adapter(punto-gtk-editor-adapter)
endif()

# WordEditor owns bounded text execution; do not link the obsolete injector.
set(PUNTO_FORBIDDEN_PRODUCTION_SOURCES
    src/key_injector.cpp
    src/editor_transaction_client.cpp
)
foreach(forbidden_source IN LISTS PUNTO_FORBIDDEN_PRODUCTION_SOURCES)
    list(FIND PUNTO_SOURCES "${forbidden_source}" forbidden_index)
    if(NOT forbidden_index EQUAL -1)
        message(FATAL_ERROR
            "inactive mutation component entered production: ${forbidden_source}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Инсталляция
# -----------------------------------------------------------------------------
install(TARGETS punto DESTINATION bin)
install(TARGETS punto-session-bridge DESTINATION libexec)
install(FILES ${CMAKE_SOURCE_DIR}/../protocol/punto-dbus-v1.xml
        DESTINATION share/punto-switcher/protocol)
install(FILES ${CMAKE_SOURCE_DIR}/../punto-session-bridge.service
        DESTINATION lib/systemd/user)
install(FILES ${CMAKE_SOURCE_DIR}/../punto-session-bridge.desktop
        DESTINATION /etc/xdg/autostart)
install(FILES ${CMAKE_SOURCE_DIR}/../config.yaml
        DESTINATION /etc/punto
        RENAME config.yaml.new)
install(
    DIRECTORY
        ${CMAKE_SOURCE_DIR}/../gnome/punto-input-source@antonshalin76
    DESTINATION share/gnome-shell/extensions
    FILES_MATCHING PATTERN "*.js" PATTERN "*.xml" PATTERN "metadata.json"
)

# -----------------------------------------------------------------------------
# Tray-приложение (опционально)
# -----------------------------------------------------------------------------
option(BUILD_TRAY "Сборка tray-приложения punto-tray" ON)

if(BUILD_TRAY)
    # Пробуем сначала Ayatana (Ubuntu 22.04+), затем legacy
    pkg_check_modules(APPINDICATOR ayatana-appindicator3-0.1)
    if(NOT APPINDICATOR_FOUND)
        pkg_check_modules(APPINDICATOR appindicator3-0.1)
    endif()
    
    if(GTK3_FOUND AND APPINDICATOR_FOUND)
        add_executable(punto-tray
            src/tray/main.cpp
            src/tray/ipc_client.cpp
            src/tray/tray_app.cpp
            src/tray/settings_dialog.cpp
            src/config.cpp
        )

        target_compile_definitions(punto-tray
            PRIVATE PUNTO_VERSION="${PROJECT_VERSION}"
        )
        
        target_include_directories(punto-tray
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${GTK3_INCLUDE_DIRS}
                ${APPINDICATOR_INCLUDE_DIRS}
        )
        
        target_link_libraries(punto-tray
            PRIVATE
                ${GTK3_LIBRARIES}
                ${APPINDICATOR_LIBRARIES}
                yaml-cpp::yaml-cpp
        )
        
        # Определяем, какой AppIndicator используется
        if(APPINDICATOR_VERSION VERSION_GREATER_EQUAL "0.5")
            # Ayatana AppIndicator
            target_compile_definitions(punto-tray PRIVATE HAVE_AYATANA_APPINDICATOR)
        endif()
        
        install(TARGETS punto-tray DESTINATION bin)
        
        message(STATUS "Tray app:       ENABLED")
    else()
        message(STATUS "Tray app:       DISABLED (missing GTK3 or AppIndicator)")
        if(NOT GTK3_FOUND)
            message(STATUS "  - GTK3 not found. Install: sudo apt install libgtk-3-dev")
        endif()
        if(NOT APPINDICATOR_FOUND)
            message(STATUS "  - AppIndicator not found. Install: sudo apt install libayatana-appindicator3-dev")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# Информация о сборке
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== Punto Switcher C++ Build Configuration ===")
message(STATUS "Version:        ${PROJECT_VERSION}")
message(STATUS "Build Type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "systemd Found:  ${SYSTEMD_FOUND}")
message(STATUS "XCB Found:      ${XCB_FOUND}")
message(STATUS "XCB XKB Found:  ${XCB_XKB_FOUND}")
message(STATUS "")

# -----------------------------------------------------------------------------
# Tests (CTest)
# -----------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_executable(punto-ordered-ingress-contract
        tests/test_ordered_ingress_frame.cpp
        src/ordered_ingress_frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_wire.c)
    target_sources(punto-ordered-ingress-contract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_stream.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_control.c)
    target_include_directories(punto-ordered-ingress-contract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include)
    target_link_libraries(punto-ordered-ingress-contract PRIVATE OpenSSL::Crypto)
    add_test(NAME punto-ordered-ingress-contract
        COMMAND punto-ordered-ingress-contract)
    set_tests_properties(punto-ordered-ingress-contract PROPERTIES TIMEOUT 10)

    add_executable(punto-xorg-input-transport-contract
        tests/test_xorg_input_transport.cpp
        src/xorg_input_transport.cpp
        src/ordered_ingress_frame.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_wire.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_stream.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/src/punto_ingress_control.c)
    target_include_directories(punto-xorg-input-transport-contract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include)
    target_link_libraries(punto-xorg-input-transport-contract PRIVATE
        OpenSSL::Crypto Threads::Threads)
    add_test(NAME punto-xorg-input-transport-contract
        COMMAND punto-xorg-input-transport-contract)
    set_tests_properties(punto-xorg-input-transport-contract PROPERTIES TIMEOUT 10)

    if(PUNTO_WITH_XORG_RECIPIENT)
        add_executable(punto-xorg-recipient-notification
            tests/test_xorg_recipient_notification.cpp
            src/xorg_recipient.cpp)
        target_include_directories(punto-xorg-recipient-notification PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include)
        target_link_libraries(punto-xorg-recipient-notification PRIVATE
            PkgConfig::X11 PkgConfig::X11_XCB PkgConfig::XCB)
        add_test(NAME punto-xorg-recipient-notification
            COMMAND punto-xorg-recipient-notification)
        set_tests_properties(punto-xorg-recipient-notification PROPERTIES TIMEOUT 10)

        add_executable(punto-xorg-recipient-async
            tests/test_xorg_recipient_async.cpp
            src/xorg_recipient.cpp)
        target_include_directories(punto-xorg-recipient-async PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include)
        target_link_libraries(punto-xorg-recipient-async PRIVATE
            PkgConfig::X11 PkgConfig::X11_XCB PkgConfig::XCB)
        target_link_options(punto-xorg-recipient-async PRIVATE
            -Wl,--wrap=XTryBoundedEnable_v3
            -Wl,--wrap=XTryBoundedRequest_v1
            -Wl,--wrap=XGetXCBConnection
            -Wl,--wrap=xcb_connection_has_error
            -Wl,--wrap=xcb_query_extension
            -Wl,--wrap=xcb_send_request
            -Wl,--wrap=xcb_poll_for_reply
            -Wl,--wrap=xcb_discard_reply
            -Wl,--wrap=XESetWireToEvent
            -Wl,--wrap=XInitExtension
            -Wl,--wrap=_XReply)
        add_test(NAME punto-xorg-recipient-async COMMAND punto-xorg-recipient-async)
        set_tests_properties(punto-xorg-recipient-async PROPERTIES TIMEOUT 10)
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME punto-gtk-adapter-build-contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_gtk_adapter_build_contract.py)
    set_tests_properties(punto-gtk-adapter-build-contract PROPERTIES
        TIMEOUT 180 RUN_SERIAL TRUE)
    add_test(NAME punto-clang-tidy-inventory-contract
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_clang_tidy_inventory_contract.py)
    set_tests_properties(punto-clang-tidy-inventory-contract PROPERTIES TIMEOUT 15)
    find_program(GJS_EXECUTABLE gjs REQUIRED)
    pkg_check_modules(XCB_XFIXES REQUIRED xcb-xfixes)

    add_executable(punto-editor-transaction-contract
        tests/test_editor_transaction_contract.cpp
        src/editor_transaction_client.cpp)
    target_compile_definitions(punto-editor-transaction-contract PRIVATE
        PUNTO_EDITOR_TRANSACTION_TESTING=1)
    target_include_directories(punto-editor-transaction-contract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${JSON_GLIB_INCLUDE_DIRS}
        ${XCB_INCLUDE_DIRS})
    target_link_libraries(punto-editor-transaction-contract PRIVATE
        ${JSON_GLIB_LIBRARIES}
        ${XCB_LIBRARIES})

    add_executable(punto-session-bridge-test-bin
        src/session_bridge.cpp
        src/accessible_context.cpp
        src/session_resolver.cpp)
    target_compile_definitions(punto-session-bridge-test-bin PRIVATE
        PUNTO_PROTOCOL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/punto-dbus-v1.xml"
        PUNTO_TEST_SD_LOGIN=1
    )
    target_include_directories(punto-session-bridge-test-bin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${GIO_INCLUDE_DIRS}
        ${SYSTEMD_INCLUDE_DIRS}
    )
    target_link_libraries(punto-session-bridge-test-bin PRIVATE
        ${GIO_LIBRARIES}
        ${SYSTEMD_LIBRARIES}
    )

    add_executable(punto-session-bridge-app-e2e-bin
        src/session_bridge.cpp
        src/accessible_context.cpp
        src/session_resolver.cpp)
    target_compile_definitions(punto-session-bridge-app-e2e-bin PRIVATE
        PUNTO_PROTOCOL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../protocol/punto-dbus-v1.xml"
        PUNTO_TEST_SD_LOGIN=1
        PUNTO_SESSION_BRIDGE_APP_E2E=1
    )
    target_include_directories(punto-session-bridge-app-e2e-bin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${GIO_INCLUDE_DIRS}
        ${SYSTEMD_INCLUDE_DIRS}
    )
    target_link_libraries(punto-session-bridge-app-e2e-bin PRIVATE
        ${GIO_LIBRARIES}
        ${SYSTEMD_LIBRARIES}
    )

    add_executable(punto-session-resolver-contract
        tests/test_session_resolver_contract.cpp
        src/session_resolver.cpp)
    target_include_directories(punto-session-resolver-contract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SYSTEMD_INCLUDE_DIRS})
    target_link_libraries(punto-session-resolver-contract PRIVATE
        ${SYSTEMD_LIBRARIES})
    add_test(NAME punto-session-resolver-contract
        COMMAND punto-session-resolver-contract)
    set_tests_properties(punto-session-resolver-contract PROPERTIES TIMEOUT 10)

    add_executable(punto-session-resolver-probe
        src/session_resolver_probe.cpp
        src/session_resolver.cpp)
    target_include_directories(punto-session-resolver-probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${GIO_INCLUDE_DIRS}
        ${SYSTEMD_INCLUDE_DIRS})
    target_link_libraries(punto-session-resolver-probe PRIVATE
        ${GIO_LIBRARIES}
        ${SYSTEMD_LIBRARIES})

    add_executable(punto-desktop-layout-client-test-bin
        tests/test_desktop_layout_client_driver.cpp
        src/desktop_layout_client.cpp)
    target_compile_definitions(punto-desktop-layout-client-test-bin PRIVATE
        PUNTO_DESKTOP_LAYOUT_CLIENT_TESTING=1
        PUNTO_DESKTOP_LAYOUT_PUMP_TESTING=1)
    target_include_directories(punto-desktop-layout-client-test-bin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${GIO_INCLUDE_DIRS}
        ${JSON_GLIB_INCLUDE_DIRS}
        ${SYSTEMD_INCLUDE_DIRS})
    target_link_libraries(punto-desktop-layout-client-test-bin PRIVATE
        ${GIO_LIBRARIES}
        ${JSON_GLIB_LIBRARIES}
        ${SYSTEMD_LIBRARIES})
    target_link_options(punto-desktop-layout-client-test-bin PRIVATE
        -Wl,--wrap=connect -Wl,--wrap=poll -Wl,--wrap=sd_bus_process)

    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session REQUIRED)
    add_test(
        NAME punto-session-bridge-contract
        COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} --
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_session_bridge_e2e.py
                $<TARGET_FILE:punto-session-bridge-test-bin>
                $<TARGET_FILE:punto-desktop-layout-client-test-bin>
    )
    set_tests_properties(punto-session-bridge-contract PROPERTIES
        TIMEOUT 20 RUN_SERIAL TRUE)

    add_test(
        NAME punto-desktop-observer-contract
        COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} --
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_session_bridge_e2e.py
                --desktop-observer-contract
                $<TARGET_FILE:punto-session-bridge-test-bin>
    )
    set_tests_properties(punto-desktop-observer-contract PROPERTIES
        TIMEOUT 20 RUN_SERIAL TRUE)

    add_test(
        NAME punto-session-bridge-gjs-e2e
        COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} --
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_session_bridge_gjs_e2e.py
                $<TARGET_FILE:punto-session-bridge-test-bin>
                ${GJS_EXECUTABLE}
    )
    set_tests_properties(punto-session-bridge-gjs-e2e PROPERTIES
        TIMEOUT 15 RUN_SERIAL TRUE)

    add_test(
        NAME punto-activation-rollback-rehearsal
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_activation_rollback_rehearsal.sh
                ${CMAKE_CURRENT_SOURCE_DIR}/../deployment/punto-dpkg-transaction.sh
                ${CMAKE_CURRENT_SOURCE_DIR}/../deployment/punto-activation-orchestrator.sh
    )
    set_tests_properties(punto-activation-rollback-rehearsal PROPERTIES
        TIMEOUT 30 RUN_SERIAL TRUE)

    add_test(
        NAME punto-dpkg-transaction-unresolved
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_dpkg_transaction_unresolved.sh
                ${CMAKE_CURRENT_SOURCE_DIR}/../deployment/punto-dpkg-transaction.sh
    )
    set_tests_properties(punto-dpkg-transaction-unresolved PROPERTIES
        TIMEOUT 15 RUN_SERIAL TRUE)

    add_test(
        NAME punto-gjs-dbus-adapter-dispatch
        COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} --
                ${GJS_EXECUTABLE} -m
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_gjs_dbus_adapter_dispatch.mjs
                ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/punto-dbus-v1.xml
    )
    set_tests_properties(punto-gjs-dbus-adapter-dispatch PROPERTIES
        TIMEOUT 12 RUN_SERIAL TRUE)

    add_test(
        NAME punto-gnome46-x11-compat
        COMMAND ${GJS_EXECUTABLE} -m
                ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_gnome46_x11_compat.mjs
    )
    set_tests_properties(punto-gnome46-x11-compat PROPERTIES TIMEOUT 10)

    add_executable(punto-tests
        tests/test_main.cpp
        src/input_buffer.cpp
        src/ipc_server.cpp
        src/config.cpp
        src/text_processor.cpp
        src/terminal_detection.cpp
        src/typo_corrector.cpp
        src/layout_analyzer.cpp
        src/dictionary.cpp
        src/smart_bypass.cpp
        src/undo_detector.cpp
    )

    target_include_directories(punto-tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${HUNSPELL_INCLUDE_DIRS}
    )

    target_link_libraries(punto-tests
        PRIVATE
            Threads::Threads
            ${HUNSPELL_LIBRARIES}
            yaml-cpp::yaml-cpp
    )
    target_link_options(punto-tests PRIVATE -Wl,--wrap=fsync)

    add_test(NAME punto-tests COMMAND punto-tests)
    set_tests_properties(punto-tests PROPERTIES TIMEOUT 30)

    add_executable(punto-dictionary-contract
        tests/test_dictionary_contract.cpp
        src/dictionary.cpp
    )

    target_include_directories(punto-dictionary-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${HUNSPELL_INCLUDE_DIRS}
    )

    target_link_libraries(punto-dictionary-contract
        PRIVATE
            ${HUNSPELL_LIBRARIES}
            Threads::Threads
    )

    add_test(NAME punto-dictionary-contract COMMAND punto-dictionary-contract)
    set_tests_properties(punto-dictionary-contract PROPERTIES TIMEOUT 15)

    add_test(
        NAME punto-cmake-install-contract
        COMMAND ${CMAKE_COMMAND}
                -DPUNTO_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DPUNTO_STAGE_DIR=${CMAKE_BINARY_DIR}/cmake-install-contract-root
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cmake_install_contract.cmake
    )
    set_tests_properties(
        punto-cmake-install-contract
        PROPERTIES TIMEOUT 15 RUN_SERIAL TRUE
    )

    add_executable(punto-config-contract
        tests/test_config_contract.cpp
        src/config.cpp
    )

    target_include_directories(punto-config-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(punto-config-contract PRIVATE yaml-cpp::yaml-cpp)

    target_compile_definitions(punto-config-contract
        PRIVATE
            PUNTO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )

    add_test(NAME punto-config-contract COMMAND punto-config-contract)
    set_tests_properties(punto-config-contract PROPERTIES TIMEOUT 15)

    add_executable(punto-logger-contract
        tests/test_logger_contract.cpp
        src/logger.cpp
    )

    target_include_directories(punto-logger-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(punto-logger-contract PRIVATE Threads::Threads)

    add_test(NAME punto-logger-contract COMMAND punto-logger-contract)
    set_tests_properties(punto-logger-contract PROPERTIES TIMEOUT 15)

    add_executable(punto-ipc-contract
        tests/test_ipc_contract.cpp
        src/ipc_server.cpp
    )

    target_include_directories(punto-ipc-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_definitions(punto-ipc-contract
        PRIVATE PUNTO_IPC_MAILBOX_TESTING=1
    )

    target_link_libraries(punto-ipc-contract PRIVATE Threads::Threads)

    add_test(NAME punto-ipc-contract COMMAND punto-ipc-contract)
    set_tests_properties(punto-ipc-contract PROPERTIES TIMEOUT 90)

    add_executable(punto-analysis-input-contract
        tests/test_analysis_input_contract.cpp
        src/input_buffer.cpp
        src/layout_analyzer.cpp
        src/typo_corrector.cpp
    )

    target_include_directories(punto-analysis-input-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${HUNSPELL_INCLUDE_DIRS}
    )

    target_link_libraries(punto-analysis-input-contract
        PRIVATE
            Threads::Threads
            ${HUNSPELL_LIBRARIES}
    )

    add_test(NAME punto-analysis-input-contract
             COMMAND punto-analysis-input-contract)
    set_tests_properties(punto-analysis-input-contract PROPERTIES TIMEOUT 15)

    add_executable(punto-runtime-files-contract
        tests/test_runtime_files_contract.cpp
        src/macro_lock.cpp
    )

    target_include_directories(punto-runtime-files-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(punto-runtime-files-contract PRIVATE Threads::Threads)

    add_test(NAME punto-runtime-files-contract
             COMMAND punto-runtime-files-contract)
    set_tests_properties(punto-runtime-files-contract PROPERTIES TIMEOUT 15)

    add_executable(punto-key-injector-contract
        tests/test_key_injector_contract.cpp
        src/key_injector.cpp
    )

    target_include_directories(punto-key-injector-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_test(NAME punto-key-injector-contract
             COMMAND punto-key-injector-contract)
    set_tests_properties(punto-key-injector-contract PROPERTIES TIMEOUT 10)

    add_executable(punto-runtime-health-contract
        tests/test_runtime_health_contract.cpp
    )

    target_include_directories(punto-runtime-health-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_test(NAME punto-runtime-health-contract
             COMMAND punto-runtime-health-contract)
    set_tests_properties(punto-runtime-health-contract PROPERTIES TIMEOUT 10)

    add_executable(punto-undo-detector-contract
        tests/test_undo_detector_contract.cpp
        src/undo_detector.cpp
    )

    target_include_directories(punto-undo-detector-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_test(NAME punto-undo-detector-contract
             COMMAND punto-undo-detector-contract)
    target_compile_definitions(punto-undo-detector-contract PRIVATE PUNTO_TESTING)
    set_tests_properties(punto-undo-detector-contract PROPERTIES TIMEOUT 15)

    add_executable(punto-sound-manager-contract
        tests/test_sound_manager_contract.cpp
        src/sound_manager.cpp
    )

    target_include_directories(punto-sound-manager-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_definitions(punto-sound-manager-contract
        PRIVATE PUNTO_TESTING
    )

    target_link_libraries(punto-sound-manager-contract
        PRIVATE
            Threads::Threads
    )

    add_test(NAME punto-sound-manager-contract
             COMMAND punto-sound-manager-contract)
    set_tests_properties(punto-sound-manager-contract PROPERTIES TIMEOUT 15)

    add_executable(punto-tray-ipc-contract
        tests/test_tray_ipc_contract.cpp
        src/tray/ipc_client.cpp
    )

    target_include_directories(punto-tray-ipc-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_definitions(punto-tray-ipc-contract
        PRIVATE PUNTO_IPC_CLIENT_INTERNAL_TESTING=1
    )

    target_link_libraries(punto-tray-ipc-contract PRIVATE Threads::Threads)

    add_test(NAME punto-tray-ipc-contract COMMAND punto-tray-ipc-contract)
    set_tests_properties(punto-tray-ipc-contract PROPERTIES TIMEOUT 30)

    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    find_program(XPROP_EXECUTABLE xprop)
    if(PUNTO_WITH_XORG_RECIPIENT AND XVFB_RUN_EXECUTABLE AND XPROP_EXECUTABLE)
        add_test(
            NAME punto-editor-transaction-gtk-e2e
            COMMAND ${XVFB_RUN_EXECUTABLE} -a -s "-screen 0 1024x768x24 -extension GLX"
                    ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_gtk_editor_adapter_e2e.py
                    $<TARGET_FILE:punto-editor-transaction-contract>
                    $<TARGET_FILE:punto-gtk-editor-adapter>)
        set_tests_properties(punto-editor-transaction-gtk-e2e PROPERTIES
            TIMEOUT 30
            RUN_SERIAL TRUE
            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
    endif()
    if(GTK3_FOUND AND APPINDICATOR_FOUND)
        add_executable(punto-tray-app-contract
            tests/test_tray_app_contract.cpp
            src/tray/ipc_client.cpp
            src/tray/tray_app.cpp
            src/tray/settings_dialog.cpp
            src/config.cpp
        )

        target_compile_definitions(punto-tray-app-contract
            PRIVATE PUNTO_VERSION="${PROJECT_VERSION}"
        )

        target_include_directories(punto-tray-app-contract
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${GTK3_INCLUDE_DIRS}
                ${APPINDICATOR_INCLUDE_DIRS}
        )

        target_link_libraries(punto-tray-app-contract
            PRIVATE
                Threads::Threads
                ${GTK3_LIBRARIES}
                ${APPINDICATOR_LIBRARIES}
                yaml-cpp::yaml-cpp
        )

        if(APPINDICATOR_VERSION VERSION_GREATER_EQUAL "0.5")
            target_compile_definitions(
                punto-tray-app-contract PRIVATE HAVE_AYATANA_APPINDICATOR
            )
        endif()

        if(XVFB_RUN_EXECUTABLE)
            add_test(NAME punto-tray-app-contract
                     COMMAND ${CMAKE_COMMAND} -E env
                             --unset=DBUS_SESSION_BUS_ADDRESS
                             --unset=SESSION_MANAGER NO_AT_BRIDGE=1
                             ${XVFB_RUN_EXECUTABLE} -a -s "-screen 0 1024x768x24 -extension GLX"
                             $<TARGET_FILE:punto-tray-app-contract>)
            set_tests_properties(punto-tray-app-contract PROPERTIES TIMEOUT 10)
            # GTK/Pango worker retains Fontconfig's process-global FcInit cache.
            # Keep LeakSanitizer active for every other allocation in this target.
            set_tests_properties(punto-tray-app-contract PROPERTIES ENVIRONMENT
                "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tray_fontconfig.lsan.supp")
            if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND Python3_Interpreter_FOUND)
                add_test(NAME punto-tray-lsan-negative-control
                    COMMAND ${CMAKE_COMMAND} -E env
                            --unset=DBUS_SESSION_BUS_ADDRESS
                            --unset=SESSION_MANAGER NO_AT_BRIDGE=1
                            ${XVFB_RUN_EXECUTABLE} -a -s "-screen 0 1024x768x24 -extension GLX"
                            ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_tray_lsan_negative_control.py
                            $<TARGET_FILE:punto-tray-app-contract>)
                set_tests_properties(punto-tray-lsan-negative-control PROPERTIES
                    TIMEOUT 15
                    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tray_fontconfig.lsan.supp")
            endif()
        else()
            message(WARNING "xvfb-run not found: tray app contract disabled")
        endif()
    endif()

    add_executable(punto-x11-session-contract
        tests/test_x11_session_contract.cpp
        src/x11_session.cpp
    )

    target_include_directories(punto-x11-session-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${SYSTEMD_INCLUDE_DIRS}
            ${XCB_INCLUDE_DIRS}
            ${XCB_XKB_INCLUDE_DIRS}
            ${XAU_INCLUDE_DIRS}
    )

    target_link_libraries(punto-x11-session-contract
        PRIVATE
            Threads::Threads
            ${SYSTEMD_LIBRARIES}
            ${XCB_LIBRARIES}
            ${XCB_XKB_LIBRARIES}
            ${XAU_LIBRARIES}
    )

    add_test(NAME punto-x11-session-contract
             COMMAND punto-x11-session-contract)
    set_tests_properties(punto-x11-session-contract PROPERTIES TIMEOUT 120)

    add_executable(punto-x11-target-binding
        tests/test_x11_target_binding.cpp
        src/x11_session.cpp)
    target_include_directories(punto-x11-target-binding PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SYSTEMD_INCLUDE_DIRS} ${XCB_INCLUDE_DIRS}
        ${XCB_XKB_INCLUDE_DIRS} ${XAU_INCLUDE_DIRS})
    target_link_libraries(punto-x11-target-binding PRIVATE
        Threads::Threads ${SYSTEMD_LIBRARIES}
        ${XCB_LIBRARIES} ${XCB_XKB_LIBRARIES} ${XAU_LIBRARIES})
    add_test(NAME punto-x11-target-binding COMMAND punto-x11-target-binding)
    set_tests_properties(punto-x11-target-binding PROPERTIES TIMEOUT 15)

    add_executable(punto-clipboard-contract
        tests/test_clipboard_manager_contract.cpp
        src/clipboard_manager.cpp
        src/terminal_detection.cpp
        src/x11_session.cpp
    )
    target_link_options(punto-clipboard-contract PRIVATE
        -Wl,--wrap=xcb_poll_for_reply
        -Wl,--wrap=xcb_intern_atom
        -Wl,--wrap=xcb_prefetch_extension_data
        -Wl,--wrap=xcb_get_extension_data
        -Wl,--wrap=xcb_poll_for_event
        -Wl,--wrap=xcb_get_selection_owner
        -Wl,--wrap=xcb_grab_server_checked
        -Wl,--wrap=xcb_ungrab_server_checked
        -Wl,--wrap=xcb_set_selection_owner_checked
        -Wl,--wrap=xcb_change_property_checked
        -Wl,--wrap=xcb_delete_property_checked
        -Wl,--wrap=xcb_convert_selection_checked
        -Wl,--wrap=xcb_get_input_focus
        -Wl,--wrap=xcb_disconnect
    )

    target_include_directories(punto-clipboard-contract
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${SYSTEMD_INCLUDE_DIRS}
            ${XCB_INCLUDE_DIRS}
            ${XCB_XKB_INCLUDE_DIRS}
            ${XCB_XFIXES_INCLUDE_DIRS}
            ${XAU_INCLUDE_DIRS}
    )

    target_link_libraries(punto-clipboard-contract
        PRIVATE
            Threads::Threads
            ${SYSTEMD_LIBRARIES}
            ${XCB_LIBRARIES}
            ${XCB_XKB_LIBRARIES}
            ${XCB_XFIXES_LIBRARIES}
            ${XAU_LIBRARIES}
    )

    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME punto-clipboard-contract
                 COMMAND ${XVFB_RUN_EXECUTABLE} -a -s "-screen 0 1024x768x24 -extension GLX"
                         $<TARGET_FILE:punto-clipboard-contract>)
        set_tests_properties(punto-clipboard-contract
            PROPERTIES
                TIMEOUT 30
                RUN_SERIAL TRUE
        )
    else()
        message(WARNING "xvfb-run not found: clipboard contract disabled")
    endif()

    if(Python3_Interpreter_FOUND)
        add_test(
            NAME punto-cli-python-contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_cli_contract.py
        )
        set_tests_properties(
            punto-cli-python-contract
            PROPERTIES TIMEOUT 15 RUN_SERIAL TRUE
        )

        add_executable(punto-main-exception-probe
            ${PUNTO_SOURCES}
            ${PUNTO_HEADERS}
        )
        configure_punto_daemon_target(punto-main-exception-probe)
        target_compile_definitions(punto-main-exception-probe
            PRIVATE PUNTO_ENABLE_TEST_SEAMS=1
        )

        add_test(
            NAME punto-main-exception-contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_main_exception_contract.py
                    $<TARGET_FILE:punto-main-exception-probe>
        )
        set_tests_properties(
            punto-main-exception-contract
            PROPERTIES TIMEOUT 8 RUN_SERIAL TRUE
        )

        add_test(
            NAME punto-production-surface-contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_production_surface_contract.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
                    $<TARGET_FILE:punto>
        )
        set_tests_properties(
            punto-production-surface-contract
            PROPERTIES TIMEOUT 30
        )

        set(PUNTO_EVENT_LOOP_E2E_SOURCES ${PUNTO_SOURCES})
        list(REMOVE_ITEM PUNTO_EVENT_LOOP_E2E_SOURCES src/main.cpp)
        add_executable(punto-event-loop-e2e-driver
            tests/punto_event_loop_e2e_driver.cpp
            ${PUNTO_EVENT_LOOP_E2E_SOURCES}
        )

        configure_punto_daemon_target(punto-event-loop-e2e-driver)
        target_compile_definitions(punto-event-loop-e2e-driver
            PRIVATE
                PUNTO_EVENT_LOOP_E2E_TESTING=1
                PUNTO_LAYOUT_SWITCH_E2E_TESTING=1
                PUNTO_DESKTOP_LAYOUT_CLIENT_TESTING=1
                PUNTO_IPC_MAILBOX_TESTING=1)
        target_link_options(punto-event-loop-e2e-driver PRIVATE
            "-Wl,--wrap=xcb_query_keymap"
            -Wl,--wrap=xcb_test_fake_input_checked
            -Wl,--wrap=fsync -Wl,--wrap=xcb_xkb_get_map
            -Wl,--wrap=xcb_xkb_get_state
            -Wl,--wrap=xcb_xkb_latch_lock_state_checked
            -Wl,--wrap=xcb_connection_has_error
            -Wl,--wrap=xcb_poll_for_event
            -Wl,--wrap=xcb_poll_for_queued_event
            -Wl,--wrap=xcb_poll_for_reply
            -Wl,--wrap=xcb_get_input_focus
            -Wl,--wrap=xcb_prefetch_extension_data
            -Wl,--wrap=xcb_input_xi_query_version
            -Wl,--wrap=xcb_get_setup
            -Wl,--wrap=xcb_query_pointer
            -Wl,--wrap=xcb_convert_selection_checked
            -Wl,--wrap=xcb_intern_atom
            -Wl,--wrap=xcb_disconnect)

        option(PUNTO_BUILD_DELIVERED_TRIGGER_PROTOTYPE
            "Build isolated delivered-trigger experiment targets" OFF)
        if(PUNTO_BUILD_DELIVERED_TRIGGER_PROTOTYPE AND PUNTO_WITH_XORG_RECIPIENT)
            add_executable(punto-delivered-trigger-driver
                tests/punto_event_loop_e2e_driver.cpp
                ${PUNTO_EVENT_LOOP_E2E_SOURCES})
            configure_punto_daemon_target(punto-delivered-trigger-driver)
            target_compile_definitions(punto-delivered-trigger-driver PRIVATE
                PUNTO_EVENT_LOOP_E2E_TESTING=1
                PUNTO_LAYOUT_SWITCH_E2E_TESTING=1
                PUNTO_IPC_MAILBOX_TESTING=1
                PUNTO_DELIVERED_TRIGGER_PROTOTYPE=1)
            get_target_property(PUNTO_E2E_WRAP_OPTIONS
                punto-event-loop-e2e-driver LINK_OPTIONS)
            target_link_options(punto-delivered-trigger-driver PRIVATE
                ${PUNTO_E2E_WRAP_OPTIONS})

            add_executable(punto-delivered-trigger-probe
                tests/punto_delivered_trigger_probe.cpp
                src/editor_transaction_client.cpp
                src/text_processor.cpp)
            configure_punto_daemon_target(punto-delivered-trigger-probe)

            add_library(punto-delivered-trigger-gtk-adapter MODULE
                ${CMAKE_CURRENT_SOURCE_DIR}/../gtk/punto-editor-adapter/adapter.cpp)
            set_target_properties(punto-delivered-trigger-gtk-adapter PROPERTIES
                PREFIX "lib"
                OUTPUT_NAME "punto-delivered-trigger-gtk-adapter"
                LIBRARY_OUTPUT_DIRECTORY
                    "${CMAKE_CURRENT_BINARY_DIR}/prototype/gtk-3.0/modules")
            target_compile_definitions(punto-delivered-trigger-gtk-adapter PRIVATE
                PUNTO_DELIVERED_TRIGGER_PROTOTYPE=1)
            configure_punto_gtk_adapter(punto-delivered-trigger-gtk-adapter)

            add_library(punto-delayed-commit-im MODULE
                tests/punto_delayed_commit_im.cpp)
            set_target_properties(punto-delayed-commit-im PROPERTIES
                PREFIX "im-"
                OUTPUT_NAME "punto-delayed-commit"
                LIBRARY_OUTPUT_DIRECTORY
                    "${CMAKE_CURRENT_BINARY_DIR}/prototype/immodules")
            target_include_directories(punto-delayed-commit-im PRIVATE
                ${GTK3_INCLUDE_DIRS})
            target_link_libraries(punto-delayed-commit-im PRIVATE
                ${GTK3_LIBRARIES})
        endif()

        add_executable(punto-event-loop-layout-bridge-e2e-driver
            tests/punto_event_loop_e2e_driver.cpp
            ${PUNTO_EVENT_LOOP_E2E_SOURCES}
        )
        configure_punto_daemon_target(punto-event-loop-layout-bridge-e2e-driver)
        target_compile_definitions(punto-event-loop-layout-bridge-e2e-driver
            PRIVATE
                PUNTO_EVENT_LOOP_E2E_TESTING=1
                PUNTO_DESKTOP_LAYOUT_CLIENT_TESTING=1
                PUNTO_IPC_MAILBOX_TESTING=1)
        target_link_options(punto-event-loop-layout-bridge-e2e-driver PRIVATE
            "-Wl,--wrap=xcb_query_keymap"
            -Wl,--wrap=xcb_test_fake_input_checked
            -Wl,--wrap=fsync -Wl,--wrap=xcb_xkb_get_map
            -Wl,--wrap=xcb_xkb_get_state
            -Wl,--wrap=xcb_xkb_latch_lock_state_checked
            -Wl,--wrap=xcb_connection_has_error
            -Wl,--wrap=xcb_poll_for_event
            -Wl,--wrap=xcb_poll_for_queued_event
            -Wl,--wrap=xcb_poll_for_reply
            -Wl,--wrap=xcb_get_input_focus
            -Wl,--wrap=xcb_prefetch_extension_data
            -Wl,--wrap=xcb_input_xi_query_version
            -Wl,--wrap=xcb_get_setup
            -Wl,--wrap=xcb_query_pointer
            -Wl,--wrap=xcb_convert_selection_checked
            -Wl,--wrap=xcb_intern_atom
            -Wl,--wrap=xcb_disconnect)

        set(PUNTO_GTK_E2E_TESTS)
        foreach(PUNTO_GTK_E2E_SHARD RANGE 0 3)
            math(EXPR PUNTO_GTK_E2E_NUMBER "${PUNTO_GTK_E2E_SHARD} + 1")
            if(PUNTO_GTK_E2E_SHARD EQUAL 0)
                set(PUNTO_GTK_E2E_NAME punto-event-loop-gtk-e2e)
            else()
                set(PUNTO_GTK_E2E_NAME
                    punto-event-loop-gtk-e2e-shard-${PUNTO_GTK_E2E_NUMBER})
            endif()
            add_test(
                NAME ${PUNTO_GTK_E2E_NAME}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_gtk_e2e.py
                        $<TARGET_FILE:punto-event-loop-e2e-driver>
                        $<TARGET_FILE:punto-session-bridge-app-e2e-bin>
            )
            set_tests_properties(
                ${PUNTO_GTK_E2E_NAME}
                PROPERTIES
                    TIMEOUT 120
                    SKIP_RETURN_CODE 77
                    RUN_SERIAL TRUE
                    ENVIRONMENT
                        "PUNTO_EVENT_LOOP_E2E_SHARD=${PUNTO_GTK_E2E_SHARD}/4"
            )
            list(APPEND PUNTO_GTK_E2E_TESTS ${PUNTO_GTK_E2E_NAME})
        endforeach()
        foreach(PUNTO_NATIVE_EDITOR qt editor-matrix swt-editor webkit-editor)
            string(REPLACE "-" "_" PUNTO_NATIVE_EDITOR_SCRIPT ${PUNTO_NATIVE_EDITOR})
            if(PUNTO_NATIVE_EDITOR STREQUAL "qt")
                set(PUNTO_NATIVE_EDITOR_SCRIPT event_loop_qt)
            endif()
            add_test(
                NAME punto-${PUNTO_NATIVE_EDITOR}-e2e
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_${PUNTO_NATIVE_EDITOR_SCRIPT}_e2e.py
                        $<TARGET_FILE:punto-event-loop-layout-bridge-e2e-driver>
                        $<TARGET_FILE:punto-session-bridge-app-e2e-bin>
            )
            set_tests_properties(punto-${PUNTO_NATIVE_EDITOR}-e2e
                PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
                    ENVIRONMENT "PUNTO_REAL_LAYOUT_BRIDGE_E2E=1")
        endforeach()
        set(PUNTO_GTK_SHARED_SESSION_CASE_LIST)
        foreach(PUNTO_GTK_SHARED_SESSION_REPEAT RANGE 1 10)
            list(APPEND PUNTO_GTK_SHARED_SESSION_CASE_LIST
                test_combined_fix_preserves_trailing_punctuation
                test_desktop_source_without_xkb_transition_fails_closed)
        endforeach()
        list(JOIN PUNTO_GTK_SHARED_SESSION_CASE_LIST ","
            PUNTO_GTK_SHARED_SESSION_CASES)
        add_test(
            NAME punto-event-loop-gtk-shared-session-regression
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_gtk_e2e.py
                    $<TARGET_FILE:punto-event-loop-e2e-driver>
                    $<TARGET_FILE:punto-session-bridge-app-e2e-bin>
        )
        set_tests_properties(
            punto-event-loop-gtk-shared-session-regression
            PROPERTIES
                TIMEOUT 90
                SKIP_RETURN_CODE 77
                RUN_SERIAL TRUE
                ENVIRONMENT
                    "PUNTO_EVENT_LOOP_E2E_TEST=${PUNTO_GTK_SHARED_SESSION_CASES}"
        )
        set(PUNTO_REAL_LAYOUT_CASES
            real_layout_bridge_manual_correction
            real_layout_bridge_automatic_correction
            real_layout_bridge_helper_replacement_replays_once
            real_layout_bridge_stalled_state_preserves_input_order
            real_layout_bridge_xkb_mismatch_recovers
            real_layout_bridge_selection_tolerates_slow_desktop_transitions
            real_layout_bridge_selection_disturbance_refuses_and_recovers
            test_selection_layout_hotkey_changes_real_editor_text
            test_empty_word_manual_hotkey_is_observed_and_recovers
            test_repeated_manual_word_correction_is_reversible
            test_immediate_undo_restores_word_and_then_passes_through
            test_vte_word_layout_correction_reaches_real_pty
            test_vte_space_automatic_correction_and_immediate_inverse
            test_vte_selection_layout_inserts_without_deleting_scrollback)
        list(JOIN PUNTO_REAL_LAYOUT_CASES "," PUNTO_REAL_LAYOUT_CASES)
        add_test(
            NAME punto-event-loop-layout-bridge-e2e
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_gtk_e2e.py
                    $<TARGET_FILE:punto-event-loop-layout-bridge-e2e-driver>
                    $<TARGET_FILE:punto-session-bridge-app-e2e-bin>
        )
        set_tests_properties(
            punto-event-loop-layout-bridge-e2e
            PROPERTIES
                TIMEOUT 45
                RUN_SERIAL TRUE
                ENVIRONMENT
                    "PUNTO_REAL_LAYOUT_BRIDGE_E2E=1;PUNTO_EVENT_LOOP_E2E_TEST=${PUNTO_REAL_LAYOUT_CASES}"
        )
        add_test(
            NAME punto-event-loop-sandbox-contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_sandbox_contract.py
                    $<TARGET_FILE:punto-event-loop-e2e-driver>
        )
        set_tests_properties(
            punto-event-loop-sandbox-contract
            PROPERTIES TIMEOUT 70 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE
        )
        set(PUNTO_CHROMIUM_E2E_TESTS)
        foreach(PUNTO_CHROMIUM_E2E_SHARD RANGE 0 3)
            math(EXPR PUNTO_CHROMIUM_E2E_NUMBER "${PUNTO_CHROMIUM_E2E_SHARD} + 1")
            if(PUNTO_CHROMIUM_E2E_SHARD EQUAL 0)
                set(PUNTO_CHROMIUM_E2E_NAME punto-event-loop-chromium-e2e)
            else()
                set(PUNTO_CHROMIUM_E2E_NAME
                    punto-event-loop-chromium-e2e-shard-${PUNTO_CHROMIUM_E2E_NUMBER})
            endif()
            add_test(
                NAME ${PUNTO_CHROMIUM_E2E_NAME}
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_chromium_e2e.py
                        $<TARGET_FILE:punto-event-loop-e2e-driver>
                        $<TARGET_FILE:punto-session-bridge-app-e2e-bin>
            )
            set_tests_properties(
                ${PUNTO_CHROMIUM_E2E_NAME}
                PROPERTIES
                    TIMEOUT 120
                    SKIP_RETURN_CODE 77
                    RUN_SERIAL TRUE
                    ENVIRONMENT "PUNTO_EVENT_LOOP_E2E_SHARD=${PUNTO_CHROMIUM_E2E_SHARD}/4"
            )
            list(APPEND PUNTO_CHROMIUM_E2E_TESTS ${PUNTO_CHROMIUM_E2E_NAME})
        endforeach()
        add_test(
            NAME punto-event-loop-chromium-layout-bridge-e2e
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_chromium_e2e.py
                    $<TARGET_FILE:punto-event-loop-layout-bridge-e2e-driver>
                    $<TARGET_FILE:punto-session-bridge-app-e2e-bin>
        )
        set_tests_properties(
            punto-event-loop-chromium-layout-bridge-e2e
            PROPERTIES
                TIMEOUT 45
                SKIP_RETURN_CODE 77
                RUN_SERIAL TRUE
                ENVIRONMENT
                    "PUNTO_REAL_LAYOUT_BRIDGE_E2E=1;PUNTO_EVENT_LOOP_E2E_TEST=real_layout_bridge_paste_retains_payload_during_layout_settle,real_layout_bridge_selection_ru_to_en_uses_reasserting_desktop_owner"
        )
        add_test(
            NAME punto-event-loop-chromium-soak
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_chromium_e2e.py
                    $<TARGET_FILE:punto-event-loop-e2e-driver>
                    $<TARGET_FILE:punto-session-bridge-app-e2e-bin>
        )
        set_tests_properties(
            punto-event-loop-chromium-soak
            PROPERTIES
                TIMEOUT 90
                SKIP_RETURN_CODE 77
                RUN_SERIAL TRUE
                ENVIRONMENT
                    "PUNTO_EVENT_LOOP_E2E_TEST=test_hundred_manual_conversions_survive_failure_cycles"
        )
        add_test(
            NAME punto-event-loop-chromium-failure-soak
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_chromium_e2e.py
                    $<TARGET_FILE:punto-event-loop-e2e-driver>
                    $<TARGET_FILE:punto-session-bridge-app-e2e-bin>
        )
        set_tests_properties(
            punto-event-loop-chromium-failure-soak
            PROPERTIES
                TIMEOUT 70
                SKIP_RETURN_CODE 77
                RUN_SERIAL TRUE
                ENVIRONMENT
                    "PUNTO_EVENT_LOOP_E2E_TEST=test_twenty_six_initial_failures_recover_from_zero_dispatches"
        )
        if(DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "")
            set_property(
                TEST ${PUNTO_GTK_E2E_TESTS} punto-event-loop-sandbox-contract
                     ${PUNTO_CHROMIUM_E2E_TESTS}
                     punto-event-loop-chromium-layout-bridge-e2e
                     punto-event-loop-chromium-soak
                     punto-event-loop-chromium-failure-soak
                APPEND PROPERTY ENVIRONMENT
                    "PUNTO_REQUIRE_EVENT_LOOP_E2E=1"
            )
        endif()

        add_test(
            NAME punto-desktop-editor-platform-e2e
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_desktop_editor_e2e.py
        )
        set_tests_properties(
            punto-desktop-editor-platform-e2e
            PROPERTIES
                TIMEOUT 30
                RUN_SERIAL TRUE
        )

        add_test(
            NAME punto-daemon-process-contract
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_daemon_process_contract.py
                    $<TARGET_FILE:punto>
                    ${CMAKE_CURRENT_SOURCE_DIR}/../punto-cli.sh
                    $<TARGET_FILE:punto-tray-ipc-contract>
        )
        set_tests_properties(
            punto-daemon-process-contract
            PROPERTIES
                TIMEOUT 120
                SKIP_RETURN_CODE 77
                RUN_SERIAL TRUE
        )
        if(DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "")
            set_property(
                TEST punto-daemon-process-contract
                APPEND PROPERTY ENVIRONMENT
                    "PUNTO_REQUIRE_DAEMON_PROCESS_CONTRACT=1"
            )
        endif()
    else()
        message(WARNING "Python3 not found: daemon process contract disabled")
    endif()

    if(GTK3_FOUND)
        add_executable(punto-clipboard-gtk-contract
            tests/test_clipboard_gtk_contract.cpp
            src/clipboard_manager.cpp
            src/terminal_detection.cpp
            src/x11_session.cpp
        )

        target_include_directories(punto-clipboard-gtk-contract
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${GTK3_INCLUDE_DIRS}
                ${SYSTEMD_INCLUDE_DIRS}
                ${XCB_INCLUDE_DIRS}
                ${XCB_XKB_INCLUDE_DIRS}
                ${XCB_XFIXES_INCLUDE_DIRS}
                ${XAU_INCLUDE_DIRS}
        )

        target_link_libraries(punto-clipboard-gtk-contract
            PRIVATE
                Threads::Threads
                ${GTK3_LIBRARIES}
                ${SYSTEMD_LIBRARIES}
                ${XCB_LIBRARIES}
                ${XCB_XKB_LIBRARIES}
                ${XCB_XFIXES_LIBRARIES}
                ${XAU_LIBRARIES}
        )

        if(XVFB_RUN_EXECUTABLE)
            add_test(
                NAME punto-clipboard-gtk-contract
                COMMAND ${XVFB_RUN_EXECUTABLE} -a -s "-screen 0 1024x768x24 -extension GLX"
                        $<TARGET_FILE:punto-clipboard-gtk-contract>
            )
            set_tests_properties(
                punto-clipboard-gtk-contract
                PROPERTIES
                    TIMEOUT 30
                    RUN_SERIAL TRUE
                    # gtk_init keeps process-global Cairo/GDK/Pango/AT-SPI
                    # caches alive in this short fixture. Address/UB checks
                    # stay enabled; leak checks run in non-GTK targets.
                    ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
            )
        endif()

        add_executable(punto-settings-persistence-contract
            tests/test_settings_persistence_contract.cpp
            src/tray/settings_dialog.cpp
            src/config.cpp
        )

        target_include_directories(punto-settings-persistence-contract
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${GTK3_INCLUDE_DIRS}
        )

        target_link_libraries(punto-settings-persistence-contract
            PRIVATE
                ${GTK3_LIBRARIES}
                yaml-cpp::yaml-cpp
        )

        target_compile_definitions(punto-settings-persistence-contract
            PRIVATE
                PUNTO_VERSION="${PROJECT_VERSION}"
                PUNTO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
        )

        add_test(
            NAME punto-settings-persistence-contract
            COMMAND punto-settings-persistence-contract
        )
        set_tests_properties(
            punto-settings-persistence-contract
            PROPERTIES TIMEOUT 15
        )
    endif()
endif()
