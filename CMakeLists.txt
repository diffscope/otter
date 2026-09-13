cmake_minimum_required(VERSION 3.19)

# 0.1.0.0 is the first version to carry the Level 1 contract surface. otter makes no ABI promise
# before 1.0; see README, "Versioning and ABI".
project(otter
    VERSION 0.1.0.0
    LANGUAGES CXX
    DESCRIPTION "Orchestratable Transcription, Timing and Expression Runtime"
    HOMEPAGE_URL "https://github.com/diffscope/otter"
)

# ----------------------------------
# Build Options
# ----------------------------------
option(OTTER_BUILD_TESTS "Build test cases" OFF)
option(OTTER_INSTALL "Install library" ON)

# ----------------------------------
# CMake Settings
# ----------------------------------
if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /manifest:no")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /manifest:no")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /manifest:no")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")

    if(NOT DEFINED CMAKE_DEBUG_POSTFIX)
        set(CMAKE_DEBUG_POSTFIX "d")
    endif()
elseif(MINGW)
    set(CMAKE_STATIC_LIBRARY_PREFIX "")
    set(CMAKE_SHARED_LIBRARY_PREFIX "")
elseif(WIN32)
    if(NOT DEFINED CMAKE_DEBUG_POSTFIX)
        set(CMAKE_DEBUG_POSTFIX "d")
    endif()
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()

if(OTTER_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
endif()

# ----------------------------------
# Project Variables
# ----------------------------------
set(OTTER_VERSION ${PROJECT_VERSION})
set(OTTER_INSTALL_NAME ${PROJECT_NAME})
set(OTTER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ----------------------------------
# Find basic dependencies
# ----------------------------------
find_package(qmsetup REQUIRED)

qm_import(Filesystem)

if(NOT DEFINED QMSETUP_BUILD_DIR)
    set(QMSETUP_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/out")
endif()

qm_init_directories()

find_package(synthrt CONFIG REQUIRED)

# ----------------------------------
# Optional inference backend
# ----------------------------------
# The analysis providers run their models through dsinfer's driver framework. dsinfer is built by
# synthrt[onnx] and installs its own CMake package beside synthrt's, so it is found the ordinary
# way. The library itself needs none of this: the category, the contract headers and the executive
# abstraction are pure synthrt. Only the providers are gated, and they can be refused even when
# dsinfer is installed, which is how the minimal build is asked for.
option(OTTER_DISABLE_DSINFER "Build without dsinfer even if it is installed" OFF)

if(NOT OTTER_DISABLE_DSINFER)
    find_package(dsinfer CONFIG QUIET)
endif()

if(TARGET dsinfer::dsinfer)
    set(OTTER_HAS_DSINFER ON)
    # The ONNX driver ships as a plugin under lib/plugins/dsinfer beside the library, not as a
    # link target. The dsinfer package says where that tree is through DSINFER_PLUGINS_DIR; an
    # older package that does not is searched for the tree above its config file, which is
    # share/dsinfer in a vcpkg tree and lib/cmake/dsinfer in a plain install. It is not derived
    # from the library's own location, which Windows puts under bin while the plugin tree stays
    # under lib. A vcpkg tree keeps its Debug build under debug/, which a multi configuration
    # generator picks per build, so that part of the path is a generator expression. A plugin
    # search path holds one subdirectory per plugin, so this is the parent of the onnx directory.
    # Tests hand it to the driver factory at run time.
    set(_otter_dsinfer_plugins "")
    if(DEFINED DSINFER_PLUGINS_DIR AND IS_DIRECTORY "${DSINFER_PLUGINS_DIR}")
        set(_otter_dsinfer_plugins "${DSINFER_PLUGINS_DIR}")
    else()
        set(_otter_dsinfer_prefix "${dsinfer_DIR}")
        foreach(_step RANGE 3)
            get_filename_component(_otter_dsinfer_prefix "${_otter_dsinfer_prefix}" DIRECTORY)
            if(IS_DIRECTORY "${_otter_dsinfer_prefix}/lib/plugins/dsinfer")
                set(_otter_dsinfer_plugins "${_otter_dsinfer_prefix}/lib/plugins/dsinfer")
                break()
            endif()
        endforeach()
    endif()
    set(OTTER_DSINFER_DRIVER_DIR "")
    if(_otter_dsinfer_plugins)
        get_filename_component(_otter_dsinfer_prefix "${_otter_dsinfer_plugins}/../.." ABSOLUTE)
        if(IS_DIRECTORY "${_otter_dsinfer_prefix}/debug/lib/plugins/dsinfer")
            set(OTTER_DSINFER_DRIVER_DIR
                "${_otter_dsinfer_prefix}/$<$<CONFIG:Debug>:debug/>lib/plugins/dsinfer/inferencedrivers")
        else()
            set(OTTER_DSINFER_DRIVER_DIR "${_otter_dsinfer_plugins}/inferencedrivers")
        endif()
    else()
        message(WARNING "dsinfer was found but its plugin tree was not; the model tests will "
                        "skip for want of a driver")
    endif()
    message(STATUS "dsinfer: ${dsinfer_DIR}")
else()
    set(OTTER_HAS_DSINFER OFF)
    if(OTTER_DISABLE_DSINFER)
        message(STATUS "dsinfer: disabled; no analysis provider will be built")
    else()
        message(STATUS "dsinfer: not found; no analysis provider will be built "
                       "(install synthrt with the onnx feature to enable them)")
    endif()
endif()

# Where the ONNX Runtime payload was deployed, so tests can hand it to the driver. The driver
# loads the library itself at run time, which is why this is a directory and not a link target.
find_package(onnxruntime-builds CONFIG QUIET)
if(onnxruntime-builds_FOUND)
    set(OTTER_ONNXRUNTIME_DIR "${ONNXRUNTIME_BUILDS_RUNTIME_DIR}")
    message(STATUS "ONNX Runtime: ${OTTER_ONNXRUNTIME_DIR}")
else()
    set(OTTER_ONNXRUNTIME_DIR "")
endif()

# ----------------------------------
# Tests
# ----------------------------------
if(OTTER_BUILD_TESTS)
    enable_testing()
endif()

# ----------------------------------
# Add source modules
# ----------------------------------
add_subdirectory(src)
