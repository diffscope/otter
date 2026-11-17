if(TARGET otterrmvpe OR TARGET ottergameggml OR TARGET ottertifaggml)
add_auto_test(test_ContribInterpreterPlugin_Otter.cpp synthrt-otter)
if(TARGET test_ContribInterpreterPlugin_Otter)
    # Windows resolves a loaded plugin's dependencies beside the executable. The framework is
    # already loaded by this test, while dsinfer is used only by the ONNX plugins.
    if(WIN32 AND TARGET synthrt::dsinfer)
        add_custom_command(TARGET test_ContribInterpreterPlugin_Otter POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:synthrt::dsinfer>"
                "$<TARGET_FILE_DIR:test_ContribInterpreterPlugin_Otter>"
            VERBATIM
        )
    endif()
    foreach(_entry IN ITEMS otterrmvpe:rmvpe ottergame:game otterhfa:hfa ottertifa:tifa
                            ottergameggml:game-ggml ottertifaggml:tifa-ggml)
        string(REPLACE ":" ";" _entry "${_entry}")
        list(GET _entry 0 _plugin)
        list(GET _entry 1 _directory)
        if(TARGET ${_plugin})
            string(TOUPPER "${_plugin}" _upper)
            add_dependencies(test_ContribInterpreterPlugin_Otter ${_plugin})
            target_compile_definitions(test_ContribInterpreterPlugin_Otter PRIVATE
                OTTER_TEST_${_upper}_FILE="$<TARGET_FILE:${_plugin}>"
                OTTER_TEST_${_upper}_MANIFEST="${OTTER_SOURCE_DIR}/src/plugins/inferenceinterpreters/${_directory}/plugin.json"
            )
        endif()
    endforeach()
    set_tests_properties(test_ContribInterpreterPlugin_Otter PROPERTIES ENVIRONMENT_MODIFICATION "${_otter_test_env}")
endif()
endif()
