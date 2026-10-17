# Unit tests for the front end
add_executable(hana_unit_tests
    unit/test_main.cpp
    unit/outer_parser_tests.cpp
    unit/code_lexer_tests.cpp
    unit/code_parser_tests.cpp
    unit/sema_tests.cpp
    unit/formatter_tests.cpp
)
target_link_libraries(hana_unit_tests PRIVATE hana_lang)
target_compile_options(hana_unit_tests PRIVATE ${HANA_WARNING_FLAGS})
add_test(NAME unit COMMAND hana_unit_tests)

# End-to-end tests: every e2e/run/<name>.hana is built and run, its output
# must equal <name>.out. Every e2e/errors/<name>.hana must fail to compile
# with diagnostics containing each line of <name>.err.
file(GLOB HANA_RUN_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run/*.hana)
foreach(source ${HANA_RUN_TESTS})
    get_filename_component(name ${source} NAME_WE)
    add_test(NAME run.${name}
        COMMAND ${CMAKE_COMMAND} -DHANA=$<TARGET_FILE:hana> -DSOURCE=${source} -DMODE=run
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.cmake)
endforeach()

# The same programs again under AddressSanitizer, when the C compiler used
# for linking can link with it.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -fsanitize=address)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address)
check_c_source_compiles("int main(void) { return 0; }" HANA_HAVE_ADDRESS_SANITIZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(HANA_HAVE_ADDRESS_SANITIZER)
    foreach(source ${HANA_RUN_TESTS})
        get_filename_component(name ${source} NAME_WE)
        add_test(NAME memory.${name}
            COMMAND ${CMAKE_COMMAND} -DHANA=$<TARGET_FILE:hana> -DSOURCE=${source} -DMODE=memory
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.cmake)
    endforeach()
endif()

foreach(source ${HANA_RUN_TESTS})
    get_filename_component(name ${source} NAME_WE)
    add_test(NAME format.${name}
        COMMAND ${CMAKE_COMMAND} -DHANA=$<TARGET_FILE:hana> -DSOURCE=${source} -DMODE=format
                -DTEMP_DIR=${CMAKE_CURRENT_BINARY_DIR}/formatted
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.cmake)
endforeach()

# Every example must already be in the canonical layout.
file(GLOB HANA_EXAMPLES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/examples/*.hana)
foreach(source ${HANA_EXAMPLES})
    get_filename_component(name ${source} NAME_WE)
    add_test(NAME formatted.example.${name} COMMAND hana fmt --check ${source})
endforeach()

file(GLOB HANA_ERROR_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/e2e/errors/*.hana)
foreach(source ${HANA_ERROR_TESTS})
    get_filename_component(name ${source} NAME_WE)
    add_test(NAME error.${name}
        COMMAND ${CMAKE_COMMAND} -DHANA=$<TARGET_FILE:hana> -DSOURCE=${source} -DMODE=error
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.cmake)
endforeach()
