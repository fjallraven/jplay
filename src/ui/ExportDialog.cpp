#include "ExportDialog.h"
#include "AppInternal.h"
#include "Layout.h"
#include "SkinColors.h"
#include "VideoSource.h"
#include "ImageSeq.h"
#include "RevealFile.h"

#include <unordered_map>

#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <Imath/half.h>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_filesystem.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
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

// ---- audio export formats
// One entry per choice in the Format combo. `encoder` is what ffmpeg -encoders
// must list for the entry to be offered; each variant is a bit depth (PCM and
// lossless) or a bitrate (lossy) and carries the encoder arguments it stands for.
struct AudioVariant { const char* label; const char* args; };
struct AudioFormatDef {
    const char* label;
    const char* ext;
    const char* encoder;
    bool lossy;    // variants are bitrates, not bit depths
    bool only48k;  // libopus takes 48 kHz and lower, none of the other choices
    std::vector<AudioVariant> variants;
    int defaultVariant;
};

const std::vector<AudioFormatDef>& audioFormatTable() {
    static const std::vector<AudioFormatDef> kTable = {
        { "WAV", "wav", "pcm_s16le", false, false, {
            { "16-bit",       "-c:a pcm_s16le" },
            { "24-bit",       "-c:a pcm_s24le" },
            { "32-bit float", "-c:a pcm_f32le" } }, 1 },
        { "AIFF", "aiff", "pcm_s16be", false, false, {
            { "16-bit", "-c:a pcm_s16be" },
            { "24-bit", "-c:a pcm_s24be" } }, 1 },
        { "FLAC", "flac", "flac", false, false, {
            { "16-bit", "-c:a flac -sample_fmt s16" },
            { "24-bit", "-c:a flac -sample_fmt s32 -bits_per_raw_sample 24" } }, 1 },
        { "ALAC (M4A)", "m4a", "alac", false, false, {
            { "16-bit", "-c:a alac -sample_fmt s16p" },
            { "24-bit", "-c:a alac -sample_fmt s32p -bits_per_raw_sample 24" } }, 1 },
        { "AAC (M4A)", "m4a", "aac", true, false, {
            { "128 kbps", "-c:a aac -b:a 128k" },
            { "192 kbps", "-c:a aac -b:a 192k" },
            { "256 kbps", "-c:a aac -b:a 256k" },
            { "320 kbps", "-c:a aac -b:a 320k" } }, 2 },
        { "MP3", "mp3", "libmp3lame", true, false, {
            { "128 kbps", "-c:a libmp3lame -b:a 128k" },
            { "192 kbps", "-c:a libmp3lame -b:a 192k" },
            { "256 kbps", "-c:a libmp3lame -b:a 256k" },
            { "320 kbps", "-c:a libmp3lame -b:a 320k" } }, 3 },
        { "Opus", "opus", "libopus", true, true, {
            { "64 kbps",  "-c:a libopus -b:a 64k" },
            { "96 kbps",  "-c:a libopus -b:a 96k" },
            { "128 kbps", "-c:a libopus -b:a 128k" },
            { "192 kbps", "-c:a libopus -b:a 192k" },
            { "256 kbps", "-c:a libopus -b:a 256k" } }, 2 },
    };
    return kTable;
}

struct SampleRateDef { const char* label; int hz; };
constexpr SampleRateDef kSampleRates[] = {
    { "44.1 kHz", 44100 }, { "48 kHz", 48000 }, { "96 kHz", 96000 },
};

// The movie's soundtrack codec, chosen from the container (and, for MOV, the
// video codec) rather than offered as another combo: AAC wherever it is the
// norm, PCM where the container or an intermediate codec calls for it. MXF
// only takes PCM.
std::string movieAudioArgs(const std::string& fmtId, const std::string& codecId) {
    const bool intermediate = codecId == "prores_ks" || codecId == "prores" || codecId == "dnxhd";
    if (fmtId == "mxf" || (fmtId == "mov" && intermediate))
        return "-c:a pcm_s24le";
    if (fmtId == "avi")
        return "-c:a pcm_s16le";
    return "-c:a aac -b:a 256k";
}

} // namespace

// ─── Platform pipe helpers ───────────────────────────────────────────────────
#ifdef _WIN32
// A stand-in for _popen/_pclose. _popen starts `cmd /c` with a console of its
// own, and jplay is a GUI app with none to share, so every ffmpeg run flashed a
// console window. This starts the same `cmd /c` (the commands rely on its 2>&1 /
// 2>nul redirection) with CREATE_NO_WINDOW, and hands back a FILE* on our end of
// the pipe like _popen does. The process handle is kept per FILE* so PPCLOSE can
// wait for it and return the exit code, as _pclose does.
namespace {
std::mutex g_pipeProcsMtx;
std::unordered_map<FILE*, HANDLE> g_pipeProcs;
} // namespace

// cmd /c strips the first and last quote of any command that starts with one and
// holds more than one quoted token -- a quoted ffmpeg path followed by a quoted
// output path comes out mangled and never runs. One extra pair around the whole
// string is what that stripping removes.
static FILE* pipeOpen(const std::string& cmd, const char* mode) {
    const bool reading = mode[0] == 'r';
    const bool binary  = std::strchr(mode, 'b') != nullptr;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return nullptr;
    HANDLE ours   = reading ? rd : wr;
    HANDLE theirs = reading ? wr : rd;
    SetHandleInformation(ours, HANDLE_FLAG_INHERIT, 0); // the child gets only its end

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = reading ? GetStdHandle(STD_INPUT_HANDLE) : theirs;
    si.hStdOutput = reading ? theirs : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    // Commands are UTF-8 (paths from SDL); the wide API keeps non-ASCII paths intact.
    std::string line = "cmd.exe /d /s /c \"" + cmd + "\"";
    std::wstring wline(MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wline.data(), (int)wline.size());

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, wline.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(theirs);
    if (!ok) {
        CloseHandle(ours);
        return nullptr;
    }
    CloseHandle(pi.hThread);

    const int fd = _open_osfhandle((intptr_t)ours,
                                   (reading ? _O_RDONLY : _O_WRONLY) | (binary ? _O_BINARY : _O_TEXT));
    FILE* f = fd >= 0 ? _fdopen(fd, mode) : nullptr;
    if (!f) {
        if (fd >= 0) _close(fd); else CloseHandle(ours);
        WaitForSingleObject(pi.hProcess, INFINITE); // stdin/stdout closed: it exits
        CloseHandle(pi.hProcess);
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_pipeProcsMtx);
    g_pipeProcs[f] = pi.hProcess;
    return f;
}

static int pipeClose(FILE* f) {
    HANDLE proc = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_pipeProcsMtx);
        if (auto it = g_pipeProcs.find(f); it != g_pipeProcs.end()) {
            proc = it->second;
            g_pipeProcs.erase(it);
        }
    }
    std::fclose(f); // EOF on the child's stdin (or our end of its stdout)
    if (!proc)
        return -1;
    DWORD code = (DWORD)-1;
    WaitForSingleObject(proc, INFINITE);
    GetExitCodeProcess(proc, &code);
    CloseHandle(proc);
    return (int)code;
}
#  define PPCLOSE pipeClose
#else
#  define PPCLOSE pclose
static FILE* pipeOpen(const std::string& cmd, const char* mode) {
    return popen(cmd.c_str(), mode);
}
#endif

static std::string runAndCapture(const std::string& cmd) {
    FILE* pipe = pipeOpen(cmd, "r");
    if (!pipe) return {};
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    PPCLOSE(pipe);
    return out;
}

// ─── FFmpeg discovery ────────────────────────────────────────────────────────

// The release archive ships a GPL ffmpeg beside the executable, since that is the
// only build guaranteed to carry libx264/libx265/libmp3lame. Prefer it over a
// bare name: PATH resolution only finds an ffmpeg the user installed themselves,
// and /bin/sh never searches the executable's own directory.
std::string ExportDialog::ffmpegPath() {
#ifdef _WIN32
    static constexpr const char* kExe = "ffmpeg.exe";
#else
    static constexpr const char* kExe = "ffmpeg";
#endif
    // Probed once: a bundled binary cannot appear or vanish mid-session.
    static const std::string resolved = [] {
        if (const char* base = SDL_GetBasePath()) { // exe dir, trailing sep; do not free
            std::error_code ec;
            std::string sibling = std::string(base) + kExe;
            if (fs::exists(sibling, ec))
                return sibling;
        }
        return std::string(kExe); // fall back to PATH
    }();
    return resolved;
}

// Shell token for the program, quoted because the install directory may contain
// spaces. Every command below is assembled as a string for popen(), so the
// quoting has to live here rather than in an argv array.
std::string ExportDialog::ffmpegCmd() {
    return "\"" + ffmpegPath() + "\"";
}

// The raw probe output, captured once per session: the binary is resolved once
// (ffmpegPath) and cannot change under us, while each probe is a process launch
// that cost the dialog a visible stall on every open. Probed on the first open
// (not at startup, so a session that never exports never runs ffmpeg), with the
// three in parallel.
namespace {
struct FfmpegInfo {
    std::string version;
    std::string encoders;
    std::string formats;
};

// `exe` is always ExportDialog::ffmpegCmd(), itself fixed for the session.
const FfmpegInfo& ffmpegInfo(const std::string& exe) {
    static const FfmpegInfo info = [&exe] {
        auto probe = [&exe](const char* args) {
            return std::async(std::launch::async, [cmd = exe + args] { return runAndCapture(cmd); });
        };
        auto version  = probe(" -version 2>&1");
        auto encoders = probe(" -encoders -v quiet 2>&1");
        auto formats  = probe(" -formats -v quiet 2>&1");
        return FfmpegInfo{ version.get(), encoders.get(), formats.get() };
    }();
    return info;
}
} // namespace

bool ExportDialog::ffmpegAvailable() {
    return ffmpegInfo(ffmpegCmd()).version.find("ffmpeg version") != std::string::npos;
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

    const std::string& out = ffmpegInfo(ffmpegCmd()).encoders;
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

    const std::string& out = ffmpegInfo(ffmpegCmd()).formats;
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
        else
            runImageExport(startFrame, endFrame);
        // Every way out is done, the early error returns included: without this
        // their status message never shows, since the bar and status line only
        // draw once the export is running or done.
        exportDone_ = true;
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
        return f.rgba8().data();
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

    // The soundtrack is mixed down first, to a temp file ffmpeg reads as a second
    // input: the frames already have the one stdin pipe there is.
    std::string audioTmp;
    if (movieAudioCombo_.selected() == 0
        && AudioMixdown::hasAudio(audioSegs_, startFrame, endFrame)) {
        std::error_code ec;
        fs::path tmpDir = fs::temp_directory_path(ec);
        if (ec) tmpDir = fs::path(outPath).parent_path();
        char name[64];
        SDL_snprintf(name, sizeof(name), "jplay_export_%llx.f32",
                     (unsigned long long)SDL_GetTicksNS());
        audioTmp = (tmpDir / name).string();
        if (!renderAudioTemp(startFrame, endFrame, audioTmp)) {
            fs::remove(audioTmp, ec);
            return;
        }
    }

    // Build ffmpeg command reading raw RGBA from stdin.
    std::ostringstream cmd;
    cmd << ffmpegCmd()
        << " -y -f rawvideo -pix_fmt rgba"
        << " -video_size " << outW << "x" << outH
        << " -framerate " << fps_
        << " -i pipe:0";
    if (!audioTmp.empty())
        cmd << " -f f32le -ar " << kMovieAudioRate << " -ac 2 -i \"" << audioTmp << "\""
            << " -map 0:v -map 1:a";

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
    if (!audioTmp.empty())
        cmd << " " << movieAudioArgs(fmtId, codecId);

    cmd << " \"" << outPath << "\"";

#ifdef _WIN32
    // Silence ffmpeg's own output on Windows.
    cmd << " 2>nul";
#else
    cmd << " 2>/dev/null";
#endif

    FILE* pipe = pipeOpen(cmd.str(), "wb");
    if (!pipe) {
        std::error_code ec;
        if (!audioTmp.empty()) fs::remove(audioTmp, ec);
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
    if (!audioTmp.empty()) {
        std::error_code ec;
        fs::remove(audioTmp, ec);
    }

    if (cancelExport_) {
        setStatusMsg("Cancelled.");
        exportError_ = true;
    } else {
        exportProgress_ = 100;
        setStatusMsg("Done! Saved to: " + fs::path(outPath).filename().string());
        setExportedPath(outPath, outPath);
    }
    exportDone_ = true;
}

bool ExportDialog::renderAudioTemp(int64_t startFrame, int64_t endFrame,
                                   const std::string& path) {
    FILE* f = nullptr;
#ifdef _WIN32
    _wfopen_s(&f, fs::path(path).wstring().c_str(), L"wb");
#else
    f = fopen(path.c_str(), "wb");
#endif
    if (!f) {
        setStatusMsg("Cannot write temp audio file.");
        exportError_ = true;
        return false;
    }
    AudioMixdown mix(audioSegs_, fps_, kMovieAudioRate);
    mix.begin(startFrame, endFrame);
    const int64_t total = std::max<int64_t>(mix.totalFrames(), 1);
    constexpr int kChunk = 8192;
    std::vector<float> buf((size_t)kChunk * 2);
    bool ok = true;
    for (;;) {
        if (cancelExport_) { ok = false; break; }
        const int n = mix.render(buf.data(), kChunk);
        if (n <= 0)
            break;
        if (fwrite(buf.data(), sizeof(float) * 2, (size_t)n, f) != (size_t)n) {
            setStatusMsg("Temp audio write failed (disk full?).");
            exportError_ = true;
            ok = false;
            break;
        }
        const int prog = (int)(mix.renderedFrames() * 100 / total);
        exportProgress_ = prog;
        setStatusMsg("Mixing audio... " + std::to_string(prog) + "%");
    }
    fclose(f);
    if (cancelExport_) {
        setStatusMsg("Cancelled.");
        exportError_ = true;
    }
    exportProgress_ = 0;
    return ok;
}

// ─── Audio export ────────────────────────────────────────────────────────────

void ExportDialog::runAudioExport(int64_t startFrame, int64_t endFrame) {
    const int sel = audioFormatCombo_.selected();
    if (sel < 0 || sel >= (int)audioFormats_.size()) {
        setStatusMsg("No audio format selected.");
        exportError_ = true; return;
    }
    const AudioFormatDef& def = audioFormatTable()[audioFormats_[sel]];
    const int variant = std::clamp(audioVariantCombo_.selected(), 0, (int)def.variants.size() - 1);
    int rate = 48000;
    for (const SampleRateDef& sr : kSampleRates)
        if (!sampleRateCombo_.empty() && sampleRateCombo_.value() == sr.label)
            rate = sr.hz;
    if (def.only48k)
        rate = 48000;

    if (!AudioMixdown::hasAudio(audioSegs_, startFrame, endFrame)) {
        setStatusMsg("No audio in the export range.");
        exportError_ = true; return;
    }

    std::string outPath = pathInput_.text();
    if (fs::path(outPath).extension().string().empty())
        outPath += std::string(".") + def.ext;

    // Mixed at the output rate, so ffmpeg only encodes.
    std::ostringstream cmd;
    cmd << ffmpegCmd()
        << " -y -f f32le -ar " << rate << " -ac 2 -i pipe:0"
        << " " << def.variants[(size_t)variant].args
        << " \"" << outPath << "\""
#ifdef _WIN32
        << " 2>nul";
#else
        << " 2>/dev/null";
#endif

    FILE* pipe = pipeOpen(cmd.str(), "wb");
    if (!pipe) {
        setStatusMsg("Failed to launch ffmpeg. Is it on PATH?");
        exportError_ = true; return;
    }

    AudioMixdown mix(audioSegs_, fps_, rate);
    mix.begin(startFrame, endFrame);
    const int64_t total = std::max<int64_t>(mix.totalFrames(), 1);
    constexpr int kChunk = 8192;
    std::vector<float> buf((size_t)kChunk * 2);
    bool pipeBroke = false;
    for (;;) {
        if (cancelExport_) break;
        const int n = mix.render(buf.data(), kChunk);
        if (n <= 0)
            break;
        if (fwrite(buf.data(), sizeof(float) * 2, (size_t)n, pipe) != (size_t)n) {
            pipeBroke = true; // ffmpeg exited early; its exit code says why
            break;
        }
        const int64_t done = mix.renderedFrames();
        exportProgress_ = (int)(done * 100 / total);
        char msg[64];
        SDL_snprintf(msg, sizeof(msg), "%.1f / %.1f s",
                     (double)done / rate, (double)total / rate);
        setStatusMsg(msg);
    }

    const int rc = PPCLOSE(pipe);
    if (cancelExport_) {
        setStatusMsg("Cancelled.");
        exportError_ = true;
    } else if (rc != 0 || pipeBroke) {
        setStatusMsg("ffmpeg failed (exit code " + std::to_string(rc) + ").");
        exportError_ = true;
    } else {
        exportProgress_ = 100;
        setStatusMsg("Done! Saved to: " + fs::path(outPath).filename().string());
        setExportedPath(outPath, outPath);
    }
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
                    writeEXR(outPath, frame->width, frame->height, frame->rgba8().data());
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
            cmd << ffmpegCmd()
                << " -y -f rawvideo -pix_fmt rgba"
                << " -video_size " << fw << "x" << fh
                << " -framerate 1 -i pipe:0"
                << " -frames:v 1 \"" << outPath << "\""
#ifdef _WIN32
                << " 2>nul";
#else
                << " 2>/dev/null";
#endif
            FILE* pipe = pipeOpen(cmd.str(), "wb");
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
        // The sequence is named by its printf-style pattern (what Nuke, ffmpeg
        // and OIIO read); reveal opens the folder on the first frame.
        const std::string ext = isPng ? "png" : "exr";
        char first[64];
        SDL_snprintf(first, sizeof(first), "frame_%06lld.%s",
                     (long long)startFrame, ext.c_str());
        setExportedPath((fs::path(dir) / ("frame_%06d." + ext)).string(),
                        (fs::path(dir) / first).string());
    }
    exportDone_ = true;
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
