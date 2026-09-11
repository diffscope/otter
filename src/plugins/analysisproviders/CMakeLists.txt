get_filename_component(_dir_name ${CMAKE_CURRENT_LIST_DIR} NAME)
set(CURRENT_PLUGIN_CATEGORY ${_dir_name})
set(CURRENT_PLUGIN_IID org.openvpi.otter.plugin.AnalysisProvider)

# Both shipped providers run ONNX models. Without dsinfer there is nothing for them to run on, so
# the library is built alone: the category still registers and packages still load, which is the
# same graceful degradation synthrt gives its own driver.
if(OTTER_HAS_DSINFER)
    add_subdirectory(rmvpe)
    add_subdirectory(game)
endif()

# A stub provider exists so the category, the package path and the executive lifecycle can be
# tested without a model of any kind.
if(OTTER_BUILD_TESTS)
    add_subdirectory(stub)
endif()
