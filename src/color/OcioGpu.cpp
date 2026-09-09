#include "OcioGpu.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>


#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// GL entry points.
//
// On Windows opengl32.dll only exports GL 1.1, so everything newer must be
// resolved at runtime. We load every function we use via SDL_GL_GetProcAddress
// (which falls back to the system GL lib for 1.1 symbols) rather than pull in a
// loader library. Constants come from SDL's bundled GL headers above.
// ---------------------------------------------------------------------------
namespace {

#define GLFN(ret, name, ...)                                  \
    typedef ret(APIENTRY* PFN_##name)(__VA_ARGS__);           \
    PFN_##name p_##name = nullptr;

GLFN(void, glGenTextures, GLsizei, GLuint*)
GLFN(void, glBindTexture, GLenum, GLuint)
GLFN(void, glDeleteTextures, GLsizei, const GLuint*)
GLFN(void, glTexParameteri, GLenum, GLenum, GLint)
GLFN(void, glTexImage1D, GLenum, GLint, GLint, GLsizei, GLint, GLenum, GLenum, const void*)
GLFN(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)
GLFN(void, glTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)
GLFN(void, glTexImage3D, GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)
GLFN(void, glActiveTexture, GLenum)
GLFN(void, glViewport, GLint, GLint, GLsizei, GLsizei)
GLFN(void, glClear, GLbitfield)
GLFN(void, glClearColor, GLfloat, GLfloat, GLfloat, GLfloat)
GLFN(void, glDrawArrays, GLenum, GLint, GLsizei)
GLFN(void, glPixelStorei, GLenum, GLint)
GLFN(void, glGetIntegerv, GLenum, GLint*)
GLFN(void, glEnable, GLenum)
GLFN(void, glDisable, GLenum)
GLFN(GLboolean, glIsEnabled, GLenum)
GLFN(GLenum, glGetError, void)

GLFN(void, glGenBuffers, GLsizei, GLuint*)
GLFN(void, glBindBuffer, GLenum, GLuint)
GLFN(void, glBufferData, GLenum, GLsizeiptr, const void*, GLenum)
GLFN(void*, glMapBufferRange, GLenum, GLintptr, GLsizeiptr, GLbitfield)
GLFN(GLboolean, glUnmapBuffer, GLenum)
GLFN(GLsync, glFenceSync, GLenum, GLbitfield)
GLFN(GLenum, glClientWaitSync, GLsync, GLbitfield, GLuint64)
GLFN(void, glDeleteSync, GLsync)
GLFN(void, glDeleteBuffers, GLsizei, const GLuint*)

GLFN(void, glGenVertexArrays, GLsizei, GLuint*)
GLFN(void, glBindVertexArray, GLuint)
GLFN(void, glDeleteVertexArrays, GLsizei, const GLuint*)

GLFN(GLuint, glCreateShader, GLenum)
GLFN(void, glShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)
GLFN(void, glCompileShader, GLuint)
GLFN(void, glGetShaderiv, GLuint, GLenum, GLint*)
GLFN(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)
GLFN(void, glDeleteShader, GLuint)
GLFN(GLuint, glCreateProgram, void)
GLFN(void, glAttachShader, GLuint, GLuint)
GLFN(void, glBindAttribLocation, GLuint, GLuint, const GLchar*)
GLFN(void, glLinkProgram, GLuint)
GLFN(void, glGetProgramiv, GLuint, GLenum, GLint*)
GLFN(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)
GLFN(void, glUseProgram, GLuint)
GLFN(void, glDeleteProgram, GLuint)
GLFN(GLint, glGetUniformLocation, GLuint, const GLchar*)
GLFN(void, glUniform1i, GLint, GLint)
GLFN(void, glUniform1f, GLint, GLfloat)
GLFN(void, glEnableVertexAttribArray, GLuint)
GLFN(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)

GLFN(void, glGenFramebuffers, GLsizei, GLuint*)
GLFN(void, glBindFramebuffer, GLenum, GLuint)
GLFN(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)
GLFN(GLenum, glCheckFramebufferStatus, GLenum)
GLFN(void, glDeleteFramebuffers, GLsizei, const GLuint*)

#undef GLFN

bool loadGL() {
    bool ok = true;
    auto load = [&](void** slot, const char* name) {
        *slot = (void*)SDL_GL_GetProcAddress(name);
        if (!*slot) {
            std::fprintf(stderr, "OCIO GPU: missing GL function %s\n", name);
            ok = false;
        }
    };
#define L(name) load((void**)&p_##name, #name)
    L(glGenTextures); L(glBindTexture); L(glDeleteTextures); L(glTexParameteri);
    L(glTexImage1D); L(glTexImage2D); L(glTexSubImage2D); L(glTexImage3D);
    L(glActiveTexture);
    L(glViewport); L(glClear); L(glClearColor); L(glDrawArrays); L(glPixelStorei);
    L(glGetIntegerv); L(glEnable); L(glDisable); L(glIsEnabled); L(glGetError);
    L(glGenBuffers); L(glBindBuffer); L(glBufferData); L(glDeleteBuffers);
    L(glMapBufferRange); L(glUnmapBuffer);
    L(glFenceSync); L(glClientWaitSync); L(glDeleteSync);
    L(glGenVertexArrays); L(glBindVertexArray); L(glDeleteVertexArrays);
    L(glCreateShader); L(glShaderSource); L(glCompileShader); L(glGetShaderiv);
    L(glGetShaderInfoLog); L(glDeleteShader); L(glCreateProgram); L(glAttachShader);
    L(glBindAttribLocation); L(glLinkProgram); L(glGetProgramiv); L(glGetProgramInfoLog);
    L(glUseProgram); L(glDeleteProgram); L(glGetUniformLocation); L(glUniform1i);
    L(glUniform1f); L(glEnableVertexAttribArray); L(glVertexAttribPointer);
    L(glGenFramebuffers); L(glBindFramebuffer); L(glFramebufferTexture2D);
    L(glCheckFramebufferStatus); L(glDeleteFramebuffers);
#undef L
    return ok;
}

// Compile one shader stage; returns 0 on failure (logging the compile error).
GLuint compileShader(GLenum type, const char* src) {
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, nullptr);
    p_glCompileShader(sh);
    GLint status = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint len = 0;
        p_glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log((size_t)(len > 0 ? len : 1), '\0');
        p_glGetShaderInfoLog(sh, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "OCIO GPU: shader compile failed:\n%s\n", log.c_str());
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// Fullscreen-triangle vertex shader, shared by the OCIO program and the nit map.
const char* kFullscreenVS =
    "#version 130\n"
    "in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vUV = aPos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

// Scene-referred nit heatmap: luminance in nits -> log-mapped heat colour, with
// no display transform. The 8 control colours match the on-stage legend ramp in
// App_TechCheck.cpp (renderTechOverlay) so the image and the scale agree.
const char* kNitFS =
    "#version 130\n"
    "in vec2 vUV; out vec4 fragColor;\n"
    "uniform sampler2D inputTex;\n"
    "uniform float uExposure;\n"   // scene-linear exposure, in stops
    "uniform float uNitScale;\n"   // scene-linear 1.0 -> uNitScale nits
    "vec3 heat(float t){\n"
    "  t = clamp(t, 0.0, 1.0) * 7.0;\n"
    "  vec3 c0=vec3(0.25,0.0,0.35), c1=vec3(0.10,0.15,0.70), c2=vec3(0.0,0.55,0.55),\n"
    "       c3=vec3(0.10,0.75,0.25), c4=vec3(0.50,0.50,0.50), c5=vec3(0.90,0.75,0.10),\n"
    "       c6=vec3(1.0,0.50,0.0),  c7=vec3(1.0,0.05,0.05);\n"
    "  if(t<1.0) return mix(c0,c1,t);\n"
    "  if(t<2.0) return mix(c1,c2,t-1.0);\n"
    "  if(t<3.0) return mix(c2,c3,t-2.0);\n"
    "  if(t<4.0) return mix(c3,c4,t-3.0);\n"
    "  if(t<5.0) return mix(c4,c5,t-4.0);\n"
    "  if(t<6.0) return mix(c5,c6,t-5.0);\n"
    "  return mix(c6,c7,t-6.0);\n"
    "}\n"
    "void main() {\n"
    "  vec3 lin = texture(inputTex, vUV).rgb * exp2(uExposure);\n"
    "  float L = dot(max(lin, vec3(0.0)), vec3(0.2126,0.7152,0.0722));\n"
    "  float nits = L * uNitScale;\n"
    "  float t = (log2(max(nits,1e-4)) - log2(0.1)) / (log2(10000.0) - log2(0.1));\n"
    "  fragColor = vec4(heat(t), 1.0);\n"
    "}\n";

// Compile a simple vs/fs pair into a linked program; 0 on failure (logs errors).
GLuint linkSimpleProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) { if (vs) p_glDeleteShader(vs); if (fs) p_glDeleteShader(fs); return 0; }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glBindAttribLocation(prog, 0, "aPos");
    p_glLinkProgram(prog);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint linked = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint len = 0;
        p_glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log((size_t)(len > 0 ? len : 1), '\0');
        p_glGetProgramInfoLog(prog, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "OCIO GPU: program link failed:\n%s\n", log.c_str());
        p_glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

} // namespace

bool OcioGpu::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
    ready_ = false;
    if (!renderer_) return false;

    // The GL interop only works when SDL is using its OpenGL backend.
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
    const char* name = SDL_GetStringProperty(props, SDL_PROP_RENDERER_NAME_STRING, "");
    if (!name || SDL_strcmp(name, "opengl") != 0) {
        std::fprintf(stderr, "OCIO GPU: renderer backend is '%s', not 'opengl'; "
                             "falling back to CPU.\n", name ? name : "?");
        return false;
    }
    if (!loadGL()) return false;

    // Static resources: a fullscreen triangle and the FBO we render through.
    static const float verts[6] = { -1.0f, -1.0f,  3.0f, -1.0f,  -1.0f, 3.0f };
    p_glGenVertexArrays(1, &vao_);
    p_glBindVertexArray(vao_);
    p_glGenBuffers(1, &vbo_);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
    p_glBindVertexArray(0);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);

    p_glGenFramebuffers(1, &fbo_);

    // Escape hatch for the staged-upload path: if a driver mishandles mapped
    // unpack buffers, JPLAY_NO_PBO=1 reverts to uploading straight from host
    // memory. Also the A/B for measuring what staging buys.
    const char* noPbo = SDL_getenv("JPLAY_NO_PBO");
    pboEnabled_ = !(noPbo && *noPbo && *noPbo != '0');

    ready_ = true;
    return true;
}

// The nit heatmap is a tech-check view most sessions never open, and compiling it
// is the bulk of what init() used to cost on a slow GLSL compiler (~40 ms on an
// Intel iGPU, on the critical path to the window appearing). Build it on first use
// instead, exactly as App::ensureGradeGpu defers the grade shaders. One attempt
// only: a compile failure must not be retried every frame.
bool OcioGpu::ensureNitProgram_() {
    if (nitTried_) return nitProg_ != 0;
    nitTried_ = true;
    // Called mid-frame, so leave the bound program exactly as we found it.
    GLint prevProgram = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    nitProg_ = linkSimpleProgram(kFullscreenVS, kNitFS);
    if (nitProg_) {
        p_glUseProgram(nitProg_);
        GLint loc = p_glGetUniformLocation(nitProg_, "inputTex");
        if (loc >= 0) p_glUniform1i(loc, 0);
        nitExpLoc_ = p_glGetUniformLocation(nitProg_, "uExposure");
        nitScaleLoc_ = p_glGetUniformLocation(nitProg_, "uNitScale");
    }
    p_glUseProgram((GLuint)prevProgram);
    return nitProg_ != 0;
}

void OcioGpu::destroyProgram_(Program& p) {
    if (p.id) { p_glDeleteProgram(p.id); p.id = 0; }
    for (const Lut& l : p.luts)
        if (l.id) p_glDeleteTextures(1, &l.id);
    p.luts.clear();
    p.expLoc = -1;
    p.version = -1;
}

void OcioGpu::setProcessors(const OCIO::ConstProcessorRcPtr& toWorking,
                            const OCIO::ConstProcessorRcPtr& toDisplay, int version) {
    // Gate on attempted_, not on active_(): a FAILED build also counts as handled,
    // otherwise the retry runs the whole OCIO extraction and GLSL compile on every
    // composited frame and stalls playback outright.
    if (version == version_ && attempted_) return;

    version_ = version;
    attempted_ = true;
    activeIdx_ = -1;
    if (!toDisplay) return;

    // Already built: promote to most-recently-used and rebind. This is the cut
    // between two sources of different colour spaces, taken every time the playhead
    // crosses back — the expensive display LUTs are already resident.
    for (size_t i = 0; i < programs_.size(); ++i) {
        if (programs_[i].version == version) {
            Program moved = std::move(programs_[i]);
            programs_.erase(programs_.begin() + (ptrdiff_t)i);
            programs_.push_back(std::move(moved));
            activeIdx_ = (int)programs_.size() - 1;
            return;
        }
    }

    pendingToWorking_ = toWorking;
    pendingToDisplay_ = toDisplay;
    activeIdx_ = buildProgram_(version); // build eagerly; render() falls back if it failed
    pendingToWorking_ = nullptr;
    pendingToDisplay_ = nullptr;
}

void OcioGpu::uploadDescLuts_(const OCIO::GpuShaderDescRcPtr& desc,
                              std::vector<Lut>& luts, int& unit) {
    // Tightly-packed LUT data; clear any stride left by SDL_UpdateTexture.
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    p_glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);

    // 1D / 2D LUTs.
    const unsigned numTex = desc->getNumTextures();
    for (unsigned i = 0; i < numTex; ++i) {
        const char* texName = nullptr;
        const char* samplerName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType channel = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dims = OCIO::GpuShaderDesc::TEXTURE_2D;
        OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
        desc->getTexture(i, texName, samplerName, w, h, channel, dims, interp);
        const float* values = nullptr;
        desc->getTextureValues(i, values);

        GLenum target = (dims == OCIO::GpuShaderDesc::TEXTURE_1D) ? GL_TEXTURE_1D : GL_TEXTURE_2D;
        GLint  internal = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? GL_R32F : GL_RGB32F;
        GLenum format = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? GL_RED : GL_RGB;
        GLint  filter = (interp == OCIO::INTERP_NEAREST) ? GL_NEAREST : GL_LINEAR;

        GLuint id = 0;
        p_glGenTextures(1, &id);
        p_glBindTexture(target, id);
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (target == GL_TEXTURE_1D) {
            p_glTexImage1D(target, 0, internal, (GLsizei)w, 0, format, GL_FLOAT, values);
        } else {
            p_glTexImage2D(target, 0, internal, (GLsizei)w, (GLsizei)h, 0, format, GL_FLOAT, values);
        }
        p_glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
        p_glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
        p_glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        if (target == GL_TEXTURE_2D)
            p_glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glBindTexture(target, 0);

        luts.push_back({ id, target, samplerName ? samplerName : "", unit++ });
    }

    // 3D LUTs.
    const unsigned num3D = desc->getNum3DTextures();
    for (unsigned i = 0; i < num3D; ++i) {
        const char* texName = nullptr;
        const char* samplerName = nullptr;
        unsigned edge = 0;
        OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
        desc->get3DTexture(i, texName, samplerName, edge, interp);
        const float* values = nullptr;
        desc->get3DTextureValues(i, values);

        GLint filter = (interp == OCIO::INTERP_NEAREST) ? GL_NEAREST : GL_LINEAR;
        GLuint id = 0;
        p_glGenTextures(1, &id);
        p_glBindTexture(GL_TEXTURE_3D, id);
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        p_glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F,
                       (GLsizei)edge, (GLsizei)edge, (GLsizei)edge,
                       0, GL_RGB, GL_FLOAT, values);
        p_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, filter);
        p_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, filter);
        p_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        p_glBindTexture(GL_TEXTURE_3D, 0);

        luts.push_back({ id, GL_TEXTURE_3D, samplerName ? samplerName : "", unit++ });
    }
}

int OcioGpu::buildProgram_(int version) {
    if (!ready_ || !pendingToDisplay_) return -1;

    Program prog;
    prog.version = version;
    int unit = 1; // unit 0 is the input frame

    try {
        // Two OCIO functions in one program: the media colour space into the linear
        // working space, then the working space to the display, with the exposure
        // between them. Distinct function names and resource prefixes keep the two
        // sets of generated samplers apart. The input half is absent for a media
        // already in the working space, and the wrapper then simply skips the call.
        std::string inputText;
        if (pendingToWorking_) {
            OCIO::GpuShaderDescRcPtr inDesc = OCIO::GpuShaderDesc::CreateShaderDesc();
            inDesc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_3);
            inDesc->setFunctionName("OCIOInput");
            inDesc->setResourcePrefix("ocioin_");
            pendingToWorking_->getDefaultGPUProcessor()->extractGpuShaderInfo(inDesc);
            inputText = inDesc->getShaderText();
            uploadDescLuts_(inDesc, prog.luts, unit);
        }

        OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_3);
        desc->setFunctionName("OCIOMain");
        desc->setResourcePrefix("ocio_");
        pendingToDisplay_->getDefaultGPUProcessor()->extractGpuShaderInfo(desc);
        uploadDescLuts_(desc, prog.luts, unit);

        // Assemble the GLSL program. The OCIO-generated text supplies the sampler
        // uniforms and the two functions; we wrap them with a fullscreen pass.
        const std::string ocioText = desc->getShaderText();
        const std::string vsSrc = kFullscreenVS;
        const std::string fsSrc =
            "#version 130\n"
            "in vec2 vUV;\n"
            "out vec4 fragColor;\n"
            "uniform sampler2D inputTex;\n"
            "uniform float uExposure;\n"
            + inputText + ocioText +
            "\nvoid main() {\n"
            "    vec4 c = texture(inputTex, vUV);\n"
            // The media colour space into the linear working space. An 8- or
            // 16-bit source arrives here already normalised to 0..1 by the
            // sampler, which is what an integer-encoded colour space expects.
            + (inputText.empty() ? "" : "    c = OCIOInput(c);\n") +
            // Exposure, in the working space, so a stop means the same thing on
            // an EXR and on a ProRes.
            "    c.rgb *= exp2(uExposure);\n"
            "    fragColor = OCIOMain(c);\n"
            "}\n";

        GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc.c_str());
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc.c_str());
        if (!vs || !fs) {
            if (vs) p_glDeleteShader(vs);
            if (fs) p_glDeleteShader(fs);
            destroyProgram_(prog);
            return -1;
        }

        prog.id = p_glCreateProgram();
        p_glAttachShader(prog.id, vs);
        p_glAttachShader(prog.id, fs);
        p_glBindAttribLocation(prog.id, 0, "aPos");
        p_glLinkProgram(prog.id);
        p_glDeleteShader(vs);
        p_glDeleteShader(fs);

        GLint linked = 0;
        p_glGetProgramiv(prog.id, GL_LINK_STATUS, &linked);
        if (!linked) {
            GLint len = 0;
            p_glGetProgramiv(prog.id, GL_INFO_LOG_LENGTH, &len);
            std::string log((size_t)(len > 0 ? len : 1), '\0');
            p_glGetProgramInfoLog(prog.id, (GLsizei)log.size(), nullptr, log.data());
            std::fprintf(stderr, "OCIO GPU: program link failed:\n%s\n", log.c_str());
            destroyProgram_(prog);
            return -1;
        }

        // Bind sampler uniforms to their texture units once.
        p_glUseProgram(prog.id);
        GLint loc = p_glGetUniformLocation(prog.id, "inputTex");
        if (loc >= 0) p_glUniform1i(loc, 0);
        for (const Lut& l : prog.luts) {
            GLint sl = p_glGetUniformLocation(prog.id, l.sampler.c_str());
            if (sl >= 0) p_glUniform1i(sl, l.unit);
        }
        prog.expLoc = p_glGetUniformLocation(prog.id, "uExposure");
        p_glUseProgram(0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OCIO GPU: build failed: %s\n", e.what());
        destroyProgram_(prog);
        return -1;
    }

    // Make room, oldest first. Every entry holds GL objects, so an evicted one is
    // deleted rather than dropped.
    while (programs_.size() >= kMaxPrograms) {
        destroyProgram_(programs_.front());
        programs_.erase(programs_.begin());
    }
    programs_.push_back(std::move(prog));
    return (int)programs_.size() - 1;
}

// Upload one source frame into inputTex_.
//
// Reallocating the texture is by far the most expensive thing on the colour-managed
// playback path: at 8192x3432 scene-linear half that is 168 MB of driver-side
// allocation per frame, and glTexImage2D reallocates on every call. Since the shape
// only changes when the playhead cuts to differently-sized media, allocate once per
// shape and overwrite in place after that -- the same bytes cross the bus, without
// the allocation.
//
// Caller must have GL state saved and pixel-store defaults set; leaves inputTex_
// bound on texture unit 0, which is where both callers' draws expect it.
void OcioGpu::uploadInput_(const void* pixels, InputFormat fmt, int width, int height) {
    // The two integer formats are deliberately *not* the GL_SRGB8 variants: OCIO
    // must see the stored code values, not a set GL decided to linearise behind
    // its back.
    GLint  internal = GL_RGB16F;
    GLenum format = GL_RGB, type = GL_HALF_FLOAT;
    size_t bpp = 3 * sizeof(Imath::half);
    switch (fmt) {
    case InputFormat::Rgba16: internal = GL_RGBA16; format = GL_RGBA; type = GL_UNSIGNED_SHORT;
                              bpp = 4 * sizeof(uint16_t); break;
    case InputFormat::Rgba8:  internal = GL_RGBA8;  format = GL_RGBA; type = GL_UNSIGNED_BYTE;
                              bpp = 4; break;
    case InputFormat::SceneLinearHalf: break;
    }

    if (!inputTex_) p_glGenTextures(1, &inputTex_);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, inputTex_);

    const bool reshaped = (width != inW_ || height != inH_ || (int)fmt != inFmt_);

    // Stage the frame through a pixel-unpack buffer. With a PBO bound the texture
    // call names an offset into GPU-visible memory rather than a host pointer, so
    // the driver queues a DMA and returns instead of copying 168 MB inline -- the
    // transfer then overlaps the rest of the frame. What stays synchronous is the
    // memcpy into the mapped buffer, which is plain memory bandwidth (a few ms)
    // rather than a driver upload. A reshape stages too -- glTexImage2D reads from a
    // bound unpack buffer just as glTexSubImage2D does, and one path is simpler than
    // two for the sake of the rare frame where the shape actually changed.
    // Whatever happens below, the unpack binding this was called with is restored:
    // when no buffer is bound the texture calls read `pixels` as a host pointer,
    // and when one is bound they read it as an offset into that buffer, so an
    // unexpected binding either way would have GL fetch from the wrong place.
    GLint prevUnpack = 0;
    p_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevUnpack);

    int slot = -1;
    if (pboEnabled_)
        slot = stagePbo_(pixels, (size_t)width * height * bpp);
    // A staged upload needs the ring buffer stagePbo_ left bound; an unstaged one
    // needs no buffer bound at all. Either way we must not inherit whatever the
    // caller had, and stagePbo_ may have failed with its own buffer still bound.
    if (slot < 0)
        p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    // With a buffer bound, the pixels argument is an offset into it, not a pointer.
    const void* src = slot >= 0 ? nullptr : pixels;

    // inW_/inH_/inFmt_ describe what the texture is actually allocated as, so they
    // are set on every reallocation and nowhere else -- the sub-upload below trusts
    // them to mean the internal format matches `fmt`.
    if (reshaped) {
        p_glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, format, type, src);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        inW_ = width;
        inH_ = height;
        inFmt_ = (int)fmt;
    } else {
        p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, src);
    }

    if (slot >= 0) {
        // Mark where the DMA reading this buffer sits in the command stream, so the
        // ring can tell whether it is safe to overwrite (see stagePbo_).
        pbos_[slot].sync = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)prevUnpack);
}

// Copy `bytes` from `pixels` into the next pixel-unpack buffer of the ring, leaving
// that buffer bound to GL_PIXEL_UNPACK_BUFFER for the caller's texture call.
// Returns the ring slot, or -1 if the frame could not be staged -- in which case
// nothing is left bound and the caller uploads straight from host memory.
int OcioGpu::stagePbo_(const void* pixels, size_t bytes) {
    const int slot = pboNext_;
    Pbo& b = pbos_[slot];
    if (!b.id) {
        p_glGenBuffers(1, &b.id);
        if (!b.id) return -1;
    }
    p_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, b.id);

    // Wait out the DMA that last read this buffer before overwriting it. With three
    // buffers and one upload per frame that fence is long since signalled, so this
    // costs nothing; without it a GPU running behind would have the memcpy below
    // overwrite pixels it had not finished reading. A wait that does not complete
    // means something is badly wrong, so fall back rather than risk the corruption.
    if (b.sync) {
        const GLenum r = p_glClientWaitSync((GLsync)b.sync, GL_SYNC_FLUSH_COMMANDS_BIT,
                                            1000ull * 1000ull * 1000ull); // 1 s
        p_glDeleteSync((GLsync)b.sync);
        b.sync = nullptr;
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED)
            return -1; // caller restores the binding
    }

    // Allocated once per frame shape, then reused -- the whole point of the ring.
    if (b.size != bytes) {
        p_glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_DRAW);
        b.size = bytes;
    }

    // INVALIDATE_RANGE says the previous contents are dead (we overwrite all of it)
    // without orphaning the allocation; UNSYNCHRONIZED says not to insert a wait,
    // which is safe because the fence above already established it.
    void* dst = p_glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)bytes,
                                   GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                                   GL_MAP_UNSYNCHRONIZED_BIT);
    if (!dst)
        return -1; // caller restores the binding
    SDL_memcpy(dst, pixels, bytes);
    if (p_glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_FALSE)
        return -1; // caller restores the binding

    pboNext_ = (slot + 1) % kNumPbos;
    return slot;
}

bool OcioGpu::render(const void* pixels, InputFormat fmt, int width, int height,
                     unsigned dstTexGL, unsigned dstTexTarget, float exposureEV) {
    Program* prog = active_();
    if (!ready_ || !prog || !prog->id || !pixels || width <= 0 || height <= 0)
        return false;

    // Submit SDL's queued GL commands before we change GL state out from under it.
    SDL_FlushRenderer(renderer_);

    // Save the GL state SDL relies on so we can restore it afterwards.
    GLint prevFbo = 0, prevViewport[4] = { 0, 0, 0, 0 };
    GLint prevProgram = 0, prevVao = 0, prevArrayBuf = 0, prevActiveTex = 0, prevTex2D = 0;
    p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    p_glGetIntegerv(GL_VIEWPORT, prevViewport);
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    p_glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D);
    GLboolean prevBlend   = p_glIsEnabled(GL_BLEND);
    GLboolean prevScissor = p_glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevCull    = p_glIsEnabled(GL_CULL_FACE);
    GLboolean prevDepth   = p_glIsEnabled(GL_DEPTH_TEST);
    while (p_glGetError() != GL_NO_ERROR) {} // drain pre-existing errors

    // Reset pixel-store state: SDL_UpdateTexture leaves GL_UNPACK_ROW_LENGTH (and
    // skip rows/pixels) set to the last texture's pitch, which would skew our
    // tightly-packed upload. Force defaults for an interleaved, unpadded read.
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    uploadInput_(pixels, fmt, width, height);

    // Render the OCIO pass into the destination texture via the FBO.
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             (GLenum)dstTexTarget, (GLuint)dstTexGL, 0);
    bool fboOk = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (fboOk) {
        p_glViewport(0, 0, width, height);
        p_glDisable(GL_BLEND);        // overwrite, don't blend
        p_glDisable(GL_SCISSOR_TEST); // SDL leaves a clip rect set
        p_glDisable(GL_CULL_FACE);    // don't cull the fullscreen triangle
        p_glDisable(GL_DEPTH_TEST);
        p_glUseProgram(prog->id);
        if (prog->expLoc >= 0) p_glUniform1f(prog->expLoc, exposureEV);

        p_glActiveTexture(GL_TEXTURE0);
        p_glBindTexture(GL_TEXTURE_2D, inputTex_);
        for (const Lut& l : prog->luts) {
            p_glActiveTexture(GL_TEXTURE0 + l.unit);
            p_glBindTexture(l.target, l.id);
        }

        p_glBindVertexArray(vao_);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
        p_glBindVertexArray(0);
    } else {
        std::fprintf(stderr, "OCIO GPU: framebuffer incomplete\n");
    }

    // Detach to avoid holding a reference to the SDL texture, then restore state.
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             (GLenum)dstTexTarget, 0, 0);
    p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    p_glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    p_glUseProgram((GLuint)prevProgram);
    p_glBindVertexArray((GLuint)prevVao);
    p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2D);
    p_glActiveTexture((GLenum)prevActiveTex);
    if (prevBlend)   p_glEnable(GL_BLEND);         else p_glDisable(GL_BLEND);
    if (prevScissor) p_glEnable(GL_SCISSOR_TEST);  else p_glDisable(GL_SCISSOR_TEST);
    if (prevCull)    p_glEnable(GL_CULL_FACE);     else p_glDisable(GL_CULL_FACE);
    if (prevDepth)   p_glEnable(GL_DEPTH_TEST);    else p_glDisable(GL_DEPTH_TEST);

    return fboOk;
}


bool OcioGpu::renderNitHeatmap(const Imath::half* linear, int width, int height,
                               unsigned dstTexGL, unsigned dstTexTarget,
                               float exposureEV, float nitScale) {
    if (!ready_ || !linear || width <= 0 || height <= 0)
        return false;

    SDL_FlushRenderer(renderer_);

    // Built on first use, so this is where the compile happens — after the flush, so
    // SDL's queued commands are submitted before we touch the program binding.
    if (!ensureNitProgram_())
        return false;

    // Save the GL state SDL relies on so we can restore it afterwards.
    GLint prevFbo = 0, prevViewport[4] = { 0, 0, 0, 0 };
    GLint prevProgram = 0, prevVao = 0, prevArrayBuf = 0, prevActiveTex = 0, prevTex2D = 0;
    p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    p_glGetIntegerv(GL_VIEWPORT, prevViewport);
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    p_glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D);
    GLboolean prevBlend   = p_glIsEnabled(GL_BLEND);
    GLboolean prevScissor = p_glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevCull    = p_glIsEnabled(GL_CULL_FACE);
    GLboolean prevDepth   = p_glIsEnabled(GL_DEPTH_TEST);
    while (p_glGetError() != GL_NO_ERROR) {}

    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    uploadInput_(linear, InputFormat::SceneLinearHalf, width, height);

    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             (GLenum)dstTexTarget, (GLuint)dstTexGL, 0);
    bool fboOk = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (fboOk) {
        p_glViewport(0, 0, width, height);
        p_glDisable(GL_BLEND);
        p_glDisable(GL_SCISSOR_TEST);
        p_glDisable(GL_CULL_FACE);
        p_glDisable(GL_DEPTH_TEST);
        p_glUseProgram(nitProg_);
        if (nitExpLoc_ >= 0) p_glUniform1f(nitExpLoc_, exposureEV);
        if (nitScaleLoc_ >= 0) p_glUniform1f(nitScaleLoc_, nitScale);
        p_glActiveTexture(GL_TEXTURE0);
        p_glBindTexture(GL_TEXTURE_2D, inputTex_);
        p_glBindVertexArray(vao_);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
        p_glBindVertexArray(0);
    } else {
        std::fprintf(stderr, "OCIO GPU: nit-heatmap framebuffer incomplete\n");
    }

    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             (GLenum)dstTexTarget, 0, 0);
    p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    p_glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    p_glUseProgram((GLuint)prevProgram);
    p_glBindVertexArray((GLuint)prevVao);
    p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2D);
    p_glActiveTexture((GLenum)prevActiveTex);
    if (prevBlend)   p_glEnable(GL_BLEND);         else p_glDisable(GL_BLEND);
    if (prevScissor) p_glEnable(GL_SCISSOR_TEST);  else p_glDisable(GL_SCISSOR_TEST);
    if (prevCull)    p_glEnable(GL_CULL_FACE);     else p_glDisable(GL_CULL_FACE);
    if (prevDepth)   p_glEnable(GL_DEPTH_TEST);    else p_glDisable(GL_DEPTH_TEST);

    return fboOk;
}
