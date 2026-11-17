add_auto_test(test_AnalysisError.cpp synthrt-otter)

add_auto_test(test_AnalysisExecutive_Load.cpp synthrt-otter)

add_auto_test(test_AnalysisExecutive.cpp synthrt-otter)

set(_otter_tests test_AnalysisError test_AnalysisExecutive_Load test_AnalysisExecutive)

# Every test receives the environment that the parent directory computes.
foreach(_test IN LISTS _otter_tests)
    if(TARGET ${_test})
        set_tests_properties(${_test} PROPERTIES ENVIRONMENT_MODIFICATION "${_otter_test_env}")
    endif()
endforeach()

# A cache path rather than a generated directory: the packages are data, and the script that
# rebuilds them is the single definition of what they hold, so a developer who does not run it, or
# a CI job that does not prepare the directory, gets a skip rather than a pass.
#
#     cmake -DOTTER_TEST_PACKAGES_OUTPUT=<dir> -P scripts/make-test-packages.cmake
#
# Passing the same script -DOTTER_TEST_PACKAGES_CHECK=<dir> compares an existing directory instead.
set(OTTER_TEST_PACKAGES_SOURCE "" CACHE PATH "Directory holding the generated test packages")
if(OTTER_TEST_PACKAGES_SOURCE)
    message(STATUS "Test packages: ${OTTER_TEST_PACKAGES_SOURCE}")
    if(NOT EXISTS "${OTTER_TEST_PACKAGES_SOURCE}")
        message(WARNING
            "OTTER_TEST_PACKAGES_SOURCE names a directory that does not exist; the loading and "
            "runtime tests will fail rather than skip")
    endif()
else()
    message(STATUS "Test packages: not given; test_AnalysisExecutive_Load and test_AnalysisExecutive will skip "
                   "(pass -DOTTER_TEST_PACKAGES_SOURCE=<dir>)")
endif()

# The loading and runtime tests link the stub provider, which registers itself as a static plugin
# of the inference category, so they set no plugin path. They read the packages from the
# environment rather than a compile definition, so that one build can be pointed at a directory per
# run without reconfiguring.
foreach(_needs_stub IN ITEMS test_AnalysisExecutive_Load test_AnalysisExecutive)
    if(TARGET ${_needs_stub})
        target_link_libraries(${_needs_stub} PRIVATE otterstub)
        target_compile_definitions(${_needs_stub} PRIVATE
            OTTER_TEST_SKIP_EXIT_CODE=${OTTER_TEST_SKIP_EXIT_CODE}
        )
        set_tests_properties(${_needs_stub} PROPERTIES
            SKIP_RETURN_CODE ${OTTER_TEST_SKIP_EXIT_CODE}
            ENVIRONMENT "OTTER_TEST_PACKAGES_SOURCE=${OTTER_TEST_PACKAGES_SOURCE}"
        )
    endif()
endforeach()

# The package lint and the package assembler are Python scripts with tests of their own. They need
# nothing but Python.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_CheckDeclarations
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${OTTER_SOURCE_DIR}/scripts" -p "test_*.py"
    )
else()
    message(WARNING
        "no Python 3 interpreter was found, so the package lint and the package assembler are not "
        "covered: their tests are not registered with ctest")
endif()

# What a host sees is the installed tree, not the source tree, and the helpers that install it are
# easy to change without noticing what they add. The check installs into a scratch prefix and
# asserts the shape, including the files that must not be installed, which is what keeps the
# private libraries under src/util out of the installed tree.
if(OTTER_INSTALL)
    add_test(NAME test_InstalledSurface
        COMMAND ${CMAKE_COMMAND}
                -DOTTER_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DOTTER_TEST_PREFIX=${CMAKE_BINARY_DIR}/verify-installed-surface
                -DOTTER_VERSION=${PROJECT_VERSION}
                -DOTTER_EXPECT_PLUGINS=${OTTER_HAS_DSINFER}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check-installed-surface.cmake
    )
else()
    message(WARNING
        "OTTER_INSTALL is off, so the shape of the installed tree is not covered: the check is not "
        "registered with ctest")
endif()
add_auto_test(Provider/test_AnalysisInput.cpp synthrt-otter)
if(TARGET test_AnalysisInput)
    set_tests_properties(test_AnalysisInput PROPERTIES ENVIRONMENT_MODIFICATION "${_otter_test_env}")
endif()
