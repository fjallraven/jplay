#include "OutputDeckLink.h"

#ifdef JPLAY_ENABLE_DECKLINK

#include <DeckLinkAPI.h>
#include <DeckLinkAPIVersion.h>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// The DeckLink API is COM-shaped even on Linux (see LinuxCOM.h): every getter
// hands back an owning reference and the caller Release()s it. This is the
// smallest scoped pointer that covers our use.
// ---------------------------------------------------------------------------
template <class T>
class Ref {
public:
    Ref() = default;
    explicit Ref(T* p) : p_(p) {}
    ~Ref() { reset(); }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    Ref(Ref&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Ref& operator=(Ref&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    // Out-parameter slot for the API's `/* out */ T**` getters.
    T** addr() { reset(); return &p_; }
    void reset() {
        if (p_) { p_->Release(); p_ = nullptr; }
    }

private:
    T* p_ = nullptr;
};

template <class T>
Ref<T> queryIface(IUnknown* from, REFIID iid) {
    T* out = nullptr;
    if (!from || from->QueryInterface(iid, (void**)&out) != S_OK)
        return Ref<T>();
    return Ref<T>(out);
}

// The API allocates its `const char*` out-params with malloc on Linux and hands
// ownership to us (see the SDK's DeviceList / TestPattern samples).
std::string takeString(const char* s) {
    if (!s)
        return {};
    std::string out(s);
    std::free((void*)s);
    return out;
}

// One of the card's output rasters, flattened out of IDeckLinkDisplayMode so the
// pick in submit() is a plain scan over a vector rather than a COM walk per frame.
struct Mode {
    BMDDisplayMode mode = bmdModeUnknown;
    int            width = 0;
    int            height = 0;
    double         fps = 0.0;
    std::string    name;
};

// Is the driver library actually on this machine? The SDK's dispatch TU dlopen's
// libDeckLinkAPI.so on the first Create* call and, when that fails, prints the
// dlerror to stderr -- so a DeckLink-enabled build greets every machine without
// Desktop Video with "libDeckLinkAPI.so: cannot open shared object file". Doing
// the dlopen ourselves first keeps it quiet: on failure we never enter the
// dispatch TU at all, and on success its dlopen finds the library already
// loaded and says nothing. The handle is deliberately not closed -- the dispatch
// TU holds one for the process lifetime anyway.
bool driverPresent() {
    static const bool present =
        dlopen("libDeckLinkAPI.so", RTLD_NOW | RTLD_GLOBAL) != nullptr;
    return present;
}

// The installed driver's API version, packed as the SDK packs it (0x0f030000 is
// 15.3), plus its display string. Worth reading even when nothing else works:
// IID_IDeckLinkAPIInformation and its dispatch entry point are the two things
// Blackmagic has kept stable across SDK versions, so this answers on a driver
// whose device interfaces we cannot talk to at all. Returns 0 with no driver.
int64_t driverApiVersion(std::string* strOut) {
    if (!driverPresent())
        return 0;
    Ref<IDeckLinkAPIInformation> info(CreateDeckLinkAPIInformationInstance());
    if (!info)
        return 0;
    if (strOut) {
        const char* s = nullptr;
        if (info->GetString(BMDDeckLinkAPIVersion, &s) == S_OK)
            *strOut = takeString(s);
    }
    int64_t v = 0;
    return info->GetInt(BMDDeckLinkAPIVersion, &v) == S_OK ? v : 0;
}

// Walk the first device that exposes IDeckLinkOutput. Capture-only cards and
// subdevices made inactive by the current profile fail the QueryInterface and
// are skipped. Returns a null Ref when the driver is absent or nothing can play out.
Ref<IDeckLinkOutput> firstOutputDevice(std::string* nameOut) {
    // No driver library, no devices -- and asking the dispatch TU would only make
    // it complain to stderr on the way to the same answer.
    if (!driverPresent())
        return {};
    // A null iterator with the library loaded means no card the driver can see.
    Ref<IDeckLinkIterator> it(CreateDeckLinkIteratorInstance());
    if (!it)
        return {};
    for (;;) {
        Ref<IDeckLink> dev;
        if (it->Next(dev.addr()) != S_OK || !dev)
            break;
        Ref<IDeckLinkOutput> out =
            queryIface<IDeckLinkOutput>(dev.get(), IID_IDeckLinkOutput);
        if (!out)
            continue;
        if (nameOut) {
            const char* nm = nullptr;
            if (dev->GetDisplayName(&nm) == S_OK)
                *nameOut = takeString(nm);
        }
        return out;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Fit the program image into the card's raster.
//
// NDI takes whatever resolution the player composited, but a DeckLink raster is
// fixed by the display mode, so the program has to be scaled into it. Aspect is
// preserved and the remainder is left black — the same letterbox/pillarbox the
// player would show. Bilinear, which is enough for a monitoring path and cheap
// enough to stay on the render thread.
//
// `src` is RGBA8 top-row-first (OutputManager's readback order). `dst` is the
// card's BGRA8 buffer, whose stride may exceed dstW * 4.
// ---------------------------------------------------------------------------
void fitBgra(const uint8_t* src, int srcW, int srcH,
             uint8_t* dst, int dstW, int dstH, int dstStride) {
    std::memset(dst, 0, (size_t)dstStride * dstH);
    if (srcW <= 0 || srcH <= 0)
        return;

    // Destination rect: the largest box of the source's aspect that fits.
    const double scale = std::min((double)dstW / srcW, (double)dstH / srcH);
    const int outW = std::max(1, std::min(dstW, (int)std::lround(srcW * scale)));
    const int outH = std::max(1, std::min(dstH, (int)std::lround(srcH * scale)));
    const int offX = (dstW - outW) / 2;
    const int offY = (dstH - outH) / 2;

    for (int y = 0; y < outH; ++y) {
        // Sample at pixel centres so the mapping stays symmetric at both edges.
        const double sy = ((y + 0.5) * srcH / outH) - 0.5;
        const int y0 = std::clamp((int)std::floor(sy), 0, srcH - 1);
        const int y1 = std::min(y0 + 1, srcH - 1);
        const double fy = std::clamp(sy - y0, 0.0, 1.0);
        uint8_t* out = dst + (size_t)(y + offY) * dstStride + (size_t)offX * 4;
        const uint8_t* row0 = src + (size_t)y0 * srcW * 4;
        const uint8_t* row1 = src + (size_t)y1 * srcW * 4;
        for (int x = 0; x < outW; ++x) {
            const double sx = ((x + 0.5) * srcW / outW) - 0.5;
            const int x0 = std::clamp((int)std::floor(sx), 0, srcW - 1);
            const int x1 = std::min(x0 + 1, srcW - 1);
            const double fx = std::clamp(sx - x0, 0.0, 1.0);
            const uint8_t* a = row0 + (size_t)x0 * 4;
            const uint8_t* b = row0 + (size_t)x1 * 4;
            const uint8_t* c = row1 + (size_t)x0 * 4;
            const uint8_t* d = row1 + (size_t)x1 * 4;
            // RGBA in, BGRA out: channel 0 of the destination is blue.
            for (int ch = 0; ch < 3; ++ch) {
                const double top = a[ch] + (b[ch] - a[ch]) * fx;
                const double bot = c[ch] + (d[ch] - c[ch]) * fx;
                out[2 - ch] = (uint8_t)std::lround(top + (bot - top) * fy);
            }
            out[3] = 0xFF;
            out += 4;
        }
    }
}

// An SDI/HDMI output on a DeckLink card, driven frame-at-a-time.
//
// The card's raster and rate are not known at open() — the player's program size
// follows the media and its rate follows the timeline — so EnableVideoOutput is
// deferred to the first submit() and redone whenever the incoming geometry or
// rate picks a different mode.
class DeckLinkOutput : public OutputDevice {
public:
    bool open() override {
        out_ = firstOutputDevice(&device_);
        if (!out_)
            return false;

        // Snapshot the card's rasters once; picking a mode per frame is then a
        // scan over this rather than a COM iteration.
        Ref<IDeckLinkDisplayModeIterator> it;
        if (out_->GetDisplayModeIterator(it.addr()) != S_OK || !it) {
            out_.reset();
            return false;
        }
        for (;;) {
            Ref<IDeckLinkDisplayMode> dm;
            if (it->Next(dm.addr()) != S_OK || !dm)
                break;
            BMDTimeValue dur = 0;
            BMDTimeScale ts = 0;
            if (dm->GetFrameRate(&dur, &ts) != S_OK || dur <= 0 || ts <= 0)
                continue;
            const BMDDisplayMode mode = dm->GetDisplayMode();
            // Ask once, here, whether the card can actually run this raster in our
            // pixel format: the answer is fixed for the life of the device, and
            // asking it per mode per frame would put a few dozen driver round
            // trips on the render thread.
            bool supported = false;
            BMDDisplayMode actual = bmdModeUnknown;
            if (out_->DoesSupportVideoMode(bmdVideoConnectionUnspecified, mode,
                                           kPixelFormat, bmdNoVideoOutputConversion,
                                           bmdSupportedVideoModeDefault, &actual,
                                           &supported) != S_OK ||
                !supported)
                continue;
            Mode m;
            m.mode = mode;
            m.width = (int)dm->GetWidth();
            m.height = (int)dm->GetHeight();
            m.fps = (double)ts / (double)dur;
            const char* nm = nullptr;
            if (dm->GetName(&nm) == S_OK)
                m.name = takeString(nm);
            modes_.push_back(std::move(m));
        }
        if (modes_.empty()) {
            out_.reset();
            return false;
        }
        return true;
    }

    void submit(const OutputFrame& f) override {
        if (!out_ || !f.rgba || f.width <= 0 || f.height <= 0)
            return;
        if (!configure(f.width, f.height, f.fps))
            return;

        // Newer SDKs moved pixel access off IDeckLinkVideoFrame onto
        // IDeckLinkVideoBuffer, reached by QueryInterface, and bracketed the
        // mapping with StartAccess/EndAccess. The 12.0 headers we vendor for
        // release predate that and map the frame directly.
#ifdef JPLAY_DECKLINK_HAS_VIDEO_BUFFER
        Ref<IDeckLinkVideoBuffer> buf =
            queryIface<IDeckLinkVideoBuffer>(frame_.get(), IID_IDeckLinkVideoBuffer);
        if (!buf || buf->StartAccess(bmdBufferAccessWrite) != S_OK)
            return;
        void* bytes = nullptr;
        if (buf->GetBytes(&bytes) == S_OK && bytes) {
            fitBgra(f.rgba, f.width, f.height,
                    (uint8_t*)bytes, active_.width, active_.height, stride_);
        }
        buf->EndAccess(bmdBufferAccessWrite);
#else
        void* bytes = nullptr;
        if (frame_->GetBytes(&bytes) == S_OK && bytes) {
            fitBgra(f.rgba, f.width, f.height,
                    (uint8_t*)bytes, active_.width, active_.height, stride_);
        }
#endif

        // Synchronous display: the card shows this frame at its next opportunity
        // and holds it until the next one. That matches the player driving the
        // cadence (App_Player only submits on a fresh composite, so a paused
        // timeline simply leaves the last frame on the monitor) at the cost of
        // not being locked to the card's clock. Scheduled playback is the
        // genlocked alternative and needs a feeding thread to go with it.
        if (out_->DisplayVideoFrameSync(frame_.get()) != S_OK)
            ++dropped_;
    }

    void close() override {
        frame_.reset();
        if (out_ && enabled_) {
            out_->DisableVideoOutput();
            enabled_ = false;
        }
        out_.reset();
        modes_.clear();
        active_ = Mode();
        failedMode_ = bmdModeUnknown;
    }

    // The card, the raster it is running, and whether it is locked to house
    // reference — the three things an operator needs to trust the feed. Dropped
    // frames only appear once there are some.
    std::string status() const override {
        if (!out_)
            return "off";
        if (!enabled_)
            return device_ + ", no frames yet";
        std::string s = device_ + ", " + active_.name;
        BMDReferenceStatus ref = 0;
        if (out_->GetReferenceStatus(&ref) == S_OK) {
            if (ref & bmdReferenceLocked)
                s += ", ref locked";
            else if (!(ref & bmdReferenceNotSupportedByHardware))
                s += ", no ref";
        }
        if (dropped_)
            s += ", " + std::to_string(dropped_) + " dropped";
        return s;
    }

    // 8-bit BGRA only for now, so the player's plain RGBA readback is all we
    // ever need. 10-bit v210 is what would make the float program image worth
    // asking for.
    bool wantsHdr() const override { return false; }

private:
    // Bring the card up on the mode that best carries a `w`x`h` @ `fps` program,
    // reusing the current one when the pick has not changed. False means nothing
    // on this card can carry it.
    bool configure(int w, int h, double fps) {
        const Mode* pick = pickMode(w, h, fps);
        if (!pick)
            return false;
        if (enabled_ && pick->mode == active_.mode && frame_)
            return true;

        if (enabled_) {
            frame_.reset();
            out_->DisableVideoOutput();
            enabled_ = false;
        }
        if (out_->EnableVideoOutput(pick->mode, bmdVideoOutputFlagDefault) != S_OK) {
            // submit() retries on the next composite, so this has to latch or it
            // would log at frame rate. The overwhelmingly likely cause is another
            // application already holding the card.
            if (failedMode_ != pick->mode) {
                failedMode_ = pick->mode;
                std::fprintf(stderr,
                             "DeckLink: could not enable %s on %s "
                             "(is another application using the card?)\n",
                             pick->name.c_str(), device_.c_str());
            }
            return false;
        }
        failedMode_ = bmdModeUnknown;
        enabled_ = true;
        active_ = *pick;

        // Let the card state its own stride rather than assuming width * 4.
        // RowBytesForPixelFormat arrived after 12.0; without it the packed
        // stride is right for the 8-bit BGRA we ask for.
        int32_t rowBytes = 0;
#ifdef JPLAY_DECKLINK_HAS_ROW_BYTES
        if (out_->RowBytesForPixelFormat(kPixelFormat, active_.width, &rowBytes) != S_OK)
            rowBytes = 0;
#endif
        if (rowBytes <= 0)
            rowBytes = active_.width * 4;
        stride_ = rowBytes;
        if (out_->CreateVideoFrame(active_.width, active_.height, rowBytes, kPixelFormat,
                                   bmdFrameFlagDefault, frame_.addr()) != S_OK) {
            std::fprintf(stderr, "DeckLink: CreateVideoFrame failed\n");
            out_->DisableVideoOutput();
            enabled_ = false;
            return false;
        }
        std::fprintf(stderr, "DeckLink: %s on %s\n", active_.name.c_str(), device_.c_str());
        return true;
    }

    // Pick from modes_, which open() already filtered to what the card will
    // actually run in our pixel format.
    //
    // Rate first, then raster. Matching the rate matters more than matching the
    // resolution: the wrong raster costs a scale, the wrong rate costs judder on
    // every frame. Among the modes at the right rate, the smallest that still
    // covers the program wins (a downscale loses detail that an upscale keeps),
    // falling back to the largest available when none does.
    const Mode* pickMode(int w, int h, double fps) const {
        const Mode* best = nullptr;
        bool bestRate = false;
        for (const Mode& m : modes_) {
            // 0.01 absorbs the 1000/1001 rounding between a timeline's 23.976 and
            // the card's 24000/1001.
            const bool rate = fps > 0.0 && std::fabs(m.fps - fps) < 0.01;
            if (!best) {
                best = &m;
                bestRate = rate;
                continue;
            }
            if (rate != bestRate) { // a rate match always beats a rate mismatch
                if (rate) { best = &m; bestRate = rate; }
                continue;
            }
            const bool mCovers = m.width >= w && m.height >= h;
            const bool bCovers = best->width >= w && best->height >= h;
            if (mCovers != bCovers) {
                if (mCovers) best = &m;
                continue;
            }
            const long long mArea = (long long)m.width * m.height;
            const long long bArea = (long long)best->width * best->height;
            // Both cover: take the tighter fit. Neither covers: take the roomiest.
            if (mCovers ? (mArea < bArea) : (mArea > bArea))
                best = &m;
        }
        return best;
    }

    // 8-bit BGRA is a byte swizzle away from the player's RGBA readback, with no
    // colour maths of our own in the path.
    static constexpr BMDPixelFormat kPixelFormat = bmdFormat8BitBGRA;

    Ref<IDeckLinkOutput>            out_;
    Ref<IDeckLinkMutableVideoFrame> frame_;
    std::vector<Mode>               modes_;
    Mode                            active_;
    std::string                     device_;
    int                             stride_ = 0;
    bool                            enabled_ = false;
    unsigned                        dropped_ = 0;
    BMDDisplayMode                  failedMode_ = bmdModeUnknown; // last mode we failed to enable
};

} // namespace

bool deckLinkRuntimeAvailable() {
    if (firstOutputDevice(nullptr))
        return true;
    // Nothing to play out. No driver and no card are the ordinary cases and stay
    // quiet, but there is one failure worth naming: a driver older than the SDK
    // this was built against answers none of our interface IIDs, so every device
    // is skipped and the backend greys out with no hint as to why. Say it once --
    // this runs at startup and again on every hotplug.
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::string driver;
        const int64_t have = driverApiVersion(&driver);
        if (have != 0 && have < BLACKMAGIC_DECKLINK_API_VERSION)
            std::fprintf(stderr,
                         "jplay: DeckLink driver is %s but this build targets SDK %s; "
                         "no device can be used. Rebuild with -DDECKLINK_SDK_DIR set to "
                         "an SDK no newer than the driver.\n",
                         driver.c_str(), BLACKMAGIC_DECKLINK_API_VERSION_STRING);
    }
    return false;
}

std::unique_ptr<OutputDevice> createDeckLinkOutput() {
    return std::make_unique<DeckLinkOutput>();
}

#endif // JPLAY_ENABLE_DECKLINK
