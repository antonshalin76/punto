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
project(punto VERSION "${PUNTO_VERSION}" LANGUAGES CXX)

# Для clangd/IDE: генерируем compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Стандарты и флаги компиляции
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
        $<$<CONFIG:Release>:-fPIE>
        $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=3>
        $<$<CONFIG:Release>:-D_GLIBCXX_ASSERTIONS>
    )
    add_link_options(
        $<$<CONFIG:Release>:-pie>
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
find_package(yaml-cpp REQUIRED)

# Native session discovery and recoverable X11 transport.
pkg_check_modules(SYSTEMD REQUIRED libsystemd)
pkg_check_modules(XCB REQUIRED xcb)
pkg_check_modules(XCB_XKB REQUIRED xcb-xkb)
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
)

# -----------------------------------------------------------------------------
# Основной исполняемый файл
# -----------------------------------------------------------------------------
function(configure_punto_daemon_target target_name)
    target_compile_definitions(${target_name}
        PRIVATE PUNTO_VERSION="${PROJECT_VERSION}"
    )

    target_include_directories(${target_name}
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SYSTEMD_INCLUDE_DIRS}
        ${XCB_INCLUDE_DIRS}
        ${XCB_XKB_INCLUDE_DIRS}
        ${XAU_INCLUDE_DIRS}
        ${HUNSPELL_INCLUDE_DIRS}
    )

    target_link_libraries(${target_name}
        PRIVATE
        ${SYSTEMD_LIBRARIES}
        ${XCB_LIBRARIES}
        ${XCB_XKB_LIBRARIES}
        ${XAU_LIBRARIES}
        ${HUNSPELL_LIBRARIES}
        Threads::Threads
        yaml-cpp::yaml-cpp
    )
endfunction()

add_executable(punto ${PUNTO_SOURCES} ${PUNTO_HEADERS})
configure_punto_daemon_target(punto)

# Text mutation is intentionally disabled in the v2.8.7 production daemon.
# Keep this boundary configure-time fatal; the inactive components remain
# available only to the isolated contract-test targets below.
set(PUNTO_FORBIDDEN_PRODUCTION_SOURCES
    src/clipboard_manager.cpp
    src/key_injector.cpp
    src/macro_lock.cpp
    src/sound_manager.cpp
    src/terminal_detection.cpp
    src/undo_detector.cpp
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
install(FILES ${CMAKE_SOURCE_DIR}/../config.yaml
        DESTINATION /etc/punto
        RENAME config.yaml.new)

# -----------------------------------------------------------------------------
# Tray-приложение (опционально)
# -----------------------------------------------------------------------------
option(BUILD_TRAY "Сборка tray-приложения punto-tray" ON)

if(BUILD_TRAY)
    pkg_check_modules(GTK3 gtk+-3.0)
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
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    pkg_check_modules(XCB_XFIXES REQUIRED xcb-xfixes)

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

        add_test(NAME punto-tray-app-contract
                 COMMAND punto-tray-app-contract)
        set_tests_properties(punto-tray-app-contract PROPERTIES TIMEOUT 10)
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
    set_tests_properties(punto-x11-session-contract PROPERTIES TIMEOUT 15)

    add_executable(punto-clipboard-contract
        tests/test_clipboard_manager_contract.cpp
        src/clipboard_manager.cpp
        src/terminal_detection.cpp
        src/x11_session.cpp
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

    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    if(XVFB_RUN_EXECUTABLE)
        add_test(NAME punto-clipboard-contract
                 COMMAND ${XVFB_RUN_EXECUTABLE} -a $<TARGET_FILE:punto-clipboard-contract>)
        set_tests_properties(punto-clipboard-contract
            PROPERTIES
                TIMEOUT 30
                RUN_SERIAL TRUE
        )
    else()
        message(WARNING "xvfb-run not found: clipboard contract disabled")
    endif()

    if(Python3_Interpreter_FOUND)
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
            PROPERTIES TIMEOUT 10
        )

        set(PUNTO_EVENT_LOOP_E2E_SOURCES ${PUNTO_SOURCES})
        list(REMOVE_ITEM PUNTO_EVENT_LOOP_E2E_SOURCES src/main.cpp)
        add_executable(punto-event-loop-e2e-driver
            tests/punto_event_loop_e2e_driver.cpp
            ${PUNTO_EVENT_LOOP_E2E_SOURCES}
        )

        target_include_directories(punto-event-loop-e2e-driver
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${SYSTEMD_INCLUDE_DIRS}
                ${XCB_INCLUDE_DIRS}
                ${XCB_XKB_INCLUDE_DIRS}
                ${XAU_INCLUDE_DIRS}
                ${HUNSPELL_INCLUDE_DIRS}
        )

        target_link_libraries(punto-event-loop-e2e-driver
            PRIVATE
                ${SYSTEMD_LIBRARIES}
                ${XCB_LIBRARIES}
                ${XCB_XKB_LIBRARIES}
                ${XAU_LIBRARIES}
                ${HUNSPELL_LIBRARIES}
                Threads::Threads
                yaml-cpp::yaml-cpp
        )

        add_test(
            NAME punto-event-loop-gtk-e2e
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_event_loop_gtk_e2e.py
                    $<TARGET_FILE:punto-event-loop-e2e-driver>
        )
        set_tests_properties(
            punto-event-loop-gtk-e2e
            PROPERTIES
                TIMEOUT 90
                SKIP_RETURN_CODE 77
                RUN_SERIAL TRUE
        )
        if(DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "")
            set_property(
                TEST punto-event-loop-gtk-e2e
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
                COMMAND ${XVFB_RUN_EXECUTABLE} -a
                        $<TARGET_FILE:punto-clipboard-gtk-contract>
            )
            set_tests_properties(
                punto-clipboard-gtk-contract
                PROPERTIES
                    TIMEOUT 30
                    RUN_SERIAL TRUE
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
