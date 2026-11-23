cmake_minimum_required(VERSION 3.19)

# 0.1.0.0 is the first version to carry the Level 1 contract surface.
# 0.2.0.0 moves Task cancellation and callback semantics into the shared synthrt execution kernel.
# 0.3.0.0 renames typed contract export payloads from Schema to Exports.
# 0.4.0.0 moves the contract readers and sample preparation onto their types.
# otter makes no ABI promise before 1.0. See README, Versioning and ABI.
project(synthrt-otter
    VERSION 0.4.0.0
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
set(OTTER_INSTALL_NAME ${PROJECT_NAME})
set(OTTER_INSTALL_NAMESPACE synthrt)
# A plugin is loaded at run time and never linked, so it is a module library, which has no
# import library to install.
set(OTTER_PLUGIN_TYPE MODULE)
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
# The analysis providers run their models through the dsinfer driver framework. synthrt[onnx]
# builds dsinfer and installs its CMake package beside the synthrt package, so find_package locates
# it without further configuration. The library does not depend on dsinfer: the contract headers
# and the executive abstraction depend only on synthrt. Only the providers depend on dsinfer, and
# OTTER_DISABLE_DSINFER excludes them even if dsinfer is installed, which produces the minimal
# build.
option(OTTER_DISABLE_DSINFER "Build without dsinfer even if it is installed" OFF)

if(NOT OTTER_DISABLE_DSINFER)
    find_package(synthrt-dsinfer CONFIG QUIET)
endif()

if(TARGET synthrt::dsinfer)
    # The ONNX driver is a plugin of dsinfer, which the dsinfer package exports as the imported
    # target synthrt::onnxdriver. The tests read its directory from that target.
    set(OTTER_HAS_DSINFER ON)
    message(STATUS "dsinfer: ${synthrt-dsinfer_DIR}")
else()
    set(OTTER_HAS_DSINFER OFF)
    if(OTTER_DISABLE_DSINFER)
        message(STATUS "dsinfer: disabled; no analysis provider will be built")
    else()
        message(STATUS "dsinfer: not found; no analysis provider will be built "
                       "(install synthrt with the onnx feature to enable them)")
    endif()
endif()

# Directory of the deployed ONNX Runtime binaries, which tests pass to the driver. The driver loads
# the library at run time, so the value is a directory and not a link target.
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
