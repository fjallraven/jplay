#include "HdrColorPass.h"

#include <SDL3/SDL.h>
#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// HDR color pass: apply the OpenColorIO display transform to the scene-linear
// program image on the SDL_GPU ("gpu") renderer via a custom fragment shader.
//
// Binding model (SPIR-V / SDL_GPU): fragment sampled textures live in descriptor
// set 2 (the drawn program texture at binding 0, OCIO's LUTs at bindings 1..N via
// OCIO's setDescriptorSetIndex(2, 1)); uniform buffers live in set 3 (unused here —
// exposure/gain is baked into the scene-linear input upstream, and we require the
// OCIO processor to generate no uniform buffer).

bool HdrColorPass::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
    if (!renderer_) return false;
    SDL_PropertiesID rp = SDL_GetRendererProperties(renderer_);
    device_ = (SDL_GPUDevice*)SDL_GetPointerProperty(rp, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr);
    if (!device_) return false; // GL/SDR renderer — pass is simply unused
    buildNit_(); // best-effort; the nit heatmap is unavailable if this fails
    return true;
}

// Compile a GLSL fragment shader to SPIR-V. Returns false (and logs) on error.
static bool compileFrag(const char* src, std::vector<uint32_t>& out) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    shaderc::SpvCompilationResult res = compiler.CompileGlslToSpv(
        src, shaderc_glsl_fragment_shader, "hdr.frag", "main", options);
    if (res.GetCompilationStatus() != shaderc_compilation_status_success) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HdrColorPass: shader compile failed: %s",
                    res.GetErrorMessage().c_str());
        return false;
    }
    out.assign(res.cbegin(), res.cend());
    return true;
}

bool HdrColorPass::buildNit_() {
    // Scene-linear luminance -> nit heat ramp (mirrors the GL OcioGpu nit shader).
    // The ramp colours are authored in sRGB, so they are linearised for the scRGB
    // swapchain. Exposure/gain is applied here (np.y), not baked into the uploaded
    // pixels — see the composite path in App_Player.cpp.
    const char* src = R"(#version 460
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D u_input;
layout(set = 3, binding = 0) uniform NitParams { vec4 np; }; // np.x = scene-linear 1.0 -> np.x nits, np.y = gain
vec3 heat(float t){
    t = clamp(t, 0.0, 1.0) * 7.0;
    vec3 c0=vec3(0.25,0.0,0.35), c1=vec3(0.10,0.15,0.70), c2=vec3(0.0,0.55,0.55),
         c3=vec3(0.10,0.75,0.25), c4=vec3(0.50,0.50,0.50), c5=vec3(0.90,0.75,0.10),
         c6=vec3(1.0,0.50,0.0),  c7=vec3(1.0,0.05,0.05);
    if(t<1.0) return mix(c0,c1,t);
    if(t<2.0) return mix(c1,c2,t-1.0);
    if(t<3.0) return mix(c2,c3,t-2.0);
    if(t<4.0) return mix(c3,c4,t-3.0);
    if(t<5.0) return mix(c4,c5,t-4.0);
    if(t<6.0) return mix(c5,c6,t-5.0);
    return mix(c6,c7,t-6.0);
}
void main(){
    vec3 lin = texture(u_input, v_uv).rgb * np.y;
    float L = dot(max(lin, vec3(0.0)), vec3(0.2126,0.7152,0.0722));
    float nits = L * np.x;
    float t = (log2(max(nits,1e-4)) - log2(0.1)) / (log2(10000.0) - log2(0.1));
    vec3 c = clamp(heat(t), 0.0, 1.0);
    vec3 linOut = mix(c/12.92, pow((c+0.055)/1.055, vec3(2.4)), step(0.04045, c));
    o_color = vec4(linOut, 1.0) * v_color;
}
)";
    std::vector<uint32_t> spirv;
    if (!compileFrag(src, spirv)) return false;
    SDL_GPUShaderCreateInfo shi;
    SDL_zero(shi);
    shi.code = reinterpret_cast<const Uint8*>(spirv.data());
    shi.code_size = spirv.size() * sizeof(uint32_t);
    shi.entrypoint = "main";
    shi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    shi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    shi.num_samplers = 1;
    shi.num_uniform_buffers = 1;
    nitShader_ = SDL_CreateGPUShader(device_, &shi);
    if (!nitShader_) return false;
    SDL_GPURenderStateCreateInfo ci;
    SDL_zero(ci);
    ci.fragment_shader = nitShader_;
    nitState_ = SDL_CreateGPURenderState(renderer_, &ci);
    return nitState_ != nullptr;
}

void HdrColorPass::beginNit(float nitScale, float gain) {
    if (!nitState_) return;
    float params[4] = { nitScale, gain, 0.0f, 0.0f };
    SDL_SetGPURenderStateFragmentUniforms(nitState_, 0, params, sizeof(params));
    SDL_SetGPURenderState(renderer_, nitState_);
}

void HdrColorPass::setProcessors(const OCIO::ConstProcessorRcPtr& toWorking,
                                 const OCIO::ConstProcessorRcPtr& toDisplay, int version,
                                 Encoding enc, Primaries prim) {
    if (!device_) return;
    // version bumps on any OCIO change including display switches and a cut to a
    // source read in another colour space, so it also captures enc/prim changes.
    // Gate on built_, not state_: a FAILED build also counts as "attempted",
    // otherwise the retry runs every frame (see built_).
    if (version == version_ && built_) return; // already built (or already failed)
    version_ = version;
    built_ = true;
    enc_ = enc;
    prim_ = prim;
    destroyProgram_();
    if (!toDisplay) return;
    if (!buildProgram_(toWorking, toDisplay)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "HdrColorPass: OCIO shader build failed for this transform; "
                    "frames will be shown UNTRANSFORMED until the display/view changes.");
        destroyProgram_();
    }
}

bool HdrColorPass::addLut_(bool dims3d, unsigned w, unsigned h, unsigned d, int channels,
                           const float* values, bool linearFilter) {
    SDL_GPUTextureCreateInfo ti;
    SDL_zero(ti);
    ti.type = dims3d ? SDL_GPU_TEXTURETYPE_3D : SDL_GPU_TEXTURETYPE_2D;
    ti.format = (channels == 1) ? SDL_GPU_TEXTUREFORMAT_R32_FLOAT
                                : SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = dims3d ? d : 1;
    ti.num_levels = 1;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(device_, &ti);
    if (!tex) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "HdrColorPass: SDL_CreateGPUTexture failed for a %ux%ux%u LUT: %s",
                    w, h, d, SDL_GetError());
        return false;
    }

    const size_t texels = (size_t)w * h * (dims3d ? d : 1);
    const size_t comps = (channels == 1) ? 1 : 4; // RGB is expanded to RGBA on upload
    const size_t bytes = texels * comps * sizeof(float);

    SDL_GPUTransferBufferCreateInfo tbi;
    SDL_zero(tbi);
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = (Uint32)bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbi);
    if (!tb) {
        SDL_ReleaseGPUTexture(device_, tex);
        return false;
    }
    float* map = (float*)SDL_MapGPUTransferBuffer(device_, tb, false);
    if (!map) {
        SDL_ReleaseGPUTransferBuffer(device_, tb);
        SDL_ReleaseGPUTexture(device_, tex);
        return false;
    }
    if (channels == 1) {
        SDL_memcpy(map, values, bytes);
    } else {
        for (size_t i = 0; i < texels; ++i) {
            map[i * 4 + 0] = values[i * 3 + 0];
            map[i * 4 + 1] = values[i * 3 + 1];
            map[i * 4 + 2] = values[i * 3 + 2];
            map[i * 4 + 3] = 1.0f;
        }
    }
    SDL_UnmapGPUTransferBuffer(device_, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src;
    SDL_zero(src);
    src.transfer_buffer = tb;
    src.offset = 0;
    src.pixels_per_row = w;
    src.rows_per_layer = h;
    SDL_GPUTextureRegion dst;
    SDL_zero(dst);
    dst.texture = tex;
    dst.w = w;
    dst.h = h;
    dst.d = dims3d ? d : 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);

    SDL_GPUSamplerCreateInfo si;
    SDL_zero(si);
    si.min_filter = linearFilter ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mag_filter = si.min_filter;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SDL_GPUSampler* samp = SDL_CreateGPUSampler(device_, &si);
    if (!samp) {
        SDL_ReleaseGPUTexture(device_, tex);
        return false;
    }
    luts_.push_back(tex);
    samplers_.push_back(samp);
    return true;
}

bool HdrColorPass::buildProgram_(const OCIO::ConstProcessorRcPtr& toWorking,
                                 const OCIO::ConstProcessorRcPtr& toDisplay) {
    // The pipeline runs as two OCIO-generated functions with the exposure between
    // them — the media colour space into the linear working space, the gain, then
    // the working space to the display. Distinct function names and resource
    // prefixes keep the two sets of generated samplers apart, and the second half
    // starts its bindings where the first half stopped, so the sort below still
    // sees one ascending run across both.
    //
    // LUT samplers live in descriptor set 2, bindings 1..N (binding 0 is the drawn
    // program texture).
    std::vector<unsigned> bindings;
    unsigned nextBinding = 1;

    auto extract = [&](const OCIO::ConstProcessorRcPtr& proc, const char* fn,
                       const char* prefix, std::string& outText) -> bool {
        OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
        desc->setFunctionName(fn);
        desc->setResourcePrefix(prefix);
        desc->setAllowTexture1D(false); // 2D textures only — simpler SDL_GPU mapping
        desc->setDescriptorSetIndex(2, nextBinding);
        try {
            proc->getDefaultGPUProcessor()->extractGpuShaderInfo(desc);
        } catch (const std::exception& e) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "HdrColorPass: OCIO extract failed: %s",
                        e.what());
            return false;
        }

        // What this transform needs, before we start rejecting/allocating: heavier
        // views (ACES 2.0 HDR) ask for more resources than the SDR ones and it is not
        // obvious which limit is being hit when the build fails.
        if (jplayDebugLogging())
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "HdrColorPass: OCIO %s wants %u 2D texture(s), %u 3D texture(s), "
                        "%u uniform(s), %u-byte uniform buffer",
                        fn, desc->getNumTextures(), desc->getNum3DTextures(),
                        desc->getNumUniforms(), (unsigned)desc->getUniformBufferSize());

        // This binding scheme has no room for an OCIO uniform buffer (SDL reserves
        // set 2 binding 0 for the drawn texture). Configs that need GPU uniforms
        // (dynamic properties) are unsupported here for now — bail so the caller
        // shows the frame untransformed rather than mis-render.
        if (desc->getNumUniforms() > 0 || desc->getUniformBufferSize() > 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "HdrColorPass: OCIO transform needs %u GPU uniform(s) "
                        "(uniform buffer %u bytes); unsupported on the HDR path.",
                        desc->getNumUniforms(), (unsigned)desc->getUniformBufferSize());
            for (unsigned i = 0; i < desc->getNumUniforms(); ++i) {
                OCIO::GpuShaderDesc::UniformData ud;
                const char* un = desc->getUniform(i, ud);
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "HdrColorPass:   uniform %u: \"%s\"", i, un ? un : "?");
            }
            return false;
        }

        // Upload LUTs in binding order so sampler_bindings[k] lands at set 2
        // binding k+1.
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
            const int channels = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL) ? 1 : 3;
            if (!addLut_(false, w, h ? h : 1, 1, channels, values, interp != OCIO::INTERP_NEAREST))
                return false;
            bindings.push_back(desc->getTextureShaderBindingIndex(i));
        }
        const unsigned num3D = desc->getNum3DTextures();
        for (unsigned i = 0; i < num3D; ++i) {
            const char* texName = nullptr;
            const char* samplerName = nullptr;
            unsigned edge = 0;
            OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
            desc->get3DTexture(i, texName, samplerName, edge, interp);
            const float* values = nullptr;
            desc->get3DTextureValues(i, values);
            if (!addLut_(true, edge, edge, edge, 3, values, interp != OCIO::INTERP_NEAREST))
                return false;
            bindings.push_back(desc->get3DTextureShaderBindingIndex(i));
        }
        nextBinding += numTex + num3D;
        outText = desc->getShaderText();
        return true;
    };

    // The input half is absent for a media already in the working space, in which
    // case the wrapper below simply does not call it.
    std::string inputText;
    if (toWorking && !extract(toWorking, "OCIOInput", "ocioin_", inputText))
        return false;
    std::string ocioText;
    if (!extract(toDisplay, "OCIOMain", "ocio_", ocioText))
        return false;

    // Reorder the created (texture, sampler) pairs so their positions follow the
    // shader binding indices ascending (binding 1 -> index 0, binding 2 -> 1, ...).
    const size_t n = luts_.size();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return bindings[a] < bindings[b]; });
    std::vector<SDL_GPUTexture*> lutsSorted(n);
    std::vector<SDL_GPUSampler*> sampSorted(n);
    std::vector<SDL_GPUTextureSamplerBinding> bind(n);
    for (size_t k = 0; k < n; ++k) {
        lutsSorted[k] = luts_[order[k]];
        sampSorted[k] = samplers_[order[k]];
        bind[k].texture = lutsSorted[k];
        bind[k].sampler = sampSorted[k];
    }
    luts_.swap(lutsSorted);
    samplers_.swap(sampSorted);

    // Transfer decode: OCIO emits DISPLAY-ENCODED code values; undo the display EOTF
    // to linear light. PQ decodes to absolute nits, then divides by the master
    // paper-white nits (u_refWhite) so paper white maps to 1.0 (panel SDR white) and
    // brighter tones exceed 1.0 into the HDR headroom. HLG reaches the same 1.0 by a
    // fixed normalisation instead (it carries no absolute luminance). SDR encodings map
    // display white to 1.0 (no headroom used).
    std::string decode;
    switch (enc_) {
    case Encoding::PQ:
        // NB: these locals live in the same scope as the grade block above, which
        // already declares `L` — keep the pq* prefix or the shader fails to compile.
        decode =
            "    vec3 pqM = pow(clamp(e,0.0,1.0), vec3(1.0/78.84375));\n"
            "    vec3 pqL = pow(max(pqM - 0.8359375, 0.0) / (18.8515625 - 18.6875*pqM), vec3(1.0/0.1593017578125));\n"
            "    vec3 lin = (pqL * 10000.0) / max(u_refWhite, 1.0);\n"; // nits -> paper-white-relative
        break;
    case Encoding::HLG:
        // BT.2100 HLG: inverse OETF to scene-referred light, then the OOTF (system
        // gamma 1.2) to display light, normalised so HLG diffuse white — the 75% code
        // value of BT.2408, scene-referred kHlgDiffuse — becomes 1.0 = panel SDR white.
        // Peak (100%) then lands at (1/kHlgDiffuse)^1.2 = 4.92x SDR white, matching
        // BT.2408's 1000/203-nit ratio. HLG is relative, so u_refWhite plays no part:
        // the paper-white slider moves the PQ image and leaves this one alone.
        //
        // OutputNDI.cpp inverts exactly this to re-encode for the wire and repeats the
        // constants — the two must agree or the round trip stops being an identity.
        // Same scope caveat as the PQ branch: keep the hlg* prefix.
        decode =
            "    vec3 hlgE = clamp(e,0.0,1.0);\n"
            "    vec3 hlgS = mix(hlgE*hlgE/3.0,\n"
            "                    (exp((hlgE - 0.55991073)/0.17883277) + 0.28466892)/12.0,\n"
            "                    step(0.5, hlgE));\n"
            "    vec3 lin = pow(max(hlgS/0.26496256, 0.0), vec3(1.2));\n";
        break;
    case Encoding::Gamma22: decode = "    vec3 lin = pow(clamp(e,0.0,1.0), vec3(2.2));\n"; break;
    case Encoding::Gamma24: decode = "    vec3 lin = pow(clamp(e,0.0,1.0), vec3(2.4));\n"; break;
    case Encoding::Gamma26: decode = "    vec3 lin = pow(clamp(e,0.0,1.0), vec3(2.6));\n"; break;
    default: // sRGB
        decode = "    vec3 sc = clamp(e,0.0,1.0);\n"
                 "    vec3 lin = mix(sc/12.92, pow((sc+0.055)/1.055, vec3(2.4)), step(0.04045, sc));\n";
        break;
    }
    // Primary conversion to Rec.709 (scRGB). Out-of-709-gamut colours become
    // negative components, which scRGB / the OS HDR pipeline reproduces on the
    // wide-gamut panel. Rows are applied as dot products.
    std::string prim;
    switch (prim_) {
    case Primaries::Rec2020:
        prim = "    lin = vec3(dot(vec3( 1.66049,-0.58764,-0.07285),lin),"
               "               dot(vec3(-0.12455, 1.13290,-0.00835),lin),"
               "               dot(vec3(-0.01821,-0.10057, 1.11878),lin));\n";
        break;
    case Primaries::P3D65:
        prim = "    lin = vec3(dot(vec3( 1.22494,-0.22491, 0.00000),lin),"
               "               dot(vec3(-0.04206, 1.04206, 0.00000),lin),"
               "               dot(vec3(-0.01964,-0.07862, 1.09826),lin));\n";
        break;
    default: break; // Rec.709 already
    }

    // Grade curve LUT: a 256x1 RGBA32F sampled texture bound after the OCIO LUTs
    // (set 2, binding 1+n). Created here, its contents (re)uploaded from the grade
    // curves in begin(). Persists until the program is rebuilt.
    const unsigned curveBinding = 1 + (unsigned)n;
    {
        SDL_GPUTextureCreateInfo ti;
        SDL_zero(ti);
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = 256; ti.height = 1; ti.layer_count_or_depth = 1; ti.num_levels = 1;
        curveTex_ = SDL_CreateGPUTexture(device_, &ti);
        SDL_GPUSamplerCreateInfo si;
        SDL_zero(si);
        si.min_filter = SDL_GPU_FILTER_LINEAR;
        si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        curveSampler_ = SDL_CreateGPUSampler(device_, &si);
        if (!curveTex_ || !curveSampler_) return false;
        SDL_GPUTextureSamplerBinding cb;
        cb.texture = curveTex_;
        cb.sampler = curveSampler_;
        bind.push_back(cb);
        curvesUploaded_ = false; // force an upload on the next begin()
    }

    // Assemble + compile the fragment shader. OCIO's text supplies the LUT sampler
    // declarations (set 2, bindings 1..N) and the OCIOMain(vec4) function; we add
    // the program-texture sampler (binding 0), the grade curve LUT, and the wrapper.
    std::string src =
        "#version 460\n"
        "layout(location = 0) in vec4 v_color;\n"
        "layout(location = 1) in vec2 v_uv;\n"
        "layout(location = 0) out vec4 o_color;\n"
        "layout(set = 2, binding = 0) uniform sampler2D u_input;\n"
        "layout(set = 2, binding = " + std::to_string(curveBinding) + ") uniform sampler2D u_curve;\n"
        // Params (set 3): p0 = (refWhiteNits, gamma, sat, tech); wb.xyz = wb gain,
        // wb.w = scene-linear exposure gain; tSh/tMid/tHi.xyz = per-zone colour tints;
        // lum.xyz = per-zone luma offsets.
        "layout(set = 3, binding = 0) uniform Params { vec4 p0; vec4 wb; vec4 tSh; vec4 tMid; vec4 tHi; vec4 lum; };\n"
        "#define u_refWhite (p0.x)\n"
        + inputText + ocioText +
        "\nfloat gLuma(vec3 c){ return dot(c, vec3(0.2126,0.7152,0.0722)); }\n"
        "void main() {\n"
        "    vec4 c = texture(u_input, v_uv);\n"
        // The media colour space into the linear working space. Absent when the
        // source is already there, which is every EXR under the usual config.
        + (inputText.empty() ? std::string() : std::string("    c = OCIOInput(c);\n"))
        // Exposure belongs in the working space, so it goes here rather than costing
        // a float round-trip per pixel on the CPU during the composite.
        + "    c.rgb *= wb.w;\n"
        "    vec4 disp = OCIOMain(c);\n"
        // Display-space grade + tech-check (matches the GL post-pass).
        // Gain is applied upstream in scene-linear, so it is omitted here.
        "    vec3 v = disp.rgb * wb.xyz;\n"
        "    v = pow(max(v, 0.0), vec3(1.0 / p0.y));\n"
        "    float L = clamp(gLuma(v), 0.0, 1.0);\n"
        "    float shM = (1.0-L)*(1.0-L); float hlM = L*L; float midM = clamp(1.0-shM-hlM, 0.0, 1.0);\n"
        "    v += tSh.xyz*shM + tMid.xyz*midM + tHi.xyz*hlM;\n"
        "    v += lum.x*shM + lum.y*midM + lum.z*hlM;\n"
        "    float L2 = gLuma(v); v = mix(vec3(L2), v, 1.0 + p0.z);\n"
        "    if (int(lum.w + 0.5) == 1) {\n" // per-channel grade curves (lum.w = hasCurves)
        "        vec3 cc = clamp(v, 0.0, 1.0);\n"
        "        v.r = texture(u_curve, vec2(cc.r, 0.5)).r;\n"
        "        v.g = texture(u_curve, vec2(cc.g, 0.5)).g;\n"
        "        v.b = texture(u_curve, vec2(cc.b, 0.5)).b;\n"
        "    }\n"
        "    v = clamp(v, 0.0, 1.0);\n"
        "    int tech = int(p0.w + 0.5);\n"
        "    if (tech == 2) {\n"
        "        bool crushed = (v.r<=1.5/255.0 && v.g<=1.5/255.0 && v.b<=1.5/255.0);\n"
        "        bool blown   = (v.r>=253.5/255.0 || v.g>=253.5/255.0 || v.b>=253.5/255.0);\n"
        "        v = vec3(gLuma(v));\n"
        "        if (crushed) v = vec3(0.0,1.0,1.0); else if (blown) v = vec3(1.0,0.0,0.06);\n"
        "    } else if (tech == 3) { v = vec3(gLuma(v)); }\n"
        "    else if (tech == 4) { v = vec3(v.r); }\n"
        "    else if (tech == 5) { v = vec3(v.g); }\n"
        "    else if (tech == 6) { v = vec3(v.b); }\n"
        "    vec3 e = v;\n"
        + decode
        + prim +
        "    o_color = vec4(lin, disp.a) * v_color;\n"
        "}\n";

    std::vector<uint32_t> spirv;
    if (!compileFrag(src.c_str(), spirv)) return false;

    SDL_GPUShaderCreateInfo shi;
    SDL_zero(shi);
    shi.code = reinterpret_cast<const Uint8*>(spirv.data());
    shi.code_size = spirv.size() * sizeof(uint32_t);
    shi.entrypoint = "main";
    shi.format = SDL_GPU_SHADERFORMAT_SPIRV;
    shi.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    shi.num_samplers = 1 + (Uint32)bind.size(); // drawn texture + OCIO LUTs + curve LUT
    shi.num_uniform_buffers = 1;                 // Params (set 3, binding 0)
    shader_ = SDL_CreateGPUShader(device_, &shi);
    if (!shader_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "HdrColorPass: SDL_CreateGPUShader failed (%u sampler(s)): %s",
                    shi.num_samplers, SDL_GetError());
        return false;
    }

    SDL_GPURenderStateCreateInfo ci;
    SDL_zero(ci);
    ci.fragment_shader = shader_;
    ci.num_sampler_bindings = (Sint32)bind.size();
    ci.sampler_bindings = bind.empty() ? nullptr : bind.data();
    state_ = SDL_CreateGPURenderState(renderer_, &ci);
    if (!state_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "HdrColorPass: SDL_CreateGPURenderState failed (%zu sampler binding(s)): %s",
                    bind.size(), SDL_GetError());
        return false;
    }
    if (jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "HdrColorPass: OCIO render state built (%zu LUT texture(s))", n);
    return true;
}

namespace {
// HSV (h in degrees) -> RGB in [0,1]. Mirrors GradeGpu.
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
// A 3-way wheel -> per-zone colour tint (into tint3) + luma offset. Mirrors GradeGpu.
void wheelTint(const grade::Wheel& w, float* tint3, float& lumaOut) {
    float sat = std::min(std::sqrt(w.x * w.x + w.y * w.y), 1.0f);
    float hue = std::atan2(w.y, w.x) * 57.29578f;
    float cr, cg, cb;
    hsv2rgb(hue, 1.0f, 1.0f, cr, cg, cb);
    float k = sat * 0.5f;
    tint3[0] = (cr - 0.5f) * k;
    tint3[1] = (cg - 0.5f) * k;
    tint3[2] = (cb - 0.5f) * k;
    lumaOut = w.luma / 100.0f * 0.3f;
}
bool curveEquals(const grade::Curve& a, const grade::Curve& b) {
    if (a.pts.size() != b.pts.size()) return false;
    for (size_t i = 0; i < a.pts.size(); ++i)
        if (a.pts[i].x != b.pts[i].x || a.pts[i].y != b.pts[i].y) return false;
    return true;
}
} // namespace

void HdrColorPass::uploadCurve_(const grade::State& g) {
    if (!curveTex_) return;
    std::array<float, 256> m{}, r{}, gg{}, b{};
    grade::bakeCurve(g.curveMaster, m);
    grade::bakeCurve(g.curveR, r);
    grade::bakeCurve(g.curveG, gg);
    grade::bakeCurve(g.curveB, b);
    float data[256 * 4];
    for (int i = 0; i < 256; ++i) {
        float mv = m[i];
        int mi = std::clamp((int)std::lround(mv * 255.0f), 0, 255);
        data[i * 4 + 0] = r[mi];
        data[i * 4 + 1] = gg[mi];
        data[i * 4 + 2] = b[mi];
        data[i * 4 + 3] = mv;
    }
    SDL_GPUTransferBufferCreateInfo tbi;
    SDL_zero(tbi);
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = sizeof(data);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tbi);
    if (!tb) return;
    void* map = SDL_MapGPUTransferBuffer(device_, tb, false);
    if (map) SDL_memcpy(map, data, sizeof(data));
    SDL_UnmapGPUTransferBuffer(device_, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src;
    SDL_zero(src);
    src.transfer_buffer = tb;
    src.pixels_per_row = 256;
    src.rows_per_layer = 1;
    SDL_GPUTextureRegion dst;
    SDL_zero(dst);
    dst.texture = curveTex_;
    dst.w = 256; dst.h = 1; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
}

void HdrColorPass::begin(float refWhiteNits, float gain, const grade::State& g, int tech) {
    if (!state_) return;
    const bool hasCurves = !g.curvesIdentity();
    if (curveTex_ && hasCurves &&
        (!curvesUploaded_ || !curveEquals(g.curveMaster, lastCurveMaster_) ||
         !curveEquals(g.curveR, lastCurveR_) || !curveEquals(g.curveG, lastCurveG_) ||
         !curveEquals(g.curveB, lastCurveB_))) {
        uploadCurve_(g);
        lastCurveMaster_ = g.curveMaster;
        lastCurveR_ = g.curveR;
        lastCurveG_ = g.curveG;
        lastCurveB_ = g.curveB;
        curvesUploaded_ = true;
    }
    const float t = g.temperature / 100.0f, ti = g.tint / 100.0f;
    float u[24] = { 0 }; // 6 vec4: p0, wb, tSh, tMid, tHi, lum
    u[0] = refWhiteNits; u[1] = g.gamma; u[2] = g.saturation / 100.0f; u[3] = (float)tech;
    u[4] = 1.0f + 0.25f * t; u[5] = 1.0f - 0.15f * ti; u[6] = 1.0f - 0.25f * t;
    u[7] = gain; // wb.w — scene-linear exposure, applied before OCIOMain
    float lumSh = 0, lumMid = 0, lumHi = 0;
    wheelTint(g.shadowsWheel, &u[8],  lumSh);
    wheelTint(g.midWheel,     &u[12], lumMid);
    wheelTint(g.highWheel,    &u[16], lumHi);
    u[20] = lumSh; u[21] = lumMid; u[22] = lumHi;
    u[23] = hasCurves ? 1.0f : 0.0f; // lum.w = hasCurves
    SDL_SetGPURenderStateFragmentUniforms(state_, 0, u, sizeof(u));
    SDL_SetGPURenderState(renderer_, state_);
}

void HdrColorPass::end() {
    if (state_ || nitState_) SDL_SetGPURenderState(renderer_, nullptr);
}

void HdrColorPass::destroyProgram_() {
    if (state_) {
        SDL_DestroyGPURenderState(state_);
        state_ = nullptr;
    }
    if (shader_ && device_) {
        SDL_ReleaseGPUShader(device_, shader_);
        shader_ = nullptr;
    }
    for (SDL_GPUSampler* s : samplers_)
        if (s) SDL_ReleaseGPUSampler(device_, s);
    for (SDL_GPUTexture* t : luts_)
        if (t) SDL_ReleaseGPUTexture(device_, t);
    samplers_.clear();
    luts_.clear();
    if (curveSampler_) { SDL_ReleaseGPUSampler(device_, curveSampler_); curveSampler_ = nullptr; }
    if (curveTex_)     { SDL_ReleaseGPUTexture(device_, curveTex_); curveTex_ = nullptr; }
    curvesUploaded_ = false;
}

void HdrColorPass::shutdown() {
    destroyProgram_();
    if (nitState_) {
        SDL_DestroyGPURenderState(nitState_);
        nitState_ = nullptr;
    }
    if (nitShader_ && device_) {
        SDL_ReleaseGPUShader(device_, nitShader_);
        nitShader_ = nullptr;
    }
    version_ = -1;
    built_ = false;
}
