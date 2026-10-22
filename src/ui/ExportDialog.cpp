// The dialog half of ExportDialog: lifecycle, events, layout and drawing, and
// the success view. The encoders it launches live in ExportDialog_Encode.cpp.

#include "ExportDialog.h"
#include "ExportFormats.h"
#include "AppInternal.h"
#include "Layout.h"
#include "SkinColors.h"
#include "RevealFile.h"

#include <SDL3/SDL_dialog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>

namespace fs = std::filesystem;
using namespace jplay;
using namespace ExportFormats;

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
    setExportedPath("", "");
    showSuccess_ = false;
    successNote_.clear();

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
        movieAudioCombo_.setOptions({ "Include", "None" }, 0);
    } else if (mode == Mode::Audio) {
        // Offer the formats this ffmpeg build can encode. The listing pads each
        // encoder name with spaces, which keeps "aac" from matching "libfdk_aac".
        const std::string& enc = ffmpegInfo(ffmpegCmd()).encoders;
        const auto& table = audioFormatTable();
        audioFormats_.clear();
        std::vector<std::string> labels;
        for (size_t i = 0; i < table.size(); ++i) {
            if (enc.find(std::string(" ") + table[i].encoder + " ") == std::string::npos)
                continue;
            audioFormats_.push_back((int)i);
            labels.push_back(table[i].label);
        }
        if (audioFormats_.empty()) {
            // No ffmpeg (or no listing): show WAV so the dialog is not empty;
            // Export then reports the missing ffmpeg.
            audioFormats_.push_back(0);
            labels.push_back(table[0].label);
        }
        audioFormatCombo_.setOptions(labels, 0);
        shownAudioFormat_ = -1;
        refreshAudioOptions();
    } else if (mode == Mode::Gif) {
        // Size: the source's own, then each preset width below it, labelled with
        // the frame size it comes to. Default to the largest that fits under
        // kGifDefaultMaxWidth. An unknown source size offers only "Source"; the
        // export then takes the size of the first frame it decodes.
        gifWidths_ = { 0 };
        std::vector<std::string> sizes;
        char buf[64];
        if (srcW_ > 0 && srcH_ > 0)
            SDL_snprintf(buf, sizeof(buf), "Source (%dx%d)", srcW_, srcH_);
        else
            SDL_snprintf(buf, sizeof(buf), "Source");
        sizes.push_back(buf);
        int sizeSel = 0;
        for (int w : kGifWidths) {
            if (srcW_ <= 0 || w >= srcW_)
                continue;
            if (sizeSel == 0 && w <= kGifDefaultMaxWidth && srcW_ > kGifDefaultMaxWidth)
                sizeSel = (int)gifWidths_.size();
            gifWidths_.push_back(w);
            SDL_snprintf(buf, sizeof(buf), "%dx%d", w,
                         std::max(1, (int)std::lround((double)srcH_ * w / srcW_)));
            sizes.push_back(buf);
        }
        gifSizeCombo_.setOptions(sizes, sizeSel);

        // Rate: the timeline's own, then each preset below it. Frames are dropped
        // (ffmpeg's fps filter), never blended.
        gifRates_ = { 0.0 };
        SDL_snprintf(buf, sizeof(buf), "Timeline (%.3g fps)", fps_);
        std::vector<std::string> rates = { buf };
        for (double r : kGifRates) {
            if (r >= fps_ - 0.01)
                continue;
            gifRates_.push_back(r);
            SDL_snprintf(buf, sizeof(buf), "%.3g fps", r);
            rates.push_back(buf);
        }
        gifFpsCombo_.setOptions(rates, 0);

        std::vector<std::string> qualities;
        for (const GifQualityDef& q : kGifQualities)
            qualities.push_back(q.label);
        gifQualityCombo_.setOptions(qualities, kGifDefaultQuality);
        gifLoopCombo_.setOptions({ "Forever", "Play Once" }, 0);
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

void ExportDialog::setAudioSegments(std::vector<AudioMixdown::Segment> segs) {
    audioSegs_ = std::move(segs);
    if (mode_ == Mode::Movie)
        movieAudioCombo_.setOptions({ "Include", "None" }, audioSegs_.empty() ? 1 : 0);
}

std::vector<Combobox*> ExportDialog::modeCombos() {
    switch (mode_) {
    case Mode::Movie:
        return { &containerCombo_, &codecCombo_, &qualityCombo_, &movieAudioCombo_ };
    case Mode::Audio:
        return { &audioFormatCombo_, &audioVariantCombo_, &sampleRateCombo_ };
    case Mode::Gif:
        return { &gifSizeCombo_, &gifFpsCombo_, &gifQualityCombo_, &gifLoopCombo_ };
    default:
        return { &formatCombo_ };
    }
}

// Rebuild the bit depth / bitrate and sample rate choices for the selected
// format, once per format change. The sample rate carries over when the new
// format offers it, so flipping formats does not quietly reset it.
void ExportDialog::refreshAudioOptions() {
    if (mode_ != Mode::Audio)
        return;
    const int sel = audioFormatCombo_.selected();
    if (sel == shownAudioFormat_)
        return;
    shownAudioFormat_ = sel;
    if (sel < 0 || sel >= (int)audioFormats_.size()) {
        audioVariantCombo_.setOptions({});
        return;
    }
    const AudioFormatDef& def = audioFormatTable()[audioFormats_[sel]];
    std::vector<std::string> variants;
    for (const AudioVariant& v : def.variants)
        variants.push_back(v.label);
    audioVariantCombo_.setOptions(variants, def.defaultVariant);

    const std::string prevRate = sampleRateCombo_.empty() ? std::string() : sampleRateCombo_.value();
    std::vector<std::string> rates;
    int rateSel = -1;
    for (const SampleRateDef& sr : kSampleRates) {
        if (def.only48k && sr.hz != 48000)
            continue;
        if (prevRate == sr.label)
            rateSel = (int)rates.size();
        rates.push_back(sr.label);
    }
    if (rateSel < 0) // first fill, or the old rate is not on offer: 48 kHz
        rateSel = def.only48k ? 0 : 1;
    sampleRateCombo_.setOptions(rates, rateSel);
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

void ExportDialog::setExportedPath(const std::string& path, const std::string& revealPath) {
    std::lock_guard<std::mutex> lk(statusMutex_);
    exportedPath_       = path;
    exportedRevealPath_ = revealPath;
}

void ExportDialog::enterSuccessViewIfDone() {
    if (showSuccess_ || exporting_ || !exportDone_ || exportError_) return;
    if (exportThread_.joinable()) exportThread_.join(); // finished; just reap it
    {
        std::lock_guard<std::mutex> lk(statusMutex_);
        successPath_ = exportedPath_;
        revealPath_  = exportedRevealPath_;
    }
    // A pre-flight error sets exportDone_ with exportError_, so reaching here
    // without a path would mean a worker forgot to record one: keep the form.
    if (successPath_.empty()) return;
    showSuccess_ = true;
    successNote_.clear();

    // Summarise what was written. The form is locked while the export runs, so
    // the combos and range fields still hold the settings it used.
    successDetails_.clear();
    int64_t startFrame, endFrame;
    exportRange(startFrame, endFrame);
    const int64_t count = endFrame - startFrame + 1;
    char buf[160];
    std::string res;
    if (srcW_ > 0 && srcH_ > 0) {
        SDL_snprintf(buf, sizeof(buf), "%dx%d, ", srcW_, srcH_);
        res = buf;
    }
    SDL_snprintf(buf, sizeof(buf), "%.3g fps, %lld frame%s (%lld-%lld)",
                 fps_, (long long)count, count == 1 ? "" : "s",
                 (long long)startFrame, (long long)endFrame);
    const std::string timing = buf;

    if (mode_ == Mode::Movie) {
        successDetails_.push_back(containerCombo_.value() + ", " + codecCombo_.value()
                                  + ", " + qualityCombo_.value());
        const bool audio = movieAudioCombo_.selected() == 0
                        && AudioMixdown::hasAudio(audioSegs_, startFrame, endFrame);
        successDetails_.push_back(res + timing);
        successDetails_.push_back(audio ? "Audio: Included" : "Audio: None");
    } else if (mode_ == Mode::Audio) {
        successDetails_.push_back(audioFormatCombo_.value() + ", " + audioVariantCombo_.value()
                                  + ", " + sampleRateCombo_.value());
        SDL_snprintf(buf, sizeof(buf), "%.2f s, ", fps_ > 0.0 ? count / fps_ : 0.0);
        successDetails_.push_back(buf + timing);
    } else if (mode_ == Mode::Gif) {
        // The GIF's own size and rate, which need not be the timeline's.
        int gw, gh;
        gifOutputSize(gw, gh);
        const double gfps = gifOutputFps();
        const int64_t gifFrames = std::max<int64_t>(1, (int64_t)std::llround(count * gfps / fps_));
        successDetails_.push_back("GIF, " + gifQualityCombo_.value() + ", loop "
                                  + gifLoopCombo_.value());
        SDL_snprintf(buf, sizeof(buf), "%dx%d, %.3g fps, %lld frame%s (%lld-%lld)",
                     gw, gh, gfps, (long long)gifFrames, gifFrames == 1 ? "" : "s",
                     (long long)startFrame, (long long)endFrame);
        successDetails_.push_back(buf);
    } else {
        successDetails_.push_back(formatCombo_.value());
        successDetails_.push_back(res + timing);
    }
    pathInput_.setFocus(false);
    startInput_.setFocus(false);
    endInput_.setFocus(false);
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

    enterSuccessViewIfDone();
    if (showSuccess_) return handleSuccessEvent(e, win);

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

    // An open list sits over the rows below it, so it alone sees the event:
    // otherwise a click on one of its rows would also land on the widget beneath.
    for (Combobox* c : modeCombos()) {
        if (c->isOpen()) {
            c->handleEvent(e);
            refreshAudioOptions();
            return true;
        }
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
        } else if (mode_ == Mode::Gif) {
            static const SDL_DialogFileFilter kFilters[] = {
                { "GIF images", "gif" },
                { "All files",  "*" },
            };
            SDL_ShowSaveFileDialog(&ExportDialog::onBrowseChosen, this, win,
                                   kFilters, SDL_arraysize(kFilters), nullptr);
        } else if (mode_ == Mode::Audio) {
            static const SDL_DialogFileFilter kFilters[] = {
                { "Audio files", "wav;aiff;aif;flac;m4a;mp3;opus" },
                { "All files",   "*" },
            };
            SDL_ShowSaveFileDialog(&ExportDialog::onBrowseChosen, this, win,
                                   kFilters, SDL_arraysize(kFilters), nullptr);
        } else {
            SDL_ShowOpenFolderDialog(&ExportDialog::onBrowseChosen, this, win, nullptr, false);
        }
        return true;
    }

    for (Combobox* c : modeCombos())
        consumed |= c->handleEvent(e);
    refreshAudioOptions();

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
        comboRow(qualityCombo_, comboW);   gapTop(body, gap);
        comboRow(movieAudioCombo_, comboW * .6f); gapTop(body, gap * 1.5f);
    } else if (mode_ == Mode::Audio) {
        comboRow(audioFormatCombo_, comboW);        gapTop(body, gap);
        comboRow(audioVariantCombo_, comboW * .6f); gapTop(body, gap);
        comboRow(sampleRateCombo_, comboW * .6f);   gapTop(body, gap * 1.5f);
    } else if (mode_ == Mode::Gif) {
        comboRow(gifSizeCombo_, comboW);          gapTop(body, gap);
        comboRow(gifFpsCombo_, comboW);           gapTop(body, gap);
        comboRow(gifQualityCombo_, comboW);       gapTop(body, gap);
        comboRow(gifLoopCombo_, comboW * .6f);    gapTop(body, gap * 1.5f);
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

    // Bottom buttons: Close/Cancel on the left, Export on the right. This is the
    // last row: the dialog ends one bottom pad below it.
    const float btnW = 80.f * scale;
    row = cutTop(body, rowH);
    cancelBtn_.setRect(cutLeft(row, btnW));
    cancelBtn_.setLabel(exporting_ ? "Cancel" : "Close");
    exportBtn_.setRect(cutRight(row, btnW));
    exportBtn_.setLabel("Export");
    exportBtn_.setEnabled(!exporting_ && !exportDone_);

    return body.y - top;
}

// ─── Render ──────────────────────────────────────────────────────────────────

void ExportDialog::render(SDL_Renderer* r, TextFont* font, float winW, float winH) {
    if (!open_ || !font) return;
    enterSuccessViewIfDone();
    if (showSuccess_) { renderSuccess(r, font, winW, winH); return; }

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
        const char* title = mode_ == Mode::Movie ? "Export Movie"
                          : mode_ == Mode::Audio ? "Export Audio"
                          : mode_ == Mode::Gif   ? "Export GIF Animation"
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
    const char* pathLabel = (mode_ == Mode::ImageSequence) ? "Output Dir:" : "Output File:";
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
        drawLabel(containerCombo_.rect(),  "Container:");
        drawLabel(codecCombo_.rect(),      "Codec:");
        drawLabel(qualityCombo_.rect(),    "Quality:");
        drawLabel(movieAudioCombo_.rect(), "Audio:");
    } else if (mode_ == Mode::Audio) {
        const int sel = audioFormatCombo_.selected();
        const bool lossy = sel >= 0 && sel < (int)audioFormats_.size()
                        && audioFormatTable()[audioFormats_[sel]].lossy;
        drawLabel(audioFormatCombo_.rect(),  "Format:");
        drawLabel(audioVariantCombo_.rect(), lossy ? "Bitrate:" : "Bit Depth:");
        drawLabel(sampleRateCombo_.rect(),   "Sample Rate:");
    } else if (mode_ == Mode::Gif) {
        drawLabel(gifSizeCombo_.rect(),    "Size:");
        drawLabel(gifFpsCombo_.rect(),     "Frame Rate:");
        drawLabel(gifQualityCombo_.rect(), "Quality:");
        drawLabel(gifLoopCombo_.rect(),    "Loop:");
    } else {
        drawLabel(formatCombo_.rect(), "Format:");
    }
    for (Combobox* c : modeCombos())
        c->render(r, font);

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
    for (Combobox* c : modeCombos())
        c->renderDropdown(r, font);
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
    if (!ffmpegAvailable() && mode_ != Mode::ImageSequence) {
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
        else if (mode_ == Mode::Audio)
            runAudioExport(startFrame, endFrame);
        else if (mode_ == Mode::Gif)
            runGifExport(startFrame, endFrame);
        else
            runImageExport(startFrame, endFrame);
        // Every way out is done, the early error returns included: without this
        // their status message never shows, since the bar and status line only
        // draw once the export is running or done.
        exportDone_ = true;
        exporting_ = false;
    });
}

// ─── Success view ────────────────────────────────────────────────────────────

bool ExportDialog::handleSuccessEvent(const SDL_Event& e, SDL_Window* win) {
    if (e.type == SDL_EVENT_KEY_DOWN
        && (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) {
        close(win);
        return true;
    }
    if (openBtn_.handleEvent(e)) {
        openRequest_ = revealPath_;
        close(win);
        return true;
    }
    if (copyPathBtn_.handleEvent(e)) {
        SDL_SetClipboardText(successPath_.c_str());
        successNote_ = "Path copied to clipboard.";
        return true;
    }
    if (revealBtn_.handleEvent(e)) {
        successNote_ = revealInFileManager(revealPath_) ? ""
                                                        : "Could not open File Explorer.";
        return true;
    }
    if (successCloseBtn_.handleEvent(e)) {
        close(win);
        return true;
    }
    return true; // modal: block everything else
}

void ExportDialog::renderSuccess(SDL_Renderer* r, TextFont* font, float winW, float winH) {
    const float scale = dpiScale;
    const float padX = 16.f * scale;
    const float padY = 14.f * scale;
    const float rowH = 22.f * scale;
    const float gap  =  8.f * scale;
    const float titleH = rowH + padY;
    const float glyphH = font->lineHeight();

    // A folder path may end in a separator, which leaves filename() empty.
    fs::path p = fs::path(successPath_);
    std::string name = p.filename().string();
    if (name.empty()) name = p.parent_path().filename().string();
    if (name.empty()) name = successPath_;

    const char* kOpen   = "Open";
    const char* kCopy   = "Copy Path";
    const char* kReveal = "Show in File Explorer";
    const char* kClose  = "Close";
    auto btnW = [&](const char* l) {
        return std::max(80.f * scale, font->measure(r, l) + 20.f * scale);
    };
    const float wOpen = btnW(kOpen), wCopy = btnW(kCopy), wReveal = btnW(kReveal),
                wClose = btnW(kClose);
    const float buttonsW = wOpen + wCopy + wReveal + wClose + 3.f * gap;

    // Wide enough for the buttons and the name, but never wider than the window.
    float contentW = std::max({ 360.f * scale, buttonsW, font->measure(r, name.c_str()) });
    for (const std::string& d : successDetails_)
        contentW = std::max(contentW, font->measure(r, d.c_str()));
    float dw = contentW + 2.f * padX;
    dw = std::min(dw, std::max(winW - 2.f * padX, buttonsW + 2.f * padX));
    const float detailsH = successDetails_.size() * glyphH;
    const float dh = titleH + padY + rowH + detailsH + gap + rowH + gap * 1.5f + rowH + padY;

    SDL_FRect full = { 0.f, 0.f, winW, winH };
    successRect_ = center(full, dw, dh);
    successRect_.x = std::round(successRect_.x);
    successRect_.y = std::round(successRect_.y);

    setColor(r, kOverlay);
    jplay::fillRect(r, &full);
    setColor(r, kDialogBg);
    jplay::fillRect(r, &successRect_);
    setColor(r, kBorder);
    jplay::drawRect(r, &successRect_);

    SDL_FRect body = successRect_;
    SDL_FRect titleBar = cutTop(body, titleH);
    body = inset(body, padX, padY);

    setColor(r, kTitleBg);
    jplay::fillRect(r, &titleBar);
    setColor(r, kSeparator);
    jplay::drawLine(r, titleBar.x, titleBar.y + titleBar.h,
                      titleBar.x + titleBar.w, titleBar.y + titleBar.h);
    {
        SDL_FRect slot = centerV(titleBar, glyphH);
        font->draw(r, slot.x + padX, slot.y, kTitleText, "Export Successful");
    }

    SDL_FRect row = cutTop(body, rowH);
    font->draw(r, row.x, centerV(row, glyphH).y, kSuccess, name.c_str());
    for (const std::string& d : successDetails_) {
        row = cutTop(body, glyphH);
        font->draw(r, row.x, row.y, kLabel, d.c_str());
    }
    gapTop(body, gap);

    row = cutTop(body, rowH);
    if (!successNote_.empty())
        font->draw(r, row.x, centerV(row, glyphH).y, kLabel, successNote_.c_str());
    gapTop(body, gap * 1.5f);

    // Close on the left, the action buttons right-aligned.
    row = cutTop(body, rowH);
    successCloseBtn_.setRect(cutLeft(row, wClose));
    successCloseBtn_.setLabel(kClose);
    revealBtn_.setRect(cutRight(row, wReveal));
    revealBtn_.setLabel(kReveal);
    gapRight(row, gap);
    copyPathBtn_.setRect(cutRight(row, wCopy));
    copyPathBtn_.setLabel(kCopy);
    gapRight(row, gap);
    openBtn_.setRect(cutRight(row, wOpen));
    openBtn_.setLabel(kOpen);

    openBtn_.render(r, font);
    copyPathBtn_.render(r, font);
    revealBtn_.render(r, font);
    successCloseBtn_.render(r, font);
}
