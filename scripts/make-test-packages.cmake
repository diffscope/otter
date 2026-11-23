set(_otter_test_package_files
    "bad-configuration/desc.json"
    "bad-configuration/inferences/f0/inference.json"
    "bad-exports-knob-range/desc.json"
    "bad-exports-knob-range/inferences/f0/inference.json"
    "bad-exports-missing-rate/desc.json"
    "bad-exports-missing-rate/inferences/f0/inference.json"
    "bad-exports-unknown-key/desc.json"
    "bad-exports-unknown-key/inferences/f0/inference.json"
    "foreign-executive/desc.json"
    "foreign-executive/inferences/f0/inference.json"
    "importing-with-options/desc.json"
    "importing-with-options/inferences/f0/inference.json"
    "importing-with-options/inferences/note/inference.json"
    "importing/desc.json"
    "importing/inferences/f0/inference.json"
    "importing/inferences/note/inference.json"
    "no-declaration/desc.json"
    "slow-analyzer/desc.json"
    "slow-analyzer/inferences/f0/inference.json"
    "stereo-model/desc.json"
    "stereo-model/inferences/f0/inference.json"
    "stub-aligner/desc.json"
    "stub-aligner/inferences/align/inference.json"
    "stub-analyzers/desc.json"
    "stub-analyzers/inferences/f0/inference.json"
    "stub-analyzers/inferences/note/inference.json"
    "unknobbed/desc.json"
    "unknobbed/inferences/f0/inference.json"
)

# bad-configuration/desc.json
set(_otter_test_package_data_0 [[{
    "$version": "1.0",
    "id": "otter/test-bad-configuration",
    "version": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [ { "id": "f0", "path": "./inferences/f0/inference.json" } ]
    }
}
]])

# bad-configuration/inferences/f0/inference.json
set(_otter_test_package_data_1 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "Bad F0",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        }
    },
    "configuration": {
        "frequencey": 440
    }
}
]])

# bad-exports-knob-range/desc.json
set(_otter_test_package_data_2 [[{
    "$version": "1.0",
    "id": "otter/bad-exports-knob-range",
    "version": "1.0.0.0",
    "compatVersion": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [
            {
                "id": "f0",
                "path": "./inferences/f0/inference.json"
            }
        ]
    }
}
]])

# bad-exports-knob-range/inferences/f0/inference.json
set(_otter_test_package_data_3 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "Knob range",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.5,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        }
    }
}
]])

# bad-exports-missing-rate/desc.json
set(_otter_test_package_data_4 [[{
    "$version": "1.0",
    "id": "otter/bad-exports-missing-rate",
    "version": "1.0.0.0",
    "compatVersion": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [
            {
                "id": "f0",
                "path": "./inferences/f0/inference.json"
            }
        ]
    }
}
]])

# bad-exports-missing-rate/inferences/f0/inference.json
set(_otter_test_package_data_5 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "Missing rate",
    "exports": {
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        }
    }
}
]])

# bad-exports-unknown-key/desc.json
set(_otter_test_package_data_6 [[{
    "$version": "1.0",
    "id": "otter/bad-exports-unknown-key",
    "version": "1.0.0.0",
    "compatVersion": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [
            {
                "id": "f0",
                "path": "./inferences/f0/inference.json"
            }
        ]
    }
}
]])

# bad-exports-unknown-key/inferences/f0/inference.json
set(_otter_test_package_data_7 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "Unknown key",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        },
        "frameRate": 100
    }
}
]])

# foreign-executive/desc.json
set(_otter_test_package_data_8 [[{
    "$version": "1.0",
    "id": "otter/test-foreign-executive",
    "version": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [ { "id": "f0", "path": "./inferences/f0/inference.json" } ]
    }
}
]])

# foreign-executive/inferences/f0/inference.json
set(_otter_test_package_data_9 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "F0 whose interpreter returns an executive of no contract",
    "exports": {
        "sampleRate": 16000,
        "interval": 0.01
    },
    "configuration": {
        "foreignExecutive": true
    }
}
]])

# importing-with-options/desc.json
set(_otter_test_package_data_10 [[{
    "$version": "1.0",
    "id": "otter/test-importing-with-options",
    "version": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [
            {
                "id": "f0",
                "path": "./inferences/f0/inference.json"
            },
            {
                "id": "note",
                "path": "./inferences/note/inference.json"
            }
        ]
    }
}
]])

# importing-with-options/inferences/f0/inference.json
set(_otter_test_package_data_11 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "F0",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        }
    }
}
]])

# importing-with-options/inferences/note/inference.json
set(_otter_test_package_data_12 [[{
    "interface": "org.openvpi.otter.inference.Note",
    "level": 1,
    "variant": "stub",
    "name": "Note",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "maxSegmentDuration": 60,
        "languages": [
            "zxx"
        ],
        "defaultLanguage": "zxx",
        "supportsKnownNotes": true,
        "knobs": {
            "boundaryThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.2
            },
            "boundaryRadius": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.02
            },
            "noteThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.2
            },
            "notePresenceCutoff": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.0
            },
            "steps": {
                "minimum": 1,
                "maximum": 64,
                "default": 8
            }
        }
    },
    "imports": [
        {
            "role": "analysis/reference",
            "ref": ":inference/f0",
            "options": {
                "depth": 2
            }
        }
    ]
}
]])

# importing/desc.json
set(_otter_test_package_data_13 [[{
    "$version": "1.0",
    "id": "otter/test-importing",
    "version": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [
            {
                "id": "f0",
                "path": "./inferences/f0/inference.json"
            },
            {
                "id": "note",
                "path": "./inferences/note/inference.json"
            }
        ]
    }
}
]])

# importing/inferences/f0/inference.json
set(_otter_test_package_data_14 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "F0",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        }
    }
}
]])

# importing/inferences/note/inference.json
set(_otter_test_package_data_15 [[{
    "interface": "org.openvpi.otter.inference.Note",
    "level": 1,
    "variant": "stub",
    "name": "Note",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "maxSegmentDuration": 60,
        "languages": [
            "zxx"
        ],
        "defaultLanguage": "zxx",
        "supportsKnownNotes": true,
        "knobs": {
            "boundaryThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.2
            },
            "boundaryRadius": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.02
            },
            "noteThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.2
            },
            "notePresenceCutoff": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.0
            },
            "steps": {
                "minimum": 1,
                "maximum": 64,
                "default": 8
            }
        }
    },
    "imports": [
        {
            "role": "analysis/reference",
            "ref": ":inference/f0"
        }
    ]
}
]])

# no-declaration/desc.json
set(_otter_test_package_data_16 [[{
    "$version": "1.0",
    "id": "otter/test-no-declaration",
    "version": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [ { "id": "f0" } ]
    }
}
]])

# slow-analyzer/desc.json
set(_otter_test_package_data_17 [[{
    "$version": "1.0",
    "id": "otter/test-slow",
    "version": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [ { "id": "f0", "path": "./inferences/f0/inference.json" } ]
    }
}
]])

# slow-analyzer/inferences/f0/inference.json
set(_otter_test_package_data_18 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "Slow F0",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        }
    },
    "configuration": {
        "delay": 5.0
    }
}
]])

# stereo-model/desc.json
set(_otter_test_package_data_19 [[{
    "$version": "1.0",
    "id": "otter/test-stereo-model",
    "version": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [
            { "id": "f0", "path": "./inferences/f0/inference.json" }
        ]
    }
}
]])

# stereo-model/inferences/f0/inference.json
set(_otter_test_package_data_20 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "Stereo F0",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 2,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        }
    }
}
]])

# stub-aligner/desc.json
set(_otter_test_package_data_21 [[{
    "$version": "1.0",
    "id": "otter/test-stub-aligner",
    "version": "1.0.0.0",
    "compatVersion": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [ { "id": "align", "path": "./inferences/align/inference.json" } ]
    }
}
]])

# stub-aligner/inferences/align/inference.json
set(_otter_test_package_data_22 [[{
    "interface": "org.openvpi.otter.inference.Align",
    "level": 1,
    "variant": "stub",
    "name": "Stub Align",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "maxSegmentDuration": 60,
        "languages": [
            { "language": "cmn", "scheme": "pinyin", "lyrics": "scheme", "phonemes": ["a", "b"] },
            { "language": "yue", "scheme": "jyutping", "lyrics": "scheme", "phonemes": ["aa"] },
            { "language": "yue", "scheme": "yale", "lyrics": "text", "phonemes": ["a"] }
        ],
        "defaultLanguage": "cmn",
        "nonSpeechPhonemes": ["AP", "EP"],
        "defaultNonSpeechPhonemes": ["AP"],
        "silenceLabel": "SP",
        "knobs": {
            "nonSpeechThreshold": { "minimum": 0.0, "maximum": 1.0, "default": 0.5 },
            "nonSpeechMinDuration": { "minimum": 0.0, "maximum": 2.0, "default": 0.1 },
            "gapFill": { "minimum": 0.0, "maximum": 1.0, "default": 0.1 }
        }
    }
}
]])

# stub-analyzers/desc.json
set(_otter_test_package_data_23 [[{
    "$version": "1.0",
    "id": "otter/test-stub",
    "version": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [
            { "id": "f0", "path": "./inferences/f0/inference.json" },
            { "id": "note", "path": "./inferences/note/inference.json" }
        ]
    }
}
]])

# stub-analyzers/inferences/f0/inference.json
set(_otter_test_package_data_24 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": {
        "_": "Stub F0",
        "zh-CN": "桩 F0"
    },
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60,
        "knobs": {
            "voicingThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.03
            },
            "interpolateUnvoiced": {
                "default": true
            }
        }
    }
}
]])

# stub-analyzers/inferences/note/inference.json
set(_otter_test_package_data_25 [[{
    "interface": "org.openvpi.otter.inference.Note",
    "level": 1,
    "variant": "stub",
    "name": "Stub Note",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "maxSegmentDuration": 60,
        "languages": [
            "zxx"
        ],
        "defaultLanguage": "zxx",
        "supportsKnownNotes": true,
        "knobs": {
            "boundaryThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.2
            },
            "boundaryRadius": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.02
            },
            "noteThreshold": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.2
            },
            "notePresenceCutoff": {
                "minimum": 0.0,
                "maximum": 1.0,
                "default": 0.0
            },
            "steps": {
                "minimum": 1,
                "maximum": 64,
                "default": 8
            }
        }
    },
    "configuration": {
        "beat": 0.25
    }
}
]])

# unknobbed/desc.json
set(_otter_test_package_data_26 [[{
    "$version": "1.0",
    "id": "otter/unknobbed",
    "version": "1.0.0.0",
    "compatVersion": "1.0.0.0",
    "runtimeLevel": 1,
    "contributions": {
        "inference": [
            {
                "id": "f0",
                "path": "./inferences/f0/inference.json"
            }
        ]
    }
}
]])

# unknobbed/inferences/f0/inference.json
set(_otter_test_package_data_27 [[{
    "interface": "org.openvpi.otter.inference.F0",
    "level": 1,
    "variant": "stub",
    "name": "Unknobbed",
    "exports": {
        "sampleRate": 16000,
        "channelCount": 1,
        "interval": 0.01,
        "maxSegmentDuration": 60
    }
}
]])

# Rebuilds the test packages that the loading and runtime tests read.
#
# The packages are test data rather than sources: they are not tracked, and the tests that need them
# skip when the directory they are told to read is absent. This script is the single definition of
# their contents, so a checkout without them can be brought back to the state the tests expect:
#
#     cmake -DOTTER_TEST_PACKAGES_OUTPUT=build/test-packages -P scripts/make-test-packages.cmake
#
# -DOTTER_TEST_PACKAGES_CHECK=<dir> compares an existing directory against the table, reporting every
# file that is missing, differs or is unexpected, and writes nothing. -DOTTER_TEST_PACKAGES_LIST=ON
# lists the files and writes nothing.
#
# The bytes below were recorded from the packages that were tracked before they were removed, so the
# content is the content the tests read from the source directory. CMake writes text files in text
# mode, so a run on Windows writes CRLF where the recording used LF. The readers parse JSON and do
# not care, and the check below normalises line endings, so a directory generated on either platform
# passes either way.

cmake_minimum_required(VERSION 3.21)

list(LENGTH _otter_test_package_files _otter_package_count)
if(_otter_package_count EQUAL 0)
    message(FATAL_ERROR "the package table is empty")
endif()
math(EXPR _otter_package_last "${_otter_package_count} - 1")

if(OTTER_TEST_PACKAGES_LIST)
    foreach(_i RANGE ${_otter_package_last})
        list(GET _otter_test_package_files ${_i} _otter_name)
        message(STATUS "${_otter_name}")
    endforeach()
    return()
endif()

if(OTTER_TEST_PACKAGES_CHECK)
    set(_otter_root "${OTTER_TEST_PACKAGES_CHECK}")
    set(_otter_mode check)
elseif(OTTER_TEST_PACKAGES_OUTPUT)
    set(_otter_root "${OTTER_TEST_PACKAGES_OUTPUT}")
    set(_otter_mode write)
else()
    message(FATAL_ERROR
            "pass -DOTTER_TEST_PACKAGES_OUTPUT=<dir> to write the packages, or "
            "-DOTTER_TEST_PACKAGES_CHECK=<dir> to check an existing directory")
endif()

set(_otter_bad "")
foreach(_i RANGE ${_otter_package_last})
    list(GET _otter_test_package_files ${_i} _otter_name)
    set(_otter_data "${_otter_test_package_data_${_i}}")
    # The script itself may have been checked out with CRLF. The recorded content is LF, so the
    # carriage returns are dropped here rather than carried into the packages.
    string(REPLACE "" "" _otter_data "${_otter_data}")
    set(_otter_path "${_otter_root}/${_otter_name}")
    if(_otter_mode STREQUAL "write")
        get_filename_component(_otter_dir "${_otter_path}" DIRECTORY)
        file(MAKE_DIRECTORY "${_otter_dir}")
        file(WRITE "${_otter_path}" "${_otter_data}")
    elseif(NOT EXISTS "${_otter_path}")
        list(APPEND _otter_bad "missing: ${_otter_name}")
    else()
        file(READ "${_otter_path}" _otter_actual)
        string(REPLACE "" "" _otter_actual "${_otter_actual}")
        if(NOT _otter_actual STREQUAL "${_otter_data}")
            list(APPEND _otter_bad "differs: ${_otter_name}")
        endif()
    endif()
endforeach()

if(_otter_mode STREQUAL "check")
    file(GLOB_RECURSE _otter_present RELATIVE "${_otter_root}" "${_otter_root}/*")
    foreach(_file IN LISTS _otter_present)
        if(NOT _file IN_LIST _otter_test_package_files)
            list(APPEND _otter_bad "unexpected: ${_file}")
        endif()
    endforeach()
    list(LENGTH _otter_bad _otter_bad_count)
    if(_otter_bad_count GREATER 0)
        foreach(_line IN LISTS _otter_bad)
            message(STATUS "${_line}")
        endforeach()
        message(FATAL_ERROR "${_otter_bad_count} file(s) do not match the table")
    endif()
    message(STATUS "all ${_otter_package_count} files match the table")
else()
    message(STATUS "wrote ${_otter_package_count} files to ${_otter_root}")
endif()
