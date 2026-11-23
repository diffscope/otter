include("conf.cmake")

otter_init_buildsystem()

# The plugin layout is the default of the repo helper, lib/<install name>/plugins, which is
# lib/synthrt-otter/plugins here. synthrt and dsinfer use the same default, so every library built
# on synthrt keeps its plugins beside its own library directory rather than in a shared
# lib/plugins root.

add_subdirectory(util)

add_subdirectory(lib)

add_subdirectory(plugins)

if(OTTER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

otter_install()
if(COMMAND x_vcpkg_install_local_dependencies)
    x_vcpkg_install_local_dependencies(TARGETS synthrt-otter DESTINATION bin)
endif()

otter_finish_buildsystem()

if(OTTER_INSTALL)
    # Install the license file
    install(FILES ../LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${OTTER_INSTALL_NAME}
    )
endif()
