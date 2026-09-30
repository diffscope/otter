add_auto_test(test_AnalysisContrib.cpp otter)

add_auto_test(test_AnalysisLoad.cpp otter)

add_auto_test(test_AnalysisRuntime.cpp otter)

# The readers are private to this tree and live in a static library the library does not export.
add_auto_test(test_ManifestValues.cpp otter_support)

add_auto_test(test_F0Curve.cpp otter_support)

set(_otter_tests test_AnalysisContrib test_AnalysisLoad test_AnalysisRuntime test_ManifestValues
                 test_F0Curve)

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
        add_dependencies(${_needs_stub} otterstub)
        target_compile_definitions(${_needs_stub} PRIVATE
            OTTER_TEST_PACKAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packages"
            OTTER_TEST_PLUGIN_DIR="${OTTER_BUILD_PLUGINS_DIR}/inferenceinterpreters"
        )
    endif()
endforeach()
