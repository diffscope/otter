# The readers and the checks that the library and the plugins share. They are not part of what a
# host is offered: the headers are not installed, and no binary exports the functions. Every module
# of this tree links the library individually. The functions hold no state, so a separate copy in
# each module is equivalent to a shared copy, and the objects are compiled with hidden visibility so
# that a module linking them does not export them in turn.
file(GLOB_RECURSE _src CONFIGURE_DEPENDS *.h *.cpp)

# The headers include the public headers of otter. The include directory of the source tree
# provides them, because the library cannot link libotter, which links the library.
otter_add_library(analysisutil STATIC NO_INSTALL
    SOURCES ${_src}
    FEATURES cxx_std_17
    LINKS synthrt::synthrt
    INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/.. ${OTTER_SOURCE_DIR}/include
)
set_target_properties(analysisutil PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
