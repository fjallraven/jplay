// Nav-panel translation unit: the fixed left icon strip (kSidePanelW) whose toggle
// glyphs switch between the side panels — Project Explorer, Clip Source,
// Color Grading, Tech Check, Draw, Sync Review, Settings — plus the hover
// tooltips for those buttons. Split out of App_ProjectExplorer.cpp: the strip is top-level
// navigation into panels whose logic lives across the other App_*.cpp files,
// distinct from the project-explorer content it sits beside.

#include "App.h"
#include "AppInternal.h"
#include "SkinColors.h"

using namespace jplay;

namespace {
// Icon strip shares the window-chrome fill (jplay::kPanelBg).
const SDL_Color& kDivider = jplay::colors().divider;    // panel edge line
const SDL_Color& kActive  = jplay::colors().iconActive; // active toggle
const SDL_Color& kIdle    = jplay::colors().iconIdle;   // idle toggle
const SDL_Color& kWarn    = jplay::colors().iconWarn;   // tech-mode live tint

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}
} // namespace

void App::renderSidePanel() {
    float topH = titleBar_.height();
    SDL_FRect strip = { 0, topH, kSidePanelW, panelsBottom_ - topH };
    setColor(renderer_, kPanelBg);
    jplay::fillRect(renderer_, &strip);

    // The open panel's toggle gets a fill behind its glyph, and merely hovering one
    // shows the same fill faded, so the strip previews the click. The fill is the
    // whole button — a band spanning the strip width — while the glyph keeps to the
    // square inset within it, so icon size is independent of the band's bounds. The
    // open panel additionally gets a 2px accent bar down the band's left edge.
    float mx = 0.0f, my = 0.0f;
    uiMouse(mx, my);
    auto stripBtn = [&](uint32_t icon, const SDL_FRect& btn, bool active, SDL_Color col) {
        if (btn.w <= 0.0f)
            return;
        const bool hover = !active && inRect(btn, mx, my);
        if (active || hover) {
            const Uint8 alpha = active ? 255 : 110;
            const SDL_Color& bg = colors().navBtnBg;
            SDL_SetRenderDrawColor(renderer_, bg.r, bg.g, bg.b, alpha);
            jplay::fillRect(renderer_, &btn);
        }
        if (active) {
            SDL_FRect bar = { btn.x, btn.y, 2.0f * dpiScale, btn.h };
            setColor(renderer_, colors().navSelBar);
            jplay::fillRect(renderer_, &bar);
        }
        const float side = btn.h - 4.0f * dpiScale; // the square the glyph is sized to
        SDL_FRect g = { btn.x + (btn.w - side) * 0.5f, btn.y + 2.0f * dpiScale, side, side };
        icons_.drawGlyph(renderer_, icon, g, col);
    };

    // Directory / ProjectExplorer toggle (ICON_MDI_FILE_MULTIPLE, U+F0222).
    stripBtn(0xF0222, dirButtonRect_, projectExplorerOpen_,
             projectExplorerOpen_ ? kActive : kIdle);
    // Clip Source toggle (ICON_MDI_LAYERS_TRIPLE, U+F0F58).
    stripBtn(0xF0F58, clipSourceButtonRect_, clipSourceOpen_,
             clipSourceOpen_ ? kActive : kIdle);
    // Color grading toggle (ICON_MDI_CHART_BELL_CURVE, U+F0C50).
    stripBtn(0xF0C50, gradeButtonRect_, gradeOpen_, gradeOpen_ ? kActive : kIdle);

    // Tech-check toggle (ICON_MDI_MONITOR_EYE, U+F13B4); brightened when a mode is live.
    SDL_Color techCol = techOpen_ ? kActive
                        : (techMode_ != TechMode::None ? kWarn : kIdle);
    stripBtn(0xF13B4, techButtonRect_, techOpen_, techCol);

    // Draw (freehand markup) toggle (ICON_MDI_BRUSH, U+F00E3); highlighted when active.
    stripBtn(0xF00E3, pencilBtnRect_, pencilMode_, pencilMode_ ? kActive : kIdle);

    // Sync-review session toggle (ICON_MDI_ACCOUNT_GROUP, U+F0849); brightened
    // green while a session is live, accent while the panel is open.
    SDL_Color sessCol = (syncSession_.role() != syncreview::Role::None) ? SDL_Color{ 90, 170, 110, 255 }
                        : (sessionPanelOpen_ ? kActive : kIdle);
    stripBtn(0xF0849, sessionButtonRect_, sessionPanelOpen_, sessCol);

    // Settings toggle (ICON_MDI_COG, U+F0493), directly below the pencil button.
    stripBtn(0xF0493, settingsButtonRect_, settingsOpen_, settingsOpen_ ? kActive : kIdle);

    // Edge line last.
    setColor(renderer_, kDivider);
    jplay::drawLine(renderer_, kSidePanelW - 0.5f, topH, kSidePanelW - 0.5f, panelsBottom_);
}

// ─── Panel navigation without the strip ──────────────────────────────────────
// The left panes are mutually exclusive: opening one closes the rest. The strip's
// click handler spells that rule out per button (App.cpp); these two reach it by
// pane identity instead, for the View menu's Panels submenu.

bool App::leftPanelOpen(LeftPanel p) const {
    switch (p) {
    case LeftPanel::ProjectExplorer: return projectExplorerOpen_;
    case LeftPanel::ClipSource:      return clipSourceOpen_;
    case LeftPanel::Grade:           return gradeOpen_;
    case LeftPanel::Tech:            return techOpen_;
    case LeftPanel::Draw:            return pencilMode_;
    case LeftPanel::Sync:            return sessionPanelOpen_;
    case LeftPanel::Settings:        return settingsOpen_;
    }
    return false;
}

void App::toggleLeftPanel(LeftPanel p) {
    const bool wasOpen = leftPanelOpen(p);
    projectExplorerOpen_ = false;
    gradeOpen_ = false;
    techOpen_ = false;
    settingsOpen_ = false;
    pencilMode_ = false;
    if (sessionPanelOpen_) {
        sessionPanelOpen_ = false;
        SDL_StopTextInput(window_); // the pane owns text fields, as its own toggle does
    }
    closeClipSource();
    if (wasOpen)
        return; // picking the open pane's entry closes it, like clicking its icon
    switch (p) {
    case LeftPanel::ProjectExplorer:
        projectExplorerOpen_ = true;
        refreshExplorerOrder(); // re-sort shots on open
        break;
    case LeftPanel::ClipSource:
        clipSourceOpen_ = true;
        clipSourceDirty_ = true; // describe on the next render
        clipSourceScroll_ = 0.0f;
        break;
    case LeftPanel::Grade:    gradeOpen_ = true; break;
    case LeftPanel::Tech:     techOpen_ = true; break;
    case LeftPanel::Draw:     pencilMode_ = true; break;
    case LeftPanel::Sync:     sessionPanelOpen_ = true; break;
    case LeftPanel::Settings: settingsOpen_ = true; break;
    }
}

// Hover labels for the left icon strip, drawn to the right of the hovered
// button. Called after the panels render (see App::run) so the label overlays
// whichever panel is open beside the strip rather than being covered by it.
void App::renderIconStripTooltips() {
    float mx = 0.0f, my = 0.0f;
    uiMouse(mx, my);
    auto tip = [&](const SDL_FRect& btn, const char* text) {
        if (mx < btn.x || mx >= btn.x + btn.w || my < btn.y || my >= btn.y + btn.h)
            return;
        const float pad = 5.0f * dpiScale;
        float bw = textFont_.measure(renderer_, text) + pad * 2.0f;
        float bh = textFont_.lineHeight() + pad * 2.0f;
        float bx = btn.x + btn.w + 6.0f * dpiScale;
        float by = btn.y + (btn.h - bh) * 0.5f;
        SDL_FRect box = { bx, by, bw, bh };
        setColor(renderer_, colors().tooltipBg);
        jplay::fillRect(renderer_, &box);
        setColor(renderer_, colors().border);
        jplay::drawRect(renderer_, &box);
        drawText(bx + pad, by + pad, colors().topbtnTextHover, text);
    };
    tip(dirButtonRect_,      "Project Explorer");
    tip(clipSourceButtonRect_, "Clip Source");
    tip(gradeButtonRect_,    "Color Grading");
    tip(techButtonRect_,     "Tech Check");
    tip(pencilBtnRect_,      "Draw");
    tip(settingsButtonRect_, "Settings");
    tip(sessionButtonRect_,  "Sync Review");
}
