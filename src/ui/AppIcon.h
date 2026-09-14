#pragma once

#include <cstdint>
#include <vector>

// Load the application icon embedded in the executable (resource id 1, the same
// icon Explorer and the taskbar use) as tightly-packed RGBA8 pixels, scaled to
// roughly `size` x `size`. Returns false if the icon can't be loaded.
//
// Isolated in its own translation unit so <windows.h> (and its min/max macros)
// stays out of App.cpp. Windows-only; a no-op stub elsewhere.
bool loadAppIconRGBA(int size, std::vector<uint8_t>& rgba, int& width, int& height);
