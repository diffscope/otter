# ----------------------------------
# Project Constants
# ----------------------------------
set(OTTER_INCLUDE_DIR "../include")

# Install the CMake package files and the public headers alongside the binaries.
# The generic helpers gate both on <proj>_DEVEL, which defaults to off.
set(OTTER_DEVEL ON)

# Windows resource metadata.
set(OTTER_RC_DESCRIPTION "${PROJECT_DESCRIPTION}")
set(OTTER_RC_COPYRIGHT "Copyright (c) 2026-present Team OpenVPI")

function(_otter_common_configure_target _target)
    if(WIN32)
        qm_add_win_rc(${_target}
            NAME ${OTTER_INSTALL_NAME}
            DESCRIPTION "${OTTER_RC_DESCRIPTION}"
            COPYRIGHT "${OTTER_RC_COPYRIGHT}"
        )
    endif()

    otter_set_default_install_rpath(${_target})
endfunction()

set(OTTER_POST_CONFIGURE_COMMANDS _otter_common_configure_target)

# ----------------------------------
# Include Build Helpers
# ----------------------------------
qm_import(private/BuildSystem)
qm_setup_build_repo_helpers()
