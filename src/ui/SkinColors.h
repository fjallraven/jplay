#pragma once

#include <SDL3/SDL.h>

// The application's flat colour palette.
//
// Most of the UI is text, icons, hover rows and hairlines — things SDL draws as a
// single colour. Naming them once here is what keeps the chrome consistent across
// the panels, menus and widgets that draw them.
//
// Call sites reach these through jplay::colors(). Several translation units bind
// their file-local constant to a member by reference:
//
//     static const SDL_Color& kColBorder = jplay::colors().border;

namespace jplay {

struct SkinColors {
    // ── Surfaces ──
    SDL_Color panelBg         {  18,  18,  20, 255 };
    SDL_Color divider         {  60,  64,  74, 255 };
    // The menu bar is a left-to-right ramp between these two.
    // Equal values give a flat bar.
    SDL_Color titlebarLeft    {  18,  18,  20, 255 };
    SDL_Color titlebarRight   {  18,  18,  20, 255 };
    SDL_Color titleDivider    {  60,  61,  66, 255 };
    SDL_Color topbarDivider   {  60,  62,  70, 255 };
    SDL_Color rule            {  50,  53,  60, 255 };
    SDL_Color fieldBg         {  25,  25,  30, 255 };
    SDL_Color dropBg          {  25,  25,  30, 255 };
    SDL_Color border          {  80,  82,  90, 255 };
    SDL_Color borderStrong    {  90,  92, 100, 255 };

    // ── Accents ──
    SDL_Color accent          {  55,  78, 130, 255 };
    SDL_Color accentBorder    { 110, 140, 200, 255 };
    SDL_Color focus           {  90, 130, 200, 255 };
    SDL_Color hover           {  60,  90, 150, 255 };
    SDL_Color rowSelected     {  50,  65, 110, 255 };
    SDL_Color selection       {  70, 100, 170, 255 };

    // ── Type ──
    SDL_Color text            { 225, 228, 235, 255 };
    SDL_Color textDim         { 140, 143, 150, 255 };
    SDL_Color label           { 170, 174, 182, 255 };
    SDL_Color labelOff        { 130, 134, 142, 255 };
    SDL_Color header          { 160, 170, 200, 255 };
    SDL_Color section         { 120, 150, 210, 255 };
    SDL_Color value           { 150, 190, 255, 255 };
    SDL_Color selText         { 255, 230, 150, 255 };
    SDL_Color titleText       { 205, 207, 213, 255 };
    SDL_Color menuTitle       { 165, 168, 175, 255 };
    SDL_Color menuTitleLit    { 255, 255, 255, 255 };

    // ── Buttons ──
    SDL_Color btnBg           {  25,  25,  30, 255 };
    SDL_Color btnHover        {  75, 105, 175, 255 };
    SDL_Color btnDisabledBg   {  38,  39,  43, 255 };
    SDL_Color uibtnBg         {  21,  21,  27, 255 };
    SDL_Color uibtnHover      {  40,  40,  50, 255 };
    SDL_Color uibtnBorder     {  58,  58,  69, 255 };
    SDL_Color uibtnBorderHov  {  84,  84,  97, 255 };
    SDL_Color uibtnOnBg       {  54,  69,  87, 255 };
    SDL_Color uibtnOnHover    {  64,  79,  97, 255 };
    SDL_Color uibtnOnBorder   { 120, 154, 193, 255 };
    SDL_Color uibtnOnText     { 180, 214, 255, 255 };

    // ── Top toolbar picker buttons (App_TopBar.cpp) ──
    SDL_Color topbtnBg        {  25,  25,  30, 255 };
    SDL_Color topbtnBgHover   {  58,  60,  68, 255 };
    SDL_Color topbtnOnBg      {  52,  84, 116, 255 };
    SDL_Color topbtnOnHover   {  62,  96, 130, 255 };
    SDL_Color topbtnOnBorder  {  96, 156, 214, 255 };
    SDL_Color topbtnOnText    { 180, 214, 255, 255 };
    SDL_Color topbtnText      { 210, 213, 220, 255 };
    SDL_Color topbtnTextHover { 235, 238, 245, 255 };

    // ── Icons (App_NavPanel.cpp) ──
    SDL_Color iconIdle        { 200, 205, 215, 255 };
    SDL_Color iconActive      { 150, 190, 255, 255 };
    SDL_Color iconWarn        { 235, 180,  90, 255 };
    SDL_Color iconLive        {  90, 170, 110, 255 };
    SDL_Color navBtnBg        {  34,  36,  42, 255 };
    SDL_Color navSelBar       { 120, 180, 255, 255 }; // 2px marker left of the open panel's icon

    // ── Menus (Menu.cpp) ──
    SDL_Color menuSep         {  70,  72,  80, 255 };
    SDL_Color menuTitleBg     {  30,  31,  34, 255 };
    SDL_Color menuDisabled    { 120, 122, 128, 255 };
    SDL_Color menuShortcut    { 138, 141, 150, 255 };
    SDL_Color menuShortcutOff {  95,  97, 103, 255 };
    SDL_Color menuArrow       { 180, 183, 192, 255 };
    SDL_Color menuCheck       { 120, 180, 255, 255 }; // square marking the active item
    SDL_Color msliderTrack    {  42,  45,  52, 255 };
    SDL_Color msliderEdge     {  70,  74,  84, 255 };
    SDL_Color msliderFill     {  90, 120, 180, 255 };
    SDL_Color msliderKnob     { 210, 214, 222, 255 };

    // ── Grade sliders (App_Grade.cpp) ──
    SDL_Color gsliderTrack    {  52,  54,  62, 255 };
    SDL_Color gsliderTick     { 110, 114, 124, 255 };
    SDL_Color gsliderKnob     { 220, 224, 232, 255 };

    // ── Scrollbars (Widgets.cpp) ──
    SDL_Color sbTrack         {  30,  30,  36, 255 };
    SDL_Color sbThumb         {  90,  90, 104, 255 };
    SDL_Color sbBare          { 110, 113, 122, 255 };

    // ── Curve editor (App_Grade.cpp) ──
    SDL_Color curveBg         {  22,  23,  27, 255 };
    SDL_Color curveGrid       {  44,  46,  54, 255 };
    SDL_Color curveDiag       {  60,  63,  72, 255 };
    SDL_Color curveLuma       { 210, 214, 222, 255 };
    SDL_Color curveR          { 235, 110, 110, 255 };
    SDL_Color curveG          { 110, 220, 130, 255 };
    SDL_Color curveB          { 110, 160, 240, 255 };

    // ── Dropdown bar (Dropdown.cpp) ──
    SDL_Color dbarBg          {  30,  31,  35, 255 };
    SDL_Color dbarSep         {  12,  12,  14, 255 };
    SDL_Color dbarBox         {  44,  46,  52, 255 };
    SDL_Color dbarBoxOpen     {  52,  54,  62, 255 };

    // ── Misc ──
    SDL_Color tooltipBg       {  20,  21,  24, 240 };
    SDL_Color closeHover      { 200,  50,  50, 255 };
};

// The one palette the UI draws from.
inline const SkinColors& colors() {
    static const SkinColors c;
    return c;
}

} // namespace jplay
