# synthrt, main line: the contribution framework wolf builds on (ContribCategory, PackageLoader,
# SingerCategory). The shared overlay's synthrt port pins the refactor line instead; see vcpkg.json
# for why this one carries a different name rather than shadowing it.

# The onnxruntime-builds-uptake branch takes ONNX Runtime from the onnxruntime-builds package. A
# fetch by commit needs no archive hash.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/diffscope/synthrt.git
    REF 5e51b4355e7bff17d95d8b4b6432f89d7b38ee3b
    HEAD_REF onnxruntime-builds-uptake
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        onnx WITH_ONNX
)

# dsinfer is off unless asked for: wolf needs neither it nor the ONNX driver for the linguist
# domain, since the inference and singer categories both live in synthrt itself. The onnx feature
# turns it on for the model backed G2P variant.
#
# Nothing is staged for ONNX Runtime any more. synthrt finds the onnxruntime-builds package itself,
# and the feature's dependency has already installed it into this same tree, so the headers are
# where find_package looks. An earlier version copied them into third-party/onnxruntime/default,
# which is the layout synthrt used to probe; that probe is gone.
set(_synthrt_dsinfer OFF)

if(WITH_ONNX)
    set(_synthrt_dsinfer ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSYNTHRT_BUILD_TESTS:BOOL=OFF
        -DSYNTHRT_BUILD_DSINFER:BOOL=${_synthrt_dsinfer}
        # Defaults to on upstream. No CUDA build is wanted here: Windows runs the DirectML provider
        # and every other platform the CPU one.
        -DDSINFER_ENABLE_CUDA:BOOL=OFF
        -DDSINFER_ENABLE_DIRECTML:BOOL=ON
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME synthrt CONFIG_PATH lib/cmake/synthrt)

# dsinfer installs its library, its headers and the driver plugin tree, but this line of synthrt
# exports no CMake package for it, so there is nothing further to fix up. The ONNX driver is
# reached the way every driver is: as a plugin found on a search path at run time.

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
