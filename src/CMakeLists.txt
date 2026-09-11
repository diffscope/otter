include("conf.cmake")

otter_init_buildsystem()

add_subdirectory(lib)

add_subdirectory(plugins)

if(OTTER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

otter_install()

otter_finish_buildsystem()

if(OTTER_INSTALL)
    # Install copyright
    install(FILES ../LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/${OTTER_INSTALL_NAME}
    )
endif()
