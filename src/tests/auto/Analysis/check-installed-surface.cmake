# Installs the build into a scratch prefix and asserts the shape of what lands there.
#
# The installed tree is what a host builds against, so its contents are part of the interface. Part of
# that promise is a promise of absence: analysisutil, onnxutil and ggmlutil under src/util are
# private static libraries, and a header or an archive of one of them becomes public by accident as
# soon as it is installed. The expectations name the paths that have to be there rather than every
# file, so a new public header does not fail the check while a private one does.
#
# The check also holds the promise that one version serves the library, the packages and the release
# to the artifact a host actually reads: the CMake package version file advertises the version the
# project declares.
#
# Usage:
#     cmake -DOTTER_BUILD_DIR=<build> -DOTTER_TEST_PREFIX=<scratch> -DOTTER_VERSION=<version>
#           -DOTTER_EXPECT_PLUGINS=<ON|OFF> -P check-installed-surface.cmake

cmake_minimum_required(VERSION 3.19)

foreach(_var IN ITEMS OTTER_BUILD_DIR OTTER_TEST_PREFIX OTTER_VERSION OTTER_EXPECT_PLUGINS)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "the check needs ${_var}, and it is not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${OTTER_TEST_PREFIX}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${OTTER_BUILD_DIR}" --prefix "${OTTER_TEST_PREFIX}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error
)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR "cmake --install failed (${_install_result}):\n"
                        "${_install_output}${_install_error}")
endif()

file(GLOB_RECURSE _installed RELATIVE "${OTTER_TEST_PREFIX}" "${OTTER_TEST_PREFIX}/*")

set(_problems "")

# The contract surface a host includes.
foreach(_required IN ITEMS
        "include/otter/otter_global.h"
        "include/otter/Analysis/AnalysisError.h"
        "include/otter/Analysis/AnalysisExecutive.h"
        "include/otter/Analysis/Provider/AnalysisInput.h"
        "include/otter/Analysis/Provider/AnalysisInterpreter.h"
        "include/otter/Analysis/Provider/AnalysisTask.h"
        "include/otter/Api/Common/1/CommonApiL1.h"
        "include/otter/Api/Align/1/AlignApiL1.h"
        "include/otter/Api/F0/1/F0ApiL1.h"
        "include/otter/Api/Note/1/NoteApiL1.h")
    if(NOT _required IN_LIST _installed)
        list(APPEND _problems "the contract header ${_required} is not installed")
    endif()
endforeach()

# The private libraries, which a host must not be able to include or link.
foreach(_installed_path IN LISTS _installed)
    if(_installed_path MATCHES "(^|/)(lib)?(analysisutil|onnxutil|ggmlutil)(/|[^/]*\\.(lib|a)$)")
        list(APPEND _problems
            "the private library ${CMAKE_MATCH_3} is installed: ${_installed_path}")
    endif()
    if(_installed_path MATCHES "\\.(cpp|cc|hpp|py)$")
        list(APPEND _problems "the source file ${_installed_path} is installed")
    endif()
endforeach()

# The library and the plugins beside it. The install name is the one the ecosystem uses for a
# library layered on synthrt, which prefixes the project name, so the check accepts any prefix and
# looks for the project name itself followed by the debug postfix the generators may add.
set(_library_regex "(^|/)[a-z0-9-]*otterd?\\.(dll|so|dylib)(\\.[0-9.]+)?$")
set(_found_library FALSE)
foreach(_installed_path IN LISTS _installed)
    if(_installed_path MATCHES "${_library_regex}")
        set(_found_library TRUE)
    endif()
endforeach()
if(NOT _found_library)
    list(APPEND _problems "no otter library is installed")
endif()

if(OTTER_EXPECT_PLUGINS)
    set(_plugin_count 0)
    foreach(_installed_path IN LISTS _installed)
        if(_installed_path MATCHES "^lib/synthrt-otter/plugins/inferenceinterpreters/.+\\.(dll|so|dylib)$")
            math(EXPR _plugin_count "${_plugin_count} + 1")
        endif()
    endforeach()
    if(_plugin_count EQUAL 0)
        list(APPEND _problems
             "no inference interpreter is installed under lib/synthrt-otter/plugins, and a build "
             "with dsinfer is expected to install them")
    endif()
endif()

# The plugins enter the CMake package as imported targets, so a CMake project reads the plugin
# tree from the package.
if(OTTER_EXPECT_PLUGINS)
    set(_exported_plugin FALSE)
    foreach(_installed_path IN LISTS _installed)
        if(_installed_path MATCHES "synthrt-otter-targets-[a-z]+\\.cmake$")
            file(READ "${OTTER_TEST_PREFIX}/${_installed_path}" _targets_file)
            if(_targets_file MATCHES "/lib/synthrt-otter/plugins/inferenceinterpreters/")
                set(_exported_plugin TRUE)
            endif()
        endif()
    endforeach()
    if(NOT _exported_plugin)
        list(APPEND _problems "the CMake package exports no installed plugin as a target")
    endif()
endif()

# The CMake package, and the version it advertises.
set(_configs "")
foreach(_installed_path IN LISTS _installed)
    if(_installed_path MATCHES "synthrt-otter-config\\.cmake$")
        list(APPEND _configs "${_installed_path}")
    endif()
    if(_installed_path MATCHES "synthrt-otter-config-version\\.cmake$")
        file(READ "${OTTER_TEST_PREFIX}/${_installed_path}" _version_file)
        if(NOT _version_file MATCHES "PACKAGE_VERSION \"${OTTER_VERSION}\"")
            list(APPEND _problems
                 "the installed CMake package does not advertise version ${OTTER_VERSION}: "
                 "${_installed_path}")
        endif()
    endif()
endforeach()
if(NOT _configs)
    list(APPEND _problems "the CMake package is not installed")
endif()

if(_problems)
    string(REPLACE ";" "\n  " _problems_text "${_problems}")
    message(FATAL_ERROR "the installed tree is not the surface a host expects:\n  ${_problems_text}")
endif()

list(LENGTH _installed _installed_count)
message(STATUS "the installed tree holds ${_installed_count} files, and they are the expected "
               "surface (version ${OTTER_VERSION})")
