# Blackmagic DeckLink SDK headers

Source:  Blackmagic Desktop Video SDK **12.0**, `Linux/include/`
Licence: unconditional Boost-style grant (see LICENSE.txt), carried per-file in
         the `-LICENSE-START-` block at the top of every file here. It permits
         "use, reproduce, display, distribute, execute, and transmit", and
         requires the notice be preserved only in source form -- binaries are
         explicitly carved out.

## Why 12.0 and not the current SDK

Blackmagic rewrote this grant in later SDKs. From roughly 15.x the same block
reads "... in accordance with" the DeckLink SDK EULA
(https://www.blackmagicdesign.com/EULA/DeckLinkSDK), and that EULA permits
sub-licensing only when the Software is distributed "in full ... all header,
source and documentation files" (clause 1.4) and "on terms substantially
similar to this Agreement", with clause 4.4 confining all reproduction and
distribution to those cases. A pruned header subset in a repository under
jplay's own licence does not satisfy that.

The 12.0 headers predate the change and carry the older unconditional grant --
the same vintage OBS Studio vendors. Building against them also *lowers* the
minimum Desktop Video driver users need, from 15.3 to 12.0.

## Forward compatibility with newer drivers

A binary built against 12.0 keeps working on current Desktop Video drivers.
DeckLink is COM: interfaces are versioned by IID, and the driver keeps serving
older IIDs to older binaries. Verified against 15.3:

  * `IDeckLinkOutput`, `IDeckLinkVideoFrame` and `IDeckLinkMutableVideoFrame`
    all changed layout, and all three have a *different* IID in 15.3. Asking
    with the 12.0 IID returns the 12.0 vtable.
  * `IDeckLink`, `IDeckLinkDisplayMode`, `IDeckLinkDisplayModeIterator` and
    `IDeckLinkIterator` are reached without a QueryInterface, so their layout
    has to match outright -- and it does, byte for byte, in both SDKs.
  * `IID_IDeckLinkIterator` and `IID_IDeckLinkAPIInformation` are unchanged,
    as are the dispatch entry points jplay uses
    (`CreateDeckLinkIteratorInstance_0004`,
    `CreateDeckLinkAPIInformationInstance_0001`).

What 12.0 costs is API added later, not compatibility. Two such additions are
used when present and detected in CMakeLists.txt from the header itself rather
than from a version number:

  * `JPLAY_DECKLINK_HAS_VIDEO_BUFFER` -- `IDeckLinkVideoBuffer` with
    StartAccess/EndAccess. Without it, `IDeckLinkVideoFrame::GetBytes` maps the
    frame directly, which is what 12.0 offers.
  * `JPLAY_DECKLINK_HAS_ROW_BYTES` -- `IDeckLinkOutput::RowBytesForPixelFormat`.
    Without it the packed `width * 4` stride is correct for the 8-bit BGRA
    jplay asks for.

So `DECKLINK_SDK_DIR=/path/to/current/SDK` still builds, and picks up both.

## What is vendored

The transitive `#include` closure of the three roots jplay uses --
`DeckLinkAPI.h`, `DeckLinkAPIDispatch.cpp` (compiled into jplay; it `dlopen`s
the driver's `libDeckLinkAPI.so`) and `DeckLinkAPIVersion.h` (parsed by CMake).
The SDK's further `*_v7_*` ... `*_v11_5_1` back-compatibility headers are
unreferenced and not vendored.

## Linux only

These are the Linux flavour, built around `LinuxCOM.h` and a `dlfcn` dispatch.
Windows DeckLink is real COM needing the SDK's `.idl` and a different dispatch
path, so DeckLink stays off on Windows regardless.
