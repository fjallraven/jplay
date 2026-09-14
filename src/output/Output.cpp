#include "Output.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>

#include <cstdio>

#ifdef JPLAY_ENABLE_NDI
#include "OutputNDI.h"
#endif
#ifdef JPLAY_ENABLE_DECKLINK
#include "OutputDeckLink.h"
#endif

// ---------------------------------------------------------------------------
// GL entry points for the texture readback.
//
// Mirrors the loader pattern in OcioGpu.cpp: opengl32.dll only exports GL 1.1,
// so everything is resolved through SDL_GL_GetProcAddress. We need just the FBO
// functions plus glReadPixels and a little state save/restore.
// ---------------------------------------------------------------------------
namespace {

#define GLFN(ret, name, ...)                        \
    typedef ret(APIENTRY* PFN_##name)(__VA_ARGS__); \
    PFN_##name p_##name = nullptr;

GLFN(void, glGenFramebuffers, GLsizei, GLuint*)
GLFN(void, glBindFramebuffer, GLenum, GLuint)
GLFN(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)
GLFN(GLenum, glCheckFramebufferStatus, GLenum)
GLFN(void, glDeleteFramebuffers, GLsizei, const GLuint*)
GLFN(void, glReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)
GLFN(void, glGetIntegerv, GLenum, GLint*)
GLFN(GLenum, glGetError, void)
GLFN(void, glPixelStorei, GLenum, GLint)

#undef GLFN

bool loadGL() {
    bool ok = true;
    auto load = [&](void** slot, const char* name) {
        *slot = (void*)SDL_GL_GetProcAddress(name);
        if (!*slot) {
            std::fprintf(stderr, "Output: missing GL function %s\n", name);
            ok = false;
        }
    };
#define L(name) load((void**)&p_##name, #name)
    L(glGenFramebuffers); L(glBindFramebuffer); L(glFramebufferTexture2D);
    L(glCheckFramebufferStatus); L(glDeleteFramebuffers); L(glReadPixels);
    L(glGetIntegerv); L(glGetError); L(glPixelStorei);
#undef L
    return ok;
}

} // namespace

bool OutputManager::init(SDL_Renderer* renderer) {
    renderer_ = renderer;

    // Build the backend list first: it is independent of the GL path, so the UI
    // can list (greyed-out) backends even if readback is unavailable.
    backends_.clear();
#ifdef JPLAY_ENABLE_NDI
    backends_.push_back({ "ndi", "NDI", ndiRuntimeAvailable(), &createNdiOutput });
#endif
#ifdef JPLAY_ENABLE_DECKLINK
    backends_.push_back({ "decklink", "SDI (DeckLink)", deckLinkRuntimeAvailable(),
                          &createDeckLinkOutput });
#endif

    if (!renderer_)
        return false;

    // Readback requires the OpenGL backend (same constraint as OcioGpu).
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
    const char* name = SDL_GetStringProperty(props, SDL_PROP_RENDERER_NAME_STRING, "");
    if (!name || SDL_strcmp(name, "opengl") != 0) {
        std::fprintf(stderr, "Output: renderer backend is '%s', not 'opengl'; "
                             "external output disabled.\n", name ? name : "?");
        return false;
    }
    if (!loadGL())
        return false;

    p_glGenFramebuffers(1, &fbo_);
    glReady_ = fbo_ != 0;
    return glReady_;
}

bool OutputManager::select(const std::string& id) {
    // Tear down whatever is running first.
    if (device_) {
        device_->close();
        device_.reset();
        activeId_.clear();
    }
    if (id.empty())
        return true; // "off"

    const OutputBackendInfo* be = nullptr;
    for (const auto& b : backends_)
        if (b.id == id) { be = &b; break; }
    if (!be || !be->available || !be->create) {
        std::fprintf(stderr, "Output: backend '%s' unavailable\n", id.c_str());
        return false;
    }
    if (!glReady_ && !externalFeed_) {
        std::fprintf(stderr, "Output: no readback path available; cannot start '%s'\n", id.c_str());
        return false;
    }

    auto dev = be->create();
    if (!dev || !dev->open()) {
        std::fprintf(stderr, "Output: backend '%s' failed to open\n", id.c_str());
        return false;
    }
    device_ = std::move(dev);
    activeId_ = id;
    return true;
}

std::string OutputManager::statusLine() const {
    return device_ ? device_->status() : std::string();
}

bool OutputManager::readback(SDL_Texture* tex, int x, int y, int w, int h,
                            std::vector<uint8_t>& dst) {
    if (!glReady_ || !tex || w <= 0 || h <= 0)
        return false;

    SDL_PropertiesID tp = SDL_GetTextureProperties(tex);
    Sint64 texId = SDL_GetNumberProperty(tp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER, 0);
    Sint64 texTarget = SDL_GetNumberProperty(tp, SDL_PROP_TEXTURE_OPENGL_TEXTURE_TARGET_NUMBER, 0);
    if (texId == 0 || texTarget == 0)
        return false;

    // Submit SDL's queued GL commands before we touch GL state directly.
    SDL_FlushRenderer(renderer_);

    GLint prevReadFbo = 0;
    p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
    while (p_glGetError() != GL_NO_ERROR) {} // drain pre-existing errors

    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             (GLenum)texTarget, (GLuint)texId, 0);

    bool ok = p_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        dst.resize((size_t)w * h * 4);
        p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
        p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        // Texel row 0 is the image's top row (SDL uploads top-first), and a user
        // FBO reads from its lower-left, so glReadPixels yields rows top-first —
        // exactly the image order NDI expects, no vertical flip needed. That also
        // makes `y` an image row rather than a flipped one, so a sub-rect read
        // needs no adjustment either.
        p_glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst.data());
        if (p_glGetError() != GL_NO_ERROR)
            ok = false;
    }

    // Detach and restore the previous read framebuffer.
    p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             (GLenum)texTarget, 0, 0);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevReadFbo);
    return ok;
}

const uint8_t* OutputManager::readProgram(SDL_Texture* programTex, int w, int h) {
    if (!readback(programTex, 0, 0, w, h, pixels_))
        return nullptr;
    return pixels_.data();
}

const uint8_t* OutputManager::readProgramRect(SDL_Texture* tex, int x, int y, int w, int h) {
    if (!readback(tex, x, y, w, h, rectPixels_))
        return nullptr;
    return rectPixels_.data();
}

void OutputManager::submitFrame(const uint8_t* rgba, int w, int h, double fps) {
    if (!device_ || !rgba)
        return;
    OutputFrame f;
    f.rgba = rgba;
    f.width = w;
    f.height = h;
    f.fps = fps;
    device_->submit(f);
}

bool OutputManager::wantsHdr() const {
    return device_ && device_->wantsHdr();
}

void OutputManager::submitHdrFrame(const Imath::half* scRgb, int w, int h, double fps,
                                   OutputTransfer transfer, float refWhiteNits) {
    if (!device_ || !scRgb || transfer == OutputTransfer::None)
        return;
    OutputFrame f;
    f.scRgb = scRgb;
    f.width = w;
    f.height = h;
    f.fps = fps;
    f.transfer = transfer;
    f.refWhiteNits = refWhiteNits;
    device_->submit(f);
}

void OutputManager::present(SDL_Texture* programTex, int w, int h, double fps) {
    if (!device_)
        return;
    submitFrame(readProgram(programTex, w, h), w, h, fps);
}

void OutputManager::shutdown() {
    if (device_) {
        device_->close();
        device_.reset();
        activeId_.clear();
    }
    if (fbo_ && p_glDeleteFramebuffers) {
        p_glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    glReady_ = false;
}
