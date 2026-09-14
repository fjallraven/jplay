# Vendored SDK headers

jplay's NDI and DeckLink output backends bind to their runtimes at load time
rather than at link time, so building them needs only headers -- no import
libraries, no vendor binaries. Keeping those headers here lets a clean checkout,
and CI in particular, build the backends without a manual SDK download behind a
click-through form.

Nothing in this directory ships in a jplay release. Both runtimes come from
whatever the user installed:

  * NDI      -- the NDI Runtime, located by `Processing.NDI.Lib.x64.dll` /
                `libndi.so.6` on the loader path, else `NDI_RUNTIME_DIR_V6`.
                See `loadNdiLib()` in `src/OutputNDI.cpp`.
  * DeckLink -- the Blackmagic Desktop Video driver, `libDeckLinkAPI.so`.
                See `src/OutputDeckLink.cpp`.

When neither is present the corresponding backend reports itself unavailable
and the rest of the player is unaffected.

## Licences

Both grants are per-file, carried in the files themselves. Neither extends to
the wider SDK package it came from, and neither covers the runtime binaries.

  * `ndi/`      NDI 6 SDK headers under a per-file MIT grant. Vizrt document
                redistribution with open-source projects explicitly.

  * `decklink/` Desktop Video SDK **12.0** headers, under the older
                unconditional Boost-style grant. Deliberately not the current
                SDK: from ~15.x the grant is conditioned on the DeckLink SDK
                EULA, which does not permit redistributing a header subset
                under our own terms. See `decklink/README.md` -- do not
                refresh these from a newer SDK without reading it.

Building against 12.0 does not limit which drivers jplay works with: DeckLink
versions its COM interfaces by IID, so an old binary keeps getting old vtables
from current drivers. `decklink/README.md` records how that was verified.

## Building against a full SDK instead

Set `NDI_SDK_DIR` or `DECKLINK_SDK_DIR` (CMake cache variable or environment
variable) to an installed SDK root. That takes precedence over anything here,
and CMake fails loudly rather than silently falling back if the path is wrong.
