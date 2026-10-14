cmake_minimum_required(VERSION 3.20)

project(HANA VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HANA_BUILD_TESTS "Build HANA unit tests" ON)

if(MSVC)
    set(HANA_WARNING_FLAGS /W4 /permissive- /utf-8)
else()
    set(HANA_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

# Layer 1: language (lexer, parser, analyzer, IR)
add_library(hana_lang STATIC
    src/lang/token.cpp
    src/lang/lexer.cpp
)
target_include_directories(hana_lang PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(hana_lang PRIVATE ${HANA_WARNING_FLAGS})

# Layer 4: platform (file system, window, input, main loop)
add_library(hana_platform STATIC
    src/platform/native/native_file_system.cpp
)
target_include_directories(hana_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(hana_platform PRIVATE ${HANA_WARNING_FLAGS})

# CLI: hana run / build / check
add_executable(hana src/cli/main.cpp)
target_link_libraries(hana PRIVATE hana_lang hana_platform)
target_compile_options(hana PRIVATE ${HANA_WARNING_FLAGS})
target_compile_definitions(hana PRIVATE HANA_VERSION="${PROJECT_VERSION}")

if(HANA_BUILD_TESTS)
    enable_testing()
    add_executable(hana_lang_tests tests/lexer_tests.cpp)
    target_link_libraries(hana_lang_tests PRIVATE hana_lang)
    target_compile_options(hana_lang_tests PRIVATE ${HANA_WARNING_FLAGS})
    add_test(NAME hana_lang_tests COMMAND hana_lang_tests)
endif()
