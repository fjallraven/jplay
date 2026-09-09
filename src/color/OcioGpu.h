#pragma once

#include <OpenColorIO/OpenColorIO.h>
#include <Imath/half.h>

#include <cstdint>
#include <string>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

struct SDL_Renderer;

// GPU implementation of the OCIO display transform.
//
// Renders a decoded frame through OCIO's generated GLSL shader into an existing
// SDL texture (its backing GL texture), so the transform runs on the GPU instead
// of the per-pixel CPU loop in OcioManager::CpuTransform. Requires the SDL
// renderer to be using its OpenGL backend; init() returns false otherwise and the
// caller falls back to the CPU path.
//
// The program runs the pipeline in two OCIO-generated functions with the exposure
// between them — the media colour space into the linear working space, the gain,
// then the working space to the display. Both halves come from OcioManager (see
// its Transform), and only the first changes when the playhead cuts to a source of
// a different colour space; the built programs are kept in a small cache so
// cutting back and forth between two sources is a rebind, not a shader rebuild.
class OcioGpu {
public:
    // The pixel layout of the buffer handed to render(). The two integer formats
    // are normalised to 0..1 by the sampler, which is exactly what an
    // integer-encoded OCIO colour space expects; the half format carries the
    // source values unscaled.
    enum class InputFormat {
        SceneLinearHalf, // width * height * 3 Imath::half (EXR)
        Rgba16,          // width * height * 4 uint16_t    (video deeper than 8 bit)
        Rgba8,           // width * height * 4 uint8_t     (everything else)
    };

    // Loads GL entry points from the renderer's GL context and builds the static
    // resources (fullscreen triangle, FBO, input texture). Returns false if the
    // renderer is not GL-backed or a required GL function is missing.
    bool init(SDL_Renderer* renderer);
    bool isReady() const { return ready_; }

    // Select the transform to render with: `toWorking` (the media colour space to
    // the linear working space; null for an identity conversion) and `toDisplay`
    // (working space to the selected display and view). `version` is the
    // OcioManager token covering both — an unchanged one is a no-op, and a token
    // already in the program cache rebinds instead of rebuilding. A null
    // toDisplay (or a build failure) leaves the renderer unable to render until a
    // valid pair is supplied.
    void setProcessors(const OCIO::ConstProcessorRcPtr& toWorking,
                       const OCIO::ConstProcessorRcPtr& toDisplay, int version);

    // Render `pixels` (laid out per `fmt`) through the active program into
    // `dstTexGL` (the GL texture id of an RGBA8 SDL texture, with target
    // `dstTexTarget`). `exposureEV` is applied as * 2^EV in the linear working
    // space, between the two halves of the transform, so a stop means the same
    // thing whatever the source. Returns false on any failure so the caller can
    // fall back to the CPU path.
    bool render(const void* pixels, InputFormat fmt, int width, int height,
                unsigned dstTexGL, unsigned dstTexTarget, float exposureEV = 0.0f);

    // Render a scene-referred luminance heatmap of `linear` into `dstTexGL` — a
    // "true HDR" nit map computed BEFORE any display transform. Per pixel: apply
    // `exposureEV` (scene-linear), take Rec.709 luminance, scale by `nitScale`
    // (scene-linear 1.0 -> nitScale nits), then map log10(nits) over 0.1..10000
    // to a heat colour. Independent of the OCIO processor, so it works whether or
    // not a display transform is loaded. Returns false on any failure.
    bool renderNitHeatmap(const Imath::half* linear, int width, int height,
                          unsigned dstTexGL, unsigned dstTexTarget,
                          float exposureEV, float nitScale);

private:
    bool ready_ = false;
    SDL_Renderer* renderer_ = nullptr;

    // GL object ids (unsigned to avoid leaking <GL/gl.h> into the header).
    unsigned vao_ = 0, vbo_ = 0, fbo_ = 0;
    unsigned inputTex_ = 0;
    int      inW_ = 0, inH_ = 0;
    int      inFmt_ = -1;       // InputFormat the input texture is allocated for
    unsigned nitProg_ = 0;      // scene-linear nit-heatmap program (processor-independent)
    bool nitTried_ = false;     // nitProg_ build attempted (once only; see ensureNitProgram_)
    int nitExpLoc_ = -1, nitScaleLoc_ = -1; // "uExposure"/"uNitScale" in nitProg_

    struct Lut {
        unsigned id = 0;        // GL texture id
        unsigned target = 0;    // GL_TEXTURE_1D / _2D / _3D
        std::string sampler;    // uniform name in the OCIO shader
        int unit = 0;           // texture image unit
    };

    // One built transform: the linked program and the LUT textures its two OCIO
    // functions sample. Held in a small cache keyed by the OcioManager token so a
    // cut between two sources of different colour spaces — which changes only the
    // input half — costs a rebind rather than a GLSL compile and a LUT re-upload.
    struct Program {
        int version = -1;
        unsigned id = 0;        // GL program
        int expLoc = -1;        // "uExposure"
        std::vector<Lut> luts;
    };
    static constexpr size_t kMaxPrograms = 4;
    std::vector<Program> programs_;  // most recently used last
    int version_ = -1;               // token of the selected transform
    bool attempted_ = false;         // a build has been tried for version_ (ok or not)
    // Index into programs_ of the selected transform, -1 for none. An index rather
    // than a pointer because promoting an entry to most-recently-used erases and
    // re-appends it, which would leave a pointer dangling.
    int activeIdx_ = -1;
    Program* active_() {
        return (activeIdx_ >= 0 && activeIdx_ < (int)programs_.size()) ? &programs_[activeIdx_]
                                                                      : nullptr;
    }

    // Upload a source frame into inputTex_, reallocating only when its shape or
    // format changed (see the definition -- the realloc is the expensive part).
    void uploadInput_(const void* pixels, InputFormat fmt, int width, int height);

    // Ring of pixel-unpack buffers the source frame is staged through, so the
    // host->device copy becomes a DMA the GPU runs on its own rather than a
    // blocking driver upload on the main thread. Three deep: one being filled, one
    // in flight, one spare, which at one upload per frame means the fence a slot
    // is reused behind has always long since signalled. Each holds one frame
    // (168 MB at 8192x3432 half), allocated once per frame shape.
    static constexpr int kNumPbos = 3;
    struct Pbo {
        unsigned id = 0;     // GL buffer id
        size_t   size = 0;   // bytes currently allocated
        void*    sync = nullptr; // GLsync fence for the DMA that last read it
    };
    Pbo  pbos_[kNumPbos];
    int  pboNext_ = 0;          // slot the next upload takes
    bool pboEnabled_ = true;    // cleared by JPLAY_NO_PBO=1

    // Stage one frame into the ring, leaving its buffer bound; returns the slot or
    // -1 to mean "not staged, upload from host memory".
    int  stagePbo_(const void* pixels, size_t bytes);

    bool ensureNitProgram_();   // compiles nitProg_ on first use; one attempt only
    // Compile a program for the pending pair and append it to programs_, evicting
    // the least recently used entry when the cache is full. Returns its index, or
    // -1 on failure.
    int buildProgram_(int version);
    void destroyProgram_(Program& p);
    // Upload every LUT one OCIO shader description asks for, appending them to
    // `luts` and advancing `unit`. Shared by the two halves of the transform, whose
    // samplers stay distinct because each was generated with its own prefix.
    void uploadDescLuts_(const OCIO::GpuShaderDescRcPtr& desc, std::vector<Lut>& luts,
                         int& unit);
    OCIO::ConstProcessorRcPtr pendingToWorking_, pendingToDisplay_;
};
