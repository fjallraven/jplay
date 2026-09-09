#pragma once

#include "Grade.h"

#include <OpenColorIO/OpenColorIO.h>

#include <atomic>
#include <string>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

struct SDL_Renderer;
struct SDL_GPUDevice;
struct SDL_GPUShader;
struct SDL_GPURenderState;
struct SDL_GPUTexture;
struct SDL_GPUSampler;

// HDR color pass built on SDL_GPU render state.
//
// On the cross-platform "gpu" (Vulkan/D3D12/Metal-backed) renderer used for HDR
// output, SDL_GPURenderState attaches a custom fragment shader to a normal
// SDL_RenderTexture draw — SDL owns the command-buffer / synchronization /
// swapchain work. This pass applies the OpenColorIO display transform to the
// scene-linear program image, producing linear extended-range values for the
// SRGB_LINEAR (scRGB) swapchain.
//
// The OCIO shader is generated for Vulkan GLSL and its LUT samplers are steered
// into SDL_GPU's fragment descriptor set 2 (bindings 1..N; binding 0 is the drawn
// program texture) via OCIO's setDescriptorSetIndex. The generated GLSL is compiled
// to SPIR-V with shaderc at runtime.

// The Settings panel's "Debug Logging" checkbox, as a process-wide flag.
//
// The HDR / OCIO / display-state diagnostics (swapchain composition support, the
// per-frame routing decision, colour-pass shader builds, review-window HDR
// negotiation) are verbose and only wanted while investigating a colour or output
// problem, so they are logged only when this is on. Genuine failures — shader
// compile errors, a null processor, a renderer that could not be created — stay
// unconditional at warning/error level.
//
// It lives here, rather than in a header of its own, because this is the lowest
// header shared by everything that logs on the HDR path (App.h includes it, and so
// does HdrColorPass.cpp). Written from the main thread, read wherever logging
// happens, hence the atomic.
inline std::atomic<bool>& jplayDebugLogFlag() {
    static std::atomic<bool> enabled{ false };
    return enabled;
}
inline bool jplayDebugLogging() {
    return jplayDebugLogFlag().load(std::memory_order_relaxed);
}
inline void jplaySetDebugLogging(bool enabled) {
    jplayDebugLogFlag().store(enabled, std::memory_order_relaxed);
}

class HdrColorPass {
public:
    // Cache the renderer's GPU device. Returns false (isReady() == false) if the
    // renderer is not the SDL_GPU backend (GL / SDR path); the caller then skips it.
    bool init(SDL_Renderer* renderer);
    bool isReady() const { return device_ != nullptr; }

    // The OCIO display's output transfer function and primaries, derived from the
    // active display name. The pass decodes OCIO's display-encoded output back to
    // linear light and converts to Rec.709 (scRGB) primaries; the two HDR encodings
    // additionally normalise to the panel's SDR-white-relative scale so highlights
    // land in the HDR headroom — PQ from absolute nits, HLG from its relative scale.
    enum class Encoding { sRGB, Gamma22, Gamma24, Gamma26, PQ, HLG };
    enum class Primaries { Rec709, Rec2020, P3D65 };

    // Rebuild the shader + LUT textures from the two halves of an OcioManager
    // Transform when `version` (its token, which also bumps on a display change)
    // changes: `toWorking` takes the media colour space to the linear working space
    // (null for a media already there) and `toDisplay` takes the working space to
    // the selected display and view. The exposure passed to begin() is applied
    // between them, so a stop means the same thing whatever the source.
    // `enc`/`prim` describe the selected display's output encoding/primaries (baked
    // into the shader). A null toDisplay or a build failure leaves hasState() ==
    // false; the caller then draws the frame untransformed.
    void setProcessors(const OCIO::ConstProcessorRcPtr& toWorking,
                       const OCIO::ConstProcessorRcPtr& toDisplay, int version,
                       Encoding enc, Primaries prim);
    bool hasState() const { return state_ != nullptr; }
    // The display encoding baked into the built shader. Only meaningful together with
    // hasState(); PQ or HLG is what tells a sink the pass produced a genuine HDR
    // rendering (linear scRGB with above-paper-white headroom) rather than a clipped
    // SDR one, and which transfer to re-encode with (see App::hdrProgramTransfer_).
    Encoding encoding() const { return enc_; }

    // Activate the render state for the next SDL_RenderTexture draw of the program
    // texture; end() deactivates it. `refWhiteNits` is the master paper-white level
    // mapped to panel SDR white (PQ only; live-tunable). `gain` is the exposure
    // multiplier (exp2 of the grade's stops), applied in the linear working space
    // between the two halves of the transform — it lives here rather than being
    // baked into the uploaded pixels so the composite stays a plain half copy.
    // `grade`/`tech` are applied in display space after OCIO (curves excluded);
    // `tech`: 0 none, 2 clipping, 3 monochrome, 4/5/6 single R/G/B channel.
    void begin(float refWhiteNits, float gain, const grade::State& grade, int tech);
    void end();

    // Luminance nit-heatmap pass (processor-independent; built once at init). Maps
    // the scene-linear program texture's luminance to a nit heat ramp. `nitScale`
    // is scene-linear 1.0 -> nitScale nits, `gain` as in begin(). Same begin/draw/end().
    bool hasNitState() const { return nitState_ != nullptr; }
    void beginNit(float nitScale, float gain);

    // Release all GPU objects. Call before the renderer / GPU device is destroyed.
    void shutdown();

private:
    void destroyProgram_();
    bool buildProgram_(const OCIO::ConstProcessorRcPtr& toWorking,
                       const OCIO::ConstProcessorRcPtr& toDisplay);
    bool buildNit_(); // build the static nit-heatmap shader + render state
    void uploadCurve_(const grade::State& g); // (re)bake the grade curve LUT texture
    // Create + upload one LUT as an SDL_GPU sampled texture; returns false on failure.
    // channels: 1 (red) or 3 (rgb, expanded to rgba). dims3d: true for a 3D LUT.
    bool addLut_(bool dims3d, unsigned w, unsigned h, unsigned d, int channels,
                 const float* values, bool linearFilter);

    SDL_Renderer* renderer_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;   // owned by the renderer, not us
    SDL_GPUShader* shader_ = nullptr;
    SDL_GPURenderState* state_ = nullptr;
    int version_ = -1;                  // OcioManager token of the built program
    // A build has been ATTEMPTED for version_ (successfully or not). Without this, a
    // failed build leaves state_ null and setProcessors re-runs the whole OCIO shader
    // extraction + shaderc compile on every composited frame — ~80 ms/frame, which
    // stalls playback outright. Reset only when the transform or the device changes.
    bool built_ = false;
    Encoding enc_ = Encoding::sRGB;     // baked into the built shader
    Primaries prim_ = Primaries::Rec709;

    std::vector<SDL_GPUTexture*> luts_;
    std::vector<SDL_GPUSampler*> samplers_;

    // Grade curve LUT (256x1 RGBA32F): rebuilt with the program, re-uploaded when the
    // grade curves change. Bound as the sampler after the OCIO LUTs.
    SDL_GPUTexture* curveTex_ = nullptr;
    SDL_GPUSampler* curveSampler_ = nullptr;
    grade::Curve lastCurveMaster_, lastCurveR_, lastCurveG_, lastCurveB_;
    bool curvesUploaded_ = false;

    SDL_GPUShader* nitShader_ = nullptr;      // built once at init (processor-independent)
    SDL_GPURenderState* nitState_ = nullptr;
};
