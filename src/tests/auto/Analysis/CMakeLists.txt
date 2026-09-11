add_auto_test(test_AnalysisContrib.cpp otter)

add_auto_test(test_AnalysisLoad.cpp otter)

add_auto_test(test_AnalysisRuntime.cpp otter)

add_auto_test(test_ManifestValues.cpp otter)

# Needs the driver framework and a generated model fixture, so it exists only in a build that
# carries dsinfer. The fixture itself is checked for at run time and the case skips without it.
if(OTTER_HAS_DSINFER)
    add_auto_test(test_Rmvpe.cpp otter otter::dsinfer)
    add_auto_test(test_Game.cpp otter otter::dsinfer)
endif()

# The tests load synthrt and its stdcorelib dependencies at run time. Windows resolves those
# through PATH; the ELF and Mach-O loaders need their own variables, so name all three.
set(_otter_test_runtime_dirs
    "$<TARGET_FILE_DIR:synthrt::synthrt>"
    "$<TARGET_FILE_DIR:stdcorelib::plugin>"
    "$<TARGET_FILE_DIR:stdcorelib::stdcorelib>"
)
set(_otter_test_env "")
foreach(_var IN ITEMS PATH LD_LIBRARY_PATH DYLD_LIBRARY_PATH)
    foreach(_dir IN LISTS _otter_test_runtime_dirs)
        list(APPEND _otter_test_env "${_var}=path_list_prepend:${_dir}")
    endforeach()
endforeach()

set(_otter_tests test_AnalysisContrib test_AnalysisLoad test_AnalysisRuntime test_ManifestValues
                 test_Rmvpe test_Game)

foreach(_model_test IN ITEMS test_Rmvpe test_Game)
if(TARGET ${_model_test})
    add_dependencies(${_model_test} otterrmvpe ottergame)
    target_compile_definitions(${_model_test} PRIVATE
        OTTER_TEST_FIXTURE_DIR="${OTTER_SOURCE_DIR}/build/fixtures"
        OTTER_TEST_PLUGIN_DIR="${OTTER_BUILD_PLUGINS_DIR}/analysisproviders"
        OTTER_TEST_DRIVER_PLUGIN_DIR="${OTTER_DSINFER_DRIVER_DIR}"
        OTTER_TEST_ONNXRUNTIME_DIR="${OTTER_ONNXRUNTIME_DIR}"
    )
endif()
endforeach()

# The stub provider is what the loading and runtime cases discover, so it has to exist before they
# run and they have to be told where it is.
foreach(_needs_stub IN ITEMS test_AnalysisLoad test_AnalysisRuntime)
    if(TARGET ${_needs_stub})
        add_dependencies(${_needs_stub} otteranalysisstub)
        target_compile_definitions(${_needs_stub} PRIVATE
            OTTER_TEST_PACKAGE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/packages"
            OTTER_TEST_PLUGIN_DIR="${OTTER_BUILD_PLUGINS_DIR}/analysisproviders"
        )
    endif()
endforeach()

foreach(_test IN LISTS _otter_tests)
    if(TARGET ${_test})
        set_tests_properties(${_test} PROPERTIES ENVIRONMENT_MODIFICATION "${_otter_test_env}")
    endif()
endforeach()
