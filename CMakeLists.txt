cmake_minimum_required(VERSION 3.20)

project(HANA VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HANA_BUILD_TESTS "Build HANA tests" ON)

# LLVM's version file only accepts an exact major.minor request, so the
# major version is checked here instead.
find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 18)
    message(FATAL_ERROR "HANA needs LLVM 18, found ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

if(LLVM_LINK_LLVM_DYLIB)
    set(HANA_LLVM_LIBS LLVM)
else()
    llvm_map_components_to_libnames(HANA_LLVM_LIBS
        core support passes target targetparser analysis nativecodegen)
endif()

if(MSVC)
    set(HANA_WARNING_FLAGS /W4 /permissive- /utf-8)
else()
    set(HANA_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

# Diagnostics, source files, UTF-8
add_library(hana_support STATIC
    src/support/diagnostic.cpp
    src/support/utf8.cpp
)
target_include_directories(hana_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(hana_support PRIVATE ${HANA_WARNING_FLAGS})

# Front end: JSON layer, code lexer and parser, semantic analysis
add_library(hana_lang STATIC
    src/lang/token.cpp
    src/lang/code_lexer.cpp
    src/lang/code_parser.cpp
    src/lang/outer_parser.cpp
    src/lang/ast.cpp
    src/lang/types.cpp
    src/lang/sema.cpp
)
target_link_libraries(hana_lang PUBLIC hana_support)
target_compile_options(hana_lang PRIVATE ${HANA_WARNING_FLAGS})

# Back end: LLVM IR generation and object emission
add_library(hana_codegen STATIC
    src/codegen/codegen.cpp
)
target_link_libraries(hana_codegen PUBLIC hana_lang ${HANA_LLVM_LIBS})
target_include_directories(hana_codegen SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
separate_arguments(HANA_LLVM_DEFINITIONS NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(hana_codegen PUBLIC ${HANA_LLVM_DEFINITIONS})
target_compile_options(hana_codegen PRIVATE ${HANA_WARNING_FLAGS})

# Compilation pipeline: check / build / run
add_library(hana_driver STATIC
    src/driver/driver.cpp
    src/driver/process.cpp
)
target_link_libraries(hana_driver PUBLIC hana_codegen)
target_compile_options(hana_driver PRIVATE ${HANA_WARNING_FLAGS})

# Runtime library linked into every compiled HANA program. It is placed next
# to the hana executable, where the driver looks for it.
add_library(hana_rt STATIC runtime/hana_rt.c)
set_target_properties(hana_rt PROPERTIES
    C_STANDARD 11
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_options(hana_rt PRIVATE ${HANA_WARNING_FLAGS})

add_executable(hana src/cli/main.cpp)
set_target_properties(hana PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(hana hana_rt)
target_link_libraries(hana PRIVATE hana_driver)
target_compile_options(hana PRIVATE ${HANA_WARNING_FLAGS})
target_compile_definitions(hana PRIVATE HANA_VERSION="${PROJECT_VERSION}")

if(HANA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
