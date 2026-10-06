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
    # otter_onnx_support contains the code shared by the four ONNX providers, as a static library
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
    # The aligner that reads the five exported graphs: the spectrogram front end, the acoustic
    # model, the scoring inputs, the word scores and the selection that turns them into frames.
    otter_add_interpreter_plugin(ottertifa tifa
        SOURCES tifa/main.cpp tifa/Decode.h tifa/Decode.cpp tifa/Grid.h tifa/Grid.cpp
                tifa/Scoring.h tifa/Scoring.cpp
        LINKS otter otter_support otter_onnx_support dsinfer::dsinfer
    )
endif()

# The ggml providers drive the command line tools of the game.cpp and tifa.cpp releases as child
# processes: the span travels in, the notes or the alignment travel out, and the engine, the ggml
# backends it ships with and the weights stay outside the host process, where their own release
# keeps them. They need neither dsinfer nor any source of this repository, so they build beside the
# ONNX providers, and a host that offers them needs nothing beside the package.
option(OTTER_BUILD_GAME_GGML "Build the game-ggml interpreter, which drives the game.cpp CLI" ON)
option(OTTER_BUILD_TIFA_GGML "Build the tifa-ggml interpreter, which drives the tifa.cpp CLI" ON)

# otter_cli_support contains the code the two external providers share, as a static library that
# each provider links: the child process with its redirected streams, the float32 WAV an execution
# hands its engine, and the scratch directory of one execution. Hidden visibility prevents a plugin
# from re-exporting it.
add_library(otter_cli_support STATIC external/CliProcess.h external/CliProcess.cpp)
target_link_libraries(otter_cli_support PUBLIC otter otter_support)
target_include_directories(otter_cli_support PUBLIC external)
target_compile_features(otter_cli_support PUBLIC cxx_std_17)
set_target_properties(otter_cli_support PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(OTTER_BUILD_GAME_GGML)
    otter_add_interpreter_plugin(ottergameggml game-ggml
        SOURCES game-ggml/main.cpp
        LINKS otter otter_support otter_cli_support
    )
endif()

if(OTTER_BUILD_TIFA_GGML)
    otter_add_interpreter_plugin(ottertifaggml tifa-ggml
        SOURCES tifa-ggml/main.cpp
        LINKS otter otter_support otter_cli_support
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
