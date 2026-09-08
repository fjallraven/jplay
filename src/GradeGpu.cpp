#include "GradeGpu.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_opengl_glext.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// GL entry points (resolved at runtime; see OcioGpu.cpp for the rationale).
// ---------------------------------------------------------------------------
namespace {

#define GLFN(ret, name, ...)                        \
    typedef ret(APIENTRY* PFN_##name)(__VA_ARGS__); \
    PFN_##name p_##name = nullptr;

GLFN(void, glGenTextures, GLsizei, GLuint*)
GLFN(void, glBindTexture, GLenum, GLuint)
GLFN(void, glTexParameteri, GLenum, GLenum, GLint)
GLFN(void, glTexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)
GLFN(void, glTexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)
GLFN(void, glActiveTexture, GLenum)
GLFN(void, glViewport, GLint, GLint, GLsizei, GLsizei)
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
GLFN(void, glGenVertexArrays, GLsizei, GLuint*)
GLFN(void, glBindVertexArray, GLuint)

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
GLFN(GLint, glGetUniformLocation, GLuint, const GLchar*)
GLFN(void, glUniform1i, GLint, GLint)
GLFN(void, glUniform1f, GLint, GLfloat)
GLFN(void, glUniform3f, GLint, GLfloat, GLfloat, GLfloat)
GLFN(void, glEnableVertexAttribArray, GLuint)
GLFN(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)

GLFN(void, glGenFramebuffers, GLsizei, GLuint*)
GLFN(void, glBindFramebuffer, GLenum, GLuint)
GLFN(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)
GLFN(GLenum, glCheckFramebufferStatus, GLenum)

#undef GLFN

bool loadGL() {
    bool ok = true;
    auto load = [&](void** slot, const char* name) {
        *slot = (void*)SDL_GL_GetProcAddress(name);
        if (!*slot) { std::fprintf(stderr, "Grade GPU: missing GL function %s\n", name); ok = false; }
    };
#define L(name) load((void**)&p_##name, #name)
    L(glGenTextures); L(glBindTexture); L(glTexParameteri); L(glTexImage2D);
    L(glTexSubImage2D); L(glActiveTexture); L(glViewport); L(glDrawArrays);
    L(glPixelStorei); L(glGetIntegerv); L(glEnable); L(glDisable); L(glIsEnabled);
    L(glGetError); L(glGenBuffers); L(glBindBuffer); L(glBufferData);
    L(glGenVertexArrays); L(glBindVertexArray); L(glCreateShader); L(glShaderSource);
    L(glCompileShader); L(glGetShaderiv); L(glGetShaderInfoLog); L(glDeleteShader);
    L(glCreateProgram); L(glAttachShader); L(glBindAttribLocation); L(glLinkProgram);
    L(glGetProgramiv); L(glGetProgramInfoLog); L(glUseProgram); L(glGetUniformLocation);
    L(glUniform1i); L(glUniform1f); L(glUniform3f); L(glEnableVertexAttribArray);
    L(glVertexAttribPointer); L(glGenFramebuffers); L(glBindFramebuffer);
    L(glFramebufferTexture2D); L(glCheckFramebufferStatus);
#undef L
    return ok;
}

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
        std::fprintf(stderr, "Grade GPU: shader compile failed:\n%s\n", log.c_str());
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint linkProgram(const char* vs, const char* fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { if (v) p_glDeleteShader(v); if (f) p_glDeleteShader(f); return 0; }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, v);
    p_glAttachShader(prog, f);
    p_glBindAttribLocation(prog, 0, "aPos");
    p_glLinkProgram(prog);
    p_glDeleteShader(v);
    p_glDeleteShader(f);
    GLint linked = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint len = 0;
        p_glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log((size_t)(len > 0 ? len : 1), '\0');
        p_glGetProgramInfoLog(prog, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "Grade GPU: link failed:\n%s\n", log.c_str());
        return 0;
    }
    return prog;
}

const char* kVS =
    "#version 130\n"
    "in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "void main(){ vUV = aPos*0.5+0.5; gl_Position = vec4(aPos,0.0,1.0); }\n";

const char* kCopyFS =
    "#version 130\n"
    "in vec2 vUV; out vec4 fragColor;\n"
    "uniform sampler2D uImage;\n"
    "void main(){ fragColor = texture(uImage, vUV); }\n";

const char* kGradeFS =
    "#version 130\n"
    "in vec2 vUV; out vec4 fragColor;\n"
    "uniform sampler2D uImage;\n"
    "uniform sampler2D uCurve;\n"
    "uniform vec3 uWbGain;\n"
    "uniform float uGain, uGamma;\n"
    "uniform float uSat;\n"
    "uniform vec3 uTintSh, uTintMid, uTintHi;\n"
    "uniform float uLumaSh, uLumaMid, uLumaHi;\n"
    "uniform int uHasCurves;\n"
    "uniform int uTech;\n"
    "float luma(vec3 c){ return dot(c, vec3(0.2126,0.7152,0.0722)); }\n"
    "vec3 s2l(vec3 c){ return pow(max(c,0.0), vec3(2.2)); }\n"
    "vec3 l2s(vec3 c){ return pow(max(c,0.0), vec3(1.0/2.2)); }\n"
    "void main(){\n"
    "  vec3 v = texture(uImage, vUV).rgb;\n"
    "  v *= uWbGain;\n"
    "  if(uGain != 0.0) v = l2s(s2l(v) * exp2(uGain));\n" // video path: gain in stops, display-space approx
    "  v = pow(max(v,0.0), vec3(1.0/uGamma));\n"
    "  float L = clamp(luma(v),0.0,1.0);\n"
    "  float shMask = (1.0-L)*(1.0-L);\n"
    "  float hlMask = L*L;\n"
    "  float midMask = clamp(1.0-shMask-hlMask,0.0,1.0);\n"
    "  v += uTintSh*shMask + uTintMid*midMask + uTintHi*hlMask;\n"
    "  v += uLumaSh*shMask + uLumaMid*midMask + uLumaHi*hlMask;\n"
    "  float L2 = luma(v);\n"
    "  v = mix(vec3(L2), v, 1.0 + uSat);\n"
    "  if(uHasCurves==1){\n"
    "    v = clamp(v,0.0,1.0);\n"
    "    v.r = texture(uCurve, vec2(v.r,0.5)).r;\n"
    "    v.g = texture(uCurve, vec2(v.g,0.5)).g;\n"
    "    v.b = texture(uCurve, vec2(v.b,0.5)).b;\n"
    "  }\n"
    "  v = clamp(v,0.0,1.0);\n"
    "  if(uTech == 2){\n"           // clipping warning
    "    bool crushed = (v.r <= 1.5/255.0 && v.g <= 1.5/255.0 && v.b <= 1.5/255.0);\n"
    "    bool blown   = (v.r >= 253.5/255.0 || v.g >= 253.5/255.0 || v.b >= 253.5/255.0);\n"
    "    v = vec3(luma(v));\n"
    "    if(crushed) v = vec3(0.0,1.0,1.0);\n"     // neon cyan
    "    else if(blown) v = vec3(1.0,0.0,0.06);\n" // neon red
    "  } else if(uTech == 3){\n"    // monochrome
    "    v = vec3(luma(v));\n"
    "  } else if(uTech == 4){\n"    // red channel only
    "    v = vec3(v.r);\n"
    "  } else if(uTech == 5){\n"    // green channel only
    "    v = vec3(v.g);\n"
    "  } else if(uTech == 6){\n"    // blue channel only
    "    v = vec3(v.b);\n"
    "  }\n"
    "  fragColor = vec4(v, 1.0);\n"
    "}\n";

bool curveEquals(const grade::Curve& a, const grade::Curve& b) {
    if (a.pts.size() != b.pts.size()) return false;
    for (size_t i = 0; i < a.pts.size(); ++i)
        if (a.pts[i].x != b.pts[i].x || a.pts[i].y != b.pts[i].y) return false;
    return true;
}

// HSV (h in degrees) -> RGB in [0,1].
void hsv2rgb(float h, float s, float val, float& r, float& g, float& b) {
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    float c = val * s;
    float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float m = val - c;
    float rr = 0, gg = 0, bb = 0;
    if (h < 1) { rr = c; gg = x; }
    else if (h < 2) { rr = x; gg = c; }
    else if (h < 3) { gg = c; bb = x; }
    else if (h < 4) { gg = x; bb = c; }
    else if (h < 5) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    r = rr + m; g = gg + m; b = bb + m;
}

} // namespace

bool GradeGpu::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
    ready_ = false;
    if (!renderer_) return false;
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
    const char* name = SDL_GetStringProperty(props, SDL_PROP_RENDERER_NAME_STRING, "");
    if (!name || SDL_strcmp(name, "opengl") != 0) return false;
    if (!loadGL()) return false;

    // init() is deferred to the first graded/tech-check frame, so it runs in the
    // middle of SDL's own rendering: flush its queue and put back every binding
    // we touch, or SDL's cached GL state goes stale. Same contract as apply().
    SDL_FlushRenderer(renderer_);
    GLint prevProgram = 0, prevVao = 0, prevArrayBuf = 0, prevTex2D = 0, prevUnpack = 4;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D);
    p_glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
    auto restoreState = [&] {
        p_glUseProgram((GLuint)prevProgram);
        p_glBindVertexArray((GLuint)prevVao);
        p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
        p_glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2D);
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
    };

    static const float verts[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
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

    if (!buildPrograms_()) { restoreState(); return false; }

    // 256x1 RGBA32F curve LUT.
    p_glGenTextures(1, &curveTex_);
    p_glBindTexture(GL_TEXTURE_2D, curveTex_);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    std::array<float, 256 * 4> id{};
    for (int i = 0; i < 256; ++i) { float x = i / 255.0f; id[i*4]=x; id[i*4+1]=x; id[i*4+2]=x; id[i*4+3]=x; }
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, 1, 0, GL_RGBA, GL_FLOAT, id.data());

    restoreState();
    ready_ = true;
    return true;
}

bool GradeGpu::buildPrograms_() {
    copyProg_ = linkProgram(kVS, kCopyFS);
    gradeProg_ = linkProgram(kVS, kGradeFS);
    if (!copyProg_ || !gradeProg_) return false;
    // Bind sampler units once.
    p_glUseProgram(copyProg_);
    GLint l = p_glGetUniformLocation(copyProg_, "uImage");
    if (l >= 0) p_glUniform1i(l, 0);
    p_glUseProgram(gradeProg_);
    l = p_glGetUniformLocation(gradeProg_, "uImage");
    if (l >= 0) p_glUniform1i(l, 0);
    l = p_glGetUniformLocation(gradeProg_, "uCurve");
    if (l >= 0) p_glUniform1i(l, 1);

    locWbGain_ = p_glGetUniformLocation(gradeProg_, "uWbGain");
    locGain_ = p_glGetUniformLocation(gradeProg_, "uGain");
    locGamma_ = p_glGetUniformLocation(gradeProg_, "uGamma");
    locSat_ = p_glGetUniformLocation(gradeProg_, "uSat");
    locTintSh_ = p_glGetUniformLocation(gradeProg_, "uTintSh");
    locLumaSh_ = p_glGetUniformLocation(gradeProg_, "uLumaSh");
    locTintMid_ = p_glGetUniformLocation(gradeProg_, "uTintMid");
    locLumaMid_ = p_glGetUniformLocation(gradeProg_, "uLumaMid");
    locTintHi_ = p_glGetUniformLocation(gradeProg_, "uTintHi");
    locLumaHi_ = p_glGetUniformLocation(gradeProg_, "uLumaHi");
    locHasCurves_ = p_glGetUniformLocation(gradeProg_, "uHasCurves");
    locTech_ = p_glGetUniformLocation(gradeProg_, "uTech");

    p_glUseProgram(0);
    return true;
}

void GradeGpu::ensureScratch_(int w, int h) {
    if (!scratchTex_) {
        p_glGenTextures(1, &scratchTex_);
        p_glBindTexture(GL_TEXTURE_2D, scratchTex_);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        scratchW_ = scratchH_ = 0;
    }
    if (w != scratchW_ || h != scratchH_) {
        p_glBindTexture(GL_TEXTURE_2D, scratchTex_);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        scratchW_ = w; scratchH_ = h;
    }
}

void GradeGpu::uploadCurves_(const grade::State& s) {
    std::array<float, 256> m{}, r{}, g{}, b{};
    grade::bakeCurve(s.curveMaster, m);
    grade::bakeCurve(s.curveR, r);
    grade::bakeCurve(s.curveG, g);
    grade::bakeCurve(s.curveB, b);
    // Compose master into each channel: composed(x) = channel(master(x)).
    std::array<float, 256 * 4> data{};
    for (int i = 0; i < 256; ++i) {
        float mv = m[i];
        int mi = std::clamp((int)std::lround(mv * 255.0f), 0, 255);
        data[i*4+0] = r[mi];
        data[i*4+1] = g[mi];
        data[i*4+2] = b[mi];
        data[i*4+3] = mv;
    }
    p_glBindTexture(GL_TEXTURE_2D, curveTex_);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_FLOAT, data.data());
    p_glBindTexture(GL_TEXTURE_2D, 0);
}

bool GradeGpu::apply(unsigned dstTexGL, unsigned dstTexTarget, int width, int height,
                     const grade::State& s, int techMode) {
    if (!ready_ || !dstTexGL || width <= 0 || height <= 0) return false;

    SDL_FlushRenderer(renderer_);

    GLint prevFbo = 0, prevViewport[4] = { 0, 0, 0, 0 };
    GLint prevProgram = 0, prevVao = 0, prevArrayBuf = 0, prevActiveTex = 0, prevTex2D = 0;
    p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    p_glGetIntegerv(GL_VIEWPORT, prevViewport);
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    p_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
    p_glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D);
    GLboolean prevBlend = p_glIsEnabled(GL_BLEND);
    GLboolean prevScissor = p_glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prevCull = p_glIsEnabled(GL_CULL_FACE);
    GLboolean prevDepth = p_glIsEnabled(GL_DEPTH_TEST);
    while (p_glGetError() != GL_NO_ERROR) {}

    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ensureScratch_(width, height);
    if (!curvesUploaded_ || !curveEquals(s.curveMaster, lastCurveMaster_) ||
        !curveEquals(s.curveR, lastCurveR_) || !curveEquals(s.curveG, lastCurveG_) ||
        !curveEquals(s.curveB, lastCurveB_)) {
        uploadCurves_(s);
        lastCurveMaster_ = s.curveMaster;
        lastCurveR_ = s.curveR;
        lastCurveG_ = s.curveG;
        lastCurveB_ = s.curveB;
        curvesUploaded_ = true;
    }

    p_glDisable(GL_BLEND);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_DEPTH_TEST);
    p_glViewport(0, 0, width, height);
    p_glBindVertexArray(vao_);

    // Pass 1: copy base image (dstTex) -> scratch.
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratchTex_, 0);
    bool ok = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        p_glUseProgram(copyProg_);
        p_glActiveTexture(GL_TEXTURE0);
        p_glBindTexture(GL_TEXTURE_2D, (GLuint)dstTexGL);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Pass 2: grade scratch -> dstTex.
    if (ok) {
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 (GLenum)dstTexTarget, (GLuint)dstTexGL, 0);
        ok = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (ok) {
        p_glUseProgram(gradeProg_);
        auto u1f = [&](GLint l, float v) { if (l >= 0) p_glUniform1f(l, v); };
        auto u3f = [&](GLint l, float a, float b, float c) { if (l >= 0) p_glUniform3f(l, a, b, c); };
        auto u1i = [&](GLint l, int v) { if (l >= 0) p_glUniform1i(l, v); };

        float t = s.temperature / 100.0f, ti = s.tint / 100.0f;
        u3f(locWbGain_, 1.0f + 0.25f * t, 1.0f - 0.15f * ti, 1.0f - 0.25f * t);
        u1f(locGain_, s.gain);
        u1f(locGamma_, s.gamma);
        u1f(locSat_, s.saturation / 100.0f);

        // 3-way wheels -> per-zone colour tint + luma offset.
        auto wheelTint = [&](const grade::Wheel& w, GLint tintLoc, GLint lumaLoc) {
            float sat = std::min(std::sqrt(w.x * w.x + w.y * w.y), 1.0f);
            float hue = std::atan2(w.y, w.x) * 57.29578f;
            float cr, cg, cb;
            hsv2rgb(hue, 1.0f, 1.0f, cr, cg, cb);
            float k = sat * 0.5f;
            u3f(tintLoc, (cr - 0.5f) * k, (cg - 0.5f) * k, (cb - 0.5f) * k);
            u1f(lumaLoc, w.luma / 100.0f * 0.3f);
        };
        wheelTint(s.shadowsWheel, locTintSh_, locLumaSh_);
        wheelTint(s.midWheel, locTintMid_, locLumaMid_);
        wheelTint(s.highWheel, locTintHi_, locLumaHi_);

        u1i(locHasCurves_, s.curvesIdentity() ? 0 : 1);
        u1i(locTech_, techMode);

        p_glActiveTexture(GL_TEXTURE0);
        p_glBindTexture(GL_TEXTURE_2D, scratchTex_);
        p_glActiveTexture(GL_TEXTURE1);
        p_glBindTexture(GL_TEXTURE_2D, curveTex_);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Detach + restore.
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, (GLenum)dstTexTarget, 0, 0);
    p_glBindVertexArray((GLuint)prevVao);
    p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    p_glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    p_glUseProgram((GLuint)prevProgram);
    p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
    p_glActiveTexture(GL_TEXTURE1);
    p_glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex2D);
    p_glActiveTexture((GLenum)prevActiveTex);
    if (prevBlend) p_glEnable(GL_BLEND); else p_glDisable(GL_BLEND);
    if (prevScissor) p_glEnable(GL_SCISSOR_TEST); else p_glDisable(GL_SCISSOR_TEST);
    if (prevCull) p_glEnable(GL_CULL_FACE); else p_glDisable(GL_CULL_FACE);
    if (prevDepth) p_glEnable(GL_DEPTH_TEST); else p_glDisable(GL_DEPTH_TEST);
    return ok;
}
