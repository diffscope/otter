project(otterrmvpe
    VERSION ${OTTER_VERSION}
    LANGUAGES CXX
)

file(GLOB_RECURSE _src *.h *.cpp)

otter_add_plugin(${PROJECT_NAME} CATEGORY ${CURRENT_PLUGIN_CATEGORY}/rmvpe NO_EXPORT
    SOURCES ${_src}
    FEATURES cxx_std_17
    LINKS otter dsinfer::dsinfer
    INCLUDE_PRIVATE *
)
otter_drop_plugin_postfix(${PROJECT_NAME})

stdc_add_plugin_metadata(TARGET ${PROJECT_NAME} IID ${CURRENT_PLUGIN_IID})

# configure_file rather than a copy command: it re-runs when plugin.json changes and restores the
# output if it goes missing, neither of which the copy command does. A stale manifest is not a
# harmless one, because discovery resolves the library through its name field.
configure_file(plugin.json
    "${OTTER_BUILD_PLUGINS_DIR}/${CURRENT_PLUGIN_CATEGORY}/rmvpe/plugin.json"
    COPYONLY
)

if(OTTER_INSTALL)
    install(FILES plugin.json
        DESTINATION "${OTTER_INSTALL_PLUGINS_DIR}/${CURRENT_PLUGIN_CATEGORY}/rmvpe"
    )
endif()
