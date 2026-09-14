#include "AppIcon.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

bool loadAppIconRGBA(int size, std::vector<uint8_t>& rgba, int& width, int& height) {
    // Pull resource id 1 (see jplay.rc) at the requested size; LoadImage picks
    // the best-matching frame from the .ico and scales it for us.
    HMODULE mod = GetModuleHandleW(nullptr);
    HICON icon = (HICON)LoadImageW(mod, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size,
                                   LR_DEFAULTCOLOR);
    if (!icon)
        return false;

    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) {
        DestroyIcon(icon);
        return false;
    }
    BITMAP bm{};
    GetObject(ii.hbmColor, sizeof(bm), &bm);
    const int w = bm.bmWidth, h = bm.bmHeight;

    // Pull the color bitmap out as top-down 32-bit BGRA.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // negative => top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgra((size_t)w * h * 4);
    HDC dc = GetDC(nullptr);
    int rows = GetDIBits(dc, ii.hbmColor, 0, h, bgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    DestroyIcon(icon);
    if (rows == 0)
        return false;

    // BGRA -> RGBA. If the icon carries no alpha (older formats store it only in
    // the mask), treat it as fully opaque so it isn't rendered invisible.
    rgba.resize((size_t)w * h * 4);
    bool anyAlpha = false;
    const size_t px = (size_t)w * h;
    for (size_t i = 0; i < px; ++i) {
        const uint8_t b = bgra[i * 4 + 0], g = bgra[i * 4 + 1];
        const uint8_t r = bgra[i * 4 + 2], a = bgra[i * 4 + 3];
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = a;
        anyAlpha = anyAlpha || a != 0;
    }
    if (!anyAlpha)
        for (size_t i = 0; i < px; ++i)
            rgba[i * 4 + 3] = 255;

    width = w;
    height = h;
    return true;
}

#else // non-Windows: decode the bundled jplay_icon.png (see CMake POST_BUILD).

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <cstring>
#include <string>

bool loadAppIconRGBA(int size, std::vector<uint8_t>& rgba, int& width, int& height) {
    if (size <= 0)
        return false;

    const char* base = SDL_GetBasePath();
    std::string path = std::string(base ? base : "") + "jplay_icon.png";

    SDL_Surface* loaded = IMG_Load(path.c_str());
    if (!loaded)
        return false;

    // Scale to size x size, then normalize to tightly-packed RGBA so callers get
    // a predictable layout (both the title-bar texture and the window icon).
    SDL_Surface* scaled = SDL_ScaleSurface(loaded, size, size, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(loaded);
    if (!scaled)
        return false;

    SDL_Surface* surf = (scaled->format == SDL_PIXELFORMAT_RGBA32)
                            ? scaled
                            : SDL_ConvertSurface(scaled, SDL_PIXELFORMAT_RGBA32);
    if (surf != scaled)
        SDL_DestroySurface(scaled);
    if (!surf)
        return false;

    // Copy row by row: SDL_Surface::pitch may exceed width*4 due to row padding.
    rgba.resize((size_t)size * size * 4);
    const uint8_t* src = static_cast<const uint8_t*>(surf->pixels);
    for (int y = 0; y < size; ++y)
        std::memcpy(rgba.data() + (size_t)y * size * 4,
                    src + (size_t)y * surf->pitch,
                    (size_t)size * 4);
    SDL_DestroySurface(surf);

    width = size;
    height = size;
    return true;
}

#endif
