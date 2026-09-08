#include "ExrSource.h"

#include "ImageSeq.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <Imath/ImathBox.h>
#include <Imath/half.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

// half (bit pattern) -> 8-bit sRGB, built once. EXR pixels are scene-linear;
// review display applies the sRGB transfer curve.
static const std::array<uint8_t, 65536>& halfToSrgbLut() {
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

// pixelAspectRatio is a required EXR attribute, so it is always present — but
// writers do emit 0 or a NaN for it, and a bad value would collapse or explode
// the display rect. Anything not plausible falls back to square pixels.
static float sanePixelAspect(float pa) {
    return (std::isfinite(pa) && pa >= 0.01f && pa <= 100.0f) ? pa : 1.0f;
}

static const char* compressionName(Imf::Compression c) {
    switch (c) {
    case Imf::NO_COMPRESSION:    return "none";
    case Imf::RLE_COMPRESSION:   return "RLE";
    case Imf::ZIPS_COMPRESSION:  return "ZIPS (per scanline)";
    case Imf::ZIP_COMPRESSION:   return "ZIP";
    case Imf::PIZ_COMPRESSION:   return "PIZ";
    case Imf::PXR24_COMPRESSION: return "PXR24";
    case Imf::B44_COMPRESSION:   return "B44";
    case Imf::B44A_COMPRESSION:  return "B44A";
    case Imf::DWAA_COMPRESSION:  return "DWAA";
    case Imf::DWAB_COMPRESSION:  return "DWAB";
    default:                     return "unknown";
    }
}

// Which part of an EXR, and which channel-name prefix within it, holds the image.
struct ExrColorPart {
    int part = 0;
    std::string layer; // "" = channels named R/G/B (or Y) with no layer prefix
    bool found = false;
};

static bool isDeepPart(const Imf::Header& h) {
    return h.hasType() && (h.type() == Imf::DEEPSCANLINE || h.type() == Imf::DEEPTILE);
}

// How well a channel-name prefix within one part matches "this is the picture":
// full RGB triple > luminance (Imf::RgbaInputFile expands Y, and Y/RY/BY chroma,
// to RGB itself) > a partial triple. 0 means no colour channels under `prefix`.
static int colorScore(const Imf::ChannelList& c, const std::string& prefix) {
    const bool r = c.findChannel(prefix + "R") != nullptr;
    const bool g = c.findChannel(prefix + "G") != nullptr;
    const bool b = c.findChannel(prefix + "B") != nullptr;
    if (r && g && b)
        return 3;
    if (c.findChannel(prefix + "Y"))
        return 2;
    return (r || g || b) ? 1 : 0;
}

// Locate the part (and layer prefix within it) that carries the image.
//
// Imf::RgbaInputFile reads part 0 unless told otherwise, and fills any channel
// it cannot find with 0.0 instead of failing — so a multi-part file whose part 0
// is an AOV, or a part whose colour channels are layer-prefixed, decodes to a
// silently black frame with no error. Multi-part files are normal output from a
// comp pipeline (one part per layer), so search all parts for the colour
// channels rather than assuming part 0 holds them.
static ExrColorPart findColorPart(const Imf::MultiPartInputFile& mp) {
    static const char* kPreferredLayers[] = { "rgba", "rgb", "beauty", "main", "color", "colour" };

    ExrColorPart best;
    int bestScore = 0;
    // bias breaks ties between equally-coloured candidates: unprefixed channels
    // first, then a conventionally-named colour layer, then any other layer.
    auto consider = [&](int part, const std::string& layer, int bias) {
        const std::string prefix = layer.empty() ? std::string() : layer + ".";
        const int s = colorScore(mp.header(part).channels(), prefix);
        if (s == 0)
            return;
        const int ranked = s * 10 + bias;
        if (ranked > bestScore) {
            bestScore = ranked;
            best = { part, layer, true };
        }
    };

    for (int i = 0; i < mp.parts(); ++i) {
        if (isDeepPart(mp.header(i)))
            continue; // deep parts cannot be read through RgbaInputFile
        consider(i, "", 2);
        std::set<std::string> layers;
        mp.header(i).channels().layers(layers);
        for (const std::string& l : layers) {
            std::string lower = l;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            const bool preferred = std::any_of(std::begin(kPreferredLayers), std::end(kPreferredLayers),
                                               [&](const char* p) { return lower == p; });
            consider(i, l, preferred ? 1 : 0);
        }
    }
    return best; // found == false => nothing colour-like anywhere; caller reports it
}

// Translate the EXR header into overlay lines: which part/layer is being read
// (multi-part or layer-prefixed files only), bit depth (per-channel pixel type),
// channel list, layers, views (multi-view files), and compression.
static std::vector<InfoField> buildExrDetails(const Imf::Header& h, int parts, const ExrColorPart& pick) {
    std::vector<InfoField> f;
    const Imf::ChannelList& chans = h.channels();

    if (parts > 1 || !pick.layer.empty()) {
        std::string s;
        if (parts > 1) {
            s = "part " + std::to_string(pick.part) + " of " + std::to_string(parts);
            if (h.hasName())
                s += " \"" + h.name() + "\"";
        }
        if (!pick.layer.empty())
            s += (s.empty() ? "layer \"" : ", layer \"") + pick.layer + "\"";
        f.push_back({ "Reading", s });
    }

    std::set<Imf::PixelType> types;
    int nchan = 0;
    std::vector<std::string> names;
    for (auto it = chans.begin(); it != chans.end(); ++it) {
        ++nchan;
        types.insert(it.channel().type);
        if (names.size() < 16)
            names.push_back(it.name());
    }

    std::string depth;
    for (Imf::PixelType t : types) {
        if (!depth.empty()) depth += ", ";
        switch (t) {
        case Imf::HALF:  depth += "16-bit float (half)"; break;
        case Imf::FLOAT: depth += "32-bit float"; break;
        case Imf::UINT:  depth += "32-bit uint"; break;
        default:         depth += "unknown"; break;
        }
    }
    if (!depth.empty())
        f.push_back({ "Bit depth", depth });

    std::string chanList = std::to_string(nchan) + ": ";
    for (size_t i = 0; i < names.size(); ++i)
        chanList += (i ? ", " : "") + names[i];
    if ((int)names.size() < nchan)
        chanList += ", ...";
    f.push_back({ "Channels", chanList });

    std::set<std::string> layerSet;
    chans.layers(layerSet);
    if (!layerSet.empty()) {
        std::string layers;
        for (const auto& l : layerSet)
            layers += (layers.empty() ? "" : ", ") + l;
        f.push_back({ "Layers", layers });
    } else {
        f.push_back({ "Layers", "none" });
    }

    if (Imf::hasMultiView(h)) {
        const std::vector<std::string>& views = Imf::multiView(h);
        std::string v;
        for (const auto& s : views)
            v += (v.empty() ? "" : ", ") + s;
        f.push_back({ "Views", v.empty() ? "default" : v });
    } else {
        f.push_back({ "Views", "single" });
    }

    const float pa = sanePixelAspect(h.pixelAspectRatio());
    if (std::fabs(pa - 1.0f) > 1e-4f) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.4g (non-square pixels)", (double)pa);
        f.push_back({ "Pixel aspect", buf });
    }

    f.push_back({ "Compression", compressionName(h.compression()) });
    return f;
}

std::shared_ptr<ExrSequenceSource> ExrSequenceSource::open(const std::string& anyFrameFile, std::string& err) {
    auto src = std::shared_ptr<ExrSequenceSource>(new ExrSequenceSource());
    src->files_ = ImageSeq::files(anyFrameFile);
    if (src->files_.empty()) {
        err = "file not found";
        return nullptr;
    }
    src->firstFile_ = src->files_.front();

    // Recover the sequence's starting frame number from the first file's trailing
    // digit run (e.g. "shot.0994.exr" -> 994), so the readout shows real frame
    // numbers. Stays 0 for a single still with no numbering.
    {
        std::string stem = fs::path(src->firstFile_).stem().string();
        size_t d = stem.size();
        while (d > 0 && std::isdigit((unsigned char)stem[d - 1]))
            --d;
        if (d < stem.size())
            src->firstFrame_ = std::stoll(stem.substr(d));
    }

    // Read the first file's headers for display-window dimensions (headers only,
    // no pixel data), so callers can query resolution without a full decode, and
    // settle which part/layer the frame reads come from. The sequence is assumed
    // homogeneous, as it already is for the dimensions.
    try {
        Imf::MultiPartInputFile mp(src->firstFile_.c_str());
        const ExrColorPart pick = findColorPart(mp);
        src->part_ = pick.part;
        src->layer_ = pick.layer;

        const Imf::Header& h = mp.header(pick.part);
        const Imath::Box2i disp = h.displayWindow();
        src->width_ = disp.max.x - disp.min.x + 1;
        src->height_ = disp.max.y - disp.min.y + 1;
        src->pixelAspect_ = sanePixelAspect(h.pixelAspectRatio());
        src->details_ = buildExrDetails(h, mp.parts(), pick);

        if (!pick.found) {
            // Frames will decode black (see findColorPart); dump what the file
            // does contain so the channel naming can be identified.
            std::string dump;
            for (int i = 0; i < mp.parts(); ++i) {
                dump += "\n  part " + std::to_string(i) + ": ";
                const Imf::ChannelList& c = mp.header(i).channels();
                int n = 0;
                for (auto it = c.begin(); it != c.end() && n < 12; ++it, ++n)
                    dump += (n ? ", " : "") + std::string(it.name());
            }
            std::fprintf(stderr, "EXR has no R/G/B or Y channels in any part (%s):%s\n",
                         src->firstFile_.c_str(), dump.c_str());
        }
    } catch (const std::exception&) {
        // Leave dimensions at 0; readFrame reports per-frame errors.
    }
    return src;
}

FramePtr ExrSequenceSource::readFrame(int64_t index) {
    if (files_.empty())
        return nullptr;
    if (index < 0) index = 0;
    if (index >= (int64_t)files_.size()) index = (int64_t)files_.size() - 1;

    const std::string& file = files_[(size_t)index];
    try {
        Imf::RgbaInputFile in(part_, file.c_str(), layer_);
        const Imath::Box2i disp = in.displayWindow();
        const Imath::Box2i data = in.dataWindow();

        const int dispW = disp.max.x - disp.min.x + 1;
        const int dispH = disp.max.y - disp.min.y + 1;
        const int dataW = data.max.x - data.min.x + 1;
        const int dataH = data.max.y - data.min.y + 1;
        if (dispW <= 0 || dispH <= 0 || dataW <= 0 || dataH <= 0)
            return nullptr;

        // Per-thread scratch buffer for the raw scanline read; this source is
        // stateless and reads run fully in parallel across worker threads, so the
        // scratch must be thread-local rather than shared on the instance.
        thread_local std::vector<Imf::Rgba> pixels;
        pixels.resize((size_t)dataW * dataH);
        in.setFrameBuffer(pixels.data() - data.min.x - (size_t)data.min.y * dataW, 1, (size_t)dataW);
        in.readPixels(data.min.y, data.max.y);

        // Reuse a retired output buffer pair instead of allocating fresh every read.
        ExrBuffers bufs;
        {
            std::lock_guard<std::mutex> lk(pool_->mtx);
            if (!pool_->free.empty()) {
                bufs = std::move(pool_->free.back());
                pool_->free.pop_back();
            }
        }

        auto* raw = new Frame();
        raw->width = dispW;
        raw->height = dispH;
        raw->pixelAspect = pixelAspect_;
        // Whether the data window covers the whole display window -- the ordinary
        // case, and the one every frame of a rendered sequence takes. When it does
        // the composite below writes every pixel of both buffers, so clearing them
        // first is ~80 MB of zero-fill per 4K frame that is overwritten immediately
        // after (and a buffer back from the pool is already the right size, so the
        // resize is then free). Only a data window smaller than the display window
        // leaves a border no pixel is written to, and only that case has to clear --
        // otherwise the border would show whatever frame the pooled buffer last held.
        const bool covers = data.min.x <= disp.min.x && data.min.y <= disp.min.y &&
                            data.max.x >= disp.max.x && data.max.y >= disp.max.y;
        const size_t npx = (size_t)dispW * dispH;
        if (covers) {
            bufs.rgba.resize(npx * 4);
            bufs.linearRgb.resize(npx * 3);
        } else {
            bufs.rgba.assign(npx * 4, 0);
            bufs.linearRgb.assign(npx * 3, Imath::half(0.0f));
        }
        raw->rgba = std::move(bufs.rgba);
        raw->linearRgb = std::move(bufs.linearRgb);

        // Composite the data window into display-window space (intersection only).
        // rgba is filled via the baked sRGB LUT (fallback / thumbnails).
        // linearRgb carries the raw half-float scene-linear values for OCIO display.
        const auto& lut = halfToSrgbLut();
        const int y0 = std::max(disp.min.y, data.min.y);
        const int y1 = std::min(disp.max.y, data.max.y);
        const int x0 = std::max(disp.min.x, data.min.x);
        const int x1 = std::min(disp.max.x, data.max.x);
        for (int y = y0; y <= y1; ++y) {
            const Imf::Rgba* srcRow = pixels.data() + (size_t)(y - data.min.y) * dataW + (x0 - data.min.x);
            uint8_t* dstRow    = raw->rgba.data()      + ((size_t)(y - disp.min.y) * dispW + (x0 - disp.min.x)) * 4;
            Imath::half* linRow = raw->linearRgb.data() + ((size_t)(y - disp.min.y) * dispW + (x0 - disp.min.x)) * 3;
            for (int x = x0; x <= x1; ++x, ++srcRow, dstRow += 4, linRow += 3) {
                dstRow[0] = lut[srcRow->r.bits()];
                dstRow[1] = lut[srcRow->g.bits()];
                dstRow[2] = lut[srcRow->b.bits()];
                dstRow[3] = 255;
                linRow[0] = srcRow->r;
                linRow[1] = srcRow->g;
                linRow[2] = srcRow->b;
            }
        }
        // Opaque alpha for the area outside the data window. Inside it the loop
        // above already wrote 255 per pixel, so a data window covering the display
        // window leaves nothing to do here -- and this walks every one of the 33 MB
        // of a 4K buffer four bytes at a time, touching every cache line in it.
        if (!covers)
            for (size_t i = 3; i < raw->rgba.size(); i += 4)
                raw->rgba[i] = 255;

        std::shared_ptr<BufferPool> pool = pool_;
        return std::shared_ptr<Frame>(raw, [pool](Frame* p) {
            {
                std::lock_guard<std::mutex> lk(pool->mtx);
                if (pool->free.size() < 4)
                    pool->free.push_back({ std::move(p->rgba), std::move(p->linearRgb) });
            }
            delete p;
        });
    } catch (const std::exception& e) {
        std::fprintf(stderr, "EXR read failed (%s): %s\n", file.c_str(), e.what());
        return nullptr;
    }
}
