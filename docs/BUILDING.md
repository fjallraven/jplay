# Building the player

CMake ≥ 3.21, a C++17 compiler, and [vcpkg](https://vcpkg.io) for the
dependencies. Windows and Linux x64 are the supported targets at the moment.

Dependencies come from `vcpkg.json` / `vcpkg-configuration.json`

OpenTimelineIO is not in vcpkg list (`custom-ports/`) plus (`custom-triplets/`) to deal with that.

## Get vcpkg

```sh
git clone https://github.com/microsoft/vcpkg
./vcpkg/bootstrap-vcpkg.sh      # .\vcpkg\bootstrap-vcpkg.bat on Windows
```

## Windows

```sh
cmake -S . -B build -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows-release
cmake --build build --config Release --parallel
```

## Linux

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=jplay-x64-linux
cmake --build build --parallel
```

## Installing

```sh
cmake --install build --config Release --prefix dist/jplay
```

This gathers the executable, the runtime libraries, the `python/` scripts, the
fonts and — with `JPLAY_BUNDLE_PYTHON` on — a trimmed Python home.


## Options

| Option | Default | Effect |
| --- | --- | --- |
| `JPLAY_BUNDLE_PYTHON` | `ON` | Ship a Python home with the install, and install fonttools for the icon-font build step. |
| `JPLAY_ENABLE_NDI` | `ON` | Build the NDI output backend. Headers are vendored; `NDI_SDK_DIR` overrides them. |
| `JPLAY_BUNDLE_NDI` | `OFF` | Ship the NDI runtime from `NDI_SDK_DIR` beside the binary. Off so releases stay clear of the NDI SDK EULA's redistribution obligations — users install the NDI Runtime themselves. |
| `JPLAY_ENABLE_DECKLINK` | `ON` | Build the DeckLink SDI backend. Linux only; needs `third_party/decklink/include` or `DECKLINK_SDK_DIR`. |
| `JPLAY_BUILD_TESTS` | `OFF` | Build the test and benchmark suite. |

## Tests

The naming and discovery layer is plain Python shipped as-is, so it is tested
without a build at all — the suite is stdlib-only and stubs the embedded `jplay`
module itself:

```sh
python tests/test_naming_conventions.py
python tests/test_discovery.py
```

Each suite is its own `main()` and returns non-zero on failure.

C++ tests and benchmarks are behind `-DJPLAY_BUILD_TESTS=ON`.
