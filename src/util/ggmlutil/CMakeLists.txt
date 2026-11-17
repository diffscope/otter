# The files that the two ggml providers hand their engine: the float32 WAV file of an execution and
# its scratch directory. The engine process itself is a stdc::Popen of each provider. Hidden
# visibility prevents a plugin from re-exporting the code.
file(GLOB_RECURSE _src CONFIGURE_DEPENDS *.h *.cpp)

otter_add_library(ggmlutil STATIC NO_INSTALL
    SOURCES ${_src}
    FEATURES cxx_std_17
    LINKS synthrt::synthrt stdcorelib::stdcorelib
    INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/..
)
set_target_properties(ggmlutil PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
