vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO AcademySoftwareFoundation/OpenTimelineIO
    REF v0.16.0
    SHA512 c9bfe4c2bbed034beaf04e94977adecc622f6c22f0674cecd959d0aa4df48f78c78fd2e1c0fc59a0b35db5915188640e4a2c0201338ebdd7e57ee1b5467a8a61
    HEAD_REF main
)
if(NOT EXISTS "${SOURCE_PATH}/src/deps/rapidjson/include/rapidjson")
    message(STATUS "Injecting external RapidJSON headers into OTIO submodule tree...")
    file(COPY "D:/sdk/vcpkg/installed/x64-windows-release/include/rapidjson" DESTINATION "${SOURCE_PATH}/src/deps/rapidjson/include")
endif()
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DOTIO_PYTHON_INSTALL=OFF
        -DOTIO_AUTOMATIC_SUBMODULES=OFF
        -DOTIO_FIND_IMATH=ON
        -DOTIO_FIND_RAPIDJSON=ON
        -DUSE_DEPS_IMATH=OFF
        -DUSE_DEPS_RAPIDJSON=OFF
        -DOTIO_DEPENDENCIES_INSTALL=OFF
        -DOTIO_CXX_EXAMPLES=OFF
)

vcpkg_cmake_install()
# OTIO installs two separate CMake config packages: OpenTime (OTIO::opentime) and
# OpenTimelineIO (OTIO::opentimelineio). Both must be fixed up, otherwise the
# un-fixed one keeps its debug/release configs split across debug/share + share
# and CMake resolves the debug-only config — which has no Release config for our
# Release build to map onto. DO_NOT_DELETE_PARENT_CONFIG_PATH on all but
# the last call preserves debug/share until the final fixup cleans it up.
vcpkg_cmake_config_fixup(PACKAGE_NAME opentime DO_NOT_DELETE_PARENT_CONFIG_PATH)
vcpkg_cmake_config_fixup(PACKAGE_NAME opentimelineio)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")