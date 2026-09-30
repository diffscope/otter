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

find_package(otter CONFIG REQUIRED)

if(NOT TARGET otter::otter)
    message(FATAL_ERROR "the otter package defines no otter::otter target")
endif()

# The plugin tree is a path rather than a target. No target links it, and no other check detects
# its absence. A host passes the category directory to SynthUnit::setPluginPaths, which requires
# the package to export its location.
if(NOT DEFINED OTTER_PLUGINS_DIR)
    message(FATAL_ERROR "the otter package exports no OTTER_PLUGINS_DIR")
endif()
if(NOT IS_DIRECTORY "${OTTER_PLUGINS_DIR}/inferenceinterpreters")
    message(FATAL_ERROR "OTTER_PLUGINS_DIR (${OTTER_PLUGINS_DIR}) contains no "
                        "inferenceinterpreters directory")
endif()
message(STATUS "otter plugins: ${OTTER_PLUGINS_DIR}")
if(DEFINED OTTER_PLUGINS_DIR_DEBUG)
    message(STATUS "otter plugins (debug): ${OTTER_PLUGINS_DIR_DEBUG}")
endif()

# otter/Support contains readers private to the library and its plugins. They are compiled into a
# static library that is not installed, so a Support header in the install tree would expose
# declarations without definitions to a host.
get_target_property(_otter_include_dirs otter::otter INTERFACE_INCLUDE_DIRECTORIES)
set(_otter_found_headers OFF)
foreach(_dir IN LISTS _otter_include_dirs)
    if(EXISTS "${_dir}/otter/Analysis/AnalysisExecutive.h")
        set(_otter_found_headers ON)
    endif()
    if(EXISTS "${_dir}/otter/Support")
        message(FATAL_ERROR "the otter package installs otter/Support (${_dir}/otter/Support), "
                            "which is private to the library and its plugins")
    endif()
endforeach()
if(NOT _otter_found_headers)
    message(FATAL_ERROR "no include directory of otter::otter contains the public headers "
                        "(${_otter_include_dirs})")
endif()

add_executable(otter_package_consumer main.cpp)
target_link_libraries(otter_package_consumer PRIVATE otter::otter)
