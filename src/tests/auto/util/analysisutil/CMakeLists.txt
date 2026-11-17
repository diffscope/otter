# The readers and the checks of the private static library analysisutil, which the library and the
# plugins share and no binary exports.
add_auto_test(test_ManifestValues.cpp analysisutil)

add_auto_test(test_F0Curve.cpp analysisutil)

# Every test receives the environment that the parent directory computes.
foreach(_test IN ITEMS test_ManifestValues test_F0Curve)
    if(TARGET ${_test})
        set_tests_properties(${_test} PROPERTIES ENVIRONMENT_MODIFICATION "${_otter_test_env}")
    endif()
endforeach()
add_auto_test(test_KnownNotes.cpp analysisutil)
if(TARGET test_KnownNotes)
    set_tests_properties(test_KnownNotes PROPERTIES ENVIRONMENT_MODIFICATION "${_otter_test_env}")
endif()
