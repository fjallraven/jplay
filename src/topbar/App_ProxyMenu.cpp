// Top-bar "Proxy" dropdown: the global media-representation mode (ProxyMode.h)
// that every clip's decoder reopens against when it changes (see
// Media::ensureOpen). Deliberately its own popup rather than a reuse of the
// OCIO list-menu helper (App_Ocio.cpp's renderOcioListMenu /
// ocioListMenuHandleEvent): that helper identifies and labels every option by
// the same string, but a proxy mode needs a separate value (round-trips to
// Python's resolve_proxy_path) and label (display-only), so this picks by
// index instead. Popup mechanics — anchor below the button, scroll, hover,
// panel styling — mirror that shared helper's body. The button itself is
// drawn in App_TopBar.cpp, immediately left of Letterbox.
// All members of App; split out of App.cpp for the same reasons as the other App_*.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "Media.h"
#include "ProxyMode.h"
#include "PythonBridge.h"
#include "PythonStartup.h"
#include "TopBarStyle.h"

#include <algorithm>

using namespace jplay;

// The display label for a mode value: the list's, or "Full" for the built-in
// empty one. A project loaded with a non-Full mode already picked can ask before
// list_proxy_modes() has been consulted (that only happens lazily, on the
// dropdown's first open) — the raw value stands in until then.
std::string App::proxyLabelFor(const std::string& value) const {
    if (value.empty())
        return "Full";
    for (const auto& m : proxyModes_)
        if (m.value == value)
            return m.label;
    return value;
}

bool App::proxyModeOfPath(const std::string& path, std::string& outMode) {
    if (path.empty())
        return false;
    auto it = proxyPathModes_.find(path);
    if (it == proxyPathModes_.end()) {
        std::string mode;
        // Not memoised while the interpreter is still coming up: that is a
        // "no answer yet", not an answer, and caching it would leave the button
        // silent for the rest of the run.
        if (!jplayPythonReady())
            return false;
        it = proxyPathModes_.emplace(path, jplayProxyPathMode(path, mode)
                                               ? std::optional<std::string>(mode)
                                               : std::nullopt).first;
    }
    if (!it->second)
        return false;
    outMode = *it->second;
    return true;
}

// The selected mode, and — when the clip on screen is demonstrably not on it —
// what it is on instead: "Proxy (Full)" for a shot with no proxy publish, which
// decodes its nominal path under every mode (resolve_proxy_path answers None and
// Media falls back silently). Only a positive classification annotates: a file
// the naming config does not recognise, or a site that registers no
// proxy_path_mode at all, leaves the plain mode label as before rather than
// crying fallback over media the convention simply has no opinion on.
//
// This is per clip, so the label changes as the playhead crosses into a shot
// whose publish differs — deliberately: the button states the mode that was
// asked for, which is what clicking it edits, and only appends what the frame in
// front of you actually is.
std::string App::proxyModeLabel() {
    const std::string sel = proxyLabelFor(timeline_.proxyMode);
    auto m = playheadMedia();
    if (!m || !m->isOpen())
        return sel; // nothing decoded to classify (yet)
    std::string actual;
    if (!proxyModeOfPath(m->resolvedPath(), actual) || actual == timeline_.proxyMode)
        return sel;
    buildProxyModes(); // the parenthetical needs the mode list's labels
    // The list's labels are written to stand alone in the menu ("Quicktime
    // (h264)"), which nests badly inside a second pair of brackets, so only the
    // part before the first " (" goes in the button.
    std::string act = proxyLabelFor(actual);
    act = act.substr(0, act.find(" ("));
    return sel + " (" + act + ")";
}

// Consults Python at most once per run, and only from here — never at
// startup or on every layout pass — so a site with no proxy config pays
// nothing for this feature. "Full" is always first and is never supplied by
// Python; it is the only mode that exists when no callback is registered.
void App::buildProxyModes() {
    if (proxyModesBuilt_)
        return;
    proxyModesBuilt_ = true;
    proxyModes_.clear();
    proxyModes_.push_back({ "", "Full", 0 });
    std::vector<ProxyModeOption> py;
    if (jplayListProxyModes(py))
        for (auto& m : py)
            proxyModes_.push_back({ std::move(m.value), std::move(m.label), m.slateFrames,
                                    m.isDefault });
}

// A project that carries no proxy selection of its own — File > New, an .otio
// import, a .jpproj saved before the dropdown existed — opens on the mode the
// naming config marks with "default": true rather than on Full: where the media
// under review is published as proxies, full-res is the exception. Nothing to
// adopt while the feature is off, or over a mode the project does record.
//
// Full and "no selection" are the same stored value (""), so a project saved
// while the dropdown said Full reopens on the default mode rather than on Full.
// Deliberate: the mode is a review preference, not project data, and giving Full
// a distinct persisted value to tell the two apart would change the file format
// and the ""-is-Full contract in ProxyMode.h for a one-session preference.
void App::adoptDefaultProxyMode() {
    pendingProxyDefault_ = false;
    if (!proxyEnabled_ || !timeline_.proxyMode.empty())
        return;
    if (!jplayPythonReady()) {
        // Asked too early to have an answer — a project named on the command line
        // loads during init(), before run() has even started the interpreter. Left
        // for the run loop rather than resolved to Full here.
        pendingProxyDefault_ = true;
        return;
    }
    buildProxyModes();
    for (const auto& m : proxyModes_)
        if (m.isDefault) {
            timeline_.proxyMode = m.value;
            break;
        }
}

// The deferred half of the above, run from the run loop once Python is up. Unlike
// the load-path call it lands on a project whose frames are already decoded (as
// Full), so it goes through applyProxyMode; and it lands after finishLoad stamped
// the unsaved-changes baseline, so a project that was clean is re-stamped —
// adopting a default is not an edit the user made.
void App::applyPendingProxyDefault() {
    const bool wasClean = (savedSignature_ == projectSignature());
    adoptDefaultProxyMode();
    if (timeline_.proxyMode.empty())
        return; // no mode claims the default: the project stays on Full
    applyProxyMode();
    if (wasClean)
        savedSignature_ = projectSignature();
}

void App::pushProxyMode() {
    const std::string mode = proxyEnabled_ ? timeline_.proxyMode : "";
    int64_t slate = 0;
    if (!mode.empty()) {
        // The slate count arrives with the mode list, so a project that opens
        // straight into a proxy mode is the one case that consults Python before
        // the dropdown is ever opened. Full costs nothing, as before.
        buildProxyModes();
        for (const auto& m : proxyModes_)
            if (m.value == mode) {
                slate = m.slate;
                break;
            }
    }
    // Slate first: setProxyMode bumps the generation, and the worker that sees the
    // bump reopens against the new mode expecting the new mode's slate.
    jplay::setProxySlateFrames(slate);
    jplay::setProxyMode(mode);
}

void App::openProxyMenu() {
    buildProxyModes();
    proxyMenuOpen_ = !proxyMenuOpen_; // the button toggles the popup
    proxyHoverRow_ = -1;
    proxyMenuScroll_ = 0.0f;
}

void App::applyProxyMode() {
    pushProxyMode();
    cache_->clear();               // drop decoded pixels from the old mode
    if (gridView())
        startClipThumbnails();     // regenerate tiles under the new mode
    hasTexture_ = false;            // the player re-renders through the new source
}

void App::renderProxyMenu() {
    if (!proxyMenuOpen_)
        return;
    if (proxyModes_.empty()) { // list_proxy_modes/"Full" both failed to populate
        proxyMenuOpen_ = false;
        return;
    }

    const float pad = 10.0f * dpiScale;
    const float lineH = textFont_.lineHeight();
    const float rowH = lineH + 6.0f * dpiScale;
    const float gap = 8.0f * dpiScale;
    const float checkW = 16.0f * dpiScale;
    const float mark = 6.0f * dpiScale;
    const char* header = "PROXY";

    float contentW = textFont_.measure(renderer_, header);
    for (const auto& m : proxyModes_)
        contentW = std::max(contentW, checkW + textFont_.measure(renderer_, m.label.c_str()));
    float panelW = std::max(contentW + pad * 2.0f, 170.0f * dpiScale);

    float panelX = proxyBtnRect_.x;
    float panelY = proxyBtnRect_.y + proxyBtnRect_.h;
    if (panelX + panelW > winW_ - 4.0f)
        panelX = winW_ - panelW - 4.0f;
    panelX = std::max(panelX, 4.0f);

    const float headH = pad + lineH + gap;
    const float footH = pad;
    float fullRowsH = (float)proxyModes_.size() * rowH;
    float maxPanelH = std::max(headH + rowH + footH, winH_ - panelY - 6.0f * dpiScale);
    float panelH = std::min(headH + fullRowsH + footH, maxPanelH);
    float viewH = panelH - headH - footH;
    float maxScroll = std::max(0.0f, fullRowsH - viewH);
    proxyMenuScroll_ = std::clamp(proxyMenuScroll_, 0.0f, maxScroll);

    proxyMenuRect_ = { panelX, panelY, panelW, panelH };
    jplay::drawPopupPanel(renderer_, proxyMenuRect_);

    SDL_FRect body = proxyMenuRect_;
    gapTop(body, pad);
    SDL_FRect hdr = cutTop(body, lineH);
    drawText(hdr.x + pad, hdr.y, kHeader, header);
    gapTop(body, gap);
    gapBottom(body, footH);
    const SDL_FRect view = body;

    SDL_Rect clip = { (int)view.x, (int)view.y, (int)view.w, (int)view.h };
    SDL_SetRenderClipRect(renderer_, &clip);

    SDL_FRect column = { view.x + 2.0f, view.y - proxyMenuScroll_, view.w - 4.0f, kUnbounded };
    proxyRowRects_.assign(proxyModes_.size(), SDL_FRect{});
    for (size_t i = 0; i < proxyModes_.size(); ++i) {
        SDL_FRect row = cutTop(column, rowH);
        if (!visibleIn(row, view))
            continue;
        proxyRowRects_[i] = row;
        bool current = (proxyModes_[i].value == timeline_.proxyMode);
        if ((int)i == proxyHoverRow_) {
            jplay::drawRowHover(renderer_, row);
        } else if (current) {
            setColor(renderer_, kCurrent);
            jplay::fillRect(renderer_, &row);
        }
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
        drawText(lbl.x, lbl.y, kLabel, proxyModes_[i].label.c_str());
    }
    SDL_SetRenderClipRect(renderer_, nullptr);

    drawScrollbar(renderer_, view, fullRowsH, proxyMenuScroll_, dpiScale, false, proxyMenuSb_);
}

bool App::proxyMenuHandleEvent(const SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) {
            proxyMenuOpen_ = false; proxyHoverRow_ = -1;
            return true;
        }
        float mx = e.button.x, my = e.button.y;
        if (inRect(proxyBtnRect_, mx, my))
            return false; // let the button handler toggle the popup closed
        if (!inRect(proxyMenuRect_, mx, my)) {
            proxyMenuOpen_ = false; proxyHoverRow_ = -1;
            return true;
        }
        if (proxyMenuSb_.press(mx, my, proxyMenuScroll_, dpiScale))
            return true;
        for (size_t i = 0; i < proxyRowRects_.size() && i < proxyModes_.size(); ++i) {
            if (inRect(proxyRowRects_[i], mx, my)) {
                timeline_.proxyMode = proxyModes_[i].value;
                applyProxyMode();
                proxyMenuOpen_ = false; proxyHoverRow_ = -1;
                return true;
            }
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        float mx = e.motion.x, my = e.motion.y;
        if (proxyMenuSb_.dragging) {
            proxyMenuSb_.drag(my, proxyMenuScroll_, dpiScale);
            return true;
        }
        proxyHoverRow_ = -1;
        for (int i = 0; i < (int)proxyRowRects_.size(); ++i)
            if (inRect(proxyRowRects_[i], mx, my)) {
                proxyHoverRow_ = i;
                break;
            }
        return inRect(proxyMenuRect_, mx, my);
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (inRect(proxyMenuRect_, e.wheel.mouse_x, e.wheel.mouse_y)) {
            const float rowH = textFont_.lineHeight() + 6.0f * dpiScale;
            proxyMenuScroll_ -= e.wheel.y * rowH * 2.0f; // clamped in renderProxyMenu
            if (proxyMenuScroll_ < 0.0f)
                proxyMenuScroll_ = 0.0f;
            return true;
        }
        return false;
    }
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE) {
            proxyMenuOpen_ = false; proxyHoverRow_ = -1;
            return true;
        }
        return false;
    default:
        return false;
    }
}
