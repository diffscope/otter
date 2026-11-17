get_filename_component(_dir_name ${CMAKE_CURRENT_LIST_DIR} NAME)
set(CURRENT_PLUGIN_CATEGORY ${_dir_name})
# The inference category belongs to synthrt; these interpreters therefore use synthrt's plugin IID.
set(CURRENT_PLUGIN_IID org.openvpi.synthrt.plugin.InferenceInterpreter)

# Declares one interpreter plugin of this category, from the directory of that name:
#
# otter_add_interpreter_plugin(<target> <directory>
#     [LINKS <libraries...>]
#     [LINKS_PRIVATE <libraries...>]
# )
#
# The plugin is built into <plugins>/<category>/<directory>, carries the category's IID, and has
# its plugin.json copied into the same directory. Its sources are every header and source of the
# directory, and the directory name is its include namespace. Every plugin links otter. LINKS adds
# the libraries that the headers of the plugin use, and LINKS_PRIVATE those that only its sources
# use.
function(otter_add_interpreter_plugin _target _directory)
    set(multiValueArgs LINKS LINKS_PRIVATE)
    cmake_parse_arguments(FUNC "" "" "${multiValueArgs}" ${ARGN})

    synthrt_add_plugin_objects(${_target} DIRECTORY ${_directory}
        FEATURES cxx_std_17
        LINKS synthrt-otter ${FUNC_LINKS}
        LINKS_PRIVATE ${FUNC_LINKS_PRIVATE}
        INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}
        INCLUDE_PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${_directory}/*
    )
    otter_add_plugin(${_target} CATEGORY ${CURRENT_PLUGIN_CATEGORY}/${_directory}
        SOURCES ${_directory}/main.cpp
        FEATURES cxx_std_17
        LINKS_PRIVATE ${_target}_objects ${FUNC_LINKS_PRIVATE}
    )
    stdc_add_plugin_metadata(TARGET ${_target} IID ${CURRENT_PLUGIN_IID})
    synthrt_configure_plugin_manifest(${_target} otter FILE ${_directory}/plugin.json)
endfunction()

# The shipped interpreters run ONNX models and require dsinfer. Without dsinfer only the library is
# built: packages still load, and only analyzer execution is unavailable. synthrt degrades in the
# same way without its own driver.
if(OTTER_HAS_DSINFER)
    # The headers of the providers use otter and synthrt only, so onnxutil, the code the providers
    # share, and dsinfer are private to their sources.
    set(_onnx_links analysisutil onnxutil synthrt::dsinfer)
    otter_add_interpreter_plugin(otterrmvpe rmvpe LINKS_PRIVATE ${_onnx_links})
    otter_add_interpreter_plugin(ottergame game LINKS_PRIVATE ${_onnx_links})
    otter_add_interpreter_plugin(otterhfa hfa LINKS_PRIVATE ${_onnx_links})
    # The aligner that reads the five exported graphs: the spectrogram front end, the acoustic
    # model, the scoring inputs, the word scores and the selection that turns them into frames.
    otter_add_interpreter_plugin(ottertifa tifa LINKS_PRIVATE ${_onnx_links})
endif()

# The ggml providers drive the command line tools of the game.cpp and tifa.cpp releases as child
# processes: the span travels in, the notes or the alignment travel out, and the engine, the ggml
# backends it ships with and the weights stay outside the host process, where their own release
# keeps them. They need neither dsinfer nor any source of this repository, so they build beside the
# ONNX providers, and a host that offers them needs nothing beside the package.
option(OTTER_BUILD_GAME_GGML "Build the game-ggml interpreter, which drives the game.cpp CLI" ON)
option(OTTER_BUILD_TIFA_GGML "Build the tifa-ggml interpreter, which drives the tifa.cpp CLI" ON)

# Each ggml provider runs its engine as a stdc::Popen and links ggmlutil for the files it hands the
# engine.
if(OTTER_BUILD_GAME_GGML)
    otter_add_interpreter_plugin(ottergameggml game-ggml LINKS_PRIVATE analysisutil ggmlutil)
endif()

if(OTTER_BUILD_TIFA_GGML)
    otter_add_interpreter_plugin(ottertifaggml tifa-ggml LINKS_PRIVATE analysisutil ggmlutil)
endif()
