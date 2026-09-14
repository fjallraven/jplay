// Sequence view-filter popup: the dropdown opened from the toolbar's "Viewing:"
// button that scopes the timeline to one sequence or to a whole project. It lists
// only what is already loaded; reaching something that isn't is the job of the two
// "Open …" buttons left of the button that opens this popup. The rows are prepared
// once when the popup opens (buildSequenceMenuRows, which lives elsewhere) and this
// unit draws and dispatches them. All three buttons are drawn in App_TopBar.cpp.
// All members of App; split out of App.cpp for the same reasons as the other App_*.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "TopBarStyle.h"

#include <algorithm>
#include <cstddef>

using namespace jplay;

// ─── Sequence view-filter popup ──────────────────────────────────────────────

void App::openSequenceMenu() {
    sequenceMenuOpen_ = !sequenceMenuOpen_; // the button toggles the popup
    sequenceHoverRow_ = -1;
    sequenceMenuScroll_ = 0.0f;
    sequenceScrollToRow_ = -1;
    if (sequenceMenuOpen_)
        buildSequenceMenuRows(); // naming lookups per sequence: once per open, not per frame
}

void App::renderSequenceMenu() {
    if (!sequenceMenuOpen_)
        return;

    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    const float rowH = lineH + 6.0f * dpiScale;
    const float gap = 8.0f * dpiScale;
    const float checkW = 16.0f * dpiScale;
    const float mark = 6.0f * dpiScale;     // filled dot marking the scoped row
    const float indentW = 12.0f * dpiScale; // a sequence sitting under its project header

    // The rows were built when the popup opened (buildSequenceMenuRows): a header
    // per project with its sequences under it, then the sequences belonging to no
    // project. There is no "All" row (a project row is the way to view more than
    // one sequence).
    const size_t nRows = sequenceMenuRows_.size();

    // Panel width: widest of the header and the rows.
    float contentW = textFont_.measure(renderer_, "SEQUENCE");
    for (const SeqMenuRow& r : sequenceMenuRows_)
        contentW = std::max(contentW, checkW + (r.indent ? indentW : 0.0f) +
                                          textFont_.measure(renderer_, r.label.c_str()));
    // Never narrower than the button it drops from.
    float panelW = std::max(contentW + pad * 2.0f, sequenceBtnRect_.w);

    // Anchor the panel's top at the button's bottom edge; grow downward. Clamp
    // within the window on the right side.
    float panelX = sequenceBtnRect_.x;
    float panelY = sequenceBtnRect_.y + sequenceBtnRect_.h;
    if (panelX + panelW > winW_ - 4.0f)
        panelX = winW_ - panelW - 4.0f;
    panelX = std::max(panelX, 4.0f);

    // Height: header block + all rows, capped so the panel stays on-screen. When
    // the full list is taller than the cap the rows viewport scrolls — same shape
    // as the OCIO list popups (renderOcioListMenu).
    const float headH = pad + lineH + gap;
    const float footH = pad;
    float fullRowsH = (float)nRows * rowH;
    float maxPanelH = std::max(headH + rowH + footH, winH_ - panelY - 6.0f * dpiScale);
    float panelH = std::min(headH + fullRowsH + footH, maxPanelH);
    float viewH = panelH - headH - footH;
    float maxScroll = std::max(0.0f, fullRowsH - viewH);
    // Keyboard nav asked for a row: bring it inside the viewport before clamping.
    if (sequenceScrollToRow_ >= 0 && sequenceScrollToRow_ < (int)nRows) {
        const float top = (float)sequenceScrollToRow_ * rowH;
        sequenceMenuScroll_ = std::clamp(sequenceMenuScroll_, top + rowH - viewH, top);
        sequenceScrollToRow_ = -1;
    }
    sequenceMenuScroll_ = std::clamp(sequenceMenuScroll_, 0.0f, maxScroll);

    sequenceMenuRect_ = { panelX, panelY, panelW, panelH };

    jplay::drawPopupPanel(renderer_, sequenceMenuRect_);

    // Walk the panel top-down. The body keeps the panel's full width, since the
    // row highlight runs edge to edge; the pad is taken per block instead.
    SDL_FRect body = sequenceMenuRect_;
    gapTop(body, pad);
    SDL_FRect hdr = cutTop(body, lineH);
    drawText(hdr.x + pad, hdr.y, kHeader, "SEQUENCE");
    gapTop(body, gap);
    gapBottom(body, footH);
    const SDL_FRect view = body;

    // Rows are drawn into a clipped viewport so partial rows at the edges don't
    // spill over the header or border.
    SDL_Rect clip = { (int)view.x, (int)view.y, (int)view.w, (int)view.h };
    SDL_SetRenderClipRect(renderer_, &clip);

    const int fsi = filteredSeqIdx(); // -1 = no single sequence scoped
    // Cut the rows off an unbounded column started at the scrolled-up origin (a
    // column bounded by the viewport would clamp every row past its bottom edge to
    // zero height — see Layout.h). One drawing pass over the prepared rows, which
    // also stamps each row's hit rect for sequenceMenuHandleEvent; rows scrolled
    // out keep an empty (unhittable) rect.
    SDL_FRect column = { view.x, view.y - sequenceMenuScroll_, view.w, kUnbounded };
    for (size_t i = 0; i < nRows; ++i) {
        SeqMenuRow& r = sequenceMenuRows_[i];
        SDL_FRect row = cutTop(column, rowH);
        row = inset(row, 2.0f, 0.0f);
        if (!visibleIn(row, view)) {
            r.rect = SDL_FRect{};
            continue;
        }
        r.rect = row;
        // The marked row is whatever the view is scoped to: one sequence, or a
        // whole project.
        const bool current =
            (r.kind == SeqMenuRow::Kind::Sequence && r.seqIdx == fsi) ||
            (r.kind == SeqMenuRow::Kind::Project &&
             seqMenuProjects_[r.projIdx].id == viewProjId_);
        if ((int)i == sequenceHoverRow_) {
            jplay::drawRowHover(renderer_, row);
        } else if (current) {
            setColor(renderer_, kCurrent);
            jplay::fillRect(renderer_, &row);
        }
        // Indent (a sequence under its project header), marker gutter, then the
        // label in what is left of the row.
        SDL_FRect inner = { panelX + pad, row.y, panelW - pad * 2.0f, row.h };
        gapLeft(inner, r.indent ? indentW : 0.0f);
        SDL_FRect gutter = cutLeft(inner, checkW);
        if (current) {
            gapLeft(gutter, 2.0f);
            SDL_FRect mkCol = cutLeft(gutter, mark);
            SDL_FRect mk = centerV(mkCol, mark);
            setColor(renderer_, kCheck);
            jplay::fillRect(renderer_, &mk);
        }
        // Project names in white so they read as the group they open.
        SDL_FRect lbl = centerV(inner, lineH);
        drawText(lbl.x, lbl.y,
                 r.kind == SeqMenuRow::Kind::Project ? kGroupHead : kLabel, r.label.c_str());
    }
    SDL_SetRenderClipRect(renderer_, nullptr);

    // Scrollbar thumb along the right edge of the viewport. Grabbable, so the draw
    // also records what it maps onto for a press or drag on it next frame.
    drawScrollbar(renderer_, view, fullRowsH, sequenceMenuScroll_, dpiScale, false,
                  sequenceMenuSb_);
}

bool App::sequenceMenuHandleEvent(const SDL_Event& e) {
    auto closeMenu = [&]() {
        sequenceMenuOpen_ = false;
        sequenceHoverRow_ = -1;
    };
    // Activate row `i` — shared by mouse clicks and keyboard Enter.
    auto activate = [&](int i) {
        const SeqMenuRow r = sequenceMenuRows_[i]; // by value: the actions rebuild rows
        closeMenu();
        switch (r.kind) {
        case SeqMenuRow::Kind::Project:
            setProjectView(seqMenuProjects_[r.projIdx].id,
                           seqMenuProjects_[r.projIdx].seqIds);
            break;
        case SeqMenuRow::Kind::Sequence:
            setSequenceView(r.seqIdx);
            break;
        }
    };
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            closeMenu();
            return true;
        }
        float mx = e.button.x, my = e.button.y;
        // Clicking the button itself: let it fall through so the button handler
        // toggles the popup closed.
        if (inRect(sequenceBtnRect_, mx, my))
            return false;
        if (!inRect(sequenceMenuRect_, mx, my)) {
            closeMenu();
            return true; // outside click just dismisses
        }
        // The scrollbar takes the press ahead of the rows, which span nearly the
        // full panel width and so reach under the bar.
        if (sequenceMenuSb_.press(mx, my, sequenceMenuScroll_, dpiScale))
            return true;
        for (size_t i = 0; i < sequenceMenuRows_.size(); ++i) {
            if (!inRect(sequenceMenuRows_[i].rect, mx, my))
                continue;
            activate((int)i);
            return true;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        float mx = e.motion.x, my = e.motion.y;
        // A held scrollbar follows the cursor anywhere, and keeps the row under it
        // from taking the hover while it does.
        if (sequenceMenuSb_.dragging) {
            sequenceMenuSb_.drag(my, sequenceMenuScroll_, dpiScale);
            return true;
        }
        sequenceHoverRow_ = -1;
        for (int i = 0; i < (int)sequenceMenuRows_.size(); ++i)
            if (inRect(sequenceMenuRows_[i].rect, mx, my)) {
                sequenceHoverRow_ = i;
                break;
            }
        return inRect(sequenceMenuRect_, mx, my);
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (inRect(sequenceMenuRect_, e.wheel.mouse_x, e.wheel.mouse_y)) {
            const float rowH = textFont_.lineHeight() + 6.0f * dpiScale;
            sequenceMenuScroll_ -= e.wheel.y * rowH * 2.0f; // clamped in renderSequenceMenu
            if (sequenceMenuScroll_ < 0.0f)
                sequenceMenuScroll_ = 0.0f;
            return true;
        }
        return false;
    }
    case SDL_EVENT_KEY_DOWN: {
        const int n = (int)sequenceMenuRows_.size();
        switch (e.key.key) {
        case SDLK_ESCAPE:
            closeMenu();
            return true;
        case SDLK_DOWN:
            if (n > 0) {
                sequenceHoverRow_ = (sequenceHoverRow_ < 0) ? 0
                                                            : (sequenceHoverRow_ + 1) % n;
                sequenceScrollToRow_ = sequenceHoverRow_; // follow it if it is off-viewport
            }
            return true;
        case SDLK_UP:
            if (n > 0) {
                sequenceHoverRow_ = (sequenceHoverRow_ <= 0) ? n - 1
                                                             : sequenceHoverRow_ - 1;
                sequenceScrollToRow_ = sequenceHoverRow_;
            }
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (sequenceHoverRow_ >= 0 && sequenceHoverRow_ < n)
                activate(sequenceHoverRow_);
            return true;
        default:
            return false;
        }
    }
    default:
        return false;
    }
}
