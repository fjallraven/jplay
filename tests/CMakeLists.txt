# Test and benchmark suite. Built only under -DJPLAY_BUILD_TESTS=ON; nothing here
# is installed.
#
#   cachebench          Benchmark of the FrameCache hot paths. Links the
#                       media stack directly rather than the jplay target, which
#                       is a GUI executable with a main() of its own. Takes an
#                       EXR sequence path as argv[1]; without one it still runs
#                       the synthetic halves and reports that it found no media.

add_executable(cachebench
    cachebench.cpp
    ../src/FrameCache.cpp
    ../src/Media.cpp
    ../src/VideoSource.cpp
    ../src/AudioSource.cpp
    ../src/ImageSeq.cpp
    ../src/ExrSource.cpp
    ../src/StillSource.cpp
    ../src/ProxyMode.cpp
)

target_include_directories(cachebench PRIVATE ../src ${FFMPEG_INCLUDE_DIRS})
target_link_directories(cachebench PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(cachebench PRIVATE
    SDL3::SDL3
    SDL3_image::SDL3_image
    OpenEXR::OpenEXR
    Imath::Imath
    ${FFMPEG_LIBRARIES}
    Threads::Threads
)

if(MSVC)
    target_compile_options(cachebench PRIVATE -D_CRT_SECURE_NO_WARNINGS /W3 /utf-8 /O2)
else()
    target_compile_options(cachebench PRIVATE -Wall -O2)
endif()

if(WIN32)
    add_custom_command(TARGET cachebench POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:cachebench>
            $<TARGET_RUNTIME_DLLS:cachebench>
        COMMAND_EXPAND_LISTS)
    if(VCPKG_EXTRA_DLLS)
        add_custom_command(TARGET cachebench POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${VCPKG_EXTRA_DLLS} $<TARGET_FILE_DIR:cachebench>)
    endif()
endif()
