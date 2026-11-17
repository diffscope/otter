# Every plugin is declared by otter_add_interpreter_plugin(), which combines otter_add_plugin()
# with the building blocks of AddPlugin.cmake in the synthrt package: the sources but main.cpp are
# compiled into the object library <target>_objects that the tests link. AddPlugin.cmake
# describes the layout and the include namespace.
include("${SYNTHRT_MODULES_DIR}/AddPlugin.cmake")

add_subdirectory(inferenceinterpreters)
