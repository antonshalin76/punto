add_executable(punto-tests
    test_history_manager.cpp
)

target_include_directories(punto-tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_features(punto-tests PRIVATE cxx_std_20)

enable_testing()
add_test(NAME punto-tests COMMAND punto-tests)
