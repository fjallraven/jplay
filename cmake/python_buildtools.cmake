# fontTools is the one build-time Python dependency: tools/gen_icon_font.py uses
# it to subset materialdesignicons-webfont.ttf down to the icons the source
# actually references, and hard-exits without it.
#
# Must be included AFTER find_package(Python3), because it deliberately installs
# with the same interpreter jplay embeds. fontTools ships compiled accelerators,
# so a wheel built for a different minor version would either fail to load or
# quietly drop to the pure-Python path.
#
# --target keeps the package in the build tree instead of the interpreter's own
# site-packages: for a vcpkg-provided Python that directory lives inside
# vcpkg_installed/, which is discarded and restored wholesale from the binary
# cache and would lose the install.

set(JPLAY_PYTHON_BUILD_TOOLS "${CMAKE_BINARY_DIR}/_python/buildtools")

# Re-install whenever the interpreter changes minor version, since the wheel is
# ABI-specific; delete _python/ to force it otherwise.
set(_bt_stamp "${JPLAY_PYTHON_BUILD_TOOLS}/.jplay_fonttools_ready")
set(_bt_have "")
if(EXISTS "${_bt_stamp}")
    file(READ "${_bt_stamp}" _bt_have)
    string(STRIP "${_bt_have}" _bt_have)
endif()

if(NOT _bt_have STREQUAL "${Python3_VERSION}")
    # vcpkg's python3 tool ships no pip. Fall back to the wheel ensurepip
    # carries: a .whl is a zip with a top-level pip/ package, so naming it as a
    # directory on the command line puts it on sys.path and runs pip/__main__.py
    # -- without writing anything into the interpreter's own tree.
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import pip"
                    RESULT_VARIABLE _bt_import_pip
                    OUTPUT_QUIET ERROR_QUIET)
    if(_bt_import_pip EQUAL 0)
        set(_bt_pip "${Python3_EXECUTABLE}" -m pip)
    else()
        file(GLOB _bt_wheel "${Python3_STDLIB}/ensurepip/_bundled/pip-*.whl")
        if(NOT _bt_wheel)
            message(FATAL_ERROR
                "${Python3_EXECUTABLE} has neither pip nor a bundled ensurepip "
                "wheel, so fonttools cannot be installed for it. Configure with "
                "-DJPLAY_BUNDLE_PYTHON=OFF and pip install fonttools into the "
                "Python you want used instead.")
        endif()
        list(GET _bt_wheel 0 _bt_wheel)
        set(_bt_pip "${Python3_EXECUTABLE}" "${_bt_wheel}/pip")
    endif()

    message(STATUS "Installing fonttools for Python ${Python3_VERSION} (${Python3_EXECUTABLE})")
    file(REMOVE_RECURSE "${JPLAY_PYTHON_BUILD_TOOLS}")   # never mix ABIs
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env PYTHONNOUSERSITE=1
                ${_bt_pip} install --no-warn-script-location
                --disable-pip-version-check --no-warn-conflicts
                --target "${JPLAY_PYTHON_BUILD_TOOLS}" fonttools
        RESULT_VARIABLE _bt_res)
    if(NOT _bt_res EQUAL 0)
        message(FATAL_ERROR "pip install fonttools failed (${_bt_res})")
    endif()
    file(WRITE "${_bt_stamp}" "${Python3_VERSION}\n")
endif()
