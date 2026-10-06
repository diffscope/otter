add_auto_test(test_AnalysisContrib.cpp otter)

add_auto_test(test_AnalysisLoad.cpp otter)

add_auto_test(test_AnalysisRuntime.cpp otter)

# The readers are private to this tree and live in a static library the library does not export.
add_auto_test(test_ManifestValues.cpp otter_support)

add_auto_test(test_F0Curve.cpp otter_support)

# These tests need the driver framework and the generated model fixtures, so they exist only in a
# build that includes dsinfer. The availability of the fixture generator is determined below.
if(OTTER_HAS_DSINFER)
    add_auto_test(test_Rmvpe.cpp otter dsinfer::dsinfer)
    add_auto_test(test_Game.cpp otter dsinfer::dsinfer)
    add_auto_test(test_Hfa.cpp otter dsinfer::dsinfer)
    add_auto_test(test_Tifa.cpp otter dsinfer::dsinfer)

    # The readers the providers share report refusals of their own, and only a target that links
    # the support library reaches them, since a provider hands its code to a plugin rather than to
    # a test.
    if(TARGET otter_onnx_support)
        add_auto_test(test_OnnxSupport.cpp otter otter_onnx_support dsinfer::dsinfer)
    endif()
endif()

# The ggml providers exist only in a build that named their engines, so their tests exist only
# beside them. Without a model the tests still cover the declaration surface, and a run with no
# model named in the environment reports itself as a skip.
if(TARGET ottergameggml)
    add_auto_test(test_GameGgml.cpp otter)
endif()
if(TARGET ottertifaggml)
    add_auto_test(test_TifaGgml.cpp otter)
endif()

set(_otter_tests test_AnalysisContrib test_AnalysisLoad test_AnalysisRuntime test_ManifestValues
                 test_F0Curve test_Rmvpe test_Game test_Hfa test_Tifa test_OnnxSupport
                 test_GameGgml test_TifaGgml)

# The tests load the library under test, synthrt, its stdcorelib dependencies and, if present,
# dsinfer and ONNX Runtime at run time. Windows resolves those through PATH; the ELF and Mach-O
# loaders need their own variables, so all three are set. Every test target receives the same
# environment.
#
# This covers the executables. A plugin loaded at run time resolves its own dependencies through
# the loader's search path for plugins, which on Windows is the plugin's directory, the
# executable's directory and the directories the host registered, not PATH. A deployment puts
# the libraries a plugin needs beside the executable or registers their directory.
set(_otter_test_runtime_dirs
    "$<TARGET_FILE_DIR:otter>"
    "$<TARGET_FILE_DIR:synthrt::synthrt>"
    "$<TARGET_FILE_DIR:stdcorelib::plugin>"
    "$<TARGET_FILE_DIR:stdcorelib::stdcorelib>"
)
if(OTTER_HAS_DSINFER)
    list(APPEND _otter_test_runtime_dirs "$<TARGET_FILE_DIR:dsinfer::dsinfer>")
endif()
if(OTTER_ONNXRUNTIME_DIR)
    list(APPEND _otter_test_runtime_dirs "${OTTER_ONNXRUNTIME_DIR}")
endif()
set(_otter_test_env "")
foreach(_var IN ITEMS PATH LD_LIBRARY_PATH DYLD_LIBRARY_PATH)
    foreach(_dir IN LISTS _otter_test_runtime_dirs)
        list(APPEND _otter_test_env "${_var}=path_list_prepend:${_dir}")
    endforeach()
endforeach()
foreach(_test IN LISTS _otter_tests)
    if(TARGET ${_test})
        set_tests_properties(${_test} PROPERTIES ENVIRONMENT_MODIFICATION "${_otter_test_env}")
    endif()
endforeach()

# The exit status with which a test reports that it did not run, which ctest reports as a skip.
# The value is defined once because the tests and ctest must agree on it.
set(OTTER_TEST_SKIP_EXIT_CODE 77)

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
    message(STATUS "Test packages: not given; test_AnalysisLoad and test_AnalysisRuntime will skip "
                   "(pass -DOTTER_TEST_PACKAGES_SOURCE=<dir>)")
endif()

# The loading and runtime tests discover the stub provider, so the provider must be built before
# they run, and the tests receive its location as a compile definition. They read the packages from
# the environment rather than a compile definition, so that one build can be pointed at a directory
# per run without reconfiguring.
foreach(_needs_stub IN ITEMS test_AnalysisLoad test_AnalysisRuntime)
    if(TARGET ${_needs_stub})
        add_dependencies(${_needs_stub} otterstub)
        target_compile_definitions(${_needs_stub} PRIVATE
            OTTER_TEST_SKIP_EXIT_CODE=${OTTER_TEST_SKIP_EXIT_CODE}
            OTTER_TEST_PLUGIN_DIR="${OTTER_BUILD_PLUGINS_DIR}/inferenceinterpreters"
        )
        set_tests_properties(${_needs_stub} PROPERTIES
            SKIP_RETURN_CODE ${OTTER_TEST_SKIP_EXIT_CODE}
            ENVIRONMENT "OTTER_TEST_PACKAGES_SOURCE=${OTTER_TEST_PACKAGES_SOURCE}"
        )
    endif()
endforeach()

# The model fixtures are real ONNX graphs generated by a script, so they reside in the build tree
# and are generated by the build. Generation requires a Python interpreter with the onnx and numpy
# modules. Without such an interpreter, the model tests that need the fixtures are registered as
# disabled, which ctest reports, rather than detecting the missing fixtures at run time and
# passing.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(_otter_can_make_fixtures OFF)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import onnx, numpy"
        RESULT_VARIABLE _otter_onnx_missing
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_otter_onnx_missing EQUAL 0)
        set(_otter_can_make_fixtures ON)
    endif()
endif()

set(OTTER_TEST_FIXTURE_DIR "${CMAKE_BINARY_DIR}/fixtures")
if(TARGET test_Rmvpe)
    if(_otter_can_make_fixtures)
        add_custom_command(
            OUTPUT "${OTTER_TEST_FIXTURE_DIR}/.stamp"
            COMMAND "${Python3_EXECUTABLE}" "${OTTER_SOURCE_DIR}/scripts/make-model-fixtures.py"
                    --output "${OTTER_TEST_FIXTURE_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${OTTER_TEST_FIXTURE_DIR}/.stamp"
            DEPENDS "${OTTER_SOURCE_DIR}/scripts/make-model-fixtures.py"
            COMMENT "Generating the model fixtures"
            VERBATIM
        )
        add_custom_target(otter_model_fixtures DEPENDS "${OTTER_TEST_FIXTURE_DIR}/.stamp")
        message(STATUS "Model fixtures: generated into ${OTTER_TEST_FIXTURE_DIR}")
    else()
        message(STATUS "Model fixtures: no Python with onnx and numpy; test_Rmvpe, test_Game, "
                       "test_Hfa and test_Tifa are disabled")
    endif()
endif()

foreach(_model_test IN ITEMS test_Rmvpe test_Game test_Hfa test_Tifa)
    if(TARGET ${_model_test})
        add_dependencies(${_model_test} otterrmvpe ottergame otterhfa ottertifa)
        if(_otter_can_make_fixtures)
            add_dependencies(${_model_test} otter_model_fixtures)
        else()
            set_tests_properties(${_model_test} PROPERTIES DISABLED TRUE)
        endif()
        # Missing fixtures, driver or runtime at run time are reported with this status, which
        # ctest shows as a skip rather than a pass.
        set_tests_properties(${_model_test} PROPERTIES SKIP_RETURN_CODE ${OTTER_TEST_SKIP_EXIT_CODE})
        target_compile_definitions(${_model_test} PRIVATE
            OTTER_TEST_SKIP_EXIT_CODE=${OTTER_TEST_SKIP_EXIT_CODE}
            OTTER_TEST_FIXTURE_DIR="${OTTER_TEST_FIXTURE_DIR}"
            OTTER_TEST_PLUGIN_DIR="${OTTER_BUILD_PLUGINS_DIR}/inferenceinterpreters"
            OTTER_TEST_DRIVER_PLUGIN_DIR="${OTTER_DSINFER_DRIVER_DIR}"
            OTTER_TEST_ONNXRUNTIME_DIR="${OTTER_ONNXRUNTIME_DIR}"
        )
    endif()
endforeach()

# The ggml tests load packages they write themselves and need no fixtures, packages directory or
# driver. Their execution cases run against a stand-in command line tool that answers the engines'
# protocols with canned output, which the build compiles beside the tests; the real-engine cases
# name the engine through the environment and report themselves as a skip without it.
add_executable(otterfakecli fakecli.cpp)
target_compile_features(otterfakecli PRIVATE cxx_std_17)
foreach(_ggml_test IN ITEMS test_GameGgml test_TifaGgml)
    if(TARGET ${_ggml_test})
        foreach(_ggml_plugin IN ITEMS ottergameggml ottertifaggml)
            if(TARGET ${_ggml_plugin})
                add_dependencies(${_ggml_test} ${_ggml_plugin})
            endif()
        endforeach()
        add_dependencies(${_ggml_test} otterfakecli)
        set_tests_properties(${_ggml_test} PROPERTIES SKIP_RETURN_CODE ${OTTER_TEST_SKIP_EXIT_CODE})
        target_compile_definitions(${_ggml_test} PRIVATE
            OTTER_TEST_SKIP_EXIT_CODE=${OTTER_TEST_SKIP_EXIT_CODE}
            OTTER_TEST_PLUGIN_DIR="${OTTER_BUILD_PLUGINS_DIR}/inferenceinterpreters"
            OTTER_TEST_FAKE_CLI="$<TARGET_FILE:otterfakecli>"
        )
        set_tests_properties(${_ggml_test} PROPERTIES ENVIRONMENT_MODIFICATION
            "${_otter_test_env}")
    endif()
endforeach()

# The package lint and the package assembler are Python scripts with tests of their own. They need
# nothing but Python.
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
# asserts the shape, including the headers that must not be installed, which is what keeps the
# readers under include/otter/Support private.
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
