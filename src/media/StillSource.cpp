#include "StillSource.h"

#include "ImageSeq.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

// Overlay lines for a still: everything jplay can say about a display-referred
// 8-bit source. Read once from the first frame's loaded surface.
std::vector<InfoField> buildStillDetails(SDL_PixelFormat fmt) {
    std::vector<InfoField> f;
    const int srcBpp = (int)SDL_BITSPERPIXEL(fmt);
    // Over 32 bits per pixel means more than 8 bits per channel, which readFrame
    // keeps at 16 rather than crushing on load.
    f.push_back({ "Bit depth", srcBpp > 32 ? "16-bit" : "8-bit" });
    f.push_back({ "Channels", SDL_ISPIXELFORMAT_ALPHA(fmt) ? "RGB (alpha dropped)" : "RGB" });
    f.push_back({ "Color space", "sRGB (assumed)" });
    return f;
}

} // namespace

std::shared_ptr<StillSequenceSource> StillSequenceSource::open(const std::string& anyFrameFile,
                                                              std::string& err) {
    auto src = std::shared_ptr<StillSequenceSource>(new StillSequenceSource());
    src->files_ = ImageSeq::files(anyFrameFile);
    if (src->files_.empty()) {
        err = "file not found";
        return nullptr;
    }
    src->firstFile_ = src->files_.front();

    // Recover the sequence's starting frame number from the first file's trailing
    // digit run (e.g. "shot.0994.png" -> 994), so the readout shows real frame
    // numbers. Stays 0 for a single still with no numbering.
    {
        std::string stem = fs::path(src->firstFile_).stem().string();
        size_t d = stem.size();
        while (d > 0 && std::isdigit((unsigned char)stem[d - 1]))
            --d;
        if (d < stem.size())
            src->firstFrame_ = std::stoll(stem.substr(d));
    }

    // SDL_image has no header-only entry point, so dimensions and the detail
    // lines cost one full decode of the first file; the sequence is assumed
    // homogeneous. Unlike EXR this reports failure rather than opening with zero
    // dimensions: the decoders are compiled in per format, so a first-frame
    // failure usually means the format is unsupported, which is worth surfacing.
    SDL_Surface* probe = IMG_Load(src->firstFile_.c_str());
    if (!probe) {
        err = SDL_GetError();
        return nullptr;
    }
    src->width_ = probe->w;
    src->height_ = probe->h;
    src->details_ = buildStillDetails(probe->format);
    SDL_DestroySurface(probe);
    return src;
}

FramePtr StillSequenceSource::readFrame(int64_t index) {
    if (files_.empty())
        return nullptr;
    if (index < 0) index = 0;
    if (index >= (int64_t)files_.size()) index = (int64_t)files_.size() - 1;

    const std::string& file = files_[(size_t)index];
    SDL_Surface* surf = IMG_Load(file.c_str());
    if (!surf) {
        std::fprintf(stderr, "image read failed (%s): %s\n", file.c_str(), SDL_GetError());
        return nullptr;
    }
    // One conversion normalises everything the loaders produce (palette, grey, BGR
    // ordering). A source carrying more than 8 bits per channel keeps them: a
    // display transform applied to crushed 8-bit values bands in the shadows, which
    // is the whole reason to have shot 16-bit in the first place. RGBA32 / RGBA64
    // are R,G,B,A in memory order, matching Frame::rgba / Frame::rgba16; an input
    // already in that layout skips the copy.
    const bool deep = SDL_BITSPERPIXEL(surf->format) > 32;
    const SDL_PixelFormat want = deep ? SDL_PIXELFORMAT_RGBA64 : SDL_PIXELFORMAT_RGBA32;
    if (surf->format != want) {
        SDL_Surface* conv = SDL_ConvertSurface(surf, want);
        SDL_DestroySurface(surf);
        if (!conv) {
            std::fprintf(stderr, "image convert failed (%s): %s\n", file.c_str(), SDL_GetError());
            return nullptr;
        }
        surf = conv;
    }
    const int w = surf->w, h = surf->h;
    if (w <= 0 || h <= 0) {
        SDL_DestroySurface(surf);
        return nullptr;
    }

    // Reuse a retired output buffer instead of allocating fresh every read.
    std::vector<uint8_t> rgba;
    {
        std::lock_guard<std::mutex> lk(pool_->mtx);
        if (!pool_->free.empty()) {
            rgba = std::move(pool_->free.back());
            pool_->free.pop_back();
        }
    }
    rgba.resize((size_t)w * h * 4);

    // Row-wise because pitch may exceed the packed row. Alpha is forced opaque: the
    // channel is dropped rather than composited, so a premultiplied source reads as
    // it would over black.
    std::vector<uint16_t> rgba16;
    if (deep) {
        rgba16.resize((size_t)w * h * 4);
        for (int y = 0; y < h; ++y) {
            const uint8_t* srcRow = (const uint8_t*)surf->pixels + (size_t)y * surf->pitch;
            uint16_t* dstRow = rgba16.data() + (size_t)y * w * 4;
            std::memcpy(dstRow, srcRow, (size_t)w * 8);
            for (int x = 0; x < w; ++x)
                dstRow[x * 4 + 3] = 65535;
        }
        // 8-bit companion, by truncation: 65535 >> 8 is 255, so the endpoints are
        // exact. It feeds the scopes and the small previews, never the display
        // transform, which reads the 16-bit buffer.
        for (size_t i = 0, n = rgba16.size(); i < n; ++i)
            rgba[i] = (uint8_t)(rgba16[i] >> 8);
    } else {
        for (int y = 0; y < h; ++y) {
            const uint8_t* srcRow = (const uint8_t*)surf->pixels + (size_t)y * surf->pitch;
            uint8_t* dstRow = rgba.data() + (size_t)y * w * 4;
            std::memcpy(dstRow, srcRow, (size_t)w * 4);
            for (int x = 0; x < w; ++x)
                dstRow[x * 4 + 3] = 255;
        }
    }
    SDL_DestroySurface(surf);

    // linearRgb stays empty: these formats are display-referred and carry no tags
    // of their own, so the colour management resolves their space from the config
    // (see StillSequenceSource docs).
    auto* raw = new Frame();
    raw->width = w;
    raw->height = h;
    raw->rgba = std::move(rgba);
    raw->rgba16 = std::move(rgba16);

    std::shared_ptr<BufferPool> pool = pool_;
    return std::shared_ptr<Frame>(raw, [pool](Frame* p) {
        {
            std::lock_guard<std::mutex> lk(pool->mtx);
            if (pool->free.size() < 4)
                pool->free.push_back(std::move(p->rgba));
        }
        delete p;
    });
}
