# synthrt, main line: the contribution framework on which otter builds (ContribCategory,
# PackageLoader and the dsinfer inference sessions). The synthrt port of the shared overlay pins
# the refactor line. vcpkg.json records the reason for the distinct port name.

# The pin tracks synthrt main so that this tree, the editor tree and the other consumers build
# against the same commit. A fetch by commit requires no archive hash.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/diffscope/synthrt.git
    REF f6476669500df09e44c0f4615880b10d9b90914b
    HEAD_REF main
)

# dsinfer is part of synthrt main and is always built. The onnx feature adds ONNX Runtime, which
# the ONNX driver needs. No ONNX Runtime files are staged: synthrt locates the onnxruntime-builds
# package itself, and the dependency of the feature has already installed that package into the
# same tree, so its headers are on the find_package search path.

# DirectML is a Windows API. On other platforms dsinfer runs the CPU provider, so the option is
# enabled only on Windows instead of being passed to dsinfer to ignore.
set(_synthrt_directml OFF)

if(VCPKG_TARGET_IS_WINDOWS)
    set(_synthrt_directml ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSYNTHRT_BUILD_TESTS:BOOL=OFF
        -DSYNTHRT_BUILD_DSINFER:BOOL=ON
        # Enabled by default upstream. This port builds no CUDA provider: Windows runs the DirectML
        # provider and every other platform runs the CPU provider.
        -DDSINFER_ENABLE_CUDA:BOOL=OFF
        -DDSINFER_ENABLE_DIRECTML:BOOL=${_synthrt_directml}
)

vcpkg_cmake_install()

# synthrt installs two CMake packages side by side under lib/cmake: synthrt itself and
# synthrt-dsinfer. The fixup of one package must not delete the parent directory. Otherwise the
# other package is removed without notice and consumers must locate the dsinfer library manually.
vcpkg_cmake_config_fixup(
    PACKAGE_NAME synthrt
    CONFIG_PATH lib/cmake/synthrt
    DO_NOT_DELETE_PARENT_CONFIG_PATH
)
vcpkg_cmake_config_fixup(
    PACKAGE_NAME synthrt-dsinfer
    CONFIG_PATH lib/cmake/synthrt-dsinfer
    DO_NOT_DELETE_PARENT_CONFIG_PATH
)
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/lib/cmake"
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake"
)

# The ONNX driver is not a link target. Like every driver, it is loaded at run time as a plugin
# found on a search path, under lib/synthrt-dsinfer/plugins/inferencedrivers. On Windows the
# plugins install their DLLs there, which the vcpkg layout check would otherwise reject.
set(VCPKG_POLICY_ALLOW_DLLS_IN_LIB enabled)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
