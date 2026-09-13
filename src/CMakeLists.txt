include("conf.cmake")

otter_init_buildsystem()

# Plugins install under lib/plugins/otter/<category>/<name>/, the shape dsinfer uses for its own,
# so that every library layered on synthrt puts its plugins under one root and a host deploys and
# searches one tree. The helper's default would be lib/otter/plugins.
set(OTTER_BUILD_PLUGINS_DIR ${OTTER_BUILD_LIBRARY_DIR}/plugins/${OTTER_INSTALL_NAME})
set(OTTER_INSTALL_PLUGINS_DIR ${OTTER_INSTALL_LIBRARY_DIR}/plugins/${OTTER_INSTALL_NAME})

add_subdirectory(lib)

add_subdirectory(plugins)

if(OTTER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# The helper would write a version file that accepts any newer version. otter promises no ABI
# before 1.0, so a host built against one version must not be handed another: the CMake package
# is written here with ExactVersion, and the helper installs only the headers.
otter_install(NO_EXPORT)

if(OTTER_INSTALL)
    qm_basic_install(
        NAME ${OTTER_INSTALL_NAME}
        VERSION ${OTTER_INSTALL_VERSION}
        COMPATIBILITY ExactVersion
        INSTALL_DIR ${OTTER_INSTALL_CMAKE_DIR}
        CONFIG_TEMPLATE ${OTTER_INSTALL_CONFIG_TEMPLATE}
        NAMESPACE ${OTTER_INSTALL_NAMESPACE}::
        EXPORT ${OTTER_EXPORT}
        WRITE_CONFIG_OPTIONS NO_CHECK_REQUIRED_COMPONENTS_MACRO
    )
endif()

otter_finish_buildsystem()

if(OTTER_INSTALL)
    # Install copyright
    install(FILES ../LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${OTTER_INSTALL_NAME}
    )
endif()
