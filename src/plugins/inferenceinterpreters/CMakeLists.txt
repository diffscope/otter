include("${SYNTHRT_MODULES_DIR}/AddPlugin.cmake")
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

    # The plugin loader resolves the library beside the manifest, and a multi-config generator
    # would otherwise build it into a configuration subdirectory the loader never searches. Every
    # configuration therefore builds into the manifest's directory; the plugin carries no debug
    # postfix, so the configurations overwrite one another, and only one of them is deployed at a
    # time.
    set(_plugin_output_dir "${OTTER_BUILD_PLUGINS_DIR}/${CURRENT_PLUGIN_CATEGORY}/${_directory}")
    set_target_properties(${_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_plugin_output_dir}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${_plugin_output_dir}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${_plugin_output_dir}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${_plugin_output_dir}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${_plugin_output_dir}"
    )

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
    include("${SYNTHRT_MODULES_DIR}/AddPlugin.cmake")
    synthrt_add_plugin_objects(otterrmvpe DIRECTORY rmvpe
        FEATURES cxx_std_17
        LINKS synthrt-otter
        LINKS_PRIVATE analysisutil onnxutil synthrt::dsinfer
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}
        INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rmvpe/*
    )
    otter_add_interpreter_plugin(otterrmvpe rmvpe
        SOURCES rmvpe/main.cpp
        LINKS otterrmvpe_objects analysisutil onnxutil synthrt::dsinfer
    )
    synthrt_add_plugin_objects(ottergame DIRECTORY game
        FEATURES cxx_std_17
        LINKS synthrt-otter
        LINKS_PRIVATE analysisutil onnxutil synthrt::dsinfer
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}
        INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/game/*
    )
    otter_add_interpreter_plugin(ottergame game
        SOURCES game/main.cpp
        LINKS ottergame_objects analysisutil onnxutil synthrt::dsinfer
    )
    synthrt_add_plugin_objects(otterhfa DIRECTORY hfa
        FEATURES cxx_std_17
        LINKS synthrt-otter
        LINKS_PRIVATE analysisutil onnxutil synthrt::dsinfer
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}
        INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hfa/*
    )
    otter_add_interpreter_plugin(otterhfa hfa
        SOURCES hfa/main.cpp
        LINKS otterhfa_objects analysisutil onnxutil synthrt::dsinfer
    )
    synthrt_add_plugin_objects(ottertifa DIRECTORY tifa
        FEATURES cxx_std_17
        LINKS synthrt-otter
        LINKS_PRIVATE analysisutil onnxutil synthrt::dsinfer
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}
        INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tifa/*
    )
    otter_add_interpreter_plugin(ottertifa tifa
        SOURCES tifa/main.cpp
        LINKS ottertifa_objects analysisutil onnxutil synthrt::dsinfer
    )
endif()

# The ggml providers drive the command line tools of the game.cpp and tifa.cpp releases as child
# processes: the span travels in, the notes or the alignment travel out, and the engine, the ggml
# backends it ships with and the weights stay outside the host process, where their own release
# keeps them. They need neither dsinfer nor any source of this repository, so they build beside the
# ONNX providers, and a host that offers them needs nothing beside the package.
option(OTTER_BUILD_GAME_GGML "Build the game-ggml interpreter, which drives the game.cpp CLI" ON)
option(OTTER_BUILD_TIFA_GGML "Build the tifa-ggml interpreter, which drives the tifa.cpp CLI" ON)

if(OTTER_BUILD_GAME_GGML)
    synthrt_add_plugin_objects(ottergameggml DIRECTORY game-ggml
        FEATURES cxx_std_17
        LINKS synthrt-otter
        LINKS_PRIVATE analysisutil ggmlutil
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}
        INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/game-ggml/*
    )
    otter_add_interpreter_plugin(ottergameggml game-ggml
        SOURCES game-ggml/main.cpp
        LINKS ottergameggml_objects analysisutil ggmlutil
    )
endif()

if(OTTER_BUILD_TIFA_GGML)
    synthrt_add_plugin_objects(ottertifaggml DIRECTORY tifa-ggml
        FEATURES cxx_std_17
        LINKS synthrt-otter
        LINKS_PRIVATE analysisutil ggmlutil
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}
        INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tifa-ggml/*
    )
    otter_add_interpreter_plugin(ottertifaggml tifa-ggml
        SOURCES tifa-ggml/main.cpp
        LINKS ottertifaggml_objects analysisutil ggmlutil
    )
endif()
