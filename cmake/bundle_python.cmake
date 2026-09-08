set(JPLAY_PYTHON_VERSION "3.11.16")
set(JPLAY_PYTHON_RELEASE "20260814")

if(WIN32)
    set(_py_arch "x86_64-pc-windows-msvc")
    set(_py_sha  "ffaee1e94c8488f833473e56e1c980ca1a58d91126d21ddf0f6ba052e69cf511")
else()
    set(_py_arch "x86_64-unknown-linux-gnu")
    set(_py_sha  "33994fad90145ba559ebbe8a18d69fa7e56653502f7ba14ba07199b52cde3775")
endif()

set(_py_file "cpython-${JPLAY_PYTHON_VERSION}+${JPLAY_PYTHON_RELEASE}-${_py_arch}-install_only.tar.gz")
set(_py_url  "https://github.com/astral-sh/python-build-standalone/releases/download/${JPLAY_PYTHON_RELEASE}/${_py_file}")

set(_py_dir "${CMAKE_BINARY_DIR}/_python")
set(JPLAY_PYTHON_ROOT "${_py_dir}/python")             # the extracted interpreter
set(JPLAY_PYTHON_BUILD_TOOLS "${_py_dir}/buildtools")  # build-only packages (fontTools)
# Stamped once the tree is extracted, populated and pruned; delete _python/ to
# redo the whole thing (e.g. after bumping the version above).
set(_py_stamp "${_py_dir}/.jplay_python_ready")

string(REGEX MATCH "^[0-9]+\\.[0-9]+" JPLAY_PYTHON_MAJMIN "${JPLAY_PYTHON_VERSION}")
if(WIN32)
    set(_py_exe    "${JPLAY_PYTHON_ROOT}/python.exe")
    set(_py_stdlib "${JPLAY_PYTHON_ROOT}/Lib")
else()
    set(_py_exe    "${JPLAY_PYTHON_ROOT}/bin/python3")
    set(_py_stdlib "${JPLAY_PYTHON_ROOT}/lib/python${JPLAY_PYTHON_MAJMIN}")
endif()

if(NOT EXISTS "${_py_stamp}")
    if(NOT EXISTS "${_py_dir}/${_py_file}")
        message(STATUS "Downloading CPython ${JPLAY_PYTHON_VERSION} (${_py_arch})")
        file(DOWNLOAD "${_py_url}" "${_py_dir}/${_py_file}"
             EXPECTED_HASH "SHA256=${_py_sha}"
             STATUS _py_dl_status
             SHOW_PROGRESS)
        list(GET _py_dl_status 0 _py_dl_code)
        if(NOT _py_dl_code EQUAL 0)
            list(GET _py_dl_status 1 _py_dl_msg)
            file(REMOVE "${_py_dir}/${_py_file}")   # never leave a partial archive behind
            message(FATAL_ERROR "Failed to download ${_py_url}: ${_py_dl_msg}")
        endif()
    endif()

    message(STATUS "Extracting ${_py_file}")
    file(REMOVE_RECURSE "${JPLAY_PYTHON_ROOT}")
    file(ARCHIVE_EXTRACT INPUT "${_py_dir}/${_py_file}" DESTINATION "${_py_dir}")

    set(_py_pip ${CMAKE_COMMAND} -E env PYTHONNOUSERSITE=1
                "${_py_exe}" -m pip install --no-warn-script-location
                --disable-pip-version-check --no-warn-conflicts)

    # Build-only packages, kept out of the shipped tree: gen_icon_font.py needs
    # fontTools to subset the MDI webfont.
    execute_process(
        COMMAND ${_py_pip} --target "${JPLAY_PYTHON_BUILD_TOOLS}" fonttools
        RESULT_VARIABLE _py_pip_res)
    if(NOT _py_pip_res EQUAL 0)
        message(FATAL_ERROR "pip install fonttools failed (${_py_pip_res})")
    endif()

    # Trim what an embedded interpreter in a media player never uses. Tk in
    # particular is ~6 MB of shared libraries and its own script trees.
    foreach(_dead test tests idlelib tkinter turtledemo ensurepip lib2to3 pydoc_data)
        file(REMOVE_RECURSE "${_py_stdlib}/${_dead}")
    endforeach()
    if(WIN32)
        file(GLOB _dead_files "${JPLAY_PYTHON_ROOT}/DLLs/_tkinter*"
                              "${JPLAY_PYTHON_ROOT}/DLLs/tcl*.dll"
                              "${JPLAY_PYTHON_ROOT}/DLLs/tk*.dll")
        file(REMOVE ${_dead_files})
        file(REMOVE_RECURSE "${JPLAY_PYTHON_ROOT}/tcl")
    else()
        file(GLOB _dead_files "${JPLAY_PYTHON_ROOT}/lib/libtcl*" "${JPLAY_PYTHON_ROOT}/lib/libtk*"
                              "${_py_stdlib}/lib-dynload/_tkinter*")
        file(REMOVE ${_dead_files})
        file(GLOB _dead_dirs "${JPLAY_PYTHON_ROOT}/lib/tcl*" "${JPLAY_PYTHON_ROOT}/lib/tk*"
                             "${JPLAY_PYTHON_ROOT}/lib/itcl*" "${JPLAY_PYTHON_ROOT}/lib/thread*")
        foreach(_dead ${_dead_dirs})
            file(REMOVE_RECURSE "${_dead}")
        endforeach()
    endif()

    file(WRITE "${_py_stamp}" "${JPLAY_PYTHON_VERSION}+${JPLAY_PYTHON_RELEASE}\n")
endif()

# Point both find-module namespaces at the bundle before anyone searches:
# FindPython3 for our own target, and FindPython for pybind11, which resolves
# the interpreter itself when PYBIND11_FINDPYTHON is on. Without this pybind11
# would pick the system Python and jplay would link two interpreters.
set(Python3_ROOT_DIR "${JPLAY_PYTHON_ROOT}")
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_REGISTRY NEVER)
set(Python3_FIND_VIRTUALENV STANDARD)   # ignore any venv active in the shell
set(Python_ROOT_DIR "${JPLAY_PYTHON_ROOT}")
set(Python_FIND_STRATEGY LOCATION)
set(Python_FIND_REGISTRY NEVER)
set(Python_FIND_VIRTUALENV STANDARD)
set(PYBIND11_FINDPYTHON ON)
