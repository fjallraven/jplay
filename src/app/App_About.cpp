// About translation unit: the version / build-info overlay panel. Sits beside the
// keyboard-shortcuts panel in App_Help.cpp and follows it exactly — a centered
// overlay drawn last, dismissed by any click (see the MOUSE_BUTTON_DOWN handler
// in App.cpp). The Help menu item that opens it lives in App.cpp.
//
// Library versions are asked of the libraries at run time wherever they expose a
// query, so the panel reports what is actually loaded rather than what we built
// against — on Linux those genuinely differ. Only the versions with no runtime
// query come from elsewhere: OpenEXR and Imath from their headers, OpenTimelineIO
// (whose headers carry no version macro) from vcpkg's SPDX record via cmake — and
// only the facts no library knows (git stamp, build config, compiler) come from cmake.

#include <Python.h> // before the standard headers, per CPython's own rule

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "Version.h" // generated: JPLAY_VERSION / JPLAY_GIT_HASH / JPLAY_GIT_DATE

#include <SDL3/SDL_platform.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <Imath/ImathConfig.h>
#include <OpenColorIO/OpenColorIO.h>
#include <OpenEXR/OpenEXRConfig.h>

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace jplay;

namespace {

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

// SDL's libraries all report a packed int; SDL_ttf and SDL_image use SDL's own
// unpacking macros for it.
std::string sdlVersion(int v) {
    return std::to_string(SDL_VERSIONNUM_MAJOR(v)) + "." +
           std::to_string(SDL_VERSIONNUM_MINOR(v)) + "." +
           std::to_string(SDL_VERSIONNUM_MICRO(v));
}

std::string triple(int major, int minor, int patch) {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

} // namespace

void App::renderAboutPanel() {
    if (!aboutOpen_)
        return;

    struct Row { std::string label; std::string value; };
    struct Section { const char* title; std::vector<Row> rows; };

    std::vector<Section> leftSections;
    leftSections.push_back({ "Build", {
        { "Version",  JPLAY_VERSION },
        { "Commit",   JPLAY_GIT_HASH },
        { "Date",     JPLAY_GIT_DATE },
        { "Config",   JPLAY_BUILD_CONFIG },
        { "Compiler", JPLAY_BUILD_COMPILER },
        { "Platform", SDL_GetPlatform() },
    }});

    // Output backends are compiled in but load their runtime dynamically, so what
    // matters here is whether that runtime was found on this machine.
    std::vector<Row> outputRows;
    for (const OutputBackendInfo& b : output_.backends()) {
        std::string value = b.available ? "available" : "runtime not found";
#ifdef JPLAY_DECKLINK_SDK_VERSION
        if (b.id == "decklink")
            value += "  (SDK " JPLAY_DECKLINK_SDK_VERSION ")";
#endif
        outputRows.push_back({ b.label, value });
    }
    if (!outputRows.empty())
        leftSections.push_back({ "Output", std::move(outputRows) });

    int ftMajor = 0, ftMinor = 0, ftPatch = 0;
    TTF_GetFreeTypeVersion(&ftMajor, &ftMinor, &ftPatch);
    int hbMajor = 0, hbMinor = 0, hbPatch = 0;
    TTF_GetHarfBuzzVersion(&hbMajor, &hbMinor, &hbPatch);

    // Py_GetVersion() appends the build banner ("3.11.9 (main, ...)"); keep the
    // number. Safe before Py_Initialize, so it does not depend on startup order.
    std::string python(Py_GetVersion());
    python = python.substr(0, python.find(' '));

    std::vector<Section> rightSections;
    rightSections.push_back({ "Libraries", {
        { "SDL",            sdlVersion(SDL_GetVersion()) },
        { "SDL_ttf",        sdlVersion(TTF_Version()) },
        { "SDL_image",      sdlVersion(IMG_Version()) },
        { "FreeType",       triple(ftMajor, ftMinor, ftPatch) },
        { "HarfBuzz",       triple(hbMajor, hbMinor, hbPatch) },
        { "FFmpeg",         av_version_info() },
        { "OpenColorIO",    OCIO_NAMESPACE::GetVersion() },
        { "OpenEXR",        OPENEXR_VERSION_STRING },
        { "Imath",          IMATH_VERSION_STRING },
        { "OpenTimelineIO", JPLAY_OTIO_VERSION },
        { "Python",         python },
    }});

    const std::vector<Section>* columns[] = { &leftSections, &rightSections };
    const int nColumns = (int)(sizeof(columns) / sizeof(columns[0]));

    const float pad = 20.0f * dpiScale;
    const float lh = textFont_.lineHeight();
    const float rowH = lh + 6.0f * dpiScale;
    const float colGap = 24.0f * dpiScale;     // between a column's label and value
    const float sectionGap = 40.0f * dpiScale; // between the two side-by-side columns
    const float headerGap = 14.0f * dpiScale;  // extra space above a section header
    const char* title = "jplay";

    // Each column is sized from its own widest label / value, across all its
    // sections — the same measure-then-lay-out pass as the shortcuts panel.
    float colLabelW[nColumns] = { 0.0f, 0.0f };
    float colValueW[nColumns] = { 0.0f, 0.0f };
    for (int ci = 0; ci < nColumns; ++ci)
        for (const Section& s : *columns[ci])
            for (const Row& r : s.rows) {
                colLabelW[ci] = std::max(colLabelW[ci], textFont_.measure(renderer_, r.label.c_str()));
                colValueW[ci] = std::max(colValueW[ci], textFont_.measure(renderer_, r.value.c_str()));
            }
    float colW[nColumns];
    float bodyW = 0.0f;
    float tallestColH = 0.0f;
    for (int ci = 0; ci < nColumns; ++ci) {
        colW[ci] = colLabelW[ci] + colGap + colValueW[ci];
        bodyW += colW[ci] + (ci ? sectionGap : 0.0f);
        float colH = 0.0f;
        for (const Section& s : *columns[ci])
            colH += headerGap + rowH + s.rows.size() * rowH;
        tallestColH = std::max(tallestColH, colH);
    }

    // Title lockup: the app icon then the name. iconTex_ is the title bar's
    // texture, decoded at twice its 18px slot, so a square the height of the
    // title line is still sampling down from it and stays crisp. Null when the
    // icon failed to load, and the lockup is then just the name.
    const float titleLh = headerFont_.lineHeight();
    const float iconSz = iconTex_ ? std::round(titleLh) : 0.0f;
    const float iconGap = iconTex_ ? 10.0f * dpiScale : 0.0f;
    float titleW = headerFont_.measure(renderer_, title);
    float lockupW = iconSz + iconGap + titleW;
    float panelW = std::max(bodyW, lockupW) + pad * 2.0f;
    float panelH = pad * 2.0f + titleLh + 12.0f * dpiScale // title + gap
                 + tallestColH;

    // Dim the window behind the panel.
    SDL_FRect full = { 0.0f, 0.0f, winW_, winH_ };
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 150);
    jplay::fillRect(renderer_, &full);

    // Panel background + border.
    SDL_FRect box = center(full, panelW, panelH);
    SDL_SetRenderDrawColor(renderer_, 26, 27, 31, 245);
    jplay::fillRect(renderer_, &box);
    setColor(renderer_, { 80, 82, 90, 255 });
    jplay::drawRect(renderer_, &box);

    // Title lockup, centered across the panel.
    SDL_FRect body = inset(box, pad, pad);
    SDL_FRect titleRow = cutTop(body, titleLh);
    SDL_FRect lockup = centerH(titleRow, lockupW);
    if (iconTex_) {
        SDL_FRect dst = { lockup.x, lockup.y, iconSz, iconSz };
        SDL_RenderTexture(renderer_, iconTex_, nullptr, &dst);
        gapLeft(lockup, iconSz + iconGap);
    }
    headerFont_.draw(renderer_, lockup.x, lockup.y, { 235, 238, 245, 255 }, title);
    gapTop(body, 12.0f * dpiScale);

    // Two columns side by side, each stacking its sections: a header row followed
    // by that section's label/value rows. panelH was sized from the same steps, so
    // the tallest column ends exactly at the bottom pad.
    for (int ci = 0; ci < nColumns; ++ci) {
        SDL_FRect col = cutLeft(body, colW[ci]);
        gapLeft(body, sectionGap);
        for (const Section& s : *columns[ci]) {
            gapTop(col, headerGap);
            SDL_FRect header = cutTop(col, rowH);
            drawText(header.x, header.y, { 160, 163, 172, 255 }, s.title);
            for (const Row& r : s.rows) {
                SDL_FRect row = cutTop(col, rowH);
                SDL_FRect label = cutLeft(row, colLabelW[ci] + colGap);
                drawText(label.x, label.y, { 150, 200, 255, 255 }, r.label);
                drawText(row.x, row.y, { 210, 213, 220, 255 }, r.value);
            }
        }
    }
}
