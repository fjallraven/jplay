// Review-monitor translation unit: a borderless full-screen window on a second
// display that mirrors the clean program frame — the graded/teched image plus
// pencil annotations at the same zoom/pan as the GUI player view, but with no
// status text, LOADING badge, tech-check legend, or grid/overview. All members
// of App; split out of App.cpp for the same reasons as the other App_*.cpp files.
//
// The frame pixels are read back from the main renderer (the shared OutputManager
// GL readback on the SDR path, the HdrColorPass offscreen float target on the HDR
// one) and uploaded into a texture owned by the review renderer — SDL_Renderer
// textures are not shareable across renderers, so the program image must round-trip
// through CPU. Either way the image arrives already display-transformed; this window
// applies no colour transform of its own.
//
// While a review monitor is up it is the sole HDR sink and the main window presents
// sRGB (App::syncHdrSink_). Its renderer also holds vsync (playback's timing master)
// and the GUI renderer presents unthrottled.

#include "App.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Reconcile the live window with the user's toggle/selection. Called once per
// rendered frame; cheap when nothing changed. Requires the GL readback path
// (canRead()) — on a non-GL renderer the review window stays unavailable, same
// constraint as NDI.
void App::syncReviewWindow() {
    bool want = reviewEnabled_ && reviewDisplay_ != 0 && output_.canRead();
    if (want) {
        if (!reviewWindow_) {
            openReviewWindow(reviewDisplay_);
            // Seed the fresh window with the frame currently on the GUI. The feed
            // otherwise only fires when a *new* program frame composites, so opening the
            // review monitor while parked on a static frame would leave it black until
            // the next frame change.
            if (reviewWindow_ && programShown_ && texture_ && texW_ > 0 && texH_ > 0) {
                if (hdrPipeline_) {
                    // The scene-linear source is uploaded to the review device on a
                    // composite (feedReviewSource_), and its color pass is built there
                    // too. Force one recomposite next frame so the parked frame — and
                    // the review's OCIO program — appear immediately.
                    displayedKey_ = CacheKey{};
                } else {
                    const uint8_t* px = output_.readProgram(texture_, texW_, texH_);
                    if (px)
                        feedReviewFrame(px, texW_, texH_);
                }
            }
        }
    } else if (reviewWindow_) {
        closeReviewWindow();
    }
}

void App::openReviewWindow(SDL_DisplayID display) {
    if (reviewWindow_ || display == 0)
        return;

    SDL_Rect b{};
    if (!SDL_GetDisplayBounds(display, &b)) {
        std::fprintf(stderr, "Review: display %u bounds unavailable: %s\n",
                     display, SDL_GetError());
        return;
    }

    // Borderless window covering the display, then flipped to fullscreen (SDL3
    // defaults to fullscreen-desktop with no mode set, so no resolution switch).
    reviewWindow_ = SDL_CreateWindow("jplay review", b.w, b.h, SDL_WINDOW_BORDERLESS);
    if (!reviewWindow_) {
        std::fprintf(stderr, "Review: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return;
    }
    SDL_SetWindowPosition(reviewWindow_, b.x, b.y);
    SDL_SetWindowFullscreen(reviewWindow_, true);

    // When the HDR pipeline is running (the "gpu" backend + HdrColorPass) and this
    // display is in HDR mode, give the review renderer an scRGB swapchain so it can
    // present the program image in HDR. While a review monitor is up it is the only
    // HDR sink — the main window is forced to SDR (see App::syncHdrSink_) — so the
    // single main-renderer color pass produces the transformed scRGB image and the
    // review window just blits it (feedReviewSource_). Otherwise (SDR review display,
    // or HDR pipeline off) fall through to the 8-bit path fed by the shared readback.
    reviewHdr_ = false;
    reviewRenderer_ = nullptr;
    if (!hdrPipeline_ && jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Review monitor: HDR pipeline off (hdrOutput pref / no SDL_GPU); "
                    "using the SDR 8-bit path on display %u.", display);
    if (hdrPipeline_) {
        reviewGpuDevice_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
        if (!reviewGpuDevice_)
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Review monitor: SDL_CreateGPUDevice failed: %s", SDL_GetError());
        if (reviewGpuDevice_) {
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
            SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, reviewWindow_);
            SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_GPU_DEVICE_POINTER, reviewGpuDevice_);
            SDL_SetBooleanProperty(props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
            SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER,
                                  SDL_COLORSPACE_SRGB_LINEAR);
            reviewRenderer_ = SDL_CreateRendererWithProperties(props);
            SDL_DestroyProperties(props);
            if (!reviewRenderer_)
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Review monitor: scRGB \"gpu\" renderer failed: %s", SDL_GetError());
        }
        if (reviewRenderer_) {
            logHdrDisplayState_("Review monitor", reviewWindow_, reviewGpuDevice_);
            SDL_PropertiesID rp = SDL_GetRendererProperties(reviewRenderer_);
            const char* dname = SDL_GetDisplayName(display) ? SDL_GetDisplayName(display) : "?";
            reviewHdr_ = windowCanPresentHdr_(reviewWindow_, reviewGpuDevice_);
            if (jplayDebugLogging())
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Review monitor: display %u \"%s\" — keeping scRGB=%s, renderer=%s, "
                            "RENDERER_HDR_ENABLED=%s, headroom=%.2f, sdr_white=%.2f",
                            display, dname, reviewHdr_ ? "yes" : "NO",
                            SDL_GetRendererName(reviewRenderer_) ? SDL_GetRendererName(reviewRenderer_) : "?",
                            SDL_GetBooleanProperty(rp, SDL_PROP_RENDERER_HDR_ENABLED_BOOLEAN, false) ? "yes" : "no",
                            SDL_GetFloatProperty(rp, SDL_PROP_RENDERER_HDR_HEADROOM_FLOAT, 1.0f),
                            SDL_GetFloatProperty(rp, SDL_PROP_RENDERER_SDR_WHITE_POINT_FLOAT, 1.0f));
            if (reviewHdr_) {
                // The review monitor transforms on its own device from the scene-linear
                // source; the composite path feeds it the same processor as the main
                // pass (see renderPlayer).
                hdrColorPassReview_.init(reviewRenderer_);
                setStatus("Review monitor: HDR (scRGB).", 4000);
            } else {
                // Display isn't in HDR mode; drop the gpu renderer and use SDR below.
                // Note this also hands the fullscreen surface to OpenGL, which on some
                // setups makes Windows drop the monitor out of HDR — so only do it when
                // the display genuinely isn't HDR.
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Review monitor: display %u \"%s\" cannot present HDR "
                            "(Windows HDR off for that monitor?); falling back to the SDR 8-bit path.",
                            display, dname);
                setStatus("Review monitor: display is not in HDR mode; showing SDR.", 5000);
                SDL_DestroyRenderer(reviewRenderer_);
                reviewRenderer_ = nullptr;
            }
        }
        if (!reviewRenderer_ && reviewGpuDevice_) {
            SDL_DestroyGPUDevice(reviewGpuDevice_);
            reviewGpuDevice_ = nullptr;
        }
    }

    // SDR path (and HDR fallback): prefer OpenGL, then the platform default.
    if (!reviewRenderer_)
        reviewRenderer_ = SDL_CreateRenderer(reviewWindow_, "opengl");
    if (!reviewRenderer_)
        reviewRenderer_ = SDL_CreateRenderer(reviewWindow_, nullptr);
    if (!reviewRenderer_) {
        std::fprintf(stderr, "Review: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(reviewWindow_);
        reviewWindow_ = nullptr;
        return;
    }
    SDL_SetRenderVSync(reviewRenderer_, 1); // review monitor is the timing master
    SDL_SetRenderDrawBlendMode(reviewRenderer_, SDL_BLENDMODE_BLEND);
    reviewFrameFresh_ = false;

    reloadReviewOverlayFont();

    // Hand vsync to the review monitor; the GUI now presents unthrottled and just
    // follows. (Restored in closeReviewWindow.)
    SDL_SetRenderVSync(renderer_, 0);
}

// The burn-in needs glyph textures owned by the review renderer, so that window
// keeps its own copy of the font. It has no logical presentation, so its units are
// device pixels and the font is opened at 1:1 in them -- at the Size choice's point
// size grown for the monitor, since UI-sized text on a 4K panel would be a rumour.
// Failure is silent: the overlay simply does not draw there.
void App::reloadReviewOverlayFont() {
    if (!reviewRenderer_)
        return;
    int ow = 0, oh = 0;
    SDL_GetRenderOutputSize(reviewRenderer_, &ow, &oh);
    const float grow = std::clamp(oh / 1080.0f, 1.0f, 4.0f);
    const char* base = SDL_GetBasePath();
    std::string fontPath = std::string(base ? base : "") + "Inter-Regular.ttf";
    reviewTextFont_.destroy();
    reviewTextFont_.load(reviewRenderer_, fontPath.c_str(), overlayFontPt() * grow, 1.0f);
}

void App::closeReviewWindow() {
    // Release the review device's color pass + textures before the renderer/device.
    hdrColorPassReview_.shutdown();
    if (reviewSrcTex_) {
        SDL_DestroyTexture(reviewSrcTex_);
        reviewSrcTex_ = nullptr;
    }
    reviewSrcW_ = reviewSrcH_ = 0;
    if (reviewTex_) {
        SDL_DestroyTexture(reviewTex_);
        reviewTex_ = nullptr;
    }
    reviewTexW_ = reviewTexH_ = 0;
    if (reviewRenderer_) {
        reviewTextFont_.destroy(); // holds textures of this renderer; release first
        SDL_DestroyRenderer(reviewRenderer_);
        reviewRenderer_ = nullptr;
    }
    if (reviewGpuDevice_) { // HDR review renderer's device; released after the renderer
        SDL_DestroyGPUDevice(reviewGpuDevice_);
        reviewGpuDevice_ = nullptr;
    }
    reviewHdr_ = false;
    // The main window was rendering the SDR companion display while the HDR review
    // monitor owned the selected one (mainForcedSdrOcio); force one recomposite so its
    // color pass is rebuilt with the selected transform now that it is the reference.
    if (hdrPipeline_)
        displayedKey_ = CacheKey{};
    if (reviewWindow_) {
        SDL_DestroyWindow(reviewWindow_);
        reviewWindow_ = nullptr;
    }
    reviewFrameFresh_ = false;
    // Return the timing role to the GUI renderer.
    if (renderer_)
        SDL_SetRenderVSync(renderer_, 1);
}

// Upload one freshly-composited program frame (RGBA8, top row first, native media
// resolution) into the review texture. Called from renderPlayer with the same
// pixels handed to the output device, so at most one GL readback per new frame.
void App::feedReviewFrame(const uint8_t* rgba, int w, int h) {
    if (!reviewRenderer_ || !rgba || w <= 0 || h <= 0)
        return;
    if (!reviewTex_ || reviewTexW_ != w || reviewTexH_ != h) {
        if (reviewTex_)
            SDL_DestroyTexture(reviewTex_);
        reviewTex_ = SDL_CreateTexture(reviewRenderer_, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, w, h);
        if (reviewTex_)
            SDL_SetTextureScaleMode(reviewTex_, SDL_SCALEMODE_LINEAR);
        reviewTexW_ = w;
        reviewTexH_ = h;
    }
    if (reviewTex_) {
        SDL_UpdateTexture(reviewTex_, nullptr, rgba, w * 4);
        reviewFrameFresh_ = true;
    }
}

// Upload one scene-linear program frame (RGBA half, top row first) to the HDR review
// monitor's OWN GPU device, into a float texture tagged SRGB_LINEAR. renderReviewWindow
// then applies the display transform there, so the transformed frame never round-trips
// through a CPU readback of the main renderer. Only used while reviewHdr_ is set.
void App::feedReviewSource_(const Imath::half* rgba, int w, int h) {
    if (!reviewRenderer_ || !rgba || w <= 0 || h <= 0)
        return;
    if (!reviewSrcTex_ || reviewSrcW_ != w || reviewSrcH_ != h) {
        if (reviewSrcTex_)
            SDL_DestroyTexture(reviewSrcTex_);
        SDL_PropertiesID tp = SDL_CreateProperties();
        SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_RGBA64_FLOAT);
        SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
        SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
        SDL_SetNumberProperty(tp, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
        reviewSrcTex_ = SDL_CreateTextureWithProperties(reviewRenderer_, tp);
        SDL_DestroyProperties(tp);
        if (reviewSrcTex_)
            SDL_SetTextureScaleMode(reviewSrcTex_, SDL_SCALEMODE_LINEAR);
        reviewSrcW_ = w;
        reviewSrcH_ = h;
    }
    if (reviewSrcTex_) {
        SDL_UpdateTexture(reviewSrcTex_, nullptr, rgba, w * 4 * (int)sizeof(Imath::half));
        reviewFrameFresh_ = true;
    }
}

// The image rect for the review window, reproducing the GUI framing: whatever
// image point sits under the GUI player-rect centre is placed at the review-window
// centre, at the same zoom factor (relative to each window's own fit scale). This
// mirrors both zoom and pan independent of the review monitor's size/aspect.
SDL_FRect App::reviewDstRect() const {
    int ww = 0, wh = 0;
    if (reviewRenderer_)
        SDL_GetRenderOutputSize(reviewRenderer_, &ww, &wh);
    if (texW_ <= 0 || texH_ <= 0 || ww <= 0 || wh <= 0)
        return { 0, 0, 0, 0 };

    // The program image's rect and the centre of the area it sits in — tile 0's
    // cell on the Layout stage, so the review monitor reproduces the framing of the
    // program tile rather than of a stage it knows nothing about.
    SDL_FRect g = programDstRect(); // GUI on-screen image rect
    SDL_FRect area = programViewRect();
    float gcx = area.x + area.w * 0.5f;
    float gcy = area.y + area.h * 0.5f;
    float u = g.w > 0.0f ? (gcx - g.x) / g.w : 0.5f; // image UV under the GUI centre
    float v = g.h > 0.0f ? (gcy - g.y) / g.h : 0.5f;

    float fit = fitScaleIn({ 0.0f, 0.0f, (float)ww, (float)wh }, texDispW(), (float)texH_);
    float scale = fit * frameZoom_;
    float dw = texDispW() * scale, dh = texH_ * scale;
    return { ww * 0.5f - u * dw, wh * 0.5f - v * dh, dw, dh };
}

void App::renderReviewWindow() {
    if (!reviewRenderer_)
        return;

    SDL_SetRenderDrawColor(reviewRenderer_, 0, 0, 0, 255);
    SDL_RenderClear(reviewRenderer_);

    // Show the frame only while a live program frame is on screen in the GUI (so a
    // gap over empty timeline reads as black here too, matching the GUI backdrop).
    // HDR review transforms the scene-linear source on this device; SDR review blits
    // the pre-tonemapped 8-bit frame. Either texture must match the media resolution.
    bool haveHdr = reviewHdr_ && reviewSrcTex_ && reviewSrcW_ == texW_ && reviewSrcH_ == texH_;
    bool haveSdr = !reviewHdr_ && reviewTex_ && reviewTexW_ == texW_ && reviewTexH_ == texH_;
    if (programShown_ && (haveHdr || haveSdr) && texW_ > 0 && texH_ > 0) {
        SDL_FRect dst = reviewDstRect();
        if (dst.w > 0.0f && dst.h > 0.0f) {
            // Mirrors the GUI's own pass selection in renderPlayer.
            const float gain = std::exp2(grade_.gain);
            const bool pass = haveHdr && reviewSrcSceneLinear_;
            if (pass && nitHeatmapShown_) {
                hdrColorPassReview_.beginNit(nitRef_, gain);
            } else if (pass && hdrColorPassReview_.hasState()) {
                int gradeTech = (techMode_ == TechMode::Luminance) ? 0 : (int)techMode_;
                hdrColorPassReview_.begin(hdrRefWhiteNits_, gain, grade_, gradeTech);
            }
            // Video (already display-referred) or no transform available: blit as-is.
            SDL_RenderTexture(reviewRenderer_, haveHdr ? reviewSrcTex_ : reviewTex_,
                              nullptr, &dst);
            hdrColorPassReview_.end();
            // Annotations are stored in image coordinates; re-project onto this
            // window's dst rect. The live buffer already tracks the current frame
            // (syncAnnotBuffer runs each GUI frame before this).
            if (!annotStrokes_.empty()) {
                std::vector<SDL_Vertex> verts;
                std::vector<int> idx;
                annotationGeometry(dst, verts, idx);
                if (!verts.empty())
                    SDL_RenderGeometry(reviewRenderer_, nullptr, verts.data(),
                                       (int)verts.size(), idx.data(), (int)idx.size());
            }
            // Letterbox matte, on top of frame + annotations, image-relative (tracks
            // the same crop the GUI and NDI output show).
            drawLetterbox(reviewRenderer_, dst);
            // Burn-in, over the matte as in the GUI. The strings were built for this
            // frame by updateFrameOverlay(), which runs earlier in renderPlayer().
            drawFrameOverlay(reviewRenderer_, reviewTextFont_, dst);
        }
    }

    SDL_RenderPresent(reviewRenderer_); // vsync here: paces the loop to the review monitor
}
