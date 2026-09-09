// Nav-panel translation unit: the fixed left icon strip (kSidePanelW) and the
// registry of panes its toggle glyphs switch between -- Project Explorer, Clip
// Source, Color Grading, Tech Check, Draw, Sync Review, Settings -- plus the
// strip's layout, hit-testing and hover tooltips.
//
// registerLeftPanels() below is the single list. The strip, the View > Panels
// menu, cinema mode, the layout and event dispatch all read it, so a pane is
// added by appending one entry here and writing its render/handleEvent in its
// own translation unit. Nothing else needs to learn the pane exists. See
// docs/ADDING_A_PANEL.md.

#include "App.h"
#include "AppInternal.h"
#include "IconFont.h"
#include "Layout.h"
#include "SkinColors.h"

#include <algorithm>

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

// ─── The registry ────────────────────────────────────────────────────────────
// Built once, from App::init. Order here is the order the toggles stack down
// the strip and the order the View > Panels submenu lists them; the
// LeftPanelId enum in App.h names the indices, and the two must agree.
//
// To add a pane: write its render (and, if it has widgets, its handleEvent) in
// a new src/panels/App_*.cpp, declare them in App.h, add the file to
// CMakeLists.txt, and append an entry below. If code outside your pane's own
// file needs to name it, add it to LeftPanelId as well -- most panes do not.
void App::registerLeftPanels() {
    leftPanels_.clear();

    jplay::LeftPanelDesc pe;
    pe.id = "project-explorer";
    pe.label = "Project Explorer";
    pe.icon = iconCp(ICON_MDI_FILE_MULTIPLE);
    pe.resizable = true;
    pe.render = [this] { renderProjectExplorer(); };
    pe.handleEvent = [this](const SDL_Event& e) { return projectExplorerHandleEvent(e); };
    pe.handleEventEarly = [this](const SDL_Event& e) { return projectTreeHandleEvent(e); };
    pe.onOpen = [this] { refreshExplorerOrder(); }; // re-sort shots on open
    // Three scrollable regions: the MEDIA info sub-panel when the cursor is
    // over it, otherwise whichever tab is showing.
    pe.onWheel = [this](float d, float mx, float my) {
        const float step = d * 48.0f;
        if (peActiveTab_ == PeTabSources && inRect(sourceInfoRect_, mx, my))
            sourceInfoScroll_ = std::max(0.0f, sourceInfoScroll_ - step);
        else if (peActiveTab_ == PeTabSources)
            peSourceScroll_ = std::max(0.0f, peSourceScroll_ - step);
        else if (peActiveTab_ == PeTabSequences)
            peSeqScroll_ = std::max(0.0f, peSeqScroll_ - step);
    };
    leftPanels_.push_back(pe);

    jplay::LeftPanelDesc cs;
    cs.id = "clip-source";
    cs.label = "Clip Source";
    cs.icon = iconCp(ICON_MDI_LAYERS_TRIPLE);
    cs.resizable = true;
    cs.render = [this] {
        updateClipSourceData(); // re-describe the pickers when the active media changed
        renderClipSourcePanel();
    };
    cs.handleEvent = [this](const SDL_Event& e) { return clipSourceHandleEvent(e); };
    cs.onOpen = [this] {
        clipSourceDirty_ = true; // describe on the next render
        clipSourceScroll_ = 0.0f;
    };
    cs.onClose = [this] { closeClipSource(); };
    cs.onWheel = [this](float d, float, float) { // upper clamp in renderClipSourcePanel
        clipSourceScroll_ = std::max(0.0f, clipSourceScroll_ - d * 48.0f);
    };
    leftPanels_.push_back(cs);

    jplay::LeftPanelDesc grade;
    grade.id = "grade";
    grade.label = "Color Grading";
    grade.icon = iconCp(ICON_MDI_CHART_BELL_CURVE);
    grade.render = [this] { renderGradePanel(); };
    grade.handleEvent = [this](const SDL_Event& e) { return gradeHandleEvent(e); };
    grade.onWheel = [this](float d, float, float) { // upper clamp in renderGradePanel
        gradeScroll_ = std::max(0.0f, gradeScroll_ - d * 48.0f);
    };
    leftPanels_.push_back(grade);

    jplay::LeftPanelDesc tech;
    tech.id = "tech-check";
    tech.label = "Tech Check";
    tech.icon = iconCp(ICON_MDI_MONITOR_EYE);
    tech.render = [this] { renderTechPanel(); };
    tech.handleEvent = [this](const SDL_Event& e) { return techHandleEvent(e); };
    tech.handleEventEarly = [this](const SDL_Event& e) { return techNitHandleEvent(e); };
    // Brightened while a mode is live, so the strip shows tech check is on even
    // with the pane shut.
    tech.iconTint = [this](bool isOpen, SDL_Color fallback) {
        if (isOpen)
            return fallback;
        return techMode_ != TechMode::None ? kWarn : fallback;
    };
    leftPanels_.push_back(tech);

    jplay::LeftPanelDesc draw;
    draw.id = "draw";
    draw.label = "Draw";
    draw.icon = iconCp(ICON_MDI_BRUSH);
    draw.render = [this] { renderDrawPanel(); };
    // The draw pane's widgets are click-only (Clear / size / hue), and the
    // press handler wants the cursor, not the event.
    draw.handleEvent = [this](const SDL_Event& e) {
        if (e.type != SDL_EVENT_MOUSE_BUTTON_DOWN || e.button.button != SDL_BUTTON_LEFT)
            return false;
        return drawPanelHandlePress(e.button.x, e.button.y);
    };
    leftPanels_.push_back(draw);

    jplay::LeftPanelDesc sync;
    sync.id = "sync-review";
    sync.label = "Sync Review";
    sync.icon = iconCp(ICON_MDI_ACCOUNT_GROUP);
    sync.render = [this] { renderSessionPanel(); };
    // The pane's widgets are text fields and pickers that must keep receiving
    // keystrokes wherever the cursor is, so it takes events early and has no
    // separate in-pane handler.
    sync.handleEventEarly = [this](const SDL_Event& e) { return sessionHandleEvent(e); };
    // The pane owns text fields; leaving it must give text input back.
    sync.onClose = [this] { SDL_StopTextInput(window_); };
    // Green while a session is live, whether or not the pane is open.
    sync.iconTint = [this](bool isOpen, SDL_Color fallback) {
        if (syncSession_.role() != syncreview::Role::None)
            return SDL_Color{ 90, 170, 110, 255 };
        return fallback;
    };
    leftPanels_.push_back(sync);

    jplay::LeftPanelDesc settings;
    settings.id = "settings";
    settings.label = "Settings";
    settings.icon = iconCp(ICON_MDI_COG);
    settings.render = [this] { renderSettingsPanel(); };
    settings.handleEvent = [this](const SDL_Event& e) { return settingsHandleEvent(e); };
    // The three editable combos (FPS, UI Scale, cache size) open dropdowns that
    // reach past the pane, so they are fed before the rest of the app.
    settings.handleEventEarly = [this](const SDL_Event& e) {
        return settingsFpsHandleEvent(e) || settingsUiScaleHandleEvent(e)
               || settingsCacheHandleEvent(e);
    };
    settings.onWheel = [this](float d, float, float) { // upper clamp in renderSettingsPanel
        settingsScroll_ = std::max(0.0f, settingsScroll_ - d * 48.0f);
    };
    leftPanels_.push_back(settings);

    // The enum names indices into what was just built; if they disagree, every
    // panelOpen() test in the app is quietly wrong.
    SDL_assert(leftPanels_.size() == (size_t)kPanelBuiltinCount);
}

// ─── Opening and closing ─────────────────────────────────────────────────────
// The panes are mutually exclusive, and this is the only place that enforces
// it: openLeftPanel closes whatever was open before opening anything, so no
// caller has to know which panes exist. Everything that opens or closes a pane
// -- the strip, the View menu, cinema mode, compact timeline -- comes through
// here.

void App::openLeftPanel(int id) {
    if (id == openPanel_)
        return;
    if (openPanel_ >= 0 && openPanel_ < (int)leftPanels_.size()) {
        if (auto& onClose = leftPanels_[openPanel_].onClose)
            onClose();
    }
    openPanel_ = (id >= 0 && id < (int)leftPanels_.size()) ? id : -1;
    if (openPanel_ >= 0) {
        if (auto& onOpen = leftPanels_[openPanel_].onOpen)
            onOpen();
    }
}

// A pane's own toggle -- its strip button or its View menu row: picking the
// open one closes it.
void App::toggleLeftPanel(int id) {
    openLeftPanel(panelOpen(id) ? -1 : id);
}

// Width of the open pane, 0 when none is. Panes with no width of their own
// take the shared default; a resizable pane the user has dragged keeps what
// they set, held to the same limits the drag applies.
float App::leftPaneWidth(int id) const {
    if (id < 0 || id >= (int)leftPanels_.size())
        return 0.0f;
    const jplay::LeftPanelDesc& d = leftPanels_[id];
    // A pane that named no width of its own takes the shared default, which
    // follows the DPI scale between fixed bounds.
    const float base = d.defaultW > 0.0f
                           ? d.defaultW * dpiScale
                           : std::clamp(std::round(kLeftPanelW * dpiScale),
                                        kLeftPaneDefMinW, kLeftPaneDefMaxW);
    if (!d.resizable || d.userW <= 0.0f)
        return base;
    return std::clamp(d.userW, kLeftPaneMinW, winW_ * 0.5f);
}

float App::openPanelW() const {
    return openPanel_ < 0 ? 0.0f : leftPaneWidth(openPanel_);
}

// The open pane's rect: from just below the title bar down to the timeline,
// starting at the strip's right edge.
SDL_FRect App::leftPaneRect() const {
    const float topH = titleBar_.height();
    return { kSidePanelW, topH, openPanelW(), panelsBottom_ - topH };
}

// ─── Strip layout ────────────────────────────────────────────────────────────
// One column of toggles, anchored just below the title bar and stacked in
// registry order. The column ends at the timeline, not at the window bottom: a
// short window would otherwise carry on stacking toggles down across the
// timeline's info bar. Once the room runs out the remaining toggles are
// skipped outright rather than squashed -- a zero rect draws no glyph and
// hit-tests false, so the button simply is not there at that window height.
//
// A button spans the full strip width and runs 4px taller than the square its
// glyph is sized to, so the hover/active fill reads as a band across the strip.
// Those 4px come out of the gap below, leaving the column pitch unchanged. The
// glyph square is kNavIconSide, not derived from the strip width, so the strip
// can be narrowed (its side padding is what shrinks) without the icons
// following it down.
void App::layoutLeftPanelStrip(float topH) {
    const float pad = 8.0f * dpiScale; // top inset and the gap between buttons
    const float btnSide = kNavIconSide * dpiScale;
    const float btnH = btnSide + 4.0f * dpiScale;
    SDL_FRect strip = { 0.0f, topH + pad, kSidePanelW, panelsBottom_ - topH - pad };
    for (jplay::LeftPanelDesc& d : leftPanels_) {
        if (strip.h < btnH) {
            d.btnRect = SDL_FRect{}; // no whole button's worth of room left
            continue;
        }
        d.btnRect = cutTop(strip, btnH);
        gapTop(strip, pad - 4.0f * dpiScale);
    }
}

// Which pane's toggle a point falls on, or -1.
int App::leftPanelAt(float mx, float my) const {
    for (size_t i = 0; i < leftPanels_.size(); ++i) {
        if (leftPanels_[i].btnRect.w > 0.0f && inRect(leftPanels_[i].btnRect, mx, my))
            return (int)i;
    }
    return -1;
}

// ─── Resize edges ────────────────────────────────────────────────────────────
// The hit zone straddles the open pane's right edge, kOverviewHandleHitW/2 into
// either side, and runs the pane's full height. Only a pane that asked to be
// resizable has one.
int App::panelResizeEdgeAt(float mx, float my) const {
    if (my < titleBar_.height() || my >= panelsBottom_)
        return -1;
    if (openPanel_ < 0 || !leftPanels_[openPanel_].resizable)
        return -1;
    const float half = kOverviewHandleHitW * 0.5f;
    if (std::abs(mx - (kSidePanelW + openPanelW())) <= half)
        return openPanel_;
    return -1;
}

// ─── Rendering ───────────────────────────────────────────────────────────────

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
    for (size_t i = 0; i < leftPanels_.size(); ++i) {
        const jplay::LeftPanelDesc& d = leftPanels_[i];
        const SDL_FRect& btn = d.btnRect;
        if (btn.w <= 0.0f)
            continue;
        const bool active = openPanel_ == (int)i;
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
        SDL_Color col = active ? kActive : kIdle;
        if (d.iconTint)
            col = d.iconTint(active, col);
        const float side = btn.h - 4.0f * dpiScale; // the square the glyph is sized to
        SDL_FRect g = { btn.x + (btn.w - side) * 0.5f, btn.y + 2.0f * dpiScale, side, side };
        icons_.drawGlyph(renderer_, d.icon, g, col);
    }

    // Edge line last.
    setColor(renderer_, kDivider);
    jplay::drawLine(renderer_, kSidePanelW - 0.5f, topH, kSidePanelW - 0.5f, panelsBottom_);
}

// The open pane draws itself, into the rect the layout reserved for it.
void App::renderLeftPanel() {
    if (openPanel_ < 0)
        return;
    if (auto& render = leftPanels_[openPanel_].render)
        render();
}

// Hover labels for the left icon strip, drawn to the right of the hovered
// button. Called after the panels render (see App::run) so the label overlays
// whichever panel is open beside the strip rather than being covered by it.
void App::renderIconStripTooltips() {
    float mx = 0.0f, my = 0.0f;
    uiMouse(mx, my);
    const int hit = leftPanelAt(mx, my);
    if (hit < 0)
        return;
    const jplay::LeftPanelDesc& d = leftPanels_[hit];
    const SDL_FRect& btn = d.btnRect;
    const float pad = 5.0f * dpiScale;
    float bw = textFont_.measure(renderer_, d.label) + pad * 2.0f;
    float bh = textFont_.lineHeight() + pad * 2.0f;
    float bx = btn.x + btn.w + 6.0f * dpiScale;
    float by = btn.y + (btn.h - bh) * 0.5f;
    SDL_FRect box = { bx, by, bw, bh };
    setColor(renderer_, colors().tooltipBg);
    jplay::fillRect(renderer_, &box);
    setColor(renderer_, colors().border);
    jplay::drawRect(renderer_, &box);
    drawText(bx + pad, by + pad, colors().topbtnTextHover, d.label);
}
