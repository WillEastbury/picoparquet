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

set(PICOCOMPRESS_ROOT "" CACHE PATH "PicoCompress source root for codec integration")
set(PICOCOMPRESS_INCLUDE_DIR "" CACHE PATH "Path containing picocompress/host.h")
option(PICOPARQUET_WITH_PICOCOMPRESS "Build the optional PicoCompress codec bridge" ON)

if (PICOPARQUET_WITH_PICOCOMPRESS)
    if(PICOCOMPRESS_ROOT AND NOT PICOCOMPRESS_INCLUDE_DIR)
        set(PICOCOMPRESS_INCLUDE_DIR "${PICOCOMPRESS_ROOT}/include")
    endif()
    if(NOT PICOCOMPRESS_INCLUDE_DIR)
        find_path(PICOCOMPRESS_INCLUDE_DIR picocompress/host.h)
    endif()
    if(PICOCOMPRESS_INCLUDE_DIR)
        add_library(picoparquet_picocompress STATIC
            src/picoparquet_picocompress.c
        )
        target_compile_features(picoparquet_picocompress PUBLIC c_std_11)
        target_include_directories(picoparquet_picocompress
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${PICOCOMPRESS_INCLUDE_DIR}
        )
        target_link_libraries(picoparquet_picocompress PUBLIC picoparquet)
        if (MSVC)
            target_compile_options(picoparquet_picocompress PRIVATE /W4)
        else()
            target_compile_options(picoparquet_picocompress PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    else()
        message(STATUS "PicoCompress headers not found; PicoParquet codec bridge skipped")
    endif()
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

    if(TARGET picoparquet_picocompress AND PICOCOMPRESS_ROOT)
        add_executable(picoparquet_picocompress_tests
            tests/test_picocompress_adapter.c
            ${PICOCOMPRESS_ROOT}/src/host.c
        )
        target_compile_features(picoparquet_picocompress_tests PRIVATE c_std_11)
        target_include_directories(picoparquet_picocompress_tests PRIVATE
            ${PICOCOMPRESS_ROOT}/include
        )
        target_link_libraries(picoparquet_picocompress_tests PRIVATE
            picoparquet_picocompress
            ${CMAKE_DL_LIBS}
        )
        if (MSVC)
            target_compile_options(picoparquet_picocompress_tests PRIVATE /W4)
        else()
            target_compile_options(picoparquet_picocompress_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME picoparquet_picocompress_tests COMMAND picoparquet_picocompress_tests)
    endif()
endif()
