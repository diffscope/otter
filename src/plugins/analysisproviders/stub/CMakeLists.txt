project(otteranalysisstub
    VERSION ${OTTER_VERSION}
    LANGUAGES CXX
)

file(GLOB_RECURSE _src *.h *.cpp)

# A test double: never installed, so a deployment carries only the providers that analyze.
otter_add_plugin(${PROJECT_NAME} CATEGORY ${CURRENT_PLUGIN_CATEGORY}/stub NO_EXPORT NO_INSTALL
    SOURCES ${_src}
    FEATURES cxx_std_17
    LINKS otter
    INCLUDE_PRIVATE *
)
otter_drop_plugin_postfix(${PROJECT_NAME})

stdc_add_plugin_metadata(TARGET ${PROJECT_NAME} IID ${CURRENT_PLUGIN_IID})

configure_file(plugin.json
    "${OTTER_BUILD_PLUGINS_DIR}/${CURRENT_PLUGIN_CATEGORY}/stub/plugin.json"
    COPYONLY
)
