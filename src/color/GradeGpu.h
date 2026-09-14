#pragma once

#include "Grade.h"

struct SDL_Renderer;

// GPU color-grading post-pass.
//
// Applies a grade::State to an already-display-referred RGBA8 SDL texture, in
// place, on the GPU. The grade runs after OCIO (for EXR) or directly on the
// decoded RGBA (for video), so it works uniformly for every source. Requires the
// SDL renderer to be GL-backed (same constraint as OcioGpu); init() returns false
// otherwise and the caller simply skips the pass.
//
// In place is achieved with a scratch texture: the base image is copied to the
// scratch, then the grade shader samples the scratch and writes back to the
// destination texture through an FBO.
class GradeGpu {
public:
    bool init(SDL_Renderer* renderer);
    bool isReady() const { return ready_; }

    // Grade `dstTexGL` (GL id of an RGBA8 SDL texture, target `dstTexTarget`,
    // size width*height) in place using `s`, then apply the tech-check overlay
    // `techMode` (0 none, 1 false-color, 2 clipping, 3 monochrome, 4/5/6 single
    // R/G/B channel as grayscale) to the result.
    // Returns false on any failure.
    bool apply(unsigned dstTexGL, unsigned dstTexTarget, int width, int height,
               const grade::State& s, int techMode);

private:
    bool ready_ = false;
    SDL_Renderer* renderer_ = nullptr;

    unsigned vao_ = 0, vbo_ = 0, fbo_ = 0;
    unsigned copyProg_ = 0, gradeProg_ = 0;
    unsigned scratchTex_ = 0;
    int scratchW_ = 0, scratchH_ = 0;
    unsigned curveTex_ = 0; // 256x1 RGBA32F: r/g/b channel curves + master in alpha

    // gradeProg_ uniform locations, resolved once in buildPrograms_() instead of
    // per-apply() (uniform locations are fixed for the lifetime of a linked program).
    int locWbGain_ = -1, locGain_ = -1, locGamma_ = -1, locSat_ = -1;
    int locTintSh_ = -1, locLumaSh_ = -1, locTintMid_ = -1, locLumaMid_ = -1;
    int locTintHi_ = -1, locLumaHi_ = -1, locHasCurves_ = -1, locTech_ = -1;

    // Last curves uploaded to curveTex_, so apply() can skip the bake+upload when
    // the curves haven't changed since the previous call (curve edits are rare
    // UI interactions, not per-frame events).
    grade::Curve lastCurveMaster_, lastCurveR_, lastCurveG_, lastCurveB_;
    bool curvesUploaded_ = false;

    bool buildPrograms_();
    void ensureScratch_(int w, int h);
    void uploadCurves_(const grade::State& s);
};
