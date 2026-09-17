// Frame's display-referred 8-bit buffer: the lazy build, and the LUT it is built
// through. Kept out of ExrSource.cpp because every source type's frames can be
// asked for it, not only the one that leaves it unbuilt.

#include "MediaSource.h"

#include <array>
#include <cmath>

namespace {

// half (bit pattern) -> 8-bit sRGB, built once. EXR pixels are scene-linear;
// review display applies the sRGB transfer curve.
const std::array<uint8_t, 65536>& halfToSrgbLut() {
    static const std::array<uint8_t, 65536> lut = [] {
        std::array<uint8_t, 65536> t{};
        for (int i = 0; i < 65536; ++i) {
            Imath::half h;
            h.setBits((unsigned short)i);
            float v = (float)h;
            if (!(v > 0.0f)) v = 0.0f; // also catches NaN
            if (v > 1.0f) v = 1.0f;
            float s = v <= 0.0031308f ? v * 12.92f
                                      : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            t[i] = (uint8_t)std::lround(s * 255.0f);
        }
        return t;
    }();
    return lut;
}

std::atomic<bool> g_decodeRgba8{ false };

} // namespace

void setDecodeRgba8(bool on) { g_decodeRgba8.store(on, std::memory_order_relaxed); }
bool decodeRgba8() { return g_decodeRgba8.load(std::memory_order_relaxed); }

uint8_t srgb8FromHalfBits(unsigned short bits) { return halfToSrgbLut()[bits]; }

Frame::Frame(const Frame& o)
    : width(o.width), height(o.height), pixelAspect(o.pixelAspect), rgba16(o.rgba16),
      linearRgb(o.linearRgb) {
    // Copy the buffer only if the source really has one; a copy of an unbuilt
    // frame is unbuilt too, and builds from its own (possibly modified) linearRgb
    // when someone asks. fadeFrame relies on that: it scales linearRgb in place on
    // a copy, and the 8-bit buffer that comes out later has the fade in it.
    if (o.rgbaReady_.load(std::memory_order_acquire)) {
        rgba_ = o.rgba_;
        rgbaReady_.store(true, std::memory_order_release);
    }
}

Frame& Frame::operator=(const Frame& o) {
    if (this == &o)
        return *this;
    width = o.width;
    height = o.height;
    pixelAspect = o.pixelAspect;
    rgba16 = o.rgba16;
    linearRgb = o.linearRgb;
    std::lock_guard<std::mutex> lk(rgbaMtx_);
    if (o.rgbaReady_.load(std::memory_order_acquire)) {
        rgba_ = o.rgba_;
        rgbaReady_.store(true, std::memory_order_release);
    } else {
        rgba_.clear();
        rgba_.shrink_to_fit();
        rgbaReady_.store(false, std::memory_order_release);
    }
    return *this;
}

bool Frame::hasRgba8() const { return rgbaReady_.load(std::memory_order_acquire); }

const std::vector<uint8_t>& Frame::rgba8() const {
    if (rgbaReady_.load(std::memory_order_acquire))
        return rgba_;
    std::lock_guard<std::mutex> lk(rgbaMtx_);
    // Another thread may have built it while this one waited for the lock.
    if (!rgbaReady_.load(std::memory_order_relaxed))
        buildLocked_();
    return rgba_;
}

// The decode-time path: same build, but handed a retired buffer to fill so the
// decoder's pool keeps recycling one allocation per frame instead of the build
// asking the allocator for 34 MB.
void Frame::buildRgba8(std::vector<uint8_t> recycled) {
    std::lock_guard<std::mutex> lk(rgbaMtx_);
    if (rgbaReady_.load(std::memory_order_relaxed))
        return;
    rgba_ = std::move(recycled);
    buildLocked_();
}

void Frame::buildLocked_() const {
    const size_t n = (size_t)width * height;
    if (n && linearRgb.size() >= n * 3) {
        // The same pass ExrSequenceSource::readFrame runs when decodeRgba8() is
        // set: scene-linear half through the baked sRGB LUT, alpha opaque. A pixel
        // outside the data window is 0 in linearRgb, which maps to opaque black.
        const auto& lut = halfToSrgbLut();
        rgba_.resize(n * 4);
        const Imath::half* lin = linearRgb.data();
        uint8_t* out = rgba_.data();
        for (size_t i = 0; i < n; ++i, lin += 3, out += 4) {
            out[0] = lut[lin[0].bits()];
            out[1] = lut[lin[1].bits()];
            out[2] = lut[lin[2].bits()];
            out[3] = 255;
        }
    }
    // Nothing to build from (an empty frame, or one that carries only rgba16):
    // the buffer stays empty and is marked built, so this is asked once.
    rgbaReady_.store(true, std::memory_order_release);
}

std::vector<uint8_t>& Frame::rgba8Mut() {
    rgbaReady_.store(true, std::memory_order_release);
    return rgba_;
}

void Frame::setRgba8(std::vector<uint8_t> px) {
    rgba_ = std::move(px);
    rgbaReady_.store(true, std::memory_order_release);
}

std::vector<uint8_t> Frame::releaseRgba8() {
    std::vector<uint8_t> px = std::move(rgba_);
    rgba_.clear();
    rgbaReady_.store(false, std::memory_order_release);
    return px;
}

size_t Frame::bytes() const {
    // Not under the lock: a torn read can only mean the 8-bit buffer was built
    // concurrently, and the caller (the cache's budget) re-reads it later anyway.
    return rgba_.size() + rgba16.size() * sizeof(uint16_t) +
           linearRgb.size() * sizeof(Imath::half);
}
