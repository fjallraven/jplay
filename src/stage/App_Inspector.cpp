// Inspector translation unit: the overlay panel showing sequence/shot/clip
// info for whatever is active under the playhead (toggled with 'i'), plus
// renderSourceInfoPanel — the selected-source file/media info sub-panel drawn at
// the bottom of the ProjectExplorer's SOURCES tab. App members, split out of
// App.cpp purely to keep that file manageable.

#include "App.h"
#include "AppInternal.h"
#include "ImageSeq.h"
#include "Layout.h"

#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using namespace jplay;

namespace {

// Human-readable byte count (B / KB / MB / ...).
std::string humanSize(uint64_t bytes) {
    const char* unit[] = { "B", "KB", "MB", "GB", "TB" };
    double s = (double)bytes;
    int i = 0;
    while (s >= 1024.0 && i < 4) { s /= 1024.0; ++i; }
    char buf[32];
    if (i == 0)
        SDL_snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    else
        SDL_snprintf(buf, sizeof(buf), "%.1f %s", s, unit[i]);
    return buf;
}

// Filesystem last-write time formatted as "YYYY-MM-DD HH:MM" (local time).
std::string modTimeStr(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec)
        return {};
    // C++17: bridge the file clock to system_clock via a now()-to-now() offset.
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tmv{};
#ifdef _WIN32
    if (localtime_s(&tmv, &tt) != 0)
        return {};
#else
    if (!localtime_r(&tt, &tmv))
        return {};
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

} // namespace

void App::renderInspector() {
    if (!inspectorOpen_)
        return;

    const float pad = 12.0f * dpiScale;
    const float lineH = textFont_.lineHeight(); // row pitch: glyph height, no extra leading
    const float keyColW = textFont_.measure(renderer_, "Pixel format") + 8.0f;
    const float closeBtn = 14.0f; // X button, reserved at the right of a header row

    // The rows are collected first and drawn afterwards: the panel is sized to fit
    // its own text, so its width is not known until every value is in hand.
    struct Row {
        std::string key, val;
        bool header = false;   // section label rather than a key/value pair
        bool wrap = false;     // long value: split across lines at the width cap
        float gapAfter = 0.0f; // extra space below the row
    };
    std::vector<Row> rows;

    // Key-value row helper.
    auto row = [&](const std::string& key, const std::string& val) {
        rows.push_back({ key, val, false, false, 0.0f });
    };

    // Section header.
    auto header = [&](const std::string& label) {
        rows.push_back({ label, {}, true, false, 5.0f });
    };

    // Extra space below the last row emitted (section separation).
    auto gap = [&](float g) {
        if (!rows.empty())
            rows.back().gapAfter += g;
    };

    // Timecode formatter HH:MM:SS:FF, or raw frame number if the user chose Frames.
    // When the total project is under 1 hour the leading "HH:" is omitted.
    auto fmt = [&](int64_t frames) -> std::string {
        if (showAsFrames())
            return std::to_string(frames);
        double fps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
        int64_t totalSec = (int64_t)(frames / fps);
        int h = (int)(totalSec / 3600);
        int m = (int)((totalSec % 3600) / 60);
        int s = (int)(totalSec % 60);
        int f = (int)(frames % (int64_t)std::llround(fps));
        char buf[32];
        bool shortFmt = timeline_.length() < (int64_t)(3600.0 * fps);
        if (shortFmt)
            SDL_snprintf(buf, sizeof(buf), "%02d:%02d:%02d", m, s, f);
        else
            SDL_snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", h, m, s, f);
        return buf;
    };

    // Resolve the three items active under the playhead.
    const Clip* clip = playheadClip();

    const Sequence* seq = clip ? timeline_.sequenceOfClip(clip->id) : nullptr;
    if (!seq && activeSequenceIdx_ < (int)timeline_.sequences.size())
        seq = &timeline_.sequences[activeSequenceIdx_];

    const Shot* shot = nullptr;
    if (clip && clip->shotId >= 0)
        for (const auto& sh : timeline_.shots)
            if (sh.id == clip->shotId) { shot = &sh; break; }

    // ---- PROJECT ----
    header("PROJECT");
    {
        char buf[32];
        SDL_snprintf(buf, sizeof(buf), "%.2f", timeline_.fps > 0.0 ? timeline_.fps : 24.0);
        row("FPS", buf);
    }

    gap(22.0f);

    // ---- SEQUENCE ----
    header("SEQUENCE");
    row("Name", seq ? seq->name : "\xe2\x80\x94");
    if (seq) {
        row("Shots",    std::to_string(seq->shotIds.size()));
        row("Clips",    std::to_string(seq->clips.size()));
        int64_t dur = 0;
        for (const auto& cl : seq->clips) dur = std::max(dur, cl.end());
        row("Duration", fmt(dur));
    }

    gap(22.0f);

    // ---- SHOT ----
    header("SHOT");
    row("Name", shot ? shot->name : "\xe2\x80\x94");
    if (shot) {
        row("Start",    fmt(shot->timelineStart));
        row("End",      fmt(shot->end()));
        row("Duration", fmt(shot->duration));
        row("Cut In",   std::to_string(shot->cutIn));
        row("Cut Out",  std::to_string(shot->cutOut));
    }

    gap(12.0f);

    // ---- CLIP ----
    std::string clipLabel = "\xe2\x80\x94";
    auto mptr = clip ? timeline_.findMediaById(clip->mediaId) : nullptr;
    if (mptr) clipLabel = fs::path(mptr->path()).filename().string();

    header("CLIP");
    row("Name", clipLabel);
    if (mptr)
        rows.push_back({ "Path", mptr->path(), false, true, 0.0f });
    if (clip) {
        // Exactly what the timeline header shows for the row: the track's custom
        // name, or its derived "Video 1" / "Audio 2".
        row("Track",    trackLabel(clip->track));
        row("Start",    fmt(clip->timelineStart));
        row("End",      fmt(clip->end()));
        row("Duration", fmt(clip->duration));
        row("Src In",   std::to_string(clip->sourceOffset));
        row("Src Out",  std::to_string(clip->sourceOffset + clip->duration));

        if (mptr) {
            row("Type", mptr->type() == ClipType::ImageSequence ? ImageSeq::typeLabel(mptr->path())
                    : mptr->type() == ClipType::Audio           ? "Audio" : "Video");
            MediaInfo info = mptr->info();
            if (info.width > 0)
                row("Size", std::to_string(info.width) + " x " + std::to_string(info.height));
            if (info.frameCount > 0)
                row("Frames", std::to_string(info.frameCount));
            if (info.fps > 0.0) {
                double projFps = timeline_.fps > 0.0 ? timeline_.fps : 24.0;
                char buf[32];
                if (std::abs(info.fps - projFps) > 0.01) {
                    SDL_snprintf(buf, sizeof(buf), "%.3f (native)", info.fps);
                } else {
                    SDL_snprintf(buf, sizeof(buf), "%.3f", info.fps);
                }
                row("FPS", buf);
            }
            // ---- FORMAT ----
            // The decoder-reported fields (bit depth, codec, pixel format, ...),
            // the same set the media bin's FORMAT section lists.
            std::string err;
            if (auto src = mptr->ensureOpen(err)) {
                auto fields = src->describe();
                if (!fields.empty()) {
                    gap(12.0f);
                    header("FORMAT");
                    for (const auto& f : fields)
                        row(f.key, f.value);
                }
            }
        }
    }

    // ---- size the panel to the content ----
    // Width is the widest row, capped so a long path cannot push the panel past
    // the frame; anything wider than the cap is wrapped onto extra lines below.
    const float maxContentW = std::max(kInspectorW, playerRect_.w * 0.9f - pad * 2.0f);
    float contentW = kInspectorW; // floor, so a near-empty panel is not a sliver
    for (const Row& r : rows) {
        float w = r.header ? textFont_.measure(renderer_, r.key.c_str()) + 8.0f + closeBtn
                           : keyColW + textFont_.measure(renderer_, r.val.c_str());
        contentW = std::max(contentW, w);
    }
    contentW = std::min(contentW, maxContentW);

    // Wrap the flagged rows into the value column. Both the fits/does-not-fit test
    // and the character budget come from the row's own measured width, so a value
    // that sized the panel is never split; only a value clipped by maxContentW is.
    const float valColW = contentW - keyColW;
    std::vector<Row> lines;
    for (const Row& r : rows) {
        if (!r.wrap || r.val.empty()) {
            lines.push_back(r);
            continue;
        }
        // Wrap only if the value really does not fit: the widest row is what sized
        // contentW, so for that row valColW is its own measured width (the 0.5px
        // tolerance covers the rounding in that round trip).
        const float valW = textFont_.measure(renderer_, r.val.c_str());
        if (valW <= valColW + 0.5f) {
            lines.push_back(r);
            continue;
        }
        const size_t maxChars = (size_t)std::max(8.0f, std::floor(valColW / (valW / (float)r.val.size())));
        for (size_t pos = 0; pos < r.val.size();) {
            size_t n = std::min(maxChars, r.val.size() - pos);
            if (pos + n < r.val.size()) {
                // Prefer breaking just after a path separator, so a directory name
                // is not split when it does not have to be.
                size_t sep = r.val.find_last_of("/\\", pos + n - 1);
                if (sep != std::string::npos && sep >= pos + maxChars / 2)
                    n = sep - pos + 1;
                // Never break inside a UTF-8 sequence (paths are UTF-8 throughout).
                while (n > 1 && ((unsigned char)r.val[pos + n] & 0xC0) == 0x80)
                    --n;
            }
            Row l = r;
            l.wrap = false;
            l.key = (pos == 0) ? r.key : std::string();
            l.val = r.val.substr(pos, n);
            pos += n;
            l.gapAfter = (pos >= r.val.size()) ? r.gapAfter : 0.0f;
            lines.push_back(l);
        }
    }

    float contentH = 0.0f;
    for (const Row& r : lines)
        contentH += lineH + r.gapAfter;

    const float panelW = contentW + pad * 2.0f;
    const float panelH = std::min(contentH + pad * 2.0f, playerRect_.h * 0.9f);
    inspectorRect_ = { std::round(playerRect_.x + (playerRect_.w - panelW) * 0.5f),
                       std::round(playerRect_.y + (playerRect_.h - panelH) * 0.5f),
                       std::round(panelW), std::round(panelH) };

    // Translucent background (the frame stays readable through it) + border.
    SDL_SetRenderDrawColor(renderer_, kPanelBg.r, kPanelBg.g, kPanelBg.b, 224);
    jplay::fillRect(renderer_, &inspectorRect_);
    SDL_SetRenderDrawColor(renderer_, 80, 82, 90, 220);
    jplay::drawRect(renderer_, &inspectorRect_);

    // ---- draw the rows ----
    // Clipped to the panel and scrolled by inspectorScroll_, which only travels
    // when the content is taller than the (height-capped) panel.
    const float viewTop = inspectorRect_.y + pad;
    const float viewH = inspectorRect_.h - pad * 2.0f;
    inspectorScroll_ = std::clamp(inspectorScroll_, 0.0f, std::max(0.0f, contentH - viewH));
    SDL_Rect listClip = { (int)inspectorRect_.x, (int)viewTop,
                          (int)inspectorRect_.w, (int)std::ceil(viewH) };
    SDL_SetRenderClipRect(renderer_, &listClip);
    float y = viewTop - inspectorScroll_;
    for (const Row& r : lines) {
        if (r.header) {
            drawText(inspectorRect_.x + pad, y, { 160, 170, 200, 255 }, r.key);
        } else {
            if (!r.key.empty())
                drawText(inspectorRect_.x + pad, y, { 130, 135, 148, 255 }, r.key);
            drawText(inspectorRect_.x + pad + keyColW, y, { 215, 218, 225, 255 }, r.val);
        }
        y += lineH + r.gapAfter;
    }
    SDL_SetRenderClipRect(renderer_, nullptr);

    // Scroll indicator: thumb along the right edge, only when the content is taller
    // than the panel. Sits inside the right padding, so it costs the rows no width.
    drawScrollbar(renderer_, { inspectorRect_.x, viewTop, inspectorRect_.w, viewH },
                  contentH, inspectorScroll_, dpiScale, false);

    // Close (X) button, upper-right, centered on the first header row. Drawn last
    // (unclipped) so it stays fixed while the list scrolls beneath it. Clicks are
    // handled in the event loop (see the inspectorCloseRect_ hit-test in App.cpp).
    {
        inspectorCloseRect_ = { inspectorRect_.x + inspectorRect_.w - pad - closeBtn,
                                viewTop + (lineH - closeBtn) * 0.5f, closeBtn, closeBtn };
        float mx = 0.0f, my = 0.0f;
        uiMouse(mx, my);
        bool hov = inRect(inspectorCloseRect_, mx, my);
        icons_.drawGlyph(renderer_, 0xF0156, inspectorCloseRect_, // ICON_MDI_CLOSE
                         hov ? SDL_Color{ 235, 238, 245, 255 } : SDL_Color{ 140, 145, 156, 255 });
    }
}

// Source-info sub-panel: drawn at the bottom of the ProjectExplorer's SOURCES tab
// whenever a media row is selected (inspectMediaPath_). Shows the selected
// source's file + media info. Facts (mod time / size / range) are cached until
// the selection changes so we don't stat the disk every frame. `area` is the
// panel rect handed in by renderProjectExplorer.
void App::renderSourceInfoPanel(const SDL_FRect& area) {
    if (inspectMediaPath_.empty())
        return;

    Media* mptr = nullptr;
    for (const auto& kv : timeline_.media)
        if (kv.second && kv.second->path() == inspectMediaPath_) { mptr = kv.second.get(); break; }

    if (inspectFileInfoPath_ != inspectMediaPath_) {
        inspectFileInfoPath_ = inspectMediaPath_;
        inspectModTime_.clear();
        inspectFileSize_.clear();
        inspectFrameRange_.clear();
        if (mptr) {
            inspectModTime_ = modTimeStr(mptr->path());
            if (mptr->type() == ClipType::ImageSequence) {
                std::error_code ec;
                uint64_t total = 0;
                for (const auto& f : ImageSeq::files(mptr->path())) {
                    uint64_t sz = (uint64_t)fs::file_size(f, ec);
                    if (!ec) total += sz;
                }
                inspectFileSize_ = humanSize(total);
                std::string err;
                if (auto src = mptr->ensureOpen(err)) {
                    int64_t first = src->firstFrameNumber();
                    int64_t cnt = src->frameCount();
                    if (cnt > 0)
                        inspectFrameRange_ = std::to_string(first) + "-" +
                            std::to_string(first + cnt - 1) +
                            " (" + std::to_string(cnt) + ")";
                }
            } else {
                std::error_code ec;
                uint64_t sz = (uint64_t)fs::file_size(mptr->path(), ec);
                if (!ec)
                    inspectFileSize_ = humanSize(sz);
            }
        }
    }

    // Panel background + top divider separating it from the scrolling list.
    SDL_SetRenderDrawColor(renderer_, 26, 27, 31, 255);
    jplay::fillRect(renderer_, &area);
    SDL_SetRenderDrawColor(renderer_, 12, 12, 14, 255);
    jplay::drawLine(renderer_, area.x, area.y + 0.5f, area.x + area.w, area.y + 0.5f);

    SDL_Rect clip = { (int)area.x, (int)area.y, (int)area.w, (int)area.h };
    SDL_SetRenderClipRect(renderer_, &clip);

    const float pad = 8.0f;
    const float lineH = 16.0f;
    const float keyColW = textFont_.measure(renderer_, "Pixel format") + 8.0f;
    const float closeBtn = 14.0f; // X button, reserved at the right of the MEDIA header row
    // Content scrolls with sourceInfoScroll_ under the clip set above. Clamp
    // against last frame's content height before drawing so an over-scrolled
    // offset is never rendered (avoids a one-frame flicker at the limits).
    const float viewTop = area.y + pad;
    const float viewH = area.h - pad - 4.0f;
    float maxScroll = std::max(0.0f, sourceInfoContentH_ - viewH);
    sourceInfoScroll_ = std::clamp(sourceInfoScroll_, 0.0f, maxScroll);
    float y = viewTop - sourceInfoScroll_;

    auto row = [&](const std::string& key, const std::string& val) {
        drawText(area.x + pad, y, { 130, 135, 148, 255 }, key);
        drawText(area.x + pad + keyColW, y, { 215, 218, 225, 255 }, val);
        y += lineH;
    };
    auto header = [&](const std::string& label) {
        drawText(area.x + pad, y, { 160, 170, 200, 255 }, label);
        y += lineH + 5.0f;
    };

    header("MEDIA");
    if (!inspectModTime_.empty())    row("Modified",  inspectModTime_);
    if (!inspectFileSize_.empty())   row("File Size", inspectFileSize_);
    if (!inspectFrameRange_.empty()) row("Range",     inspectFrameRange_);
    if (mptr) {
        y += 6.0f;
        fs::path mp(mptr->path());
        std::string nm = hashSeqStem(mp.stem().string(),
                                     mptr->type() == ClipType::ImageSequence) +
                         mp.extension().string();
        row("Filename", nm);
        row("Directory", mp.parent_path().string());
        row("Type", mptr->type() == ClipType::ImageSequence ? ImageSeq::typeLabel(mptr->path())
                    : mptr->type() == ClipType::Audio       ? "Audio" : "Video");
        MediaInfo info = mptr->info();
        if (info.width > 0)
            row("Resolution", std::to_string(info.width) + " x " + std::to_string(info.height));
        if (info.frameCount > 0)
            row("Frames", std::to_string(info.frameCount));
        if (info.fps > 0.0) {
            char buf[32];
            SDL_snprintf(buf, sizeof(buf), "%.3f", info.fps);
            row("FPS", buf);
        }

        std::string err;
        if (auto src = mptr->ensureOpen(err)) {
            y += 6.0f;
            header("FORMAT");
            for (const auto& f : src->describe())
                row(f.key, f.value);
        }
    } else {
        row("Name", fs::path(inspectMediaPath_).filename().string());
    }

    SDL_SetRenderClipRect(renderer_, nullptr);

    // Record content height for next frame's clamp.
    sourceInfoContentH_ = y + sourceInfoScroll_ - viewTop;

    // Scroll indicator, using the height just measured rather than last frame's.
    drawScrollbar(renderer_, { area.x, viewTop, area.w, viewH }, sourceInfoContentH_,
                  sourceInfoScroll_, dpiScale, true);

    // Close (X) button, upper-right, centered on the MEDIA header row. Drawn
    // unclipped so it stays fixed while the list scrolls beneath it. Clicks are
    // handled in the event loop (see the sourceInfoCloseRect_ hit-test in App.cpp).
    sourceInfoCloseRect_ = { area.x + area.w - pad - closeBtn,
                             viewTop + (lineH - closeBtn) * 0.5f, closeBtn, closeBtn };
    {
        float mx = 0.0f, my = 0.0f;
        uiMouse(mx, my);
        bool hov = inRect(sourceInfoCloseRect_, mx, my);
        icons_.drawGlyph(renderer_, 0xF0156, sourceInfoCloseRect_, // ICON_MDI_CLOSE
                         hov ? SDL_Color{ 235, 238, 245, 255 } : SDL_Color{ 140, 145, 156, 255 });
    }
}

// Media currently shown in the source-info sub-panel (selected in the media bin),
// or null when nothing is selected / the path no longer resolves.
Media* App::inspectedMedia() const {
    if (inspectMediaPath_.empty())
        return nullptr;
    for (const auto& kv : timeline_.media)
        if (kv.second && kv.second->path() == inspectMediaPath_)
            return kv.second.get();
    return nullptr;
}

