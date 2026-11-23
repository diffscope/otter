# Fetches the model packages of a release into a directory.
#
# The packages are data, not sources: they are published as assets of a release rather than tracked
# in this repository, and docs/Packages.md is the authority on what a release holds. This script
# downloads the manifest of the release, checks every archive against the SHA512 that manifest
# states for it, and unpacks it, so that a checkout can be given the real packages in one command,
# without a Python interpreter.
#
#     cmake -DOTTER_FETCH_OUTPUT=build/models -P scripts/fetch-models.cmake
#     cmake -DOTTER_FETCH_OUTPUT=build/models -DOTTER_FETCH_VARIANTS=hfa -P scripts/fetch-models.cmake
#     cmake -DOTTER_FETCH_OUTPUT=build/models -DOTTER_FETCH_VARIANTS=all -P scripts/fetch-models.cmake
#
# With no variants stated the script reads the manifest and only lists what the release holds: the
# packages are hundreds of megabytes, so fetching them is asked for rather than assumed.
#
# Every argument is read as a cache variable, so -D has to precede -P.
#
#   DOTTER_FETCH_OUTPUT          directory holding the manifest, the archives and the unpacked
#                                packages. Required, because the script exists to write something.
#   DOTTER_FETCH_VARIANTS        variants to fetch, separated by semicolons or commas, or the word
#                                all. An empty value lists the packages and fetches nothing.
#   DOTTER_FETCH_TAG             release tag (default: models-v0.1)
#   DOTTER_FETCH_REPOSITORY      owner/name of the repository holding the release. The project's
#                                homepage is the default.
#   DOTTER_FETCH_BASE_URL        where the assets are read from. Defaults to the download URL of
#                                the tag, and is what the self-check points at a local release.
#   DOTTER_FETCH_MANIFEST        read this manifest instead of downloading one, which makes the run
#                                work offline. The manifest of a fetched release is kept in the
#                                output directory and can be handed back this way.
#   DOTTER_FETCH_KEEP_ARCHIVES   keep the archives beside the unpacked packages (default: OFF,
#                                because a full release is twice its unpacked size)
#
# A setting can be given as an environment variable or as a cache variable (-D), and the environment
# is what the examples use, because it is the form that arrived in every run measured here: with
# cmake 4.3.1 an untyped -D entry for DOTTER_FETCH_OUTPUT was seen to be dropped, which left the
# script to refuse a run that had stated everything it needed. A run that states nothing is told so
# by name, rather than fetching something else.
#
#   DOTTER_FETCH_OUTPUT=build/models DOTTER_FETCH_VARIANTS=hfa cmake -P scripts/fetch-models.cmake
#
# The manifest is the format scripts/make-package.py writes: a bundle version and one entry per
# package, each naming the archive, its SHA512, the directory inside it and the models it carries.

cmake_minimum_required(VERSION 3.19)

# --------------------------------------------------
# Arguments
# --------------------------------------------------

# Every setting is read from the environment first and from a cache variable second. See the note
# above on why both are accepted.
macro(_otter_fetch_setting name default)
    if(DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
        set(${name} "$ENV{${name}}")
    elseif(NOT DEFINED ${name} OR "${${name}}" STREQUAL "")
        set(${name} "${default}")
    endif()
endmacro()

_otter_fetch_setting(DOTTER_FETCH_OUTPUT "")
if("${DOTTER_FETCH_OUTPUT}" STREQUAL "")
    message(FATAL_ERROR
        "state where the packages go, as -DOTTER_FETCH_OUTPUT:PATH=<dir> or as the environment "
        "variable DOTTER_FETCH_OUTPUT, for example\n"
        "    cmake -DOTTER_FETCH_OUTPUT:PATH=build/models -DOTTER_FETCH_VARIANTS:STRING=hfa -P ${CMAKE_CURRENT_LIST_FILE}")
endif()
_otter_fetch_setting(DOTTER_FETCH_TAG "models-v0.1")
_otter_fetch_setting(DOTTER_FETCH_REPOSITORY "diffscope/otter")
_otter_fetch_setting(DOTTER_FETCH_VARIANTS "")
_otter_fetch_setting(DOTTER_FETCH_MANIFEST "")
_otter_fetch_setting(DOTTER_FETCH_KEEP_ARCHIVES "")
if(NOT DEFINED DOTTER_FETCH_BASE_URL OR "${DOTTER_FETCH_BASE_URL}" STREQUAL "")
    if(DEFINED ENV{DOTTER_FETCH_BASE_URL} AND NOT "$ENV{DOTTER_FETCH_BASE_URL}" STREQUAL "")
        set(DOTTER_FETCH_BASE_URL "$ENV{DOTTER_FETCH_BASE_URL}")
    else()
        set(DOTTER_FETCH_BASE_URL
            "https://github.com/${DOTTER_FETCH_REPOSITORY}/releases/download/${DOTTER_FETCH_TAG}")
    endif()
endif()

set(_output "${DOTTER_FETCH_OUTPUT}")
message(STATUS "output: ${_output}")
message(STATUS "release: ${DOTTER_FETCH_TAG} of ${DOTTER_FETCH_REPOSITORY}")
message(STATUS "variants: [${DOTTER_FETCH_VARIANTS}]")
file(MAKE_DIRECTORY "${_output}")

# --------------------------------------------------
# The manifest
# --------------------------------------------------

set(_manifest_file "${_output}/manifest.json")
if(DEFINED DOTTER_FETCH_MANIFEST AND NOT "${DOTTER_FETCH_MANIFEST}" STREQUAL "")
    if(NOT EXISTS "${DOTTER_FETCH_MANIFEST}")
        message(FATAL_ERROR "DOTTER_FETCH_MANIFEST names ${DOTTER_FETCH_MANIFEST}, which does not exist")
    endif()
    set(_manifest_file "${DOTTER_FETCH_MANIFEST}")
    message(STATUS "manifest: ${_manifest_file}")
else()
    message(STATUS "downloading ${DOTTER_FETCH_BASE_URL}/manifest.json")
    file(DOWNLOAD "${DOTTER_FETCH_BASE_URL}/manifest.json" "${_manifest_file}"
         STATUS _manifest_status TLS_VERIFY ON SHOW_PROGRESS)
    list(GET _manifest_status 0 _manifest_code)
    if(NOT _manifest_code EQUAL 0)
        message(FATAL_ERROR
            "the manifest of ${DOTTER_FETCH_TAG} was not downloaded from "
            "${DOTTER_FETCH_BASE_URL}/manifest.json: ${_manifest_status}")
    endif()
endif()

file(READ "${_manifest_file}" _manifest)
string(JSON _bundle ERROR_VARIABLE _bundle_error GET "${_manifest}" bundleVersion)
if(_bundle_error)
    message(FATAL_ERROR "${_manifest_file} states no bundleVersion: ${_bundle_error}")
endif()
string(JSON _entry_count ERROR_VARIABLE _packages_error LENGTH "${_manifest}" packages)
if(_packages_error)
    message(FATAL_ERROR "${_manifest_file} lists no packages: ${_packages_error}")
endif()
if(_entry_count EQUAL 0)
    message(FATAL_ERROR "${_manifest_file} lists no packages")
endif()

# A release tag is the bundle version without its trailing zero components (docs/Packages.md), so a
# manifest announcing a version whose tag is not the tag this run reads came from another release.
# Fetching it anyway would put packages of one release beside the name of another.
set(_trimmed "${_bundle}")
while(_trimmed MATCHES "\\.0$")
    string(REGEX REPLACE "\\.0$" "" _trimmed "${_trimmed}")
endwhile()
set(_expected_tag "models-v${_trimmed}")
if(NOT _expected_tag STREQUAL DOTTER_FETCH_TAG)
    message(FATAL_ERROR
        "${_manifest_file} announces bundleVersion ${_bundle}, whose release tag is ${_expected_tag}, "
        "while this run reads the tag ${DOTTER_FETCH_TAG}")
endif()

# --------------------------------------------------
# The entries
# --------------------------------------------------

set(_ids "")
set(_files "")
set(_digests "")
set(_directories "")
set(_sizes "")
set(_model_counts "")
math(EXPR _last "${_entry_count} - 1")
foreach(_index RANGE 0 ${_last})
    string(JSON _id ERROR_VARIABLE _error GET "${_manifest}" packages ${_index} id)
    if(_error)
        message(FATAL_ERROR "entry ${_index} of ${_manifest_file} states no id: ${_error}")
    endif()
    string(JSON _file ERROR_VARIABLE _error GET "${_manifest}" packages ${_index} file)
    if(_error)
        message(FATAL_ERROR "entry ${_index} of ${_manifest_file} (${_id}) states no archive: ${_error}")
    endif()
    string(JSON _digest ERROR_VARIABLE _error GET "${_manifest}" packages ${_index} sha512)
    if(_error)
        message(FATAL_ERROR "entry ${_index} of ${_manifest_file} (${_id}) states no SHA512: ${_error}")
    endif()
    string(JSON _directory ERROR_VARIABLE _error GET "${_manifest}" packages ${_index} directory)
    if(_error)
        message(FATAL_ERROR "entry ${_index} of ${_manifest_file} (${_id}) states no directory: ${_error}")
    endif()
    string(JSON _size ERROR_VARIABLE _error GET "${_manifest}" packages ${_index} size)
    if(_error)
        message(FATAL_ERROR "entry ${_index} of ${_manifest_file} (${_id}) states no size: ${_error}")
    endif()
    string(JSON _model_count ERROR_VARIABLE _error LENGTH "${_manifest}" packages ${_index} models)
    if(_error)
        message(FATAL_ERROR "entry ${_index} of ${_manifest_file} (${_id}) lists no models: ${_error}")
    endif()
    math(EXPR _size_mb "${_size} / 1048576")
    list(APPEND _ids "${_id}")
    list(APPEND _files "${_file}")
    list(APPEND _digests "${_digest}")
    list(APPEND _directories "${_directory}")
    list(APPEND _sizes "${_size_mb} MB")
    list(APPEND _model_counts "${_model_count}")
endforeach()

# --------------------------------------------------
# What to fetch
# --------------------------------------------------

set(_wanted "${DOTTER_FETCH_VARIANTS}")
string(REPLACE "," ";" _wanted "${_wanted}")
if("${_wanted}" STREQUAL "")
    message(STATUS "${DOTTER_FETCH_TAG} announces bundle version ${_bundle} and holds ${_entry_count} packages:")
    foreach(_index RANGE 0 ${_last})
        list(GET _ids ${_index} _id)
        list(GET _files ${_index} _file)
        list(GET _sizes ${_index} _size)
        list(GET _model_counts ${_index} _model_count)
        list(GET _directories ${_index} _directory)
        message(STATUS "  ${_id}  ${_file}  ${_size}  ${_model_count} models  -> ${_directory}")
    endforeach()
    message(STATUS "nothing was fetched: pass -DOTTER_FETCH_VARIANTS=<variant>[;<variant>] or all")
    return()
endif()

set(_selected "")
foreach(_variant IN LISTS _wanted)
    if("${_variant}" STREQUAL "all")
        set(_selected "${_ids}")
    elseif("${_variant}" MATCHES "/")
        list(APPEND _selected "${_variant}")
    else()
        list(APPEND _selected "otter/${_variant}")
    endif()
endforeach()

# --------------------------------------------------
# Fetch
# --------------------------------------------------

foreach(_wanted_id IN LISTS _selected)
    list(FIND _ids "${_wanted_id}" _position)
    if(_position EQUAL -1)
        message(FATAL_ERROR "${DOTTER_FETCH_TAG} holds no package ${_wanted_id}; it holds: ${_ids}")
    endif()
    list(GET _files ${_position} _file)
    list(GET _digests ${_position} _digest)
    list(GET _directories ${_position} _directory)
    set(_archive "${_output}/${_file}")

    # An archive already in place is reused when it carries the recorded digest, so that a second
    # run downloads nothing. Any other state is downloaded again rather than trusted.
    set(_have_archive OFF)
    if(EXISTS "${_archive}")
        file(SHA512 "${_archive}" _present)
        if(_present STREQUAL _digest)
            set(_have_archive ON)
            message(STATUS "${_file}: already in place with the recorded SHA512")
        else()
            message(STATUS "${_file}: in place but its SHA512 differs; downloading it again")
        endif()
    endif()
    if(NOT _have_archive)
        message(STATUS "downloading ${DOTTER_FETCH_BASE_URL}/${_file}")
        file(DOWNLOAD "${DOTTER_FETCH_BASE_URL}/${_file}" "${_archive}"
             STATUS _status TLS_VERIFY ON SHOW_PROGRESS)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            message(FATAL_ERROR
                "${_file} was not downloaded from ${DOTTER_FETCH_BASE_URL}/${_file}: ${_status}")
        endif()
        # The digest is checked here instead of through EXPECTED_HASH so that a mismatch reports
        # what the manifest records and removes what arrived, which leaves the next run a clean
        # start rather than a file that fails again for the same reason.
        file(SHA512 "${_archive}" _downloaded)
        if(NOT _downloaded STREQUAL _digest)
            file(REMOVE "${_archive}")
            message(FATAL_ERROR
                "${_file} carries the SHA512 ${_downloaded} while ${_manifest_file} records "
                "${_digest} for it")
        endif()
    endif()

    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_output}")
    if(NOT IS_DIRECTORY "${_output}/${_directory}")
        message(FATAL_ERROR "${_file} did not unpack into ${_directory}, the directory its entry states")
    endif()
    if(NOT DOTTER_FETCH_KEEP_ARCHIVES)
        file(REMOVE "${_archive}")
    endif()
    message(STATUS "${_directory}: unpacked into ${_output}/${_directory}")
endforeach()

message(STATUS "release ${DOTTER_FETCH_TAG}, bundle version ${_bundle}, manifest ${_manifest_file}")
