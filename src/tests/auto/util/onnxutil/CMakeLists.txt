# The readers the ONNX providers share report refusals of their own, and only a target that links
# onnxutil reaches them, since a provider hands its code to a plugin rather than to a test.
add_auto_test(test_OnnxSupport.cpp synthrt-otter onnxutil synthrt::dsinfer)

# The test receives the environment that the parent directory computes.
if(TARGET test_OnnxSupport)
    set_tests_properties(test_OnnxSupport PROPERTIES ENVIRONMENT_MODIFICATION "${_otter_test_env}")
endif()
