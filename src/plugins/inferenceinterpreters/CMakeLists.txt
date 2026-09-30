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
# its plugin.json copied beside it. NO_INSTALL is for a test double, so that a deployment carries
# only the providers that analyze.
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

    # configure_file rather than a copy command: it re-runs when plugin.json changes and restores
    # the output if it goes missing, neither of which the copy command does. A stale manifest is
    # not a harmless one, because discovery resolves the library through its name field.
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

# The stub interpreter allows the package path, interpreter discovery and the executive lifecycle
# to be tested without any model.
if(OTTER_BUILD_TESTS)
    otter_add_interpreter_plugin(otterstub stub NO_INSTALL
        SOURCES stub/main.cpp
        LINKS otter otter_support
    )
endif()
