add_subdirectory(analysisutil)

# onnxutil exists only in a build that includes dsinfer.
if(TARGET onnxutil)
    add_subdirectory(onnxutil)
endif()
