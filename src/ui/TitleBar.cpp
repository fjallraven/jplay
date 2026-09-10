#include "TitleBar.h"
#include "AppInternal.h" // jplay::dpiScale
#include "SkinColors.h"

bool TitleBar::isMaximized() const {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
}

void TitleBar::toggleMaximize() {
    if (!window_)
        return;
    if (isMaximized())
        SDL_RestoreWindow(window_);
    else
        SDL_MaximizeWindow(window_);
}

int TitleBar::buttonAt(float x, float y) const {
    for (int i = 0; i < kButtonCount; ++i) {
        const SDL_FRect& rect = btnRect_[i];
        if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h)
            return i;
    }
    return -1;
}

void TitleBar::layout(float winW) {
    winW_ = winW;
    float barH = height();
    float btnW = kBtnW * jplay::dpiScale;
    float x = winW - kButtonCount * btnW;
    for (int i = 0; i < kButtonCount; ++i)
        btnRect_[i] = SDL_FRect{ x + (float)i * btnW, 0.0f, btnW, barH };
}

bool TitleBar::handleEvent(const SDL_Event& e, bool* shouldQuit) {
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        hover_ = buttonAt(e.motion.x, e.motion.y);
        return false; // don't swallow motion; just track hover
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e.button.button != SDL_BUTTON_LEFT)
            return false;
        int b = buttonAt(e.button.x, e.button.y);
        if (b < 0)
            return false;
        switch (b) {
        case kMin:
            SDL_MinimizeWindow(window_);
            break;
        case kMax:
            toggleMaximize();
            break;
        case kClose:
            if (shouldQuit) *shouldQuit = true;
            break;
        }
        return true;
    }
    default:
        return false;
    }
}

void TitleBar::render(SDL_Renderer* r) {
    float barH = height();
    float s = jplay::dpiScale;
    // A left-to-right ramp between two palette colours. Equal stops give a flat
    // bar, which is what the shipped palette asks for.
    const SDL_Color& cl = jplay::colors().titlebarLeft;
    const SDL_Color& cr = jplay::colors().titlebarRight;
    auto fcol = [](const SDL_Color& c) {
        return SDL_FColor{ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
    };
    const SDL_FColor fl = fcol(cl), fr = fcol(cr);
    const SDL_Vertex verts[4] = {
        { { 0.0f,  0.0f }, fl, {} },
        { { winW_, 0.0f }, fr, {} },
        { { winW_, barH }, fr, {} },
        { { 0.0f,  barH }, fl, {} },
    };
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(r, nullptr, verts, 4, idx, 6);

    // The bottom divider is the bar's edge against what follows, drawn by hand
    // now that no art carries it.
    const SDL_Color& div = jplay::colors().titleDivider;
    SDL_SetRenderDrawColor(r, div.r, div.g, div.b, div.a);
    jplay::drawLine(r, 0.0f, barH - 0.5f, winW_, barH - 0.5f);

    // Centered title (the menu lives on the left, controls on the right).
    if (textFont_) {
        float titleW = textFont_->measure(r, title_.c_str());
        float glyphH = textFont_->lineHeight();
        textFont_->draw(r, (winW_ - titleW) * 0.5f, (barH - glyphH) * 0.5f,
                        {205, 207, 213, 255}, title_.c_str());
    }

    for (int i = 0; i < kButtonCount; ++i) {
        const SDL_FRect& rect = btnRect_[i];
        if (i == hover_) {
            if (i == kClose)
                SDL_SetRenderDrawColor(r, 200, 50, 50, 255);
            else
                SDL_SetRenderDrawColor(r, 70, 72, 80, 255);
            jplay::fillRect(r, &rect);
        }

        float cx = rect.x + rect.w * 0.5f;
        float cy = rect.y + rect.h * 0.5f;
        SDL_SetRenderDrawColor(r, 225, 228, 235, 255);
        switch (i) {
        case kMin:
            jplay::drawLine(r, cx - 5.0f * s, cy, cx + 5.0f * s, cy);
            break;
        case kMax:
            if (isMaximized()) {
                SDL_FRect front{ cx - 5.0f * s, cy - 3.0f * s, 8.0f * s, 8.0f * s };
                jplay::drawRect(r, &front);
                // hint of the window behind: top and right edges, offset up-right
                jplay::drawLine(r, cx - 1.0f * s, cy - 5.0f * s, cx + 5.0f * s, cy - 5.0f * s);
                jplay::drawLine(r, cx + 5.0f * s, cy - 5.0f * s, cx + 5.0f * s, cy + 1.0f * s);
            } else {
                SDL_FRect sq{ cx - 5.0f * s, cy - 5.0f * s, 10.0f * s, 10.0f * s };
                jplay::drawRect(r, &sq);
            }
            break;
        case kClose:
            jplay::drawLine(r, cx - 5.0f * s, cy - 5.0f * s, cx + 5.0f * s, cy + 5.0f * s);
            jplay::drawLine(r, cx - 5.0f * s, cy + 5.0f * s, cx + 5.0f * s, cy - 5.0f * s);
            break;
        }
    }
}
