# Release-only, dynamically-linked Linux build -- the counterpart of the builtin
# x64-windows-release triplet this project uses on Windows.
#
# Deliberately project-prefixed: vcpkg already ships x64-linux, x64-linux-dynamic
# and x64-linux-release (static), and an overlay triplet reusing one of those
# names would silently shadow the builtin.
#
# Dynamic linkage is deliberate: the top-level install() gathers the resulting
# .so files with RUNTIME_DEPENDENCY_SET and sets an $ORIGIN rpath, so the
# archive ships its own ffmpeg/OCIO/OpenEXR next to the executable.
#
# VCPKG_BUILD_TYPE=release skips the debug half of every dependency, which
# roughly halves a cold CI build.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Rewrite each shared library's rpath to $ORIGIN so the bundled set resolves
# against its own directory once installed beside the executable.
set(VCPKG_FIXUP_ELF_RPATH ON)
