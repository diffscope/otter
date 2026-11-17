# Minimal consumer of otter through the installed CMake package, as a host consumes it.
#
# The library's own build does not verify the installed package. The install rules write every
# path in the package, and only a consumer reads them. A target file that names an uninstalled
# library, a config file without find_dependency(synthrt), or an include directory inside the build
# tree passes the test suite and fails in the host. CI therefore installs otter and configures this
# project, and the assertions below check the package rather than the code.
cmake_minimum_required(VERSION 3.19)
project(otter_package_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(synthrt-otter CONFIG REQUIRED)

if(NOT TARGET synthrt::otter)
    message(FATAL_ERROR "the synthrt-otter package defines no synthrt::otter target")
endif()

# analysisutil, onnxutil and ggmlutil contain code private to the library and its plugins. Each is
# compiled into a static library that is not installed, so a header of one of them in the install
# tree would expose declarations without definitions to a host.
get_target_property(_otter_include_dirs synthrt::otter INTERFACE_INCLUDE_DIRECTORIES)
set(_otter_found_headers OFF)
foreach(_dir IN LISTS _otter_include_dirs)
    if(EXISTS "${_dir}/otter/Analysis/AnalysisExecutive.h")
        set(_otter_found_headers ON)
    endif()
    foreach(_private IN ITEMS analysisutil onnxutil ggmlutil)
        if(EXISTS "${_dir}/${_private}")
            message(FATAL_ERROR "the otter package installs ${_private} (${_dir}/${_private}), "
                                "which is private to the library and its plugins")
        endif()
    endforeach()
endforeach()
if(NOT _otter_found_headers)
    message(FATAL_ERROR "no include directory of synthrt::otter contains the public headers "
                        "(${_otter_include_dirs})")
endif()

# The package exports every installed plugin as an imported target. The plugin tree is a directory
# rather than a link dependency, so no link step detects its absence, and the consumer checks the
# category directory above the bundle of one exported plugin. A host that does not use CMake adds
# lib/synthrt-otter/plugins/inferenceinterpreters of the install prefix instead.
set(_otter_plugin "")
foreach(_candidate IN ITEMS otterrmvpe ottergame otterhfa ottertifa ottergameggml ottertifaggml)
    if(TARGET synthrt::${_candidate})
        set(_otter_plugin synthrt::${_candidate})
        break()
    endif()
endforeach()
if(NOT _otter_plugin)
    message(FATAL_ERROR "the synthrt-otter package exports no plugin target")
endif()

add_executable(otter_package_consumer main.cpp)
target_link_libraries(otter_package_consumer PRIVATE synthrt::otter)
target_compile_definitions(otter_package_consumer PRIVATE
    OTTER_CONSUMER_PLUGIN_DIR="$<TARGET_FILE_DIR:${_otter_plugin}>/.."
)
