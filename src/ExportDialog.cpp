#include "ExportDialog.h"
#include "AppInternal.h"
#include "Layout.h"
#include "SkinColors.h"
#include "VideoSource.h"
#include "ImageSeq.h"

#include <unordered_map>

#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <Imath/half.h>

#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace jplay;

namespace {

// ---- export-dialog palette
// Static colors used by render(). The status text color is chosen per
// error/success state but each variant is a fixed constant; the progress-bar
// fill width is computed but its color is constant.
constexpr SDL_Color kOverlay   {   0,   0,   0, 160 }; // dim backdrop behind dialog
constexpr SDL_Color kDialogBg  {  30,  31,  37, 255 }; // dialog background
constexpr SDL_Color kBorder    {  80,  82,  90, 255 }; // dialog/progress border + greyed hint text
constexpr SDL_Color kTitleBg   {  38,  39,  47, 255 }; // title bar fill
constexpr SDL_Color kSeparator {  70,  72,  80, 255 }; // title separator line
constexpr SDL_Color kTitleText { 225, 228, 235, 255 }; // dialog title text
constexpr SDL_Color kLabel     { 150, 155, 165, 255 }; // row label text
constexpr SDL_Color kTrackBg   {  42,  43,  50, 255 }; // progress bar track
constexpr SDL_Color kProgress  {  90, 130, 200, 255 }; // progress bar fill
constexpr SDL_Color kError     { 220,  80,  80, 255 }; // error status text
constexpr SDL_Color kSuccess   { 150, 200, 150, 255 }; // success status text

inline void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

} // namespace

// ─── Platform pipe helpers ───────────────────────────────────────────────────
#ifdef _WIN32
#  define PPOPEN  _popen
#  define PPCLOSE _pclose
#else
#  define PPOPEN  popen
#  define PPCLOSE pclose
#endif

static std::string runAndCapture(const std::string& cmd) {
    FILE* pipe = PPOPEN(cmd.c_str(), "r");
    if (!pipe) return {};
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    PPCLOSE(pipe);
    return out;
}

// ─── FFmpeg discovery ────────────────────────────────────────────────────────

std::string ExportDialog::ffmpegPath() {
#ifdef _WIN32
    return "ffmpeg";   // cmd.exe finds ffmpeg.exe on PATH automatically
#else
    return "ffmpeg";
#endif
}

bool ExportDialog::ffmpegAvailable() {
    std::string out = runAndCapture(ffmpegPath() + " -version 2>&1");
    return out.find("ffmpeg version") != std::string::npos;
}

std::vector<ExportDialog::CodecInfo> ExportDialog::queryVideoEncoders() {
    // Curated list: (ffmpeg encoder id, human label).
    // We check which ones are available in the current ffmpeg build.
    static const std::array<std::pair<const char*, const char*>, 8> kWanted = {{
        {"libx264",   "H.264 (libx264)"},
        {"libx265",   "H.265/HEVC (libx265)"},
        {"prores_ks", "ProRes (prores_ks)"},
        {"prores",    "ProRes"},
        {"dnxhd",     "DNxHD"},
        {"libvpx-vp9","VP9"},
        {"mjpeg",     "MJPEG"},
        {"mpeg4",     "MPEG-4"},
    }};

    std::string out = runAndCapture(ffmpegPath() + " -encoders -v quiet 2>&1");
    std::vector<CodecInfo> result;
    for (const auto& [id, label] : kWanted) {
        if (out.find(id) != std::string::npos)
            result.push_back({ id, label });
    }
    return result;
}

std::vector<ExportDialog::FormatInfo> ExportDialog::queryContainerFormats() {
    // Curated container list; check muxer availability.
    static const std::array<std::pair<const char*, const char*>, 5> kWanted = {{
        {"mp4",      "MP4"},
        {"mov",      "QuickTime (MOV)"},
        {"matroska", "Matroska (MKV)"},
        {"avi",      "AVI"},
        {"mxf",      "MXF"},
    }};

    std::string out = runAndCapture(ffmpegPath() + " -formats -v quiet 2>&1");
    std::vector<FormatInfo> result;
    for (const auto& [id, label] : kWanted) {
        if (out.find(id) != std::string::npos)
            result.push_back({ id, label });
    }
    if (result.empty()) {
        // ffmpeg not found or muxers couldn't be queried; add basic fallbacks.
        result.push_back({ "mp4", "MP4" });
        result.push_back({ "mov", "QuickTime (MOV)" });
    }
    return result;
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

ExportDialog::~ExportDialog() {
    cancelExport_ = true;
    if (exportThread_.joinable()) exportThread_.join();
}

void ExportDialog::open(Mode mode, int64_t timelineLen,
                        int64_t inPoint, int64_t outPoint,
                        double fps, int width, int height,
                        SDL_Window* win) {
    if (exporting_) return; // don't reopen while running

    // Cancel any previous export thread.
    cancelExport_ = true;
    if (exportThread_.joinable()) exportThread_.join();
    cancelExport_ = false;
    exporting_ = false;
    exportDone_ = false;
    exportError_ = false;
    exportProgress_ = 0;
    setStatusMsg("");

    mode_        = mode;
    timelineLen_ = timelineLen;
    fps_         = fps;
    srcW_        = width;
    srcH_        = height;
    open_        = true;

    // Pre-fill range.
    bool hasRange = (outPoint >= 0);
    int64_t rangeStart = inPoint;
    int64_t rangeEnd   = (outPoint >= 0) ? outPoint : std::max<int64_t>(timelineLen - 1, 0);

    rangeGroup_.setOptions({ "Full Range", "Custom Range" });
    rangeGroup_.setSelected(hasRange ? 1 : 0);

    char buf[32];
    SDL_snprintf(buf, sizeof(buf), "%lld", (long long)rangeStart);
    startInput_.setText(buf);
    SDL_snprintf(buf, sizeof(buf), "%lld", (long long)rangeEnd);
    endInput_.setText(buf);

    pathInput_.setText("");
    pathInput_.setFocus(false);
    startInput_.setFocus(false);
    endInput_.setFocus(false);

    if (mode == Mode::Movie) {
        // Populate container / codec / quality combos.
        auto formats = queryContainerFormats();
        std::vector<std::string> fLabels;
        for (auto& f : formats) fLabels.push_back(f.label);
        containerCombo_.setOptions(fLabels, 0);

        auto codecs = queryVideoEncoders();
        std::vector<std::string> cLabels;
        for (auto& c : codecs) cLabels.push_back(c.label);
        if (cLabels.empty()) cLabels.push_back("(none found)");
        codecCombo_.setOptions(cLabels, 0);

        qualityCombo_.setOptions({ "High (CRF 18)", "Medium (CRF 23)", "Low (CRF 28)" }, 1);
    } else {
        formatCombo_.setOptions({ "EXR (Half Float)", "PNG" }, 0);
    }

    SDL_StartTextInput(win);
}

void ExportDialog::close(SDL_Window* win) {
    if (!open_) return;
    if (exporting_) { cancelExport_ = true; return; } // let the thread finish first
    open_ = false;
    pathInput_.setFocus(false);
    startInput_.setFocus(false);
    endInput_.setFocus(false);
    SDL_StopTextInput(win);
}

// ─── Status helpers ──────────────────────────────────────────────────────────

void ExportDialog::setStatusMsg(const std::string& s) {
    std::lock_guard<std::mutex> lk(statusMutex_);
    exportStatusMsg_ = s;
}
std::string ExportDialog::getStatusMsg() const {
    std::lock_guard<std::mutex> lk(statusMutex_);
    return exportStatusMsg_;
}

// ─── Browse dialog callback ──────────────────────────────────────────────────

void SDLCALL ExportDialog::onBrowseChosen(void* userdata, const char* const* filelist, int) {
    auto* self = static_cast<ExportDialog*>(userdata);
    if (!filelist || !filelist[0]) return;
    std::lock_guard<std::mutex> lk(self->browseMutex_);
    self->pendingBrowsePath_ = filelist[0];
}

// ─── Event handling ──────────────────────────────────────────────────────────

bool ExportDialog::handleEvent(const SDL_Event& e, SDL_Window* win) {
    if (!open_) return false;

    // Consume all events while open so nothing underneath fires.
    // Drain pending browse path.
    {
        std::lock_guard<std::mutex> lk(browseMutex_);
        if (!pendingBrowsePath_.empty()) {
            pathInput_.setText(pendingBrowsePath_);
            pendingBrowsePath_.clear();
        }
    }

    // If export just finished, allow re-click on Cancel to close.
    if (exportDone_ && !exporting_) {
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (cancelBtn_.handleEvent(e)) { close(win); return true; }
        }
        return true; // block everything else
    }

    if (exporting_) {
        // Only cancel button is active during export.
        if (cancelBtn_.handleEvent(e)) {
            cancelExport_ = true;
            setStatusMsg("Cancelling...");
        }
        return true;
    }

    // Normal (non-exporting) interaction.
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        close(win); return true;
    }

    // Focus management for text inputs on click.
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        float mx = e.button.x, my = e.button.y;
        bool customRange = (rangeGroup_.selected() == 1);
        bool pathHit  = inRect(pathInput_.rect(),  mx, my);
        bool startHit = customRange && inRect(startInput_.rect(), mx, my);
        bool endHit   = customRange && inRect(endInput_.rect(),   mx, my);
        pathInput_.setFocus(pathHit);
        startInput_.setFocus(startHit);
        endInput_.setFocus(endHit);
    }

    bool consumed = false;
    consumed |= pathInput_.handleEvent(e);
    if (rangeGroup_.selected() == 1) {
        consumed |= startInput_.handleEvent(e);
        consumed |= endInput_.handleEvent(e);
    }
    consumed |= rangeGroup_.handleEvent(e);

    if (browseBtn_.handleEvent(e)) {
        if (mode_ == Mode::Movie) {
            static const SDL_DialogFileFilter kFilters[] = {
                { "Video files", "mp4;mov;mkv;avi;mxf" },
                { "All files",   "*" },
            };
            SDL_ShowSaveFileDialog(&ExportDialog::onBrowseChosen, this, win,
                                   kFilters, SDL_arraysize(kFilters), nullptr);
        } else {
            SDL_ShowOpenFolderDialog(&ExportDialog::onBrowseChosen, this, win, nullptr, false);
        }
        return true;
    }

    if (mode_ == Mode::Movie) {
        consumed |= containerCombo_.handleEvent(e);
        consumed |= codecCombo_.handleEvent(e);
        consumed |= qualityCombo_.handleEvent(e);
    } else {
        consumed |= formatCombo_.handleEvent(e);
    }

    if (exportBtn_.handleEvent(e)) {
        startExport();
        return true;
    }
    if (cancelBtn_.handleEvent(e)) {
        close(win);
        return true;
    }

    return true; // always consume while open
}

// ─── Layout ──────────────────────────────────────────────────────────────────

float ExportDialog::layoutRows(SDL_Renderer* r, TextFont* font, SDL_FRect body) {
    const float scale = dpiScale;
    const float rowH = 22.f * scale;
    const float gap  =  8.f * scale;
    const float top  = body.y;

    // Every row reserves the same left gutter for its label; the widest label
    // ("Output Directory:") sets it, so the fields line up down the dialog.
    const float labelW  = font->measure(r, "Output Directory:") + 6.f * scale;
    const float browseW = font->measure(r, "Browse") + 16.f * scale;
    const float comboW  = 200.f * scale;

    // Path row: the field spans from the gutter to the Browse button on the right.
    SDL_FRect row = cutTop(body, rowH);
    gapLeft(row, labelW);
    browseBtn_.setRect(cutRight(row, browseW));
    browseBtn_.setLabel("Browse");
    gapRight(row, gap);
    pathInput_.setRect(row);
    gapTop(body, gap * 1.5f);

    // Range group
    row = cutTop(body, rowH);
    gapLeft(row, labelW);
    rangeGroup_.layoutHorizontal(row.x, row.y, rowH, 20.f * scale, font, r);
    gapTop(body, gap);

    // Start / End inputs (same row, visible when Custom Range selected). Each
    // input is preceded by the gap its own label is drawn into.
    const float numW = 70.f * scale;
    row = cutTop(body, rowH);
    gapLeft(row, labelW + font->measure(r, "Start:") + 4.f * scale);
    startInput_.setRect(cutLeft(row, numW));
    gapLeft(row, gap + font->measure(r, "End:") + 4.f * scale + gap);
    endInput_.setRect(cutLeft(row, numW));
    gapTop(body, gap * 1.5f);

    // Format-specific section
    auto comboRow = [&](Combobox& c, float w) {
        SDL_FRect cr = cutTop(body, rowH);
        gapLeft(cr, labelW);
        c.setRect(cutLeft(cr, w));
    };
    if (mode_ == Mode::Movie) {
        comboRow(containerCombo_, comboW); gapTop(body, gap);
        comboRow(codecCombo_, comboW);     gapTop(body, gap);
        comboRow(qualityCombo_, comboW);   gapTop(body, gap * 1.5f);
    } else {
        comboRow(formatCombo_, comboW * .6f);
        gapTop(body, gap * 1.5f);
    }

    // Progress bar, full content width, with the status text on the line below.
    if (exporting_ || exportDone_) {
        progressBarOuter_ = cutTop(body, rowH * .5f);
        gapTop(body, gap);
        gapTop(body, rowH + gap); // status text line
    } else {
        progressBarOuter_ = {};
    }

    // Bottom buttons, right-aligned. This is the last row: the dialog ends one
    // bottom pad below it.
    const float btnW = 80.f * scale;
    row = cutTop(body, rowH);
    exportBtn_.setRect(cutRight(row, btnW));
    exportBtn_.setLabel("Export");
    exportBtn_.setEnabled(!exporting_ && !exportDone_);
    gapRight(row, gap);
    cancelBtn_.setRect(cutRight(row, btnW));
    cancelBtn_.setLabel(exporting_ ? "Cancel" : "Close");

    return body.y - top;
}

// ─── Render ──────────────────────────────────────────────────────────────────

void ExportDialog::render(SDL_Renderer* r, TextFont* font, float winW, float winH) {
    if (!open_ || !font) return;

    const float scale = dpiScale;
    const float dw = 460.f * scale;
    const float padX = 16.f * scale;
    const float padY = 14.f * scale;
    const float rowH = 22.f * scale;
    const float gap  =  8.f * scale;
    const float titleH = rowH + padY;

    // Measure the row stack into a throwaway unbounded rect (cuts clamp, so a
    // bounded one would collapse anything past its bottom edge), then centre a
    // dialog of that height and lay the rows out again into its real content
    // rect. Same code both times, so the box always fits exactly what it holds.
    SDL_FRect probe = { 0.f, 0.f, dw - 2.f * padX, kUnbounded };
    const float dh = titleH + padY + layoutRows(r, font, probe) + padY;
    SDL_FRect full = { 0.f, 0.f, winW, winH };
    dialogRect_ = center(full, dw, dh);
    dialogRect_.x = std::round(dialogRect_.x);
    dialogRect_.y = std::round(dialogRect_.y);

    // Dim overlay.
    setColor(r, kOverlay);
    jplay::fillRect(r, &full);

    // Dialog background.
    setColor(r, kDialogBg);
    jplay::fillRect(r, &dialogRect_);
    setColor(r, kBorder);
    jplay::drawRect(r, &dialogRect_);

    // Split the dialog into its title bar and the content rect the rows go into.
    SDL_FRect body = dialogRect_;
    SDL_FRect titleBar = cutTop(body, titleH);
    body = inset(body, padX, padY);

    setColor(r, kTitleBg);
    jplay::fillRect(r, &titleBar);
    // Title separator line.
    setColor(r, kSeparator);
    jplay::drawLine(r, titleBar.x, titleBar.y + titleBar.h,
                      titleBar.x + titleBar.w, titleBar.y + titleBar.h);

    float glyphH = font->lineHeight();
    {
        const char* title = (mode_ == Mode::Movie) ? "Export Movie"
                                                   : "Export Image Sequence";
        SDL_FRect slot = centerV(titleBar, glyphH);
        font->draw(r, slot.x + padX, slot.y, kTitleText, title);
    }

    layoutRows(r, font, body);

    // ── Helper: draw row label ────────────────────────────────────────────
    auto drawLabel = [&](const SDL_FRect& fieldRect, const char* label) {
        float tw = font->measure(r, label);
        float ty = fieldRect.y + (fieldRect.h - glyphH) * .5f;
        font->draw(r, fieldRect.x - tw - 6.f * scale, ty, kLabel, label);
    };

    // Path
    const char* pathLabel = (mode_ == Mode::Movie) ? "Output File:" : "Output Dir:";
    drawLabel(pathInput_.rect(), pathLabel);
    pathInput_.render(r, font);
    browseBtn_.render(r, font);

    // Range
    {
        SDL_FRect r0 = rangeGroup_.optionRect(0);
        float ty = r0.h > 0.f ? r0.y + (r0.h - glyphH) * .5f : 0.f;
        float lx = dialogRect_.x + padX;
        font->draw(r, lx, ty, kLabel, "Range:");
    }
    rangeGroup_.render(r, font);

    // Start / End (only when Custom Range selected).
    if (rangeGroup_.selected() == 1) {
        drawLabel(startInput_.rect(), "Start:");
        startInput_.render(r, font);
        drawLabel(endInput_.rect(), "End:");
        endInput_.render(r, font);
    } else {
        // Show greyed-out hint.
        SDL_Color dim = kBorder;
        float ty = startInput_.rect().y + (startInput_.rect().h - glyphH) * .5f;
        font->draw(r, startInput_.rect().x - font->measure(r, "Start:") - 6.f * scale,
                   ty, dim, "Start:");
        font->draw(r, startInput_.rect().x + startInput_.rect().w + gap
                      + font->measure(r, "End:") * .2f, ty, dim, "End:");
    }

    // Format section.
    if (mode_ == Mode::Movie) {
        drawLabel(containerCombo_.rect(), "Container:");
        drawLabel(codecCombo_.rect(),     "Codec:");
        drawLabel(qualityCombo_.rect(),   "Quality:");
        containerCombo_.render(r, font);
        codecCombo_.render(r, font);
        qualityCombo_.render(r, font);
    } else {
        drawLabel(formatCombo_.rect(), "Format:");
        formatCombo_.render(r, font);
    }

    // Progress bar.
    if (exporting_ || exportDone_) {
        int prog = exportProgress_.load();
        float barW = progressBarOuter_.w;
        float fillW = barW * (prog / 100.f);

        setColor(r, kTrackBg);
        jplay::fillRect(r, &progressBarOuter_);
        setColor(r, kProgress);
        SDL_FRect fill{ progressBarOuter_.x, progressBarOuter_.y, fillW, progressBarOuter_.h };
        jplay::fillRect(r, &fill);
        setColor(r, kBorder);
        jplay::drawRect(r, &progressBarOuter_);

        std::string status = getStatusMsg();
        float ty = progressBarOuter_.y + progressBarOuter_.h + gap;
        SDL_Color sc = exportError_ ? kError : kSuccess;
        font->draw(r, progressBarOuter_.x, ty, sc, status.c_str());
    }

    // Buttons.
    cancelBtn_.render(r, font);
    if (!exportDone_) exportBtn_.render(r, font);

    // Open dropdown lists rendered last so they overlay all other widgets.
    if (mode_ == Mode::Movie) {
        containerCombo_.renderDropdown(r, font);
        codecCombo_.renderDropdown(r, font);
        qualityCombo_.renderDropdown(r, font);
    } else {
        formatCombo_.renderDropdown(r, font);
    }
}

// ─── Export range helpers ─────────────────────────────────────────────────────

void ExportDialog::exportRange(int64_t& outStart, int64_t& outEnd) const {
    if (rangeGroup_.selected() == 0) {
        outStart = 0;
        outEnd   = std::max<int64_t>(timelineLen_ - 1, 0);
    } else {
        try { outStart = std::stoll(startInput_.text()); } catch (...) { outStart = 0; }
        try { outEnd   = std::stoll(endInput_.text()); }   catch (...) { outEnd   = std::max<int64_t>(timelineLen_ - 1, 0); }
        outStart = std::clamp(outStart, (int64_t)0, timelineLen_ - 1);
        outEnd   = std::clamp(outEnd,   outStart,   timelineLen_ - 1);
    }
}

// ─── Export launch ───────────────────────────────────────────────────────────

void ExportDialog::startExport() {
    if (exporting_) return;
    if (pathInput_.text().empty()) {
        setStatusMsg("Please enter an output path.");
        exportDone_ = true;
        exportError_ = true;
        return;
    }
    if (!ffmpegAvailable() && mode_ == Mode::Movie) {
        setStatusMsg("ffmpeg not found on PATH.");
        exportDone_ = true;
        exportError_ = true;
        return;
    }
    if (!ffmpegAvailable() && mode_ == Mode::ImageSequence
        && formatCombo_.selected() == 1 /* PNG */) {
        setStatusMsg("ffmpeg not found on PATH (required for PNG export).");
        exportDone_ = true;
        exportError_ = true;
        return;
    }

    int64_t startFrame, endFrame;
    exportRange(startFrame, endFrame);

    exporting_      = true;
    exportDone_     = false;
    exportError_    = false;
    exportProgress_ = 0;
    cancelExport_   = false;
    setStatusMsg("Starting...");

    exportThread_ = std::thread([this, startFrame, endFrame] {
        if (mode_ == Mode::Movie)
            runMovieExport(startFrame, endFrame);
        else
            runImageExport(startFrame, endFrame);
        exporting_ = false;
    });
}

// ─── Frame decoding helper ───────────────────────────────────────────────────

// Open-source cache keyed by file path; keeps sources alive across frames.
using SourceCache = std::unordered_map<std::string, std::shared_ptr<MediaSource>>;

static FramePtr decodeFrame(const ExportDialog::FrameSource& fs, SourceCache& cache) {
    if (fs.path.empty()) return nullptr;
    auto it = cache.find(fs.path);
    if (it == cache.end()) {
        std::string err;
        std::shared_ptr<MediaSource> src;
        if (fs.type == ClipType::ImageSequence)
            src = ImageSeq::open(fs.path, err);
        else
            src = VideoSource::open(fs.path, err);
        if (!src) return nullptr;
        cache[fs.path] = src;
        it = cache.find(fs.path);
    }
    return it->second->readFrame(fs.sourceFrame);
}

const uint8_t* ExportDialog::displayPixels_(const FrameSource& fs, const Frame& f,
                                            std::vector<uint8_t>& buf) const {
    auto it = colorXf_.find(fs.path);
    if (it == colorXf_.end() || !it->second.valid())
        return f.rgba.data();
    buf.resize((size_t)f.width * f.height * 4);
    it->second.apply(f, buf.data());
    return buf.data();
}

// ─── Movie export ─────────────────────────────────────────────────────────────

void ExportDialog::runMovieExport(int64_t startFrame, int64_t endFrame) {
    // Resolve codec id from label index.
    auto codecs  = queryVideoEncoders();
    auto formats = queryContainerFormats();

    std::string codecId  = codecs.empty()  ? "libx264" : codecs[codecCombo_.selected()].id;
    std::string fmtId    = formats.empty() ? "mp4"     : formats[containerCombo_.selected()].id;
    std::string ext      = (fmtId == "matroska") ? "mkv" : fmtId;

    // Map quality combo index → CRF (or ProRes profile).
    static const int kCRFs[3] = { 18, 23, 28 };
    int qualIdx = qualityCombo_.selected();
    int crf = kCRFs[std::clamp(qualIdx, 0, 2)];

    std::string outPath = pathInput_.text();
    // Ensure correct extension.
    fs::path p(outPath);
    if (p.extension().string().empty())
        outPath += "." + ext;

    SourceCache srcCache;

    // Determine output dimensions from the first available frame.
    int outW = srcW_, outH = srcH_;
    if (outW <= 0 || outH <= 0) {
        for (int64_t f = startFrame; f <= endFrame; ++f) {
            if (f < (int64_t)frames_.size()) {
                if (auto fp = decodeFrame(frames_[f], srcCache)) {
                    outW = fp->width; outH = fp->height; break;
                }
            }
        }
    }
    if (outW <= 0 || outH <= 0) {
        setStatusMsg("No renderable frames in range.");
        exportError_ = true; return;
    }

    // Build ffmpeg command reading raw RGBA from stdin.
    std::ostringstream cmd;
    cmd << ffmpegPath()
        << " -y -f rawvideo -pix_fmt rgba"
        << " -video_size " << outW << "x" << outH
        << " -framerate " << fps_
        << " -i pipe:0";

    if (codecId == "prores_ks" || codecId == "prores") {
        // ProRes uses -profile:v instead of -crf (quality → proxy/lt/std/hq)
        static const int kProResProfiles[3] = { 0, 2, 3 }; // proxy, standard, HQ
        cmd << " -c:v " << codecId
            << " -profile:v " << kProResProfiles[std::clamp(qualIdx, 0, 2)];
    } else {
        cmd << " -c:v " << codecId << " -crf " << crf;
    }
    // Ensure YUV420 compatibility for most players (h264/h265).
    if (codecId == "libx264" || codecId == "libx265" || codecId == "mpeg4")
        cmd << " -vf format=yuv420p";

    cmd << " \"" << outPath << "\"";

#ifdef _WIN32
    // Silence ffmpeg's own output on Windows.
    cmd << " 2>nul";
#else
    cmd << " 2>/dev/null";
#endif

    FILE* pipe = PPOPEN(cmd.str().c_str(), "wb");
    if (!pipe) {
        setStatusMsg("Failed to launch ffmpeg. Is it on PATH?");
        exportError_ = true; return;
    }

    int64_t total = endFrame - startFrame + 1;
    std::vector<uint8_t> xfBuf; // reused across frames when a transform runs
    for (int64_t i = 0; i < total; ++i) {
        if (cancelExport_) break;
        int64_t tlFrame = startFrame + i;
        FramePtr frame;
        if (tlFrame < (int64_t)frames_.size())
            frame = decodeFrame(frames_[tlFrame], srcCache);

        if (frame && frame->width == outW && frame->height == outH) {
            const uint8_t* px = displayPixels_(frames_[tlFrame], *frame, xfBuf);
            fwrite(px, 1, (size_t)outW * outH * 4, pipe);
        } else {
            // Write black frame.
            std::vector<uint8_t> black(outW * outH * 4, 0);
            fwrite(black.data(), 1, black.size(), pipe);
        }

        int prog = (int)((i + 1) * 100 / total);
        exportProgress_ = prog;
        char buf[64];
        SDL_snprintf(buf, sizeof(buf), "Frame %lld / %lld", (long long)(i + 1), (long long)total);
        setStatusMsg(buf);
    }

    PPCLOSE(pipe);

    if (cancelExport_) {
        setStatusMsg("Cancelled.");
        exportError_ = true;
    } else {
        exportProgress_ = 100;
        setStatusMsg("Done! Saved to: " + fs::path(outPath).filename().string());
    }
    exportDone_ = true;
}

// ─── Image sequence export ───────────────────────────────────────────────────

static void writeEXR(const std::string& path, int w, int h, const uint8_t* rgba) {
    using namespace Imf;
    using namespace Imath;

    Header header(w, h);
    header.channels().insert("R", Channel(HALF));
    header.channels().insert("G", Channel(HALF));
    header.channels().insert("B", Channel(HALF));
    header.channels().insert("A", Channel(HALF));

    std::vector<half> rBuf(w * h), gBuf(w * h), bBuf(w * h), aBuf(w * h);
    for (int i = 0; i < w * h; ++i) {
        rBuf[i] = rgba[i * 4 + 0] / 255.0f;
        gBuf[i] = rgba[i * 4 + 1] / 255.0f;
        bBuf[i] = rgba[i * 4 + 2] / 255.0f;
        aBuf[i] = rgba[i * 4 + 3] / 255.0f;
    }

    FrameBuffer fb;
    size_t rowStride = sizeof(half) * w;
    fb.insert("R", Slice(HALF, (char*)rBuf.data(), sizeof(half), rowStride));
    fb.insert("G", Slice(HALF, (char*)gBuf.data(), sizeof(half), rowStride));
    fb.insert("B", Slice(HALF, (char*)bBuf.data(), sizeof(half), rowStride));
    fb.insert("A", Slice(HALF, (char*)aBuf.data(), sizeof(half), rowStride));

    OutputFile file(path.c_str(), header);
    file.setFrameBuffer(fb);
    file.writePixels(h);
}

// The linear working-space variant, for a source that went through a colour
// transform: RGB floats straight in, opaque alpha. The 8-bit writer above stays
// for sources with no transform, where the frame's own bytes are all there is.
static void writeEXRLinear(const std::string& path, int w, int h, const float* rgb) {
    using namespace Imf;
    using namespace Imath;

    Header header(w, h);
    header.channels().insert("R", Channel(HALF));
    header.channels().insert("G", Channel(HALF));
    header.channels().insert("B", Channel(HALF));
    header.channels().insert("A", Channel(HALF));

    std::vector<half> rBuf(w * h), gBuf(w * h), bBuf(w * h), aBuf(w * h);
    for (int i = 0; i < w * h; ++i) {
        rBuf[i] = rgb[i * 3 + 0];
        gBuf[i] = rgb[i * 3 + 1];
        bBuf[i] = rgb[i * 3 + 2];
        aBuf[i] = 1.0f;
    }

    FrameBuffer fb;
    size_t rowStride = sizeof(half) * w;
    fb.insert("R", Slice(HALF, (char*)rBuf.data(), sizeof(half), rowStride));
    fb.insert("G", Slice(HALF, (char*)gBuf.data(), sizeof(half), rowStride));
    fb.insert("B", Slice(HALF, (char*)bBuf.data(), sizeof(half), rowStride));
    fb.insert("A", Slice(HALF, (char*)aBuf.data(), sizeof(half), rowStride));

    OutputFile file(path.c_str(), header);
    file.setFrameBuffer(fb);
    file.writePixels(h);
}

void ExportDialog::runImageExport(int64_t startFrame, int64_t endFrame) {
    bool isPng  = (formatCombo_.selected() == 1);
    std::string dir = pathInput_.text();
    if (dir.empty()) { setStatusMsg("No output directory."); exportError_ = true; return; }

    // Ensure directory exists.
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { setStatusMsg("Cannot create directory: " + ec.message()); exportError_ = true; return; }

    // For PNG we pipe to ffmpeg (one invocation per frame to keep it simple).
    int64_t total = endFrame - startFrame + 1;

    SourceCache srcCache;

    // Determine output dimensions.
    int outW = srcW_, outH = srcH_;
    if (outW <= 0 || outH <= 0) {
        for (int64_t f = startFrame; f <= endFrame; ++f) {
            if (f < (int64_t)frames_.size()) {
                if (auto fp = decodeFrame(frames_[f], srcCache)) {
                    outW = fp->width; outH = fp->height; break;
                }
            }
        }
    }
    if (outW <= 0 || outH <= 0) {
        setStatusMsg("No renderable frames in range.");
        exportError_ = true; return;
    }

    std::vector<uint8_t> xfBuf;  // display-referred bytes (PNG)
    std::vector<float> linBuf;   // working-space floats (EXR)
    for (int64_t i = 0; i < total; ++i) {
        if (cancelExport_) break;
        int64_t tlFrame = startFrame + i;

        FramePtr frame;
        if (tlFrame < (int64_t)frames_.size())
            frame = decodeFrame(frames_[tlFrame], srcCache);
        // The transform for this frame's source, if it has one. Null past the end of
        // the snapshot, which is the same black-frame case as a missing decode.
        const OcioManager::CpuTransform* xf = nullptr;
        if (tlFrame < (int64_t)frames_.size()) {
            auto it = colorXf_.find(frames_[tlFrame].path);
            if (it != colorXf_.end() && it->second.valid())
                xf = &it->second;
        }

        // Build output filename: frame_000001.exr / .png
        char fname[64];
        SDL_snprintf(fname, sizeof(fname), "frame_%06lld.%s",
                     (long long)(startFrame + i), isPng ? "png" : "exr");
        std::string outPath = (fs::path(dir) / fname).string();

        if (!isPng) {
            // Write EXR directly.
            try {
                if (frame && xf) {
                    linBuf.resize((size_t)frame->width * frame->height * 3);
                    xf->applyToWorking(*frame, linBuf.data());
                    writeEXRLinear(outPath, frame->width, frame->height, linBuf.data());
                } else if (frame)
                    writeEXR(outPath, frame->width, frame->height, frame->rgba.data());
                else {
                    // Black frame.
                    std::vector<uint8_t> black(outW * outH * 4, 0);
                    writeEXR(outPath, outW, outH, black.data());
                }
            } catch (const std::exception& ex) {
                setStatusMsg(std::string("EXR write failed: ") + ex.what());
                exportError_ = true; return;
            }
        } else {
            // PNG via ffmpeg single-frame pipe.
            const uint8_t* pixels = nullptr;
            std::vector<uint8_t> black;
            if (frame) {
                pixels = displayPixels_(frames_[tlFrame], *frame, xfBuf);
            } else {
                black.assign(outW * outH * 4, 0);
                pixels = black.data();
            }
            int fw = frame ? frame->width  : outW;
            int fh = frame ? frame->height : outH;

            std::ostringstream cmd;
            cmd << ffmpegPath()
                << " -y -f rawvideo -pix_fmt rgba"
                << " -video_size " << fw << "x" << fh
                << " -framerate 1 -i pipe:0"
                << " -frames:v 1 \"" << outPath << "\""
#ifdef _WIN32
                << " 2>nul";
#else
                << " 2>/dev/null";
#endif
            FILE* pipe = PPOPEN(cmd.str().c_str(), "wb");
            if (pipe) {
                fwrite(pixels, 1, (size_t)fw * fh * 4, pipe);
                PPCLOSE(pipe);
            } else {
                setStatusMsg("Failed to launch ffmpeg for PNG export.");
                exportError_ = true; return;
            }
        }

        exportProgress_ = (int)((i + 1) * 100 / total);
        char buf[64];
        SDL_snprintf(buf, sizeof(buf), "Frame %lld / %lld", (long long)(i + 1), (long long)total);
        setStatusMsg(buf);
    }

    if (cancelExport_) {
        setStatusMsg("Cancelled.");
        exportError_ = true;
    } else {
        exportProgress_ = 100;
        setStatusMsg("Done! " + std::to_string(total) + " frames written.");
    }
    exportDone_ = true;
}
