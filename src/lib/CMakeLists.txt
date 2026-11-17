file(GLOB_RECURSE _src CONFIGURE_DEPENDS "*.cpp")

# The export macros would take their prefix from the target name, SYNTHRT_OTTER, while
# otter_global.h tests OTTER_LIBRARY and OTTER_STATIC.
otter_add_library(${PROJECT_NAME} SHARED
    MACRO_PREFIX OTTER
    SOURCES ${_src}
    LINKS synthrt::synthrt
    LINKS_PRIVATE analysisutil
    INCLUDE_PRIVATE ../../include/** **
)

# The package is synthrt-otter, and the namespace is synthrt, which the libraries built on synthrt
# share, so that a host links synthrt::otter.
set_target_properties(${PROJECT_NAME} PROPERTIES EXPORT_NAME otter)

# Every target named in the exported link interface must be imported by synthrt-otter-config.cmake.
# Otherwise the find_package(synthrt-otter) call of every consumer fails at generate time with "but
# the target was not found", and no test in this repository detects the failure, because the tests
# link the build-tree target directly and never use the installed config.
set(_otter_exported_targets synthrt::synthrt)  # keep in sync with src/synthrt-otter-config.cmake.in
get_target_property(_otter_interface_links ${PROJECT_NAME} INTERFACE_LINK_LIBRARIES)

foreach(_link IN LISTS _otter_interface_links)
    if(NOT _link MATCHES "::")
        continue()  # a plain library name or a flag, not an imported target
    endif()
    if(_link IN_LIST _otter_exported_targets)
        continue()
    endif()
    message(FATAL_ERROR
        "otter exports '${_link}' in its link interface, but src/synthrt-otter-config.cmake.in "
        "does not import that target. Either move the dependency to LINKS_PRIVATE (correct if it "
        "appears in no public header), or add a find_dependency() call for it to the config "
        "template and list it in _otter_exported_targets here.")
endforeach()

unset(_otter_exported_targets)
unset(_otter_interface_links)
