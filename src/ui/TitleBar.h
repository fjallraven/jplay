#pragma once

#include "AppInternal.h"
#include "TextFont.h"

#include <SDL3/SDL.h>

#include <string>

// Custom window chrome for a borderless SDL3 window: a title bar with the app
// title on the left and minimize / maximize-restore / close buttons on the
// right. It operates directly on the SDL_Window it is attached to. Window
// dragging and edge resizing are handled by the host's SDL_SetWindowHitTest
// callback; this class owns only the button strip and title.
class TitleBar {
public:
    void setWindow(SDL_Window* window) { window_ = window; }
    void setTitle(const std::string& title) { title_ = title; }

    // Feed events first. Returns true if consumed (a control button was hit).
    // *shouldQuit is set true when the close button is pressed.
    bool handleEvent(const SDL_Event& e, bool* shouldQuit);

    // Maximize when restored, restore when maximized. Shared by the bar's own
    // maximize button and by the host's double-click-on-the-empty-bar gesture.
    void toggleMaximize();

    void setTextFont(TextFont* f) { textFont_ = f; }

    void layout(float winW);
    void render(SDL_Renderer* r);

    float height() const { return kBarH * jplay::dpiScale; }

    // Pixel-space hit test helper for the window's SDL_SetWindowHitTest:
    // a point over a control button must NOT start a window drag.
    bool pointOverButton(float x, float y) const { return buttonAt(x, y) >= 0; }

private:
    enum Button { kMin, kMax, kClose, kButtonCount };

    static constexpr float kBarH = 31.0f; // base (1x); scale by jplay::dpiScale at use
    static constexpr float kBtnW = 46.0f; // base (1x)

    TextFont* textFont_ = nullptr;
    SDL_Window* window_ = nullptr;
    std::string title_;
    float winW_ = 0.0f;
    SDL_FRect btnRect_[kButtonCount]{};
    int hover_ = -1;

    int buttonAt(float x, float y) const;
    bool isMaximized() const;
};
