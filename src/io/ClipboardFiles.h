#pragma once

#include <string>
#include <utility>
#include <vector>

// Paths (UTF-8) the system clipboard currently names, in clipboard order: files
// copied in the file manager (Ctrl+C in Explorer puts a file list, not text, on
// the clipboard), else text holding one path per line — quoted ("Copy as path"),
// or as file:// URLs. Only entries that exist on disk are returned; anything else
// on the clipboard (prose, a URL, nothing at all) yields an empty list.
//
// Isolated in its own translation unit so <windows.h> (and its min/max macros)
// stays out of the rest of the build, same as RevealFile.
std::vector<std::string> clipboardFilePaths();

// Every text flavour on the clipboard as (MIME type, UTF-8 contents): the plain
// text under "text/plain", then each other text/* type the source put up (a
// browser copy carries "text/html"). For the resolve_clipboard Python callback,
// which recognises copied nodes (Nuke, ComfyUI) that clipboardFilePaths cannot.
std::vector<std::pair<std::string, std::string>> clipboardTextContent();
