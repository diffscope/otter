# The code shared by the four ONNX providers: the sessions of an analyzer, the driver lookup, the
# files a model states its facts in, one model call with the readers of its outputs, and the
# contract check of a plugin. It cannot reside in the otter library because the otter library does
# not depend on dsinfer. Hidden visibility prevents a plugin from re-exporting it.
file(GLOB_RECURSE _src CONFIGURE_DEPENDS *.h *.cpp)

otter_add_library(onnxutil STATIC NO_INSTALL
    SOURCES ${_src}
    FEATURES cxx_std_17
    LINKS synthrt-otter analysisutil synthrt::dsinfer
    INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/..
)
set_target_properties(onnxutil PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
