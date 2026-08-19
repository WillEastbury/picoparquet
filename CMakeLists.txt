cmake_minimum_required(VERSION 3.15)

project(picoparquet VERSION 0.1.0 LANGUAGES C)

add_library(picoparquet STATIC
    src/picoparquet.c
)

target_compile_features(picoparquet PUBLIC c_std_11)
target_include_directories(picoparquet
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (MSVC)
    target_compile_options(picoparquet PRIVATE /W4)
else()
    target_compile_options(picoparquet PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(PICOPARQUET_BUILD_TESTS "Build PicoParquet tests" ON)

if (PICOPARQUET_BUILD_TESTS)
    enable_testing()
    add_executable(picoparquet_tests
        tests/test_picoparquet.c
    )
    target_compile_features(picoparquet_tests PRIVATE c_std_11)
    target_link_libraries(picoparquet_tests PRIVATE picoparquet)
    if (MSVC)
        target_compile_options(picoparquet_tests PRIVATE /W4)
    else()
        target_compile_options(picoparquet_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME picoparquet_deterministic COMMAND picoparquet_tests)
endif()
