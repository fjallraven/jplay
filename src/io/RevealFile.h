#pragma once

#include <string>

// Open the system file browser with `path` (UTF-8) revealed — selected in its
// containing folder where the platform supports it, otherwise just the folder.
// Returns false if no file manager could be launched.
//
// Isolated in its own translation unit so <windows.h> (and its min/max macros)
// stays out of the rest of the build, same as AppIcon.
bool revealInFileManager(const std::string& path);
