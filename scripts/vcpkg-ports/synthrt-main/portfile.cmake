# synthrt main line: the contribution framework on which otter builds (ContribCategory, PackageLoader
# and the dsinfer inference sessions). The synthrt port of the shared overlay pins the refactor line;
# vcpkg.json records the reason for the distinct port name.

# The onnxruntime-builds-uptake branch takes ONNX Runtime from the onnxruntime-builds package, which
# the analyzers' ONNX sessions require. The pin tracks the branch tip, which also defines the names
# of the built-in categories, so that this tree and the editor's tree build against the same
# synthrt. A fetch by commit requires no archive hash.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/diffscope/synthrt.git
    REF f2f0f8ee3669206ed90f951c17c397a23c5e4b6d
    HEAD_REF onnxruntime-builds-uptake
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        onnx WITH_ONNX
)

# dsinfer is disabled unless the onnx feature is selected: the otter library requires neither
# dsinfer nor the ONNX driver. The onnx feature enables dsinfer for the providers, which run ONNX
# models.
#
# No files are staged for ONNX Runtime. synthrt locates the onnxruntime-builds package itself, and
# the dependency of the feature has already installed that package into the same tree, in which
# find_package searches.
set(_synthrt_dsinfer OFF)

if(WITH_ONNX)
    set(_synthrt_dsinfer ON)
endif()

# DirectML is a Windows API. On other platforms dsinfer runs the CPU provider, so the option is
# enabled only on Windows instead of relying on dsinfer to ignore it.
set(_synthrt_directml OFF)

if(VCPKG_TARGET_IS_WINDOWS)
    set(_synthrt_directml ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSYNTHRT_BUILD_TESTS:BOOL=OFF
        -DSYNTHRT_BUILD_DSINFER:BOOL=${_synthrt_dsinfer}
        # Enabled by default upstream. CUDA is not used: Windows runs the DirectML provider and all
        # other platforms run the CPU provider.
        -DDSINFER_ENABLE_CUDA:BOOL=OFF
        -DDSINFER_ENABLE_DIRECTML:BOOL=${_synthrt_directml}
)

vcpkg_cmake_install()

# synthrt installs two CMake packages side by side under lib/cmake: synthrt itself and, with the
# onnx feature, dsinfer. The fixup of one package must not delete the parent directory; otherwise
# the other package is removed without notice and consumers must locate the dsinfer library
# manually.
vcpkg_cmake_config_fixup(
    PACKAGE_NAME synthrt
    CONFIG_PATH lib/cmake/synthrt
    DO_NOT_DELETE_PARENT_CONFIG_PATH
)
if(WITH_ONNX)
    vcpkg_cmake_config_fixup(
        PACKAGE_NAME dsinfer
        CONFIG_PATH lib/cmake/dsinfer
        DO_NOT_DELETE_PARENT_CONFIG_PATH
    )
endif()
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/lib/cmake"
    "${CURRENT_PACKAGES_DIR}/debug/lib/cmake"
)

# The ONNX driver is not a link target. Like every driver, it is loaded at run time as a plugin
# found on a search path, under lib/plugins/dsinfer/inferencedrivers. On Windows the plugins install
# their DLLs in that directory, which the vcpkg layout check would otherwise reject.
set(VCPKG_POLICY_ALLOW_DLLS_IN_LIB enabled)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
