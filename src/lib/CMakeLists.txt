# The manifest readers are shared by the library and the plugins, and they are not part of what a
# host is offered: their header is not installed, and the library does not export them. They are
# therefore a private static library that every module of this tree links individually. The
# readers hold no state, so a separate copy in each module is equivalent to a shared copy, and no
# code outside this tree can depend on them. The objects are compiled with hidden visibility so
# that a module linking them does not export them in turn.
file(GLOB_RECURSE _support_src CONFIGURE_DEPENDS "Support/*.cpp")

add_library(otter_support STATIC ${_support_src})
target_link_libraries(otter_support PUBLIC synthrt::synthrt)
target_include_directories(otter_support PUBLIC "${OTTER_SOURCE_DIR}/include")
target_compile_features(otter_support PUBLIC cxx_std_17)
set_target_properties(otter_support PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

file(GLOB_RECURSE _src CONFIGURE_DEPENDS "*.cpp")
list(FILTER _src EXCLUDE REGEX "/Support/")

otter_add_library(${PROJECT_NAME} SHARED
    SOURCES ${_src}
    LINKS synthrt::synthrt
    LINKS_PRIVATE otter_support
    INCLUDE_PRIVATE ../../include/** **
)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME synthrt-otter)

# Every target named in the exported link interface must be imported by otterConfig.cmake.
# Otherwise the find_package(otter) call of every consumer fails at generate time with "but the
# target was not found", and no test in this repository detects the failure, because the tests link
# the build-tree target directly and never use the installed config.
set(_otter_exported_targets synthrt::synthrt)  # keep in sync with src/otterConfig.cmake.in
get_target_property(_otter_interface_links ${PROJECT_NAME} INTERFACE_LINK_LIBRARIES)

foreach(_link IN LISTS _otter_interface_links)
    if(NOT _link MATCHES "::")
        continue()  # a plain library name or a flag, not an imported target
    endif()
    if(_link IN_LIST _otter_exported_targets)
        continue()
    endif()
    message(FATAL_ERROR
        "otter exports '${_link}' in its link interface, but src/otterConfig.cmake.in does not "
        "import that target. Either move the dependency to LINKS_PRIVATE (correct if it appears "
        "in no public header), or add a find_dependency() call for it to the config template and "
        "list it in _otter_exported_targets here.")
endforeach()

unset(_otter_exported_targets)
unset(_otter_interface_links)
