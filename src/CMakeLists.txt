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

# This file installs the headers, because the helper installs everything under include/, and
# otter/Support is not part of the public interface: its readers are compiled into the private
# static library otter_support, which the library and each plugin in this tree link individually.
# The installed headers are those of Analysis, Api and otter_global.h.
otter_install(NO_INCLUDE)

if(OTTER_INSTALL AND OTTER_DEVEL)
    install(DIRECTORY ${OTTER_INCLUDE_DIR}/
        DESTINATION ${OTTER_INSTALL_INCLUDE_DIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "Support" EXCLUDE
    )
endif()

otter_finish_buildsystem()

if(OTTER_INSTALL)
    # Install the license file
    install(FILES ../LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${OTTER_INSTALL_NAME}
    )
endif()
