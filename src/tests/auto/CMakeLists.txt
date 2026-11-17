include("${SYNTHRT_MODULES_DIR}/AddAutoTest.cmake")

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
    "$<TARGET_FILE_DIR:synthrt-otter>"
    "$<TARGET_FILE_DIR:synthrt::synthrt>"
    "$<TARGET_FILE_DIR:stdcorelib::plugin>"
    "$<TARGET_FILE_DIR:stdcorelib::stdcorelib>"
)
if(OTTER_HAS_DSINFER)
    list(APPEND _otter_test_runtime_dirs "$<TARGET_FILE_DIR:synthrt::dsinfer>")
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

add_subdirectory(plugins/stub)

add_subdirectory(Analysis)
add_subdirectory(util)
