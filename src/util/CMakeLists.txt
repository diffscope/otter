# Private static libraries named after what they do, which the library and the plugins of this tree
# link. None of them is installed.
add_subdirectory(analysisutil)

# The code shared by the ONNX providers requires dsinfer, as the providers do.
if(OTTER_HAS_DSINFER)
    add_subdirectory(onnxutil)
endif()

add_subdirectory(ggmlutil)
