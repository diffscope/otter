add_auto_test(test_AnalysisContrib.cpp otter)

add_auto_test(test_AnalysisLoad.cpp otter)

add_auto_test(test_AnalysisRuntime.cpp otter)

add_auto_test(test_ManifestValues.cpp otter)

# Need the driver framework and the generated model fixtures, so they exist only in a build that
# carries dsinfer. Whether the fixtures can be generated is decided below.
if(OTTER_HAS_DSINFER)
    add_auto_test(test_Rmvpe.cpp otter dsinfer::dsinfer)
    add_auto_test(test_Game.cpp otter dsinfer::dsinfer)
endif()

set(_otter_tests test_AnalysisContrib test_AnalysisLoad test_AnalysisRuntime test_ManifestValues
                 test_Rmvpe test_Game)

# The tests load the library under test, synthrt, its stdcorelib dependencies and, where present,
# dsinfer and ONNX Runtime at run time. Windows resolves those through PATH; the ELF and Mach-O
# loaders need their own variables, so name all three. Every test target gets the same
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

# The stub provider is what the loading and runtime cases discover, so it has to exist before they
# run and they have to be told where it is.
foreach(_needs_stub IN ITEMS test_AnalysisLoad test_AnalysisRuntime)
    if(TARGET ${_needs_stub})
        add_dependencies(${_needs_stub} otteranalysisstub)
        target_compile_definitions(${_needs_stub} PRIVATE
            OTTER_TEST_PACKAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packages"
            OTTER_TEST_PLUGIN_DIR="${OTTER_BUILD_PLUGINS_DIR}/analysisproviders"
        )
    endif()
endforeach()

# The model fixtures are real ONNX graphs written by a script, so they live in the build tree and
# are made by the build. A Python with the onnx module is what it takes; without one the two
# tests that need them are registered disabled, which ctest reports, rather than left to discover
# the gap at run time and pass.
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
        message(STATUS "Model fixtures: no Python with onnx and numpy; test_Rmvpe and test_Game "
                       "are disabled")
    endif()
endif()

foreach(_model_test IN ITEMS test_Rmvpe test_Game)
    if(TARGET ${_model_test})
        add_dependencies(${_model_test} otterrmvpe ottergame)
        if(_otter_can_make_fixtures)
            add_dependencies(${_model_test} otter_model_fixtures)
        else()
            set_tests_properties(${_model_test} PROPERTIES DISABLED TRUE)
        endif()
        # Missing fixtures, driver or runtime at run time are reported with this status, which
        # ctest shows as a skip rather than a pass.
        set_tests_properties(${_model_test} PROPERTIES SKIP_RETURN_CODE 77)
        target_compile_definitions(${_model_test} PRIVATE
            OTTER_TEST_FIXTURE_DIR="${OTTER_TEST_FIXTURE_DIR}"
            OTTER_TEST_PLUGIN_DIR="${OTTER_BUILD_PLUGINS_DIR}/analysisproviders"
            OTTER_TEST_DRIVER_PLUGIN_DIR="${OTTER_DSINFER_DRIVER_DIR}"
            OTTER_TEST_ONNXRUNTIME_DIR="${OTTER_ONNXRUNTIME_DIR}"
        )
    endif()
endforeach()

# The package lint is a Python script with tests of its own. It needs nothing but Python.
if(Python3_Interpreter_FOUND)
    add_test(NAME test_CheckDeclarations
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${OTTER_SOURCE_DIR}/scripts" -p "test_*.py"
    )
endif()
