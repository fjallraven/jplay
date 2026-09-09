#pragma once

// Layout constants and small helpers shared across the App_* translation units
// (App.cpp, App_Timeline.cpp, App_ProjectExplorer.cpp, App_Project.cpp). These
// were previously file-local to App.cpp; they live here so the split units can
// share them without duplication. App-private state and methods stay in App.h.

#include "PixelSnap.h"
#include "SkinColors.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

namespace jplay {

// Permanently 1.0. Layout is expressed in *logical* units and the renderer scales
// logical -> device pixels for us via SDL_SetRenderLogicalPresentation (set up in
// App::computeLayout), so a 200% display gets the exact same layout as a 100% one,
// drawn at twice the resolution. The real device scale lives in App::uiScale_.
//
// It is kept, rather than deleted, so the ~190 `* dpiScale` factors spread through
// the layout code stay valid — they are now identities, and stripping them is a
// mechanical cleanup best done on its own. constexpr on purpose: reintroducing
// manual DPI scaling here would double-scale everything, so it must fail to
// compile rather than misbehave at runtime.
inline constexpr float dpiScale = 1.0f;

// Timeline / panel layout, in logical units (== render pixels on a 1x display).
inline float kTopBarH            = 22.0f;  // toolbar between the title bar and the video frame (just fits the 18px button)
inline float kInfoH              = 26.0f;
inline float kRulerH             = 26.0f;
inline float kCacheStripH        =  5.0f;
inline float kSeqBarH            = 22.0f;  // sequence bar, shown only when sequences exist
inline float kShotBarH           = 22.0f;  // shot bar, shown only when shots exist
inline float kTrackH             = 46.0f;  // height of one video track row
inline float kAudioTrackH        = 23.0f;  // height of one audio track row (half a video row)
inline float kHeaderW            = 92.0f;  // width of the left track-label gutter
inline float kNavIconSide        = 28.0f;  // left icon strip: the square each toggle glyph is sized to
inline float kSidePanelW         = 36.0f;  // left icon strip: kNavIconSide + 2*4px pad at 1x
inline float kInspectorW         = 155.0f; // minimum content width of the Inspector overlay
inline float kHoverBoxPx         = 120.0f; // fixed width of the file-drag preview box
inline float kOverviewHandleHitW =  6.0f;  // total hit width of the overview resize handle (±half each side)
inline float kTimelineMaxFrac    =  0.40f; // timeline takes at most this much of the window height
inline float kTimelineEdgeHitH   =  6.0f;  // total hit height of the timeline top-edge handle (±half each side)

// Shared button chrome. Fill/border for the flat buttons used across the app
// (launcher CREATE PROJECT / SYNC SESSION, timeline toolbar). The base RGB is
// canonical; a few sites pass their own alpha (e.g. the launcher fills over the
// video at 220). Keep these in sync so all buttons read as one control style.
//
// These are references into the live skin palette rather than constants, so a
// skin change retints them where they are already used — see jplay::colors().
inline const SDL_Color& kUiBtnBg          = colors().uibtnBg;
inline const SDL_Color& kUiBtnBgHover     = colors().uibtnHover;
inline const SDL_Color& kUiBtnBorder      = colors().uibtnBorder;
inline const SDL_Color& kUiBtnBorderHover = colors().uibtnBorderHov;

// Canonical window-chrome / panel background. Shared by the custom title bar, the
// top toolbar, the left icon strip + project explorer, the timeline, and the
// grade / settings / inspector side panels so the app chrome reads as one flat
// surface. Change here to retint all of them at once.
inline const SDL_Color& kPanelBg = colors().panelBg;

// Space reserved at the right edge of a toolbar picker button for its down-caret,
// in logical units. The top-bar layout adds this on top of the icon + label so the
// caret never crowds the text.
constexpr float kTopBarCaretW = 14.0f;

// Down-caret at the right edge of a toolbar button, marking it as a picker that
// opens a list. Same two-stroke chevron the Dropdown bar and the Settings FPS
// field draw, so every picker in the app reads the same.
inline void drawTopBarCaret(SDL_Renderer* r, const SDL_FRect& btn, SDL_Color c, float scale) {
    const float arm = 3.0f * scale;
    const float cx = btn.x + btn.w - (kTopBarCaretW * 0.5f + 3.0f) * scale;
    const float cy = btn.y + btn.h * 0.5f - scale;
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    jplay::drawLine(r, cx - arm, cy, cx, cy + arm);
    jplay::drawLine(r, cx, cy + arm, cx + arm, cy);
}

// Brightened form of a stored sequence color: the timeline's sequence bar under
// the playhead, and the Project Explorer's swatch / palette rows, which read as
// the color itself rather than as a label backdrop.
inline constexpr float kSeqBgLift = 1.8f;

// Sequence background color (Sequence::bgColor, packed 0xRRGGBB) as an SDL color.
// `lift` brightens it (see kSeqBgLift); 1.0 is the stored color.
inline SDL_Color seqBgSdlColor(uint32_t rgb, float lift = 1.0f) {
    auto ch = [lift](uint32_t v) {
        return (Uint8)std::clamp((int)std::lround(v * lift), 0, 255);
    };
    return { ch((rgb >> 16) & 0xFF), ch((rgb >> 8) & 0xFF), ch(rgb & 0xFF), 255 };
}

inline bool inRect(const SDL_FRect& r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Case-insensitive extension test (ext includes the dot, e.g. ".exr").
inline bool hasExtension(const std::string& path, const char* ext) {
    std::string e = std::filesystem::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e == ext;
}

// Audio container formats accepted on the timeline (dropped files land as audio
// clips on the audio tracks). Video containers (.mov/.mp4/...) stay video even
// when they carry sound.
inline bool isAudioPath(const std::string& path) {
    static const char* kAudioExts[] = {
        ".wav", ".aif", ".aiff", ".flac", ".mp3", ".m4a", ".aac", ".ogg", ".opus", ".wma",
    };
    for (const char* e : kAudioExts)
        if (hasExtension(path, e))
            return true;
    return false;
}

// Collapse the trailing frame-number digits of an image-sequence stem to '#'
// placeholders (e.g. "file_0001" -> "file_####"), so sequence labels read as a
// pattern rather than an arbitrary first frame. Non-sequence stems, and stems
// without trailing digits (single still images), are returned unchanged.
inline std::string hashSeqStem(std::string stem, bool isSequence) {
    if (!isSequence)
        return stem;
    size_t digitStart = stem.size();
    while (digitStart > 0 && std::isdigit((unsigned char)stem[digitStart - 1]))
        --digitStart;
    if (digitStart == stem.size())
        return stem;
    return stem.substr(0, digitStart) + std::string(stem.size() - digitStart, '#');
}

// 64-bit FNV-1a of a string, as 16 lowercase hex chars. Used for stable thumbnail
// filenames; deterministic across runs (unlike std::hash) so the on-disk cache
// survives restarts. Shared by the Overview (per-clip) and SOURCES-bin (per-source)
// thumbnail keys.
inline std::string fnv1aHex(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : s) {
        h ^= ch;
        h *= 1099511628211ULL;
    }
    char buf[17];
    for (int i = 15; i >= 0; --i) {
        buf[i] = "0123456789abcdef"[h & 0xF];
        h >>= 4;
    }
    buf[16] = '\0';
    return std::string(buf);
}

// Upper-cased basename, used for status messages and on-timeline clip labels.
inline std::string fileLabel(const std::string& path) {
    std::string s = std::filesystem::path(path).filename().string();
    //std::transform(s.begin(), s.end(), s.begin(),
    //               [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

} // namespace jplay
