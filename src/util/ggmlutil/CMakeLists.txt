# The external engine process and its files, private to the ggml providers.
file(GLOB_RECURSE _src CONFIGURE_DEPENDS *.h *.cpp)

otter_add_library(ggmlutil STATIC NO_INSTALL
    SOURCES ${_src}
    FEATURES cxx_std_17
    LINKS synthrt-otter analysisutil
    INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/..
)
set_target_properties(ggmlutil PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
