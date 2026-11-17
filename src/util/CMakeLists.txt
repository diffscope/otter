add_subdirectory(analysisutil)

if(OTTER_HAS_DSINFER)
    add_subdirectory(onnxutil)
endif()

add_subdirectory(ggmlutil)
