file(GLOB_RECURSE _src "*.cpp")

otter_add_library(${PROJECT_NAME} SHARED
    SOURCES ${_src}
    LINKS synthrt::synthrt
    INCLUDE_PRIVATE ../../include/** **
)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME synthrt-otter)

# What linking otter buys is a static registration that runs before main: the analysis category is
# added to synthrt's registry. A linker that drops unreferenced libraries, which ELF linkers do
# under --as-needed and the MSVC linker does for every import library, would drop exactly that
# dependency from a host that names no otter symbol. Rather than push a linker option onto every
# consumer's link line, the library exports otter::linkAnalysisCategory() for such a host to call.

# A target named in the exported link interface must be one otterConfig.cmake actually brings in,
# otherwise every consumer's find_package(otter) fails at generate time with "but the target was
# not found" — and nothing in this repository would notice, because the tests link the build-tree
# target directly and never go through the installed config.
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
        "bring that target in. Either move the dependency to LINKS_PRIVATE (correct when it "
        "appears in no public header), or add a find_dependency() for it to the config template "
        "and list it in _otter_exported_targets here.")
endforeach()

unset(_otter_exported_targets)
unset(_otter_interface_links)
