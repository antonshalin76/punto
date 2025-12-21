# =============================================================================
# Punto Switcher для Linux (C++20 версия)
# Высокопроизводительный плагин для interception-tools
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(punto VERSION 2.1.0 LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Стандарты и флаги компиляции
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Оптимизация под конкретную архитектуру
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -flto=auto -DNDEBUG")
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

# -----------------------------------------------------------------------------
# Поиск зависимостей
# -----------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

# X11 для работы с буфером обмена
pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(XCB xcb)

# Опционально: ICU для продвинутой работы с Unicode
# find_package(ICU COMPONENTS uc data)

# -----------------------------------------------------------------------------
# Исходные файлы
# -----------------------------------------------------------------------------
set(PUNTO_SOURCES
    src/main.cpp
    src/event_loop.cpp
    src/input_buffer.cpp
    src/key_injector.cpp
    src/config.cpp
    src/text_processor.cpp
    src/clipboard_manager.cpp
    src/x11_session.cpp
    src/layout_analyzer.cpp
    src/dictionary.cpp
    src/ipc_server.cpp
)

set(PUNTO_HEADERS
    include/punto/types.hpp
    include/punto/event_loop.hpp
    include/punto/input_buffer.hpp
    include/punto/key_injector.hpp
    include/punto/config.hpp
    include/punto/text_processor.hpp
    include/punto/clipboard_manager.hpp
    include/punto/x11_session.hpp
    include/punto/scancode_map.hpp
    include/punto/asm_utils.hpp
    include/punto/ngram_data.hpp
    include/punto/layout_analyzer.hpp
    include/punto/dictionary.hpp
    include/punto/ipc_server.hpp
)

# -----------------------------------------------------------------------------
# Основной исполняемый файл
# -----------------------------------------------------------------------------
add_executable(punto ${PUNTO_SOURCES} ${PUNTO_HEADERS})

target_include_directories(punto 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${X11_INCLUDE_DIRS}
)

target_link_libraries(punto 
    PRIVATE 
        ${X11_LIBRARIES}
)

# Если XCB найден, используем его
if(XCB_FOUND)
    target_compile_definitions(punto PRIVATE HAVE_XCB)
    target_include_directories(punto PRIVATE ${XCB_INCLUDE_DIRS})
    target_link_libraries(punto PRIVATE ${XCB_LIBRARIES})
endif()

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
# Тесты (опционально)
# -----------------------------------------------------------------------------
option(BUILD_TESTS "Сборка unit-тестов" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Информация о сборке
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== Punto Switcher C++ Build Configuration ===")
message(STATUS "Version:        ${PROJECT_VERSION}")
message(STATUS "Build Type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "X11 Found:      ${X11_FOUND}")
message(STATUS "XCB Found:      ${XCB_FOUND}")
message(STATUS "")
