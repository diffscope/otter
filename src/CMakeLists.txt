include("conf.cmake")

otter_init_buildsystem()

# Plugins are installed under lib/plugins/otter/<category>/<name>/, the layout that dsinfer uses
# for its plugins, so that every library layered on synthrt installs its plugins under one root and
# a host deploys and searches one tree. The default of the helper is lib/otter/plugins.
set(OTTER_BUILD_PLUGINS_DIR ${OTTER_BUILD_LIBRARY_DIR}/plugins/${OTTER_INSTALL_NAME})
set(OTTER_INSTALL_PLUGINS_DIR ${OTTER_INSTALL_LIBRARY_DIR}/plugins/${OTTER_INSTALL_NAME})

add_subdirectory(lib)

add_subdirectory(plugins)

if(OTTER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# The helper writes a version file that accepts any newer version. otter guarantees no ABI
# stability before 1.0, so a host built against one version must not load another; this file
# therefore writes the CMake package with ExactVersion. This file also installs the headers,
# because the helper installs everything under include/, and otter/Support is not part of the
# public interface: its readers are compiled into the private static library otter_support, which
# the library and each plugin in this tree link individually. The installed headers are those of
# Analysis and Api.
otter_install(NO_EXPORT NO_INCLUDE)

if(OTTER_INSTALL AND OTTER_DEVEL)
    install(DIRECTORY ${OTTER_INCLUDE_DIR}/
        DESTINATION ${OTTER_INSTALL_INCLUDE_DIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "Support" EXCLUDE
    )
endif()

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
    # Install the license file
    install(FILES ../LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${OTTER_INSTALL_NAME}
    )
endif()
