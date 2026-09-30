get_filename_component(_dir_name ${CMAKE_CURRENT_LIST_DIR} NAME)
set(CURRENT_PLUGIN_CATEGORY ${_dir_name})
# The inference category belongs to synthrt; these interpreters therefore use synthrt's plugin IID.
set(CURRENT_PLUGIN_IID org.openvpi.synthrt.plugin.InferenceInterpreter)

# Declares one interpreter plugin of this category.
#
# otter_add_interpreter_plugin(<target> <directory>
#     SOURCES <files...>
#     LINKS <libraries...>
#     [NO_INSTALL]
# )
#
# The plugin is built into <plugins>/<category>/<directory>, carries the category's IID, and has
# its plugin.json copied into the same directory. NO_INSTALL marks a test double, so that a
# deployment contains only the analysis providers.
function(otter_add_interpreter_plugin _target _directory)
    set(options NO_INSTALL)
    set(multiValueArgs SOURCES LINKS)
    cmake_parse_arguments(FUNC "${options}" "" "${multiValueArgs}" ${ARGN})

    set(_install_option)
    if(FUNC_NO_INSTALL)
        set(_install_option NO_INSTALL)
    endif()

    otter_add_plugin(${_target} CATEGORY ${CURRENT_PLUGIN_CATEGORY}/${_directory} NO_EXPORT
        ${_install_option}
        SOURCES ${FUNC_SOURCES}
        FEATURES cxx_std_17
        LINKS ${FUNC_LINKS}
        INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${_directory}
    )
    otter_drop_plugin_postfix(${_target})

    stdc_add_plugin_metadata(TARGET ${_target} IID ${CURRENT_PLUGIN_IID})

    # configure_file is used instead of a copy command because it re-runs when plugin.json changes
    # and restores the output if the output is missing; the copy command does neither. A stale
    # manifest is harmful because discovery resolves the library through its name field.
    configure_file(${_directory}/plugin.json
        "${OTTER_BUILD_PLUGINS_DIR}/${CURRENT_PLUGIN_CATEGORY}/${_directory}/plugin.json"
        COPYONLY
    )

    if(OTTER_INSTALL AND NOT FUNC_NO_INSTALL)
        install(FILES ${_directory}/plugin.json
            DESTINATION "${OTTER_INSTALL_PLUGINS_DIR}/${CURRENT_PLUGIN_CATEGORY}/${_directory}"
        )
    endif()
endfunction()

# The shipped interpreters run ONNX models and require dsinfer. Without dsinfer only the library is
# built: packages still load, and only analyzer execution is unavailable. synthrt degrades in the
# same way without its own driver.
if(OTTER_HAS_DSINFER)
    # otter_onnx_support contains the code shared by the three ONNX providers, as a static library
    # that each provider links. The code cannot reside in the otter library because the otter
    # library does not depend on dsinfer. Hidden visibility prevents a plugin from re-exporting it.
    add_library(otter_onnx_support STATIC onnx/OnnxSupport.h onnx/OnnxSupport.cpp)
    target_link_libraries(otter_onnx_support PUBLIC otter otter_support dsinfer::dsinfer)
    target_include_directories(otter_onnx_support PUBLIC onnx)
    target_compile_features(otter_onnx_support PUBLIC cxx_std_17)
    set_target_properties(otter_onnx_support PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    otter_add_interpreter_plugin(otterrmvpe rmvpe
        SOURCES rmvpe/main.cpp
        LINKS otter otter_support otter_onnx_support dsinfer::dsinfer
    )
    otter_add_interpreter_plugin(ottergame game
        SOURCES game/main.cpp
        LINKS otter otter_support otter_onnx_support dsinfer::dsinfer
    )
    otter_add_interpreter_plugin(otterhfa hfa
        SOURCES hfa/main.cpp hfa/Aligner.h hfa/Aligner.cpp hfa/G2p.h hfa/G2p.cpp hfa/NonSpeech.h
                hfa/NonSpeech.cpp
        LINKS otter otter_support otter_onnx_support dsinfer::dsinfer
    )
endif()

# The stub interpreter allows the package path, interpreter discovery and the executive lifecycle
# to be tested without any model.
if(OTTER_BUILD_TESTS)
    otter_add_interpreter_plugin(otterstub stub NO_INSTALL
        SOURCES stub/main.cpp
        LINKS otter otter_support
    )
endif()
