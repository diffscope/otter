# A plugin's file name is what its plugin.json names, and the bundle loader tries only the
# platform's prefix and suffix around that name. The debug postfix the library targets carry would
# turn otterrmvpe.dll into otterrmvped.dll and leave every plugin undiscoverable in a Debug build,
# so every plugin drops it right after it is declared.
function(otter_drop_plugin_postfix _target)
    set_target_properties(${_target} PROPERTIES DEBUG_POSTFIX "")
endfunction()

add_subdirectory(analysisproviders)
