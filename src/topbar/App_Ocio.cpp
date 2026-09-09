// OCIO toolbar popups: the Display, View Transform, Look, and File Colorspace
// dropdowns opened from the top toolbar. All four share one list-popup
// implementation (renderOcioListMenu / ocioListMenuHandleEvent) — a header plus a
// scrollable column of selectable rows anchored below the button — with thin
// per-button wrappers below that feed it the right item list and pick handler.
// The chosen transforms live on the OcioManager (ocio_); the buttons that open
// these popups are drawn in App_TopBar.cpp.
// All members of App; split out of App.cpp for the same reasons as the other App_*.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "TopBarStyle.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace jplay;

// ─── OCIO toolbar list popups (Display / View / File Colorspace) ─────────────
//
// The three OCIO toolbar buttons share one list-popup implementation: a header
// plus a scrollable column of selectable rows, anchored below the button. The
// panel height is capped to the window so long lists (e.g. the full color-space
// set) scroll rather than overflow off-screen.

void App::renderOcioListMenu(const char* header, const SDL_FRect& btn,
                             const std::vector<std::string>& items,
                             const std::string& active, float& scroll,
                             int hoverRow, std::vector<SDL_FRect>& outRows,
                             SDL_FRect& outPanel) {
    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    const float rowH = lineH + 6.0f * dpiScale;
    const float gap = 8.0f * dpiScale;
    const float checkW = 16.0f * dpiScale; // reserved gutter left of every label
    const float mark = 6.0f * dpiScale;    // filled dot marking the active item

    // Panel width: widest of the header and the item rows.
    float contentW = textFont_.measure(renderer_, header);
    for (const auto& it : items)
        contentW = std::max(contentW, checkW + textFont_.measure(renderer_, it.c_str()));
    float panelW = std::max(contentW + pad * 2.0f, 170.0f * dpiScale);

    // Anchor below the button; clamp within the window on the right side.
    float panelX = btn.x;
    float panelY = btn.y + btn.h;
    if (panelX + panelW > winW_ - 4.0f)
        panelX = winW_ - panelW - 4.0f;
    panelX = std::max(panelX, 4.0f);

    // Height: header block + all rows, capped so the panel stays on-screen. When
    // the full list is taller than the cap, the rows viewport scrolls.
    const float headH = pad + lineH + gap;
    const float footH = pad;
    float fullRowsH = (float)items.size() * rowH;
    float maxPanelH = std::max(headH + rowH + footH, winH_ - panelY - 6.0f * dpiScale);
    float panelH = std::min(headH + fullRowsH + footH, maxPanelH);
    float viewH = panelH - headH - footH;
    float maxScroll = std::max(0.0f, fullRowsH - viewH);
    scroll = std::clamp(scroll, 0.0f, maxScroll);

    outPanel = { panelX, panelY, panelW, panelH };
    jplay::drawPopupPanel(renderer_, outPanel);

    // Walk the panel top-down. The body keeps the panel's full width — the row
    // highlight and the clip rect both run edge to edge — so the pad on the left
    // and right is taken per block rather than up front.
    SDL_FRect body = outPanel;
    gapTop(body, pad);
    SDL_FRect hdr = cutTop(body, lineH);
    drawText(hdr.x + pad, hdr.y, kHeader, header);
    gapTop(body, gap);
    gapBottom(body, footH);
    const SDL_FRect view = body; // == { panelX, panelY + headH, panelW, viewH }

    // Rows are drawn into a clipped viewport so partial rows at the edges don't
    // spill over the header or border.
    SDL_Rect clip = { (int)view.x, (int)view.y, (int)view.w, (int)view.h };
    SDL_SetRenderClipRect(renderer_, &clip);

    // Cut the rows off an unbounded column started at the scrolled-up origin (a
    // column bounded by the viewport would clamp every row past its bottom edge
    // to zero height — see Layout.h), and draw only the ones that land inside.
    SDL_FRect column = { view.x + 2.0f, view.y - scroll, view.w - 4.0f, kUnbounded };
    outRows.assign(items.size(), SDL_FRect{}); // rows scrolled out keep an empty (unhittable) rect
    for (size_t i = 0; i < items.size(); ++i) {
        SDL_FRect row = cutTop(column, rowH);
        if (!visibleIn(row, view))
            continue;
        outRows[i] = row;
        bool current = (items[i] == active);
        if ((int)i == hoverRow) {
            jplay::drawRowHover(renderer_, row);
        } else if (current) {
            setColor(renderer_, kCurrent);
            jplay::fillRect(renderer_, &row);
        }
        // Marker gutter, then the label in what is left of the row.
        SDL_FRect inner = { panelX + pad, row.y, panelW - pad * 2.0f, row.h };
        SDL_FRect gutter = cutLeft(inner, checkW);
        if (current) {
            gapLeft(gutter, 2.0f);
            SDL_FRect mkCol = cutLeft(gutter, mark);
            SDL_FRect mk = centerV(mkCol, mark);
            setColor(renderer_, kCheck);
            jplay::fillRect(renderer_, &mk);
        }
        SDL_FRect lbl = centerV(inner, lineH);
        drawText(lbl.x, lbl.y, kLabel, items[i].c_str());
    }
    SDL_SetRenderClipRect(renderer_, nullptr);

    // Scrollbar thumb along the right edge of the viewport. Grabbable, so the draw
    // also records what it maps onto for a press or drag on it next frame.
    drawScrollbar(renderer_, view, fullRowsH, scroll, dpiScale, false, ocioMenuSb_);
}

bool App::ocioListMenuHandleEvent(const SDL_Event& e, const SDL_FRect& btn,
                                  const std::vector<std::string>& items,
                                  const SDL_FRect& panel,
                                  const std::vector<SDL_FRect>& rows,
                                  float& scroll, int& hoverRow, bool& open,
                                  const std::function<void(const std::string&)>& onPick) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            open = false; hoverRow = -1;
            return true;
        }
        float mx = e.button.x, my = e.button.y;
        // Clicking the button itself: let it fall through so the button handler
        // toggles the popup closed.
        if (inRect(btn, mx, my))
            return false;
        if (!inRect(panel, mx, my)) {
            open = false; hoverRow = -1;
            return true; // outside click just dismisses
        }
        // The scrollbar takes the press ahead of the rows, which span nearly the
        // full panel width and so reach under the bar.
        if (ocioMenuSb_.press(mx, my, scroll, dpiScale))
            return true;
        for (size_t i = 0; i < rows.size() && i < items.size(); ++i) {
            if (inRect(rows[i], mx, my)) {
                onPick(items[i]);
                hasTexture_ = false;
                open = false; hoverRow = -1;
                return true;
            }
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        float mx = e.motion.x, my = e.motion.y;
        // A held scrollbar follows the cursor anywhere, and keeps the row under it
        // from taking the hover while it does.
        if (ocioMenuSb_.dragging) {
            ocioMenuSb_.drag(my, scroll, dpiScale);
            return true;
        }
        hoverRow = -1;
        for (int i = 0; i < (int)rows.size(); ++i)
            if (inRect(rows[i], mx, my)) {
                hoverRow = i;
                break;
            }
        return inRect(panel, mx, my);
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (inRect(panel, e.wheel.mouse_x, e.wheel.mouse_y)) {
            const float rowH = textFont_.lineHeight() + 6.0f * dpiScale;
            scroll -= e.wheel.y * rowH * 2.0f; // clamped in renderOcioListMenu
            if (scroll < 0.0f)
                scroll = 0.0f;
            return true;
        }
        return false;
    }
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) {
            open = false; hoverRow = -1;
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ─── OCIO Display popup ──────────────────────────────────────────────────────

void App::openOcioDisplayMenu() {
    ocioDisplayMenuOpen_ = !ocioDisplayMenuOpen_; // the button toggles the popup
    ocioDisplayHoverRow_ = -1;
    ocioMenuScroll_ = 0.0f;
}

void App::renderOcioDisplayMenu() {
    if (!ocioDisplayMenuOpen_)
        return;
    std::vector<std::string> displays = ocio_.displays();
    if (displays.empty()) {
        ocioDisplayMenuOpen_ = false;
        return;
    }
    renderOcioListMenu("DISPLAY", ocioDisplayBtnRect_, displays, ocio_.activeDisplay(),
                       ocioMenuScroll_, ocioDisplayHoverRow_, ocioDisplayRowRects_,
                       ocioDisplayMenuRect_);
}

bool App::ocioDisplayMenuHandleEvent(const SDL_Event& e) {
    return ocioListMenuHandleEvent(
        e, ocioDisplayBtnRect_, ocio_.displays(), ocioDisplayMenuRect_,
        ocioDisplayRowRects_, ocioMenuScroll_, ocioDisplayHoverRow_, ocioDisplayMenuOpen_,
        [this](const std::string& d) {
            ocio_.setDisplay(d);
            populateOcioViewSubmenu(); // display change resets the view list
            hasTexture_ = false; // the player re-renders through the new selection
        });
}

// ─── OCIO View Transform popup ───────────────────────────────────────────────

void App::openOcioViewMenu() {
    ocioViewMenuOpen_ = !ocioViewMenuOpen_; // the button toggles the popup
    ocioViewHoverRow_ = -1;
    ocioMenuScroll_ = 0.0f;
}

void App::renderOcioViewMenu() {
    if (!ocioViewMenuOpen_)
        return;
    std::vector<std::string> views = ocio_.views(ocio_.activeDisplay());
    if (views.empty()) {
        ocioViewMenuOpen_ = false;
        return;
    }
    renderOcioListMenu("VIEW TRANSFORM", ocioViewBtnRect_, views, ocio_.activeView(),
                       ocioMenuScroll_, ocioViewHoverRow_, ocioViewRowRects_,
                       ocioViewMenuRect_);
}

bool App::ocioViewMenuHandleEvent(const SDL_Event& e) {
    return ocioListMenuHandleEvent(
        e, ocioViewBtnRect_, ocio_.views(ocio_.activeDisplay()), ocioViewMenuRect_,
        ocioViewRowRects_, ocioMenuScroll_, ocioViewHoverRow_, ocioViewMenuOpen_,
        [this](const std::string& v) {
            ocio_.setView(v);
            timeline_.ocioView = v; // persisted per-project on save
            hasTexture_ = false; // the player re-renders through the new selection
        });
}

// ─── OCIO Look popup ─────────────────────────────────────────────────────────
//
// Unlike the other three lists, the Look list carries a leading "None" row that
// clears the active Look (ocio_.setLook("")); the config's named Looks follow.

void App::openOcioLookMenu() {
    ocioLookMenuOpen_ = !ocioLookMenuOpen_; // the button toggles the popup
    ocioLookHoverRow_ = -1;
    ocioMenuScroll_ = 0.0f;
}

static std::vector<std::string> ocioLookItems(const std::vector<std::string>& looks) {
    std::vector<std::string> items;
    items.reserve(looks.size() + 1);
    items.push_back("None");
    for (const auto& l : looks)
        items.push_back(l);
    return items;
}

void App::renderOcioLookMenu() {
    if (!ocioLookMenuOpen_)
        return;
    std::vector<std::string> items = ocioLookItems(ocio_.looks());
    std::string active = ocio_.activeLook().empty() ? "None" : ocio_.activeLook();
    renderOcioListMenu("LOOK", ocioLookBtnRect_, items, active,
                       ocioMenuScroll_, ocioLookHoverRow_, ocioLookRowRects_,
                       ocioLookMenuRect_);
}

bool App::ocioLookMenuHandleEvent(const SDL_Event& e) {
    return ocioListMenuHandleEvent(
        e, ocioLookBtnRect_, ocioLookItems(ocio_.looks()), ocioLookMenuRect_,
        ocioLookRowRects_, ocioMenuScroll_, ocioLookHoverRow_, ocioLookMenuOpen_,
        [this](const std::string& l) {
            ocio_.setLook(l == "None" ? "" : l);
            hasTexture_ = false; // the player re-renders through the new selection
        });
}

// ─── OCIO File Colorspace popup ──────────────────────────────────────────────

void App::openOcioInputCsMenu() {
    ocioInputCsMenuOpen_ = !ocioInputCsMenuOpen_; // the button toggles the popup
    ocioInputCsHoverRow_ = -1;
    ocioMenuScroll_ = 0.0f;
}

// The colour space is a property of the media, not of the session: this list edits
// the source under the playhead. Its leading "Automatic" row clears the override
// and hands the choice back to the config (its file rules, then the container's own
// colour tags); the config's colour spaces follow.
static std::vector<std::string> ocioInputCsItems(const std::vector<std::string>& spaces) {
    std::vector<std::string> items;
    items.reserve(spaces.size() + 1);
    items.push_back("Automatic");
    for (const auto& cs : spaces)
        items.push_back(cs);
    return items;
}

void App::renderOcioInputCsMenu() {
    if (!ocioInputCsMenuOpen_)
        return;
    std::vector<std::string> items = ocioInputCsItems(ocio_.colorSpaces());
    if (items.size() <= 1) {
        ocioInputCsMenuOpen_ = false;
        return;
    }
    auto m = playheadMedia();
    // With no override the mark sits on "Automatic" rather than on the space the
    // config happens to have resolved — what is selected is the policy, not the
    // answer, and the button label already shows the answer.
    std::string active = (m && !m->colorSpaceOverride().empty()) ? m->colorSpaceOverride()
                                                                 : "Automatic";
    renderOcioListMenu("FILE COLORSPACE", ocioInputCsBtnRect_, items, active,
                       ocioMenuScroll_, ocioInputCsHoverRow_, ocioInputCsRowRects_,
                       ocioInputCsMenuRect_);
}

bool App::ocioInputCsMenuHandleEvent(const SDL_Event& e) {
    return ocioListMenuHandleEvent(
        e, ocioInputCsBtnRect_, ocioInputCsItems(ocio_.colorSpaces()), ocioInputCsMenuRect_,
        ocioInputCsRowRects_, ocioMenuScroll_, ocioInputCsHoverRow_, ocioInputCsMenuOpen_,
        [this](const std::string& cs) {
            auto m = playheadMedia();
            if (!m)
                return;
            m->setColorSpaceOverride(cs == "Automatic" ? "" : cs);
            // Drop the cached resolution too: clearing an override has to re-ask the
            // config rather than read back the answer from before it was set.
            m->setResolvedColorSpace("", "");
            // No dirty flag to set: the override is serialized, so the derived
            // project signature picks the change up on its own.
        });
}

// ─── Thumbnail state ─────────────────────────────────────────────────────────

// Thumbnails carry no colour state of their own — a tile is the frame's own 8-bit
// encoding, keyed by source and frame alone — so nothing about the OCIO selection
// retires one. What does is the media pool changing under them: the textures
// already uploaded, the tiles laid out from them and the hover previews all belong
// to the outgoing project. Clearing the started-sets is what makes the next render
// restart generation: the two passes compare drawn sets, not keys, and the drawn
// set has not changed.
void App::resetThumbnailState() {
    freeOverviewTextures();
    thumbStartedIds_.clear();
    thumbPendingIds_.clear();
    thumbMissAt_.clear();
    freeSourceThumbs();
    previewCache_.clear();
    previewLru_.clear();
    previewDisplayedValid_ = false;
    previewThumbWritten_.clear();
    hasTexture_ = false;
}
