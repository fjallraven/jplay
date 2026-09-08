// Top toolbar: the horizontal button strip between the title bar and the video
// frame. This translation unit owns renderTopBar (which draws every toolbar
// button — Sequence, the four OCIO buttons, Proxy, and Letterbox),
// renderTopBarTooltips (the hover labels under them) and
// closeTopBarPopups (which dismisses all of the popups those buttons open). The
// popups themselves live in App_Letterbox.cpp, App_Ocio.cpp, App_ProxyMenu.cpp,
// and App_SequenceMenu.cpp.
// All members of App; split out of App.cpp for the same reasons as the other App_*.

#include "App.h"
#include "AppInternal.h"
#include "Layout.h"
#include "SkinColors.h"
#include "TopBarStyle.h"

#include <algorithm>
#include <string>

using namespace jplay;

// ─── Top toolbar ─────────────────────────────────────────────────────────────

// Every button on the bar draws the same way, so the differences (icon, label,
// state, caret) are the parameters and the geometry is a single rect-cut: the
// icon takes a square off the left edge and the label is centered in what is
// left — or, with no label at all, the glyph centres in the whole button.
// A zero-width box means the button is not on offer this frame (an OCIO
// config that isn't loaded, an "Open …" target that isn't there) — computeLayout
// signals that by leaving the rect empty, so there is nothing to draw.
void App::drawTopBarBtn(const SDL_FRect& box, uint32_t icon, const std::string& label,
                        bool hovered, bool active, bool caret) {
    if (box.w <= 0.0f || box.h <= 0.0f)
        return;
    if (active)
        setColor(renderer_, hovered ? colors().topbtnOnHover : colors().topbtnOnBg);
    else
        setColor(renderer_, hovered ? colors().topbtnBgHover : colors().topbtnBg);
    jplay::fillRect(renderer_, &box);
    setColor(renderer_, active ? colors().topbtnOnBorder : colors().border);
    jplay::drawRect(renderer_, &box);

    const SDL_Color col = active  ? colors().topbtnOnText
                        : hovered ? colors().topbtnTextHover
                                  : colors().topbtnText;
    if (label.empty()) {
        // Icon-only (Letterbox): with no label to sit beside, the glyph centres in
        // what the caret leaves rather than taking a square off the left edge.
        // drawGlyph derives its padding from the rect's *width*, so that centred
        // well has to stay square or the glyph would shrink to the button height.
        SDL_FRect well = box;
        cutRight(well, kTopBarCaretW * dpiScale);
        well = SDL_FRect{ well.x + (well.w - box.h) * 0.5f, well.y, box.h, box.h };
        icons_.drawGlyph(renderer_, icon, well, col, 0.2f);
    } else {
        SDL_FRect inner = box;
        SDL_FRect well = cutLeft(inner, box.h);
        icons_.drawGlyph(renderer_, icon, well, col, 0.2f);
        SDL_FRect text = centerV(inner, textFont_.lineHeight());
        drawText(text.x, text.y, col, label);
    }
    if (caret)
        drawTopBarCaret(renderer_, box, col, dpiScale);
}

void App::renderTopBar() {
    if (topBarRect_.w <= 0.0f || topBarRect_.h <= 0.0f)
        return;
    // Bar background + a 1px separator along the bottom edge.
    setColor(renderer_, kPanelBg);
    jplay::fillRect(renderer_, &topBarRect_);
    setColor(renderer_, colors().topbarDivider);
    jplay::drawLine(renderer_, topBarRect_.x, topBarRect_.y + topBarRect_.h - 0.5f,
                   topBarRect_.x + topBarRect_.w, topBarRect_.y + topBarRect_.h - 0.5f);

    // Sequence view-filter button: icon + the scoped sequence or project name (or
    // "All") as its label. Opens with the "V" key as well as a click.
    // ICON_MDI_FILMSTRIP (U+F0230) — named here so the icon subsetter keeps it.
    drawTopBarBtn(sequenceBtnRect_, 0xF0230, sequenceViewLabel(),
                  hoveredSequence_ || sequenceMenuOpen_, false, true);

    // "Open Project: <name>" / "Open Sequence: <name>": one-click shortcuts for the
    // Sequence popup's two "Open …" rows, right of the Sequence view-filter button.
    // Both are laid out only when there is something to open (computeLayout ->
    // resolveOpenTargets), so a zero-width rect means "not on offer" here. No caret
    // since they act at once rather than opening a list.
    // ICON_MDI_FOLDER_OPEN (U+F0770) — named here so the icon subsetter keeps it.
    drawTopBarBtn(openProjBtnRect_, 0xF0770, "Open Project: " + openProjName_,
                  hoveredOpenProj_, false, false);
    drawTopBarBtn(openSeqBtnRect_, 0xF0770, "Open Sequence: " + openSeqName_,
                  hoveredOpenSeq_, false, false);

    // The four OCIO pickers, each labeled with the transform it currently has
    // selected. Empty rects while the config is unloaded or disabled.
    // ICON_MDI_MONITOR (U+F0379), ICON_MDI_TUNE (U+F062E),
    // ICON_MDI_PALETTE_SWATCH (U+F08B5), ICON_MDI_PALETTE (U+F03D8)
    // — named here so the icon subsetter keeps them.
    drawTopBarBtn(ocioDisplayBtnRect_, 0xF0379,
                  ocio_.activeDisplay().empty() ? "Display" : ocio_.activeDisplay(),
                  hoveredOcioDisplay_ || ocioDisplayMenuOpen_, false, true);
    drawTopBarBtn(ocioViewBtnRect_, 0xF062E,
                  ocio_.activeView().empty() ? "View" : ocio_.activeView(),
                  hoveredOcioView_ || ocioViewMenuOpen_, false, true);
    drawTopBarBtn(ocioLookBtnRect_, 0xF08B5,
                  ocio_.activeLook().empty() ? "Look" : ocio_.activeLook(),
                  hoveredOcioLook_ || ocioLookMenuOpen_, false, true);
    drawTopBarBtn(ocioInputCsBtnRect_, 0xF03D8, ocioInputCsLabel(),
                  hoveredOcioInputCs_ || ocioInputCsMenuOpen_, false, true);

    // Proxy: the global media-representation mode, left of Letterbox. Labelled with
    // the mode alone ("Full", "Proxy", ...), like the OCIO pickers above and for the
    // same reason: prefixing it with the category reads "Proxy: Proxy" in the very
    // mode most of this site's reviews run in. The icon carries the category, and
    // the popup's own header spells it out.
    // ICON_MDI_IMAGE_SIZE_SELECT_SMALL (U+F0C8F) — named here so the icon subsetter keeps it.
    drawTopBarBtn(proxyBtnRect_, 0xF0C8F, proxyModeLabel(),
                  hoveredProxy_ || proxyMenuOpen_, false, true);

    // Letterbox toggle: the one button on the bar that reads highlighted while it
    // is on (a matte ratio > 0), rather than only while hovered or open. Icon-only
    // — the aspect-ratio glyph and that active fill say what the label did, and the
    // width it gives back is what keeps the bar on one row at common window sizes.
    // The name and the active ratio move to the tooltip below.
    // ICON_MDI_ASPECT_RATIO (U+F0A24) — named here so the icon subsetter keeps it.
    drawTopBarBtn(letterboxBtnRect_, 0xF0A24, "",
                  hoveredLetterbox_ || letterboxMenuOpen_, letterboxActive(), true);
}

// ─── Tooltips ────────────────────────────────────────────────────────────────

// Hover labels for the bar's buttons, dropped just under the hovered one. Every
// button here is a picker labelled with its *value* — the display name, the proxy
// mode, the scoped sequence — rather than with what that value is for, and
// Letterbox now carries no label at all, so the category lives here. Suppressed
// while a button's own popup is open: the popup anchors to the same bottom edge
// and would bury the tip anyway.
void App::renderTopBarTooltips() {
    if (topBarRect_.w <= 0.0f || topBarRect_.h <= 0.0f)
        return;
    auto tip = [&](const SDL_FRect& btn, bool show, const std::string& text) {
        if (!show || btn.w <= 0.0f)
            return;
        const float pad = 5.0f * dpiScale;
        float bw = textFont_.measure(renderer_, text.c_str()) + pad * 2.0f;
        float bh = textFont_.lineHeight() + pad * 2.0f;
        // std::max on the upper bound: these labels are sentences, so a narrow
        // enough window would invert clamp's limits (undefined) rather than just
        // overflow the edge.
        float bx = std::clamp(btn.x + (btn.w - bw) * 0.5f, 4.0f,
                              std::max(4.0f, winW_ - bw - 4.0f));
        float by = btn.y + btn.h + 4.0f * dpiScale;
        SDL_FRect box = { bx, by, bw, bh };
        setColor(renderer_, colors().tooltipBg);
        jplay::fillRect(renderer_, &box);
        setColor(renderer_, colors().border);
        jplay::drawRect(renderer_, &box);
        drawText(bx + pad, by + pad, colors().topbtnTextHover, text);
    };
    tip(sequenceBtnRect_, hoveredSequence_ && !sequenceMenuOpen_,
        "Scope the view to a sequence (V)");
    tip(openProjBtnRect_, hoveredOpenProj_,
        "Open the project this shot was published from");
    tip(openSeqBtnRect_, hoveredOpenSeq_, "Scope the view to this shot's sequence");
    tip(ocioDisplayBtnRect_, hoveredOcioDisplay_ && !ocioDisplayMenuOpen_,
        "OCIO display device");
    tip(ocioViewBtnRect_, hoveredOcioView_ && !ocioViewMenuOpen_, "OCIO view transform");
    tip(ocioLookBtnRect_, hoveredOcioLook_ && !ocioLookMenuOpen_, "OCIO look");
    tip(ocioInputCsBtnRect_, hoveredOcioInputCs_ && !ocioInputCsMenuOpen_,
        "Colorspace the source files are read as");
    tip(proxyBtnRect_, hoveredProxy_ && !proxyMenuOpen_, "Proxy media mode");
    tip(letterboxBtnRect_, hoveredLetterbox_ && !letterboxMenuOpen_,
        "Letterbox: " + letterboxLabel());
}

// Dismiss every top-bar popup at once (e.g. on a click outside the bar). The
// individual open*/handleEvent methods live with their popups in the other units.
void App::closeTopBarPopups() {
    sequenceMenuOpen_ = false;
    proxyMenuOpen_ = false;
    ocioDisplayMenuOpen_ = false;
    ocioViewMenuOpen_ = false;
    ocioLookMenuOpen_ = false;
    ocioInputCsMenuOpen_ = false;
    letterboxMenuOpen_ = false;
    letterboxOpacityDrag_ = false;
    if (letterboxFld_.focused()) {
        letterboxFld_.setFocus(false);
        SDL_StopTextInput(window_);
    }
}
