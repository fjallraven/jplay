#include "OutputNDI.h"

#ifdef JPLAY_ENABLE_NDI

#include <SDL3/SDL.h> // SDL_getenv (avoids the deprecated-getenv CRT warning)

// Plain C structs (no inline C++ constructors), so nothing needs the import lib;
// we resolve the whole API table from the runtime DLL at load time.
#define NDILIB_CPP_DEFAULT_CONSTRUCTORS 0
#include <Processing.NDI.Lib.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

typedef const NDIlib_v5* (*PFN_load)(void);

// ---------------------------------------------------------------------------
// HDR encode: scRGB half -> Rec.2020 / PQ or HLG / 16-bit YCbCr 4:2:2 (P216)
//
// RGBA and BGRA are 8-bit only in the NDI SDK, so an HDR signal has to go out as
// P216 — the SDK's 16-bit semi-planar YCbCr 4:2:2 format — with an
// <ndi_color_info> tag naming the transfer/matrix/primaries. The device layer here
// only packs and tags, leaving the display transform to the colour pipeline upstream;
// the 16-bit level convention it writes is the studio-swing one fixed below.
// ---------------------------------------------------------------------------

// NDI's HDR tags. The value vocabulary comes from the runtime's string table and the
// SDK's NDIlib_Recv_HDR example: transfer bt_601 / bt_709 / bt_2100_pq / bt_2100_hlg,
// matrix and primaries bt_601 / bt_709 / bt_2020. Which one goes out follows the
// display transform the colour pass decoded, never a setting (OutputFrame::transfer).
// Both live in static storage because the pointer must stay valid until the async send
// completes.
const char* const kPqColorInfo =
    "<ndi_color_info transfer=\"bt_2100_pq\" matrix=\"bt_2020\" primaries=\"bt_2020\"/>";
const char* const kHlgColorInfo =
    "<ndi_color_info transfer=\"bt_2100_hlg\" matrix=\"bt_2020\" primaries=\"bt_2020\"/>";

// The SDR counterpart, sent on every 8-bit frame. The tag is nominally per-frame, but
// it describes a property of the stream and receivers differ on whether an absent tag
// resets the previous one — so an untagged SDR frame following a PQ one can be decoded
// as PQ. That transition is routine here rather than exceptional: the player picks the
// HDR path per frame (App::hdrProgramTransfer_), so crossing from an EXR clip to a video
// clip, disabling OCIO, or showing the nit heatmap all downgrade the stream mid-send.
// Tagging both paths makes every switch explicit. NDI's transfer vocabulary has no
// sRGB entry, so bt_709 is the closest description of the display-encoded 8-bit image;
// matrix is moot for an RGBA frame and named for consistency.
const char* const kRec709ColorInfo =
    "<ndi_color_info transfer=\"bt_709\" matrix=\"bt_709\" primaries=\"bt_709\"/>";

// Rec.709 -> Rec.2020 linear RGB. The inverse of the 2020->709 matrix the colour
// pass applies on its way to scRGB (HdrColorPass.cpp), so a Rec.2100-PQ display
// transform round-trips back to the primaries it was authored in.
constexpr float kR709To2020[9] = {
    0.627404f, 0.329283f, 0.043313f,
    0.069097f, 0.919540f, 0.011362f,
    0.016391f, 0.088013f, 0.895595f,
};

// Rec.2020 non-constant-luminance luma coefficients (ITU-R BT.2020 Table 4).
constexpr float kKr = 0.2627f, kKg = 0.6780f, kKb = 0.0593f;

// SMPTE ST 2084 (PQ) forward OETF, tabulated over every half-float bit pattern.
//
// The straight formula costs two pow() per channel, which at 4K is seconds per
// frame on the render thread. The input is already half-precision (and the encode
// quantises its Rec.2020 result back to half to index this), which costs well under
// a 12-bit code value of accuracy — so a 64K-entry table replaces the pow() calls
// outright. Indexed by half bits; the argument is absolute luminance in nits.
const float* pqTable() {
    static const std::vector<float> lut = [] {
        std::vector<float> t(65536);
        const float m1 = 0.1593017578125f, m2 = 78.84375f;
        const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
        for (uint32_t i = 0; i < 65536; ++i) {
            Imath::half h;
            h.setBits((uint16_t)i);
            float nits = (float)h;
            // NaN, -inf and negatives (out-of-gamut residue) all floor at black;
            // +inf saturates at peak.
            if (!(nits > 0.0f))
                nits = 0.0f;
            // Parenthesised so windows.h's min/max macros can't capture the call.
            const float y = (std::min)(nits, 10000.0f) / 10000.0f;
            const float ym = std::pow(y, m1);
            t[i] = std::pow((c1 + c2 * ym) / (1.0f + c3 * ym), m2);
        }
        return t;
    }();
    return lut.data();
}

// BT.2100 HLG forward OETF, tabulated the same way and for the same reason — but
// indexed by DISPLAY light with 1.0 = diffuse white, since HLG carries no absolute
// luminance and the paper-white value therefore never enters the encode.
//
// This inverts HdrColorPass's HLG decode step for step: undo the OOTF (system gamma
// 1.2), rescale by the scene-referred signal of diffuse white (kHlgDiffuse, the 75%
// code value of BT.2408), then apply the OETF. The constants are repeated from the
// shader there and the two must stay in step, exactly as the PQ pair does — matched,
// the round trip hands the receiver the code values OCIO authored.
const float* hlgTable() {
    static const std::vector<float> lut = [] {
        std::vector<float> t(65536);
        const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
        const float kHlgDiffuse = 0.26496256f;
        for (uint32_t i = 0; i < 65536; ++i) {
            Imath::half h;
            h.setBits((uint16_t)i);
            float d = (float)h;
            // Same flooring as the PQ table: NaN, -inf and negatives go to black,
            // +inf saturates at peak.
            if (!(d > 0.0f))
                d = 0.0f;
            // Display light -> scene-referred signal, clipped at the HLG peak (100%
            // signal, 4.92x diffuse white).
            const float s = (std::min)(kHlgDiffuse * std::pow(d, 1.0f / 1.2f), 1.0f);
            t[i] = (s <= 1.0f / 12.0f) ? std::sqrt(3.0f * s)
                                       : a * std::log(12.0f * s - b) + c;
        }
        return t;
    }();
    return lut.data();
}

// 16-bit narrow-range (studio-swing) YCbCr quantisers: luma 4096..60160 and chroma
// 4096..61440 about a neutral of 32768 — i.e. the 8-bit 64..235 / 16..240 levels
// shifted up 8 bits. The inputs here are always non-negative after the clamp, so
// +0.5f and truncate rounds without a call into libm.
inline uint16_t clampRound(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 65535.0f) return 65535;
    return (uint16_t)(v + 0.5f);
}
inline uint16_t quantY(float y) { return clampRound(y * 56064.0f + 4096.0f); } // y in [0,1]
inline uint16_t quantC(float c) { return clampRound(c * 57344.0f + 32768.0f); } // c in [-0.5,0.5]

// Run fn(yBegin, yEnd) over `h` rows, split into bands across the pool.
//
// The encode costs ~40 ms/frame at 1080p and ~160 ms at 4K single-threaded — enough
// to halve the playback rate on its own — and every row is independent, so it is split
// into bands across the thread pool. This brings 1080p to ~5 ms and 4K to ~14 ms. The
// join is a hard barrier, so no band outlives the caller's buffers.
template <typename Fn>
void parallelRows(int h, Fn fn) {
    unsigned n = std::thread::hardware_concurrency();
    if (n < 2 || h < 64) { // not worth the fan-out
        fn(0, h);
        return;
    }
    n = (std::min)(n, (unsigned)(h / 32)); // >= 32 rows per band
    const int band = (h + (int)n - 1) / (int)n;
    std::vector<std::future<void>> bands;
    bands.reserve(n - 1);
    for (int y = band; y < h; y += band) {
        const int y0 = y, y1 = (std::min)(y + band, h);
        bands.push_back(std::async(std::launch::async, [&fn, y0, y1] { fn(y0, y1); }));
    }
    fn(0, (std::min)(band, h)); // this thread takes the first band
    for (auto& b : bands)
        b.get();
}

// Encode `srcW` x `h` scRGB half pixels into `out` as P216: a 16-bit luma plane of
// `h` rows followed by a 16-bit interleaved Cb,Cr plane of `h` rows, both with a
// stride of `outW` samples. Chroma is horizontally halved by averaging each pixel
// pair rather than dropping the odd sample — averaging costs nothing here and keeps
// saturated edges from aliasing. Returns the encoded width, which is `srcW`
// rounded down to even — 4:2:2 has no way to represent a lone final column.
int encodeP216(const Imath::half* src, int srcW, int h, OutputTransfer xfer,
               float refWhiteNits, std::vector<uint8_t>& out) {
    const int outW = srcW & ~1;
    if (outW <= 0 || h <= 0)
        return 0;

    const bool hlg = xfer == OutputTransfer::HLG;
    const float* tf = hlg ? hlgTable() : pqTable();
    out.resize((size_t)outW * h * 2 * sizeof(uint16_t)); // luma plane + CbCr plane
    uint16_t* planeY = (uint16_t*)out.data();
    uint16_t* planeC = planeY + (size_t)outW * h;

    // scRGB 1.0 is paper white; PQ is absolute, so scale to nits first. HLG is relative
    // and its table is indexed by paper-white-relative light directly, so it takes no
    // scale at all — the ref-white slider simply does not reach an HLG signal.
    //
    // For PQ this must stay the same value HdrColorPass divided by, clamped the same way
    // (its PQ decode is `pqL * 10000 / max(u_refWhite, 1)`): the two scales then cancel
    // and the PQ code values sent are exactly the ones OCIO authored, so the viewing-side
    // paper-white slider re-grades the window without touching the transmitted signal.
    // Anchoring the encode at some other reference (BT.2408's 203-nit graphics white,
    // say) while the pass keeps dividing by the slider value would rescale absolute
    // luminance on the wire by out/view — that is a rescale, not a correction.
    const float nitScale = hlg ? 1.0f : (std::max)(refWhiteNits, 1.0f);

    parallelRows(h, [&](int yBegin, int yEnd) {
    for (int y = yBegin; y < yEnd; ++y) {
        const Imath::half* s = src + (size_t)y * srcW * 4;
        uint16_t* dY = planeY + (size_t)y * outW;
        uint16_t* dC = planeC + (size_t)y * outW;

        // One pixel: scRGB -> Rec.2020 light -> transfer code -> Y' plus Cb/Cr offsets.
        auto pixel = [&](int x, float& outCb, float& outCr) {
            const float r = (float)s[x * 4 + 0];
            const float g = (float)s[x * 4 + 1];
            const float b = (float)s[x * 4 + 2];
            // Negative components (colours outside Rec.709, which scRGB carries as
            // out-of-range values) can land back inside Rec.2020, so convert first
            // and let the transfer table floor whatever is still out of gamut.
            Imath::half n2020[3];
            for (int c = 0; c < 3; ++c)
                n2020[c] = (kR709To2020[c * 3 + 0] * r + kR709To2020[c * 3 + 1] * g +
                            kR709To2020[c * 3 + 2] * b) * nitScale;
            const float pr = tf[n2020[0].bits()];
            const float pg = tf[n2020[1].bits()];
            const float pb = tf[n2020[2].bits()];
            const float luma = kKr * pr + kKg * pg + kKb * pb;
            outCb = (pb - luma) / (2.0f * (1.0f - kKb));
            outCr = (pr - luma) / (2.0f * (1.0f - kKr));
            return luma;
        };

        for (int x = 0; x < outW; x += 2) {
            float cb0, cr0, cb1, cr1;
            dY[x]     = quantY(pixel(x, cb0, cr0));
            dY[x + 1] = quantY(pixel(x + 1, cb1, cr1));
            dC[x]     = quantC((cb0 + cb1) * 0.5f); // Cb of the pair
            dC[x + 1] = quantC((cr0 + cr1) * 0.5f); // Cr of the pair
        }
    }
    });
    return outW;
}

// Locate and load the NDI runtime DLL once per process, returning its exported
// function table (or nullptr). Tries the standard loader search path, then the
// NDI redist env var the SDK documents, then the SDK bin dir baked in at build
// time (dev fallback). The table is a process-wide singleton.
const NDIlib_v5* loadNdiLib() {
    static const NDIlib_v5* lib = nullptr;
    static bool tried = false;
    if (tried)
        return lib;
    tried = true;

#if defined(_WIN32)
    HMODULE dll = LoadLibraryA(NDILIB_LIBRARY_NAME);
    if (!dll) {
        if (const char* dir = SDL_getenv(NDILIB_REDIST_FOLDER)) {
            std::string p = std::string(dir) + "\\" + NDILIB_LIBRARY_NAME;
            dll = LoadLibraryA(p.c_str());
        }
    }
#ifdef JPLAY_NDI_BIN_DIR
    if (!dll) {
        std::string p = std::string(JPLAY_NDI_BIN_DIR) + "\\" + NDILIB_LIBRARY_NAME;
        dll = LoadLibraryA(p.c_str());
    }
#endif
    if (!dll)
        return nullptr;

    PFN_load load = (PFN_load)GetProcAddress(dll, "NDIlib_v6_load");
    if (!load)
        load = (PFN_load)GetProcAddress(dll, "NDIlib_v5_load");
#else
    void* dll = dlopen(NDILIB_LIBRARY_NAME, RTLD_LOCAL | RTLD_LAZY);
    if (!dll) {
        if (const char* dir = SDL_getenv(NDILIB_REDIST_FOLDER)) {
            std::string p = std::string(dir) + "/" + NDILIB_LIBRARY_NAME;
            dll = dlopen(p.c_str(), RTLD_LOCAL | RTLD_LAZY);
        }
    }
#ifdef JPLAY_NDI_BIN_DIR
    if (!dll) {
        std::string p = std::string(JPLAY_NDI_BIN_DIR) + "/" + NDILIB_LIBRARY_NAME;
        dll = dlopen(p.c_str(), RTLD_LOCAL | RTLD_LAZY);
    }
#endif
    if (!dll)
        return nullptr;

    PFN_load load = (PFN_load)dlsym(dll, "NDIlib_v6_load");
    if (!load)
        load = (PFN_load)dlsym(dll, "NDIlib_v5_load");
#endif
    if (!load)
        return nullptr;
    lib = load();
    return lib;
}

// An NDI network sender advertising the player's program output as "jplay".
class NdiOutput : public OutputDevice {
public:
    bool open() override {
        lib_ = loadNdiLib();
        if (!lib_ || !lib_->initialize())
            return false;
        NDIlib_send_create_t s;
        std::memset(&s, 0, sizeof(s));
        s.p_ndi_name = "jplay";
        s.clock_video = false; // the player's transport paces frames, not NDI
        s.clock_audio = false;
        send_ = lib_->send_create(&s);
        return send_ != nullptr;
    }

    void submit(const OutputFrame& f) override {
        if (!send_ || f.width <= 0 || f.height <= 0)
            return;
        if (!f.rgba && !f.scRgb)
            return;

        // Async send keeps the render thread free. The buffer handed to NDI must
        // stay valid until the *next* async send, so we ping-pong two buffers.
        std::vector<uint8_t>& buf = buffers_[cur_];

        NDIlib_video_frame_v2_t vf;
        std::memset(&vf, 0, sizeof(vf));
        if (f.scRgb) {
            // HDR: Rec.2020 with the frame's own transfer, 16-bit YCbCr 4:2:2. P216 is
            // semi-planar, so the stride describes one plane and the chroma plane
            // follows the luma plane contiguously in the same buffer.
            const bool hlg = f.transfer == OutputTransfer::HLG;
            const int w = encodeP216(f.scRgb, f.width, f.height, f.transfer,
                                     f.refWhiteNits, buf);
            if (w <= 0)
                return;
            vf.xres = w;
            vf.yres = f.height;
            vf.FourCC = NDIlib_FourCC_video_type_P216;
            vf.line_stride_in_bytes = w * (int)sizeof(uint16_t);
            vf.p_metadata = hlg ? kHlgColorInfo : kPqColorInfo;
        } else {
            // SDR: 8-bit RGBA straight through (the SDK has no wider RGB format).
            buf.assign(f.rgba, f.rgba + (size_t)f.width * f.height * 4);
            vf.xres = f.width;
            vf.yres = f.height;
            vf.FourCC = NDIlib_FourCC_video_type_RGBA;
            vf.line_stride_in_bytes = f.width * 4;
            vf.p_metadata = kRec709ColorInfo;
        }
        // N/1001 reproduces every standard rate exactly (24, 23.976, 25, 29.97…).
        vf.frame_rate_N = (int)(f.fps > 0.0 ? f.fps * 1001.0 + 0.5 : 30000.0);
        vf.frame_rate_D = 1001;
        vf.picture_aspect_ratio = 0.0f; // square pixels
        vf.frame_format_type = NDIlib_frame_format_type_progressive;
        vf.timecode = NDIlib_send_timecode_synthesize;
        vf.p_data = buf.data();
        lib_->send_send_video_async_v2(send_, &vf);
        signal_ = !f.scRgb                                ? "Rec.709 8-bit"
                  : f.transfer == OutputTransfer::HLG     ? "HLG Rec.2020 16-bit"
                                                          : "PQ Rec.2020 16-bit";
        cur_ ^= 1;
    }

    void close() override {
        if (send_ && lib_) {
            lib_->send_send_video_async_v2(send_, nullptr); // flush the in-flight async send
            lib_->send_destroy(send_);
            send_ = nullptr;
        }
        // The library table is a process-wide singleton shared with any future
        // sender, so we deliberately do not call lib_->destroy() here.
    }

    // The encode is ours to do, so we take the float program image whenever the
    // player has one (see submit()).
    bool wantsHdr() const override { return true; }

    // Colorimetry of the last frame sent plus the receiver count, e.g.
    // "PQ Rec.2020 16-bit, 2 receivers". The colorimetry is worth showing rather than
    // implying: it follows the frame, so a stream can drop from PQ to Rec.709 without
    // the operator doing anything (see kRec709ColorInfo).
    std::string status() const override {
        if (!send_ || !lib_)
            return "off";
        int n = lib_->send_get_no_connections(send_, 0);
        char b[64];
        std::snprintf(b, sizeof(b), "%s, %d receiver%s",
                      signal_ ? signal_ : "no frames yet", n, n == 1 ? "" : "s");
        return b;
    }

private:
    const NDIlib_v5* lib_ = nullptr;
    NDIlib_send_instance_t send_ = nullptr;
    std::vector<uint8_t> buffers_[2]; // ping-pong for async sends
    int cur_ = 0;
    // Encoding of the last frame handed to NDI, or null before the first one. Static
    // storage duration either way, so status() just borrows it.
    const char* signal_ = nullptr;
};

} // namespace

bool ndiRuntimeAvailable() {
    return loadNdiLib() != nullptr;
}

std::unique_ptr<OutputDevice> createNdiOutput() {
    return std::make_unique<NdiOutput>();
}

#endif // JPLAY_ENABLE_NDI
