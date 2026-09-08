cmake_minimum_required(VERSION 3.21)
project(JPlay VERSION 0.1.0 LANGUAGES CXX)
set(JPLAY_VERSION_STRING "${PROJECT_VERSION}-dev")

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

# Parallelize compilation of source files within each target (cl.exe /MP).
if(MSVC)
    add_compile_options(/MP)
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_ttf CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(OpenEXR CONFIG REQUIRED)
find_package(Imath CONFIG REQUIRED)
find_package(OpenColorIO CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(OpenTimelineIO CONFIG REQUIRED)

# OTIO's headers carry no version macro :(
set(JPLAY_OTIO_VERSION "unknown")
if(OpenTimelineIO_DIR AND EXISTS "${OpenTimelineIO_DIR}/vcpkg.spdx.json")
    file(READ "${OpenTimelineIO_DIR}/vcpkg.spdx.json" _otio_spdx)
    string(REPLACE "\n" " " _otio_spdx "${_otio_spdx}")
    string(REGEX MATCH "\"name\": \"opentimelineio\"[^}]*\"versionInfo\": \"([^\"]+)\"" _otio_match "${_otio_spdx}")
    if(CMAKE_MATCH_1)
        set(JPLAY_OTIO_VERSION "${CMAKE_MATCH_1}")
    endif()
    unset(_otio_spdx)
    unset(_otio_match)
endif()

find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
find_package(BZip2 REQUIRED)
find_package(freetype REQUIRED)
find_package(Threads REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)

if(WIN32)
    find_package(unofficial-shaderc CONFIG REQUIRED)
else()
    find_package(Shaderc CONFIG REQUIRED)
endif()


# NDI
if(NOT NDI_SDK_DIR AND DEFINED ENV{NDI_SDK_DIR})
    file(TO_CMAKE_PATH "$ENV{NDI_SDK_DIR}" NDI_SDK_DIR)
endif()

set(NDI_SDK_DIR "${NDI_SDK_DIR}" CACHE PATH "NDI SDK root")

set(JPLAY_ENABLE_NDI OFF)
if(NDI_SDK_DIR AND EXISTS "${NDI_SDK_DIR}/Include/Processing.NDI.Lib.h")
    set(NDI_INCLUDE_DIR "${NDI_SDK_DIR}/Include")
elseif(NDI_SDK_DIR AND EXISTS "${NDI_SDK_DIR}/include/Processing.NDI.Lib.h")
    set(NDI_INCLUDE_DIR "${NDI_SDK_DIR}/include")
endif()

if(NDI_INCLUDE_DIR)
    set(JPLAY_ENABLE_NDI ON)
    message(STATUS "NDI output enabled: ${NDI_SDK_DIR}")
else()
    message(STATUS "NDI output disabled (NDI SDK not found; set NDI_SDK_DIR to enable)")
endif()


# DeckLink SDI
if(NOT DECKLINK_SDK_DIR AND DEFINED ENV{DECKLINK_SDK_DIR})
    file(TO_CMAKE_PATH "$ENV{DECKLINK_SDK_DIR}" DECKLINK_SDK_DIR)
endif()
set(DECKLINK_SDK_DIR "${DECKLINK_SDK_DIR}" CACHE PATH "Blackmagic SDK root")
set(JPLAY_ENABLE_DECKLINK OFF)
if(NOT WIN32 AND DECKLINK_SDK_DIR AND
   EXISTS "${DECKLINK_SDK_DIR}/Linux/include/DeckLinkAPI.h")
    set(DECKLINK_INCLUDE_DIR "${DECKLINK_SDK_DIR}/Linux/include")
    set(JPLAY_ENABLE_DECKLINK ON)
    file(STRINGS "${DECKLINK_INCLUDE_DIR}/DeckLinkAPIVersion.h" _dl_ver_line
        REGEX "BLACKMAGIC_DECKLINK_API_VERSION_STRING")
    string(REGEX MATCH "\"([0-9.]+)\"" _dl_ver_m "${_dl_ver_line}")
    set(DECKLINK_SDK_VERSION "${CMAKE_MATCH_1}")
    message(STATUS "DeckLink output enabled: SDK ${DECKLINK_SDK_VERSION} (${DECKLINK_SDK_DIR})")
    message(STATUS "  requires a Desktop Video driver of ${DECKLINK_SDK_VERSION} or newer")
else()
    message(STATUS "DeckLink output disabled (SDK not found; set DECKLINK_SDK_DIR to enable)")
endif()

# Embedded MDI icon font: scan the source for ICON_MDI_* identifiers and subset
# materialdesignicons-webfont.ttf down to just those glyphs (the full font is
# ~1.3 MB / 7400 glyphs). The generated header lands in the build tree and is
# picked up by App.cpp's #include "icon_font_data.h". Needs fontTools, which the
# bundled interpreter brings along (or `pip install fonttools` into the system
# Python when configuring with -DJPLAY_BUNDLE_PYTHON=OFF).
option(JPLAY_BUNDLE_PYTHON
    "Download a pinned CPython and bundle it with the install (see cmake/bundle_python.cmake)" ON)
if(JPLAY_BUNDLE_PYTHON)
    include("${CMAKE_SOURCE_DIR}/cmake/bundle_python.cmake")
endif()
find_package(Python3 COMPONENTS Interpreter Development.Embed REQUIRED)
find_package(pybind11 CONFIG REQUIRED)
set(MDI_TTF     "${CMAKE_SOURCE_DIR}/materialdesignicons-webfont.ttf")
set(MDI_DEFINES "${CMAKE_SOURCE_DIR}/src/IconsMaterialDesignIcons.h")
set(MDI_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(MDI_HEADER  "${MDI_GEN_DIR}/icon_font_data.h")
set(MDI_SCRIPT  "${CMAKE_SOURCE_DIR}/tools/gen_icon_font.py")
if(WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows-release")
endif()

if(WIN32)
    list(GET FFMPEG_INCLUDE_DIRS 0 _ffmpeg_include)
    cmake_path(GET _ffmpeg_include PARENT_PATH _vcpkg_dir)
    file(GLOB VCPKG_EXTRA_DLLS
        "${_vcpkg_dir}/bin/avcodec-*.dll"
        "${_vcpkg_dir}/bin/avformat-*.dll"
        "${_vcpkg_dir}/bin/avutil-*.dll"
        "${_vcpkg_dir}/bin/swscale-*.dll"
        "${_vcpkg_dir}/bin/swresample-*.dll"
        "${_vcpkg_dir}/bin/z.dll"
        "${_vcpkg_dir}/bin/zlib*.dll"
        "${_vcpkg_dir}/bin/bz2.dll"
        "${_vcpkg_dir}/bin/libpng16.dll"
        "${_vcpkg_dir}/bin/jpeg62.dll"
        "${_vcpkg_dir}/bin/tiff.dll"
        "${_vcpkg_dir}/bin/brotlidec.dll"
        "${_vcpkg_dir}/bin/brotlicommon.dll"
        "${_vcpkg_dir}/bin/liblzma*.dll"
        "${_vcpkg_dir}/bin/libexpat*.dll"
        "${_vcpkg_dir}/bin/yaml-cpp*.dll"
        "${_vcpkg_dir}/bin/zstd*.dll"
    )
endif()

file(GLOB MDI_SCAN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.h")
list(REMOVE_ITEM MDI_SCAN_SOURCES "${MDI_DEFINES}")

# fontTools is a build-only dependency
if(JPLAY_BUNDLE_PYTHON)
    set(MDI_PYTHON_ENV ${CMAKE_COMMAND} -E env "PYTHONPATH=${JPLAY_PYTHON_BUILD_TOOLS}")
endif()

add_custom_command(
    OUTPUT "${MDI_HEADER}"
    COMMAND ${MDI_PYTHON_ENV} ${Python3_EXECUTABLE} "${MDI_SCRIPT}"
            --ttf "${MDI_TTF}" --defines "${MDI_DEFINES}" --out "${MDI_HEADER}"
            ${MDI_SCAN_SOURCES}
    DEPENDS "${MDI_SCRIPT}" "${MDI_TTF}" "${MDI_DEFINES}" ${MDI_SCAN_SOURCES}
    COMMENT "Subsetting MDI webfont to the icons referenced in source"
    VERBATIM)

# version: git hash + commit date written into a generated header
set(JPLAY_VERSION_HEADER "${MDI_GEN_DIR}/Version.h")
add_custom_target(jplay_version
    COMMAND ${CMAKE_COMMAND}
            -DJPLAY_SRC_DIR=${CMAKE_SOURCE_DIR}
            -DJPLAY_VERSION=${JPLAY_VERSION_STRING}
            -DJPLAY_OUT=${JPLAY_VERSION_HEADER}
            -P "${CMAKE_SOURCE_DIR}/cmake/version.cmake"
    BYPRODUCTS "${JPLAY_VERSION_HEADER}"
    COMMENT "Stamping build version"
    VERBATIM)

if(WIN32)
    set(JPLAY_ICON_PATH "${CMAKE_SOURCE_DIR}/jplay_icon.ico")
    configure_file(${CMAKE_SOURCE_DIR}/src/jplay.rc.in
                   ${CMAKE_BINARY_DIR}/jplay.rc @ONLY)
    set(JPLAY_RC ${CMAKE_BINARY_DIR}/jplay.rc)
endif()

add_executable(jplay WIN32
    src/main.cpp
    src/App.cpp
    src/App_Timeline.cpp
    src/App_TimelineClip.cpp
    src/App_CurveLane.cpp
    src/App_Player.cpp
    src/App_PixelInspector.cpp
    src/App_ProjectExplorer.cpp
    src/App_NavPanel.cpp
    src/App_Project.cpp
    src/App_Overview.cpp
    src/App_Layout.cpp
    src/App_Inspector.cpp
    src/App_Grade.cpp
    src/App_Draw.cpp
    src/App_TopBar.cpp
    src/App_Letterbox.cpp
    src/App_Ocio.cpp
    src/App_ProxyMenu.cpp
    src/App_SequenceMenu.cpp
    src/App_TechCheck.cpp
    src/App_ClipSource.cpp
    src/App_Settings.cpp
    src/App_Review.cpp
    src/App_Sync.cpp
    src/App_Control.cpp
    src/ControlServer.cpp
    src/App_Help.cpp
    src/App_About.cpp
    src/AppIcon.cpp
    src/RevealFile.cpp
    src/IconFont.cpp
    src/TextFont.cpp
    src/ThumbnailCache.cpp
    src/Menu.cpp
    src/Dropdown.cpp
    src/ContextMenu.cpp
    src/TitleBar.cpp
    src/VideoSource.cpp
    src/AudioSource.cpp
    src/AudioEngine.cpp
    src/WaveformCache.cpp
    src/ExrSource.cpp
    src/StillSource.cpp
    src/ImageSeq.cpp
    src/OcioManager.cpp
    src/OcioGpu.cpp
    src/GradeGpu.cpp
    src/HdrColorPass.cpp
    src/Output.cpp
    src/OutputNDI.cpp
    src/OutputDeckLink.cpp
    src/Media.cpp
    src/ProxyMode.cpp
    src/MediaScan.cpp
    src/FrameCache.cpp
    src/Project.cpp
    src/SharedProjects.cpp
    src/SyncSession.cpp
    src/OtioImport.cpp
    src/OtioExport.cpp
    src/Widgets.cpp
    src/ExportDialog.cpp
    src/UserData.cpp
    src/Preferences.cpp
    src/PythonStartup.cpp
    src/PythonBridge.cpp
    src/WorkQueue.cpp
    ${MDI_HEADER}
    ${JPLAY_RC}
)

get_filename_component(JPLAY_DEV_PYTHONHOME "${Python3_EXECUTABLE}" DIRECTORY)
if(WIN32)
    set(JPLAY_PYTHON_STDLIB_REL "Lib")
else()
    set(JPLAY_PYTHON_STDLIB_REL "lib/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")
endif()
target_compile_definitions(jplay PRIVATE
    JPLAY_DEV_PYTHONHOME="${JPLAY_DEV_PYTHONHOME}"
    JPLAY_PYTHON_BUNDLE_NAME="pyhome"
    JPLAY_PYTHON_STDLIB_REL="${JPLAY_PYTHON_STDLIB_REL}")

add_dependencies(jplay jplay_version) # Version.h before App_About.cpp compiles

target_compile_definitions(jplay PRIVATE
    JPLAY_BUILD_CONFIG="$<CONFIG>"
    JPLAY_BUILD_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    JPLAY_OTIO_VERSION="${JPLAY_OTIO_VERSION}")

target_include_directories(jplay PRIVATE ${FFMPEG_INCLUDE_DIRS} ${MDI_GEN_DIR} ${SHADERC_INCLUDE_DIR}
    ${RAPIDJSON_INCLUDE_DIRS})
target_link_directories(jplay PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(jplay PRIVATE    
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    OpenEXR::OpenEXR
    Imath::Imath
    OpenColorIO::OpenColorIO
    OTIO::opentimelineio
    pybind11::embed
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)

if(WIN32)
    # windows socket libraries
    target_link_libraries(jplay PRIVATE ws2_32 iphlpapi)
endif()

# shaderc
if(WIN32)
    target_link_libraries(jplay PRIVATE
        unofficial::shaderc::shaderc
    )
else()
    target_link_libraries(jplay PRIVATE
        ${SHADERC_LIBRARIES}
    )
endif()


# NDI
if(JPLAY_ENABLE_NDI)
    target_include_directories(jplay PRIVATE "${NDI_INCLUDE_DIR}")
    target_compile_definitions(jplay PRIVATE JPLAY_ENABLE_NDI)
    if(WIN32)
        target_compile_definitions(jplay PRIVATE
            JPLAY_NDI_BIN_DIR="${NDI_SDK_DIR}/Bin/x64")
    else()
        target_compile_definitions(jplay PRIVATE
            JPLAY_NDI_BIN_DIR="${NDI_SDK_DIR}/lib/x86_64-linux-gnu")
    endif()
endif()

# DeckLink SDI
if(JPLAY_ENABLE_DECKLINK)
    target_include_directories(jplay PRIVATE "${DECKLINK_INCLUDE_DIR}")
    target_compile_definitions(jplay PRIVATE JPLAY_ENABLE_DECKLINK
        JPLAY_DECKLINK_SDK_VERSION="${DECKLINK_SDK_VERSION}")
    target_sources(jplay PRIVATE "${DECKLINK_INCLUDE_DIR}/DeckLinkAPIDispatch.cpp")
    target_link_libraries(jplay PRIVATE ${CMAKE_DL_LIBS})
    set_source_files_properties("${DECKLINK_INCLUDE_DIR}/DeckLinkAPIDispatch.cpp"
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(MSVC)
    target_compile_options(jplay PRIVATE -D_CRT_SECURE_NO_WARNINGS /W3 /utf-8 /O2 /Oi /Ot /GL)    
    target_link_options(jplay PRIVATE /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO)
else()
    target_compile_options(jplay PRIVATE -Wall -O2 -ffunction-sections -fdata-sections -flto)
    target_link_options(jplay PRIVATE -Wl,--gc-sections -flto)
endif()

add_custom_command(TARGET jplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/Inter-Regular.ttf"
        "$<TARGET_FILE_DIR:jplay>/Inter-Regular.ttf")

add_custom_command(TARGET jplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/jplay_preferences.conf"
        "$<TARGET_FILE_DIR:jplay>/jplay_preferences.conf")

if(NOT WIN32)
    add_custom_command(TARGET jplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/jplay_icon.png"
            "$<TARGET_FILE_DIR:jplay>/jplay_icon.png")
endif()

if(WIN32)
    add_custom_command(TARGET jplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:jplay>
            $<TARGET_RUNTIME_DLLS:jplay>
        COMMAND_EXPAND_LISTS)
    if(VCPKG_EXTRA_DLLS)
        add_custom_command(TARGET jplay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${VCPKG_EXTRA_DLLS} $<TARGET_FILE_DIR:jplay>)
    endif()

    if(JPLAY_ENABLE_NDI AND EXISTS "${NDI_SDK_DIR}/Bin/x64/Processing.NDI.Lib.x64.dll")
        add_custom_command(TARGET jplay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${NDI_SDK_DIR}/Bin/x64/Processing.NDI.Lib.x64.dll"
                "$<TARGET_FILE_DIR:jplay>/Processing.NDI.Lib.x64.dll")
    endif()
endif()

if(WIN32 OR JPLAY_BUNDLE_PYTHON)
    add_custom_command(TARGET jplay POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D "SRC=${Python3_STDLIB}"
            -D "DST=$<TARGET_FILE_DIR:jplay>/pyhome/${JPLAY_PYTHON_STDLIB_REL}"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_dir_if_missing.cmake")
endif()
if(WIN32 AND EXISTS "${JPLAY_DEV_PYTHONHOME}/DLLs")
    add_custom_command(TARGET jplay POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D "SRC=${JPLAY_DEV_PYTHONHOME}/DLLs"
            -D "DST=$<TARGET_FILE_DIR:jplay>/pyhome/DLLs"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_dir_if_missing.cmake")
endif()

if(WIN32 AND JPLAY_BUNDLE_PYTHON)
    file(GLOB JPLAY_PYTHON_DLLS "${JPLAY_PYTHON_ROOT}/python*.dll"
                                "${JPLAY_PYTHON_ROOT}/vcruntime*.dll")
    add_custom_command(TARGET jplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${JPLAY_PYTHON_DLLS} "$<TARGET_FILE_DIR:jplay>")
endif()

add_custom_command(TARGET jplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/python" "$<TARGET_FILE_DIR:jplay>/python")

set(JPLAY_INSTALL_DIR ".")
set(JPLAY_INSTALL_TARGETS jplay)

if(WIN32)
    install(TARGETS ${JPLAY_INSTALL_TARGETS}
        RUNTIME DESTINATION "${JPLAY_INSTALL_DIR}")

    install(FILES $<TARGET_RUNTIME_DLLS:jplay> DESTINATION "${JPLAY_INSTALL_DIR}")
    if(VCPKG_EXTRA_DLLS)
        install(FILES ${VCPKG_EXTRA_DLLS} DESTINATION "${JPLAY_INSTALL_DIR}")
    endif()
    if(JPLAY_ENABLE_NDI AND EXISTS "${NDI_SDK_DIR}/Bin/x64/Processing.NDI.Lib.x64.dll")
        install(FILES "${NDI_SDK_DIR}/Bin/x64/Processing.NDI.Lib.x64.dll"
            DESTINATION "${JPLAY_INSTALL_DIR}")
    endif()
    if(EXISTS "${JPLAY_DEV_PYTHONHOME}/DLLs")
        install(DIRECTORY "${JPLAY_DEV_PYTHONHOME}/DLLs/"
            DESTINATION "${JPLAY_INSTALL_DIR}/pyhome/DLLs")
    endif()
else()
    set_target_properties(${JPLAY_INSTALL_TARGETS} PROPERTIES
        INSTALL_RPATH "$ORIGIN")
    install(TARGETS ${JPLAY_INSTALL_TARGETS}
        RUNTIME_DEPENDENCY_SET jplay_deps
        RUNTIME DESTINATION "${JPLAY_INSTALL_DIR}")

    set(JPLAY_DEP_DIRS ${FFMPEG_LIBRARY_DIRS})
    if(JPLAY_BUNDLE_PYTHON)
        list(APPEND JPLAY_DEP_DIRS "${JPLAY_PYTHON_ROOT}/lib")
    endif()
    if(SDL3_DIR)
        cmake_path(GET SDL3_DIR PARENT_PATH _p)        # .../<libdir>/cmake
        cmake_path(GET _p PARENT_PATH _libdir)         # .../<libdir>
        cmake_path(GET _libdir PARENT_PATH _root)      # deps prefix root
        list(APPEND JPLAY_DEP_DIRS "${_libdir}" "${_root}/lib" "${_root}/lib64")
    endif()
    list(REMOVE_DUPLICATES JPLAY_DEP_DIRS)

    install(RUNTIME_DEPENDENCY_SET jplay_deps
        DIRECTORIES ${JPLAY_DEP_DIRS}
        PRE_EXCLUDE_REGEXES
            "^ld-linux.*" "^libc\\.so" "^libm\\.so" "^libdl\\.so"
            "^librt\\.so" "^libpthread\\.so" "^libgcc_s\\.so"
            "^libstdc\\+\\+\\.so"
        POST_EXCLUDE_REGEXES
            "^/lib" "^/lib64" "^/usr/lib" "^/usr/lib64"
        LIBRARY DESTINATION "${JPLAY_INSTALL_DIR}")

    if(JPLAY_ENABLE_NDI AND EXISTS "${NDI_SDK_DIR}/lib/x86_64-linux-gnu/libndi.so.6")
        file(REAL_PATH "${NDI_SDK_DIR}/lib/x86_64-linux-gnu/libndi.so.6" _ndi_so_real)
        install(FILES "${_ndi_so_real}" DESTINATION "${JPLAY_INSTALL_DIR}"
            RENAME "libndi.so.6")
    endif()
endif()

if(WIN32 OR JPLAY_BUNDLE_PYTHON)
    install(DIRECTORY "${Python3_STDLIB}/"
        DESTINATION "${JPLAY_INSTALL_DIR}/pyhome/${JPLAY_PYTHON_STDLIB_REL}")
endif()
if(JPLAY_BUNDLE_PYTHON)
    if(WIN32)
        install(FILES ${JPLAY_PYTHON_DLLS} DESTINATION "${JPLAY_INSTALL_DIR}")
    else()
        file(GLOB JPLAY_LIBPYTHON "${JPLAY_PYTHON_ROOT}/lib/libpython*.so.*")
        install(FILES ${JPLAY_LIBPYTHON} DESTINATION "${JPLAY_INSTALL_DIR}")
    endif()
endif()


install(DIRECTORY "${CMAKE_SOURCE_DIR}/python/"
    DESTINATION "${JPLAY_INSTALL_DIR}/python"
    PATTERN "test_*.py" EXCLUDE
    PATTERN "__pycache__" EXCLUDE)


install(FILES "${CMAKE_SOURCE_DIR}/Inter-Regular.ttf" DESTINATION "${JPLAY_INSTALL_DIR}")
install(FILES "${CMAKE_SOURCE_DIR}/jplay_preferences.conf" DESTINATION "${JPLAY_INSTALL_DIR}")
if(NOT WIN32)
    install(FILES "${CMAKE_SOURCE_DIR}/jplay_icon.png" DESTINATION "${JPLAY_INSTALL_DIR}")
endif()
