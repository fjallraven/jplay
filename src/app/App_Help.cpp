// Help translation unit: the keyboard-shortcuts overlay panel. Split out of
// App.cpp to keep that file manageable. The 'H' key toggle and the Help top
// menu item that open it live in App.cpp; only the panel rendering is here.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"

#include <algorithm>

using namespace jplay;

namespace {
inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}
} // namespace

// Keyboard-shortcuts overlay: a fixed panel centered in the window listing the
// key bindings handled in onKeyDown. Opened from Help ▸ Show Keyboard Shortcuts
// or the 'H' key; any click dismisses it (see the MOUSE_BUTTON_DOWN handler).
// Drawn last so it sits on top of everything.
void App::renderHelpPanel() {
    if (!helpOpen_)
        return;

    struct Row { const char* keys; const char* desc; };
    struct Section { const char* title; const Row* rows; int count; };
    struct Column { const Section* sections; int count; };
    static const Row general[] = {
        { "H",             "Show / hide this panel" },
        { "F1",            "Source of the clip under the playhead" },
        { "F2 / F3 / F4 / F5", "Timeline / overview / layout / stack" },
        { "Up / Down",     "Cycle the stack order (Stack view)" },
        { "F11",           "Fullscreen video (Esc to leave)" },
        { "Tab",           "Compact timeline (ruler only)" },
        { "L",             "Toggle layout (tiled tracks)" },
        { "I",             "Toggle inspector" },
        { "Backspace",     "Back: out of a source view, or a view up" },
        { "Ctrl + N",        "New project" },
        { "Ctrl + O",        "Open project" },
        { "Ctrl + S",        "Save project" },
        { "Ctrl + Shift + S",  "Save project As" },
        { "Ctrl + W",        "Close project" },
        { "Ctrl + Z / Ctrl + Y", "Undo / redo" },
        { "Q",       "Quit" },
    };
    static const Row player[] = {
        { "R / G / B",      "View red / green / blue channel" },
        { ", / .",         "Exposure -/+ 1/3 stop (Shift: 1 stop)" },
        { "E + drag",       "Scrub exposure / gamma over the frame" },
        { "E",             "Bypass exposure / gamma (A/B)" },
        { "P",             "Toggle pixel inspector" },
        { "F",             "Fit frame or timeline" },
        { "Alt + F",         "Fit sequence under playhead" },
        { "Shift + F",       "Zoom to clip under playhead" },
        { "1 / 2 / 3 / 4",   "Frame at 1:1 / 2:1 / 3:1 / 4:1 pixels" },
    };
    static const Row playback[] = {
        { "Space",         "Play / pause" },
        { "Left / Right",  "Step 1 frame (Shift: 10 frames)" },
        { "Ctrl + Up / Down", "Previous / next clip" },
        { "PgUp / PgDn",   "Set range to this clip, then next / previous" },
        { "Shift + PgUp / PgDn", "Expand / contract range by a clip each side" },
        { "Home / End",    "Go to in / out point" },
        { "[ ]",           "Set playback in / out point" },
        { "Shift + [ ]",     "Clear playback in / out point" },
        { "X",             "Set range to clip under playhead" },
        { "Shift + X / \\",  "Clear in / out range" },
    };
    static const Row timeline[] = {
        { "S",             "Toggle snap" },
        { "D",             "Disable / enable clip" },
        { "Ctrl + C",        "Copy selected clip(s)" },
        { "Ctrl + V",        "Paste at playhead (first free track)" },
        { "Delete",        "Delete selected clip or gap" },
    };
    static const Section leftSections[] = {
        { "General",  general,  (int)(sizeof(general) / sizeof(general[0])) },
        { "Player",   player,   (int)(sizeof(player) / sizeof(player[0])) },
    };
    static const Section rightSections[] = {
        { "Playback", playback, (int)(sizeof(playback) / sizeof(playback[0])) },
        { "Timeline", timeline, (int)(sizeof(timeline) / sizeof(timeline[0])) },
    };
    static const Column columns[] = {
        { leftSections,  (int)(sizeof(leftSections) / sizeof(leftSections[0])) },
        { rightSections, (int)(sizeof(rightSections) / sizeof(rightSections[0])) },
    };
    const int nColumns = (int)(sizeof(columns) / sizeof(columns[0]));

    const float pad = 20.0f * dpiScale;
    const float lh = textFont_.lineHeight();
    const float rowH = lh + 6.0f * dpiScale;
    const float colGap = 24.0f * dpiScale;     // between a column's key and desc columns
    const float sectionGap = 40.0f * dpiScale; // between the two side-by-side columns
    const float headerGap = 14.0f * dpiScale;  // extra space above a section header
    const char* title = "Keyboard Shortcuts";

    // Each column is sized from its own widest key / desc, across all its sections.
    float colKeyW[nColumns] = { 0.0f, 0.0f };
    float colDescW[nColumns] = { 0.0f, 0.0f };
    for (int ci = 0; ci < nColumns; ++ci)
        for (int si = 0; si < columns[ci].count; ++si) {
            const Section& s = columns[ci].sections[si];
            for (int i = 0; i < s.count; ++i) {
                colKeyW[ci] = std::max(colKeyW[ci], textFont_.measure(renderer_, s.rows[i].keys));
                colDescW[ci] = std::max(colDescW[ci], textFont_.measure(renderer_, s.rows[i].desc));
            }
        }
    float colW[nColumns];
    float bodyW = 0.0f;
    float tallestColH = 0.0f;
    for (int ci = 0; ci < nColumns; ++ci) {
        colW[ci] = colKeyW[ci] + colGap + colDescW[ci];
        bodyW += colW[ci] + (ci ? sectionGap : 0.0f);
        float colH = 0.0f;
        for (int si = 0; si < columns[ci].count; ++si)
            colH += headerGap + rowH + columns[ci].sections[si].count * rowH;
        tallestColH = std::max(tallestColH, colH);
    }

    float titleW = textFont_.measure(renderer_, title);
    float panelW = std::max(bodyW, titleW) + pad * 2.0f;
    float panelH = pad * 2.0f + lh + 12.0f * dpiScale // title + gap
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

    // Title, centered across the panel.
    SDL_FRect body = inset(box, pad, pad);
    SDL_FRect titleRow = cutTop(body, lh);
    SDL_FRect titleSlot = centerH(titleRow, titleW);
    drawText(titleSlot.x, titleSlot.y, { 235, 238, 245, 255 }, title);
    gapTop(body, 12.0f * dpiScale);

    // Two columns side by side, each stacking its sections: a header row followed
    // by that section's key/description rows. panelH was sized from the same steps,
    // so the tallest column ends exactly at the bottom pad.
    for (int ci = 0; ci < nColumns; ++ci) {
        SDL_FRect col = cutLeft(body, colW[ci]);
        gapLeft(body, sectionGap);
        for (int si = 0; si < columns[ci].count; ++si) {
            const Section& s = columns[ci].sections[si];
            gapTop(col, headerGap);
            SDL_FRect header = cutTop(col, rowH);
            drawText(header.x, header.y, { 160, 163, 172, 255 }, s.title);
            for (int i = 0; i < s.count; ++i) {
                SDL_FRect row = cutTop(col, rowH);
                SDL_FRect keys = cutLeft(row, colKeyW[ci] + colGap);
                drawText(keys.x, keys.y, { 150, 200, 255, 255 }, s.rows[i].keys);
                drawText(row.x, row.y, { 210, 213, 220, 255 }, s.rows[i].desc);
            }
        }
    }
}
