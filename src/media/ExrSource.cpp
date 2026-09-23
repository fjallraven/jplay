#include "ExrSource.h"

#include "ExrFast.h"
#include "ImageSeq.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfThreading.h>
#include <OpenEXR/ImfMultiView.h>
#include <Imath/ImathBox.h>
#include <Imath/half.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>

namespace fs = std::filesystem;

static std::atomic<int> g_exrReads{0};

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

// Whether R, G and B all exist under `prefix` with no subsampling: the layout
// readFrame can slice directly into the frame buffer.
static bool hasFullResRgb(const Imf::ChannelList& c, const std::string& prefix) {
    for (const char* name : { "R", "G", "B" }) {
        const Imf::Channel* ch = c.findChannel(prefix + name);
        if (!ch || ch->xSampling != 1 || ch->ySampling != 1)
            return false;
    }
    return true;
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
static const char* pixelTypeName(Imf::PixelType t) {
    switch (t) {
    case Imf::HALF:  return "half";
    case Imf::FLOAT: return "float";
    case Imf::UINT:  return "uint";
    default:         return "unknown";
    }
}

// Every part's channels, for the CHANNELS tab (see ExrLayout).
static ExrLayout buildExrLayout(const Imf::MultiPartInputFile& mp) {
    ExrLayout l;
    if (mp.parts() > 0 && Imf::hasMultiView(mp.header(0)))
        l.views = Imf::multiView(mp.header(0));
    for (int i = 0; i < mp.parts(); ++i) {
        const Imf::Header& h = mp.header(i);
        ExrLayout::Part p;
        if (h.hasName())
            p.name = h.name();
        if (h.hasView())
            p.view = h.view();
        p.deep = isDeepPart(h);
        p.compressed = h.compression() != Imf::NO_COMPRESSION;
        for (auto it = h.channels().begin(); it != h.channels().end(); ++it) {
            ExrLayout::Channel ch;
            ch.name = it.name();
            ch.type = pixelTypeName(it.channel().type);
            ch.half = it.channel().type == Imf::HALF;
            ch.subsampled = it.channel().xSampling != 1 || it.channel().ySampling != 1;
            // A multi-part stereo file names each part's view; a single-part one
            // puts the view in the channel name, next to last ("diffuse.right.R"),
            // with the default view's channels left without one.
            std::vector<std::string> comps;
            for (size_t a = 0, b; a <= ch.name.size(); a = b + 1) {
                b = ch.name.find('.', a);
                if (b == std::string::npos)
                    b = ch.name.size();
                comps.push_back(ch.name.substr(a, b - a));
            }
            // OpenEXR answers "" for a layered channel with no view in its name
            // ("diffuse.R"); like an unprefixed one, that is the default view's.
            ch.view = !p.view.empty() ? p.view
                    : !l.views.empty() ? Imf::viewFromChannelName(ch.name, l.views)
                                       : std::string();
            if (ch.view.empty() && !l.views.empty())
                ch.view = l.views.front();
            if (p.view.empty() && !ch.view.empty() && comps.size() >= 2 && comps[comps.size() - 2] == ch.view)
                comps.erase(comps.end() - 2);
            ch.base = comps.back();
            for (size_t k = 0; k + 1 < comps.size(); ++k)
                ch.layer += (k ? "." : "") + comps[k];
            p.channels.push_back(std::move(ch));
        }
        l.parts.push_back(std::move(p));
    }
    return l;
}

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
    // Until the headers say otherwise: part 0 through Imf::RgbaInputFile, which is
    // also what a file whose headers fail to read is left with.
    auto plan = std::make_shared<ReadPlan>();
    plan->rgb[0] = "R";
    plan->rgb[1] = "G";
    plan->rgb[2] = "B";
    try {
        Imf::MultiPartInputFile mp(src->firstFile_.c_str());
        const ExrColorPart pick = findColorPart(mp);
        src->layout_ = buildExrLayout(mp);
        const std::string prefix = pick.layer.empty() ? std::string() : pick.layer + ".";
        plan->part = pick.part;
        plan->rgbaLayer = pick.layer;
        for (int c = 0; c < 3; ++c)
            plan->rgb[c] = prefix + "RGB"[c];

        const Imf::Header& h = mp.header(pick.part);
        const Imath::Box2i disp = h.displayWindow();
        src->width_ = disp.max.x - disp.min.x + 1;
        src->height_ = disp.max.y - disp.min.y + 1;
        src->pixelAspect_ = sanePixelAspect(h.pixelAspectRatio());
        src->details_ = buildExrDetails(h, mp.parts(), pick);
        plan->direct = pick.found && hasFullResRgb(h.channels(), prefix);
        plan->compressed = h.compression() != Imf::NO_COMPRESSION;
        plan->fast = plan->direct && !plan->compressed;
        // A luminance picture (Y, or Y/RY/BY) goes through RgbaInputFile; named
        // here as its Y channel in gray, which is how the CHANNELS tab shows it.
        if (!plan->direct && h.channels().findChannel(prefix + "Y"))
            plan->rgb[0] = plan->rgb[1] = plan->rgb[2] = prefix + "Y";

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
    src->defaultPlan_ = plan;
    src->plan_ = std::move(plan);
    return src;
}

ChannelSelection ExrSequenceSource::defaultSelection() const {
    ChannelSelection sel;
    sel.part = defaultPlan_->part;
    for (int c = 0; c < 3; ++c)
        sel.rgb[c] = defaultPlan_->rgb[c];
    return sel;
}

bool ExrSequenceSource::setChannelSelection(const ChannelSelection& sel) {
    // The source's own pick, and anything that spells it out, is the default plan
    // itself: that keeps the RgbaInputFile route a luminance file needs.
    if (sel.isDefault() || sel == defaultSelection()) {
        std::atomic_store(&plan_, defaultPlan_);
        return true;
    }
    if (sel.part >= (int)layout_.parts.size() || layout_.parts[(size_t)sel.part].deep)
        return false;
    const ExrLayout::Part& p = layout_.parts[(size_t)sel.part];
    auto plan = std::make_shared<ReadPlan>();
    plan->part = sel.part;
    plan->direct = true;
    plan->compressed = p.compressed;
    plan->fast = !p.compressed;
    bool any = false;
    for (int c = 0; c < 3; ++c) {
        plan->rgb[c] = sel.rgb[c];
        if (sel.rgb[c].empty()) {
            plan->fast = false; // the fast reader has no black slot
            continue;
        }
        auto it = std::find_if(p.channels.begin(), p.channels.end(),
                               [&](const ExrLayout::Channel& ch) { return ch.name == sel.rgb[c]; });
        // Subsampled channels (luma/chroma) are only readable through the default
        // plan's RgbaInputFile route; the direct slices below need full resolution.
        if (it == p.channels.end() || it->subsampled)
            return false;
        plan->fast = plan->fast && it->half;
        any = true;
    }
    if (!any)
        return false;
    std::atomic_store(&plan_, std::shared_ptr<const ReadPlan>(std::move(plan)));
    return true;
}

FramePtr ExrSequenceSource::readFrame(int64_t index) {
    if (files_.empty())
        return nullptr;
    if (index < 0) index = 0;
    if (index >= (int64_t)files_.size()) index = (int64_t)files_.size() - 1;

    const std::string& file = files_[(size_t)index];
    struct ReadCount {
        const int n = ++g_exrReads;
        ~ReadCount() { --g_exrReads; }
    } reads;
    // One snapshot for the whole read: setChannelSelection() may swap the plan
    // while this runs, and a frame must not mix two of them.
    const std::shared_ptr<const ReadPlan> plan = std::atomic_load(&plan_);
    const int threads = (reads.n == 1 && plan->compressed) ? Imf::globalThreadCount() : 0;

    try {
        Frame::HalfBuffer linear;
        int dispW = 0, dispH = 0;

        // Uncompressed half RGB has nothing to decode, so the file is streamed and
        // interleaved directly (see ExrFast.h). A file the fast reader declines --
        // this one differs from the first, or is not what it claims -- takes the
        // OpenEXR path below instead, and the result is the same either way.
        bool done = false;
        if (plan->fast && exrfast::enabled()) {
            done = exrfast::readRgbHalf(file, plan->part, plan->rgb, linear, dispW, dispH);
        }

        if (!done) {
            std::optional<Imf::MultiPartInputFile> mp;
            std::optional<Imf::InputPart> part;
            std::optional<Imf::RgbaInputFile> rgbaFile;
            Imath::Box2i disp, data;
            if (plan->direct) {
                mp.emplace(file.c_str(), threads);
                part.emplace(*mp, plan->part);
                disp = part->header().displayWindow();
                data = part->header().dataWindow();
            } else {
                rgbaFile.emplace(plan->part, file.c_str(), plan->rgbaLayer, threads);
                disp = rgbaFile->displayWindow();
                data = rgbaFile->dataWindow();
            }

            dispW = disp.max.x - disp.min.x + 1;
            dispH = disp.max.y - disp.min.y + 1;
            const int dataW = data.max.x - data.min.x + 1;
            const int dataH = data.max.y - data.min.y + 1;
            if (dispW <= 0 || dispH <= 0 || dataW <= 0 || dataH <= 0)
                return nullptr;

            // Whether the data window covers the whole display window -- the ordinary
            // case, and the one every frame of a rendered sequence takes. When it does
            // the decode below writes every pixel of linearRgb, so nothing needs
            // clearing first (and resize() on this buffer type does not zero-fill:
            // see Frame::HalfBuffer). Only a data window smaller than the display
            // window leaves a border no pixel is written to, and only that case has to
            // clear -- otherwise the border would show whatever the recycled memory
            // last held. rgba is never cleared: the LUT pass at the end writes all of it.
            const bool covers = data.min.x <= disp.min.x && data.min.y <= disp.min.y &&
                                data.max.x >= disp.max.x && data.max.y >= disp.max.y;
            const size_t npx = (size_t)dispW * dispH;
            if (covers)
                linear.resize(npx * 3);
            else
                linear.assign(npx * 3, Imath::half(0.0f));

            // The part of the data window that lands on the display window.
            const int y0 = std::max(disp.min.y, data.min.y);
            const int y1 = std::min(disp.max.y, data.max.y);
            const int x0 = std::max(disp.min.x, data.min.x);
            const int x1 = std::min(disp.max.x, data.max.x);
            const size_t pixelBytes = 3 * sizeof(Imath::half);
            auto linRow = [&](int y) {
                return linear.data() + ((size_t)(y - disp.min.y) * dispW + (x0 - disp.min.x)) * 3;
            };

            // Scratch buffers are per thread: this source is stateless and reads run
            // fully in parallel across worker threads.
            if (part && x0 <= x1 && y0 <= y1) {
                // R/G/B decode straight into linearRgb when every decoded column lands
                // inside the display window, which a rendered sequence's always do. A
                // data window wider than the display window would write past the row
                // ends, so only that case decodes into scratch and copies across.
                const bool direct = data.min.x >= disp.min.x && data.max.x <= disp.max.x;
                thread_local std::vector<Imath::half> scratch;
                char* base;
                Imath::V2i origin;
                int64_t w;
                if (direct) {
                    base = (char*)linear.data();
                    origin = disp.min;
                    w = dispW;
                } else {
                    scratch.resize((size_t)dataW * (y1 - y0 + 1) * 3);
                    base = (char*)scratch.data();
                    origin = Imath::V2i(data.min.x, y0);
                    w = dataW;
                }
                const int64_t h = direct ? dispH : (y1 - y0 + 1);
                // A channel named in more than one slot (a grayscale view) is
                // decoded once, into its first slot, and copied across after: a
                // frame buffer holds one slice per name. An empty slot names a
                // channel no file has, which OpenEXR fills with 0.
                int from[3] = { 0, 1, 2 };
                bool copies = false;
                for (int c = 1; c < 3; ++c)
                    for (int q = 0; q < c; ++q)
                        if (!plan->rgb[c].empty() && plan->rgb[c] == plan->rgb[q]) {
                            from[c] = q;
                            copies = true;
                            break;
                        }
                Imf::FrameBuffer fb;
                for (int c = 0; c < 3; ++c)
                    if (from[c] == c)
                        fb.insert(plan->rgb[c].empty() ? std::string("\x01jplay.black") + char('0' + c)
                                                       : plan->rgb[c],
                                  Imf::Slice::Make(Imf::HALF, base + c * sizeof(Imath::half), origin, w, h,
                                                   pixelBytes, pixelBytes * (size_t)w));
                part->setFrameBuffer(fb);
                part->readPixels(y0, y1);
                if (!direct)
                    for (int y = y0; y <= y1; ++y)
                        std::memcpy(linRow(y),
                                    scratch.data() + ((size_t)(y - y0) * dataW + (x0 - data.min.x)) * 3,
                                    (size_t)(x1 - x0 + 1) * pixelBytes);
                if (copies)
                    for (int y = y0; y <= y1; ++y) {
                        Imath::half* px = linRow(y);
                        for (int x = x0; x <= x1; ++x, px += 3) {
                            px[1] = px[from[1]];
                            px[2] = px[from[2]];
                        }
                    }
            } else if (rgbaFile) {
                thread_local std::vector<Imf::Rgba> pixels;
                pixels.resize((size_t)dataW * dataH);
                rgbaFile->setFrameBuffer(pixels.data() - data.min.x - (size_t)data.min.y * dataW, 1, (size_t)dataW);
                rgbaFile->readPixels(data.min.y, data.max.y);
                for (int y = y0; y <= y1; ++y) {
                    const Imf::Rgba* src = pixels.data() + (size_t)(y - data.min.y) * dataW + (x0 - data.min.x);
                    Imath::half* dst = linRow(y);
                    for (int x = x0; x <= x1; ++x, ++src, dst += 3) {
                        dst[0] = src->r;
                        dst[1] = src->g;
                        dst[2] = src->b;
                    }
                }
            }
        }

        auto* raw = new Frame();
        raw->width = dispW;
        raw->height = dispH;
        raw->pixelAspect = pixelAspect_;
        raw->linearRgb = std::move(linear);
        // The display-referred 8-bit buffer is derived from linearRgb, and running
        // its LUT here costs more than the decode it follows -- 16 ms of a 27 ms
        // 4K read -- for a buffer the colour-managed display path never looks at.
        // So it is left to Frame::rgba8(), and built here only while the player
        // says every frame will need it anyway (see decodeRgba8), into a retired
        // buffer from the pool when one is there.
        if (decodeRgba8()) {
            std::vector<uint8_t> rgba;
            {
                std::lock_guard<std::mutex> lk(pool_->mtx);
                if (!pool_->freeRgba.empty()) {
                    rgba = std::move(pool_->freeRgba.back());
                    pool_->freeRgba.pop_back();
                }
            }
            raw->buildRgba8(std::move(rgba));
        }

        std::shared_ptr<BufferPool> pool = pool_;
        return std::shared_ptr<Frame>(raw, [pool](Frame* p) {
            std::vector<uint8_t> rgba = p->releaseRgba8();
            if (!rgba.empty()) {
                std::lock_guard<std::mutex> lk(pool->mtx);
                if (pool->freeRgba.size() < 4)
                    pool->freeRgba.push_back(std::move(rgba));
            }
            delete p; // linearRgb goes back to the process-wide FramePool via its allocator
        });
    } catch (const std::exception& e) {
        std::fprintf(stderr, "EXR read failed (%s): %s\n", file.c_str(), e.what());
        return nullptr;
    }
}
