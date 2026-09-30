# A plugin's library file name is the name field of its plugin.json, and the bundle loader adds
# only the platform's prefix and suffix to that name. The debug postfix of library targets would
# turn otterrmvpe.dll into otterrmvped.dll and make every plugin undiscoverable in a Debug build;
# therefore every plugin removes the postfix immediately after its declaration.
function(otter_drop_plugin_postfix _target)
    set_target_properties(${_target} PROPERTIES DEBUG_POSTFIX "")
endfunction()

add_subdirectory(inferenceinterpreters)
