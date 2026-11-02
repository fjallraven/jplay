# NDI SDK headers

Source:  NDI 6 SDK, `Include/`  --  https://ndi.video/
Licence: MIT, granted per-file (see LICENSE.txt). Every file here opens with
         its own notice: "The following MIT license applies to this file ONLY
         and not to the SDK as a whole."

Vizrt document this explicitly for open-source projects: the headers "may be
distributed with open-source projects under the terms of the MIT license" and
are intended to be paired with dynamic loading of the runtime.
  https://docs.ndi.video/all/developing-with-ndi/sdk/software-distribution
  https://docs.ndi.video/all/developing-with-ndi/sdk/dynamic-loading-of-ndi-libraries

## What is vendored

All 17 files: the complete `#include` closure of `Processing.NDI.Lib.h`.
Nothing else from the SDK -- no import library, no `Processing.NDI.Lib.x64.dll`,
no `libndi.so`. The MIT grant covers these headers only; the runtime binaries
are under the NDI SDK EULA and are not redistributed here.

## Runtime

`loadNdiLib()` in `src/OutputNDI.cpp` resolves the runtime at load time by the
`NDILIB_LIBRARY_NAME` these headers define, falling back to the
`NDI_RUNTIME_DIR_V6` environment variable that the SDK documents. Users install
the NDI Runtime themselves (http://ndi.link/NDIRedistV6); jplay ships nothing.

Bundling the runtime in a release is possible under the NDI SDK EULA but brings
version-currency, EULA pass-through and NDI trademark/visual-identification
obligations. It is off by default -- see `JPLAY_BUNDLE_NDI` in CMakeLists.txt.

## Refreshing

Copy `Include/` from the new SDK wholesale. Check that `NDILIB_LIBRARY_NAME`
and `NDILIB_REDIST_FOLDER` still match what `loadNdiLib()` expects if the major
version changes (v6 -> v7 renames `NDI_RUNTIME_DIR_V6`).
