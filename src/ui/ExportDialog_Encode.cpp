// The encoding half of ExportDialog: the ffmpeg pipe and discovery, and the
// worker-thread bodies of each export. The dialog itself is ExportDialog.cpp.

#include "ExportDialog.h"
#include "ExportFormats.h"
#include "AppInternal.h"
#include "VideoSource.h"
#include "ImageSeq.h"

#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <Imath/half.h>

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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace jplay;
using namespace ExportFormats;

namespace {

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

namespace ExportFormats {

// See ExportFormats.h for why this is probed once.
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

} // namespace ExportFormats

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

// ─── GIF export ──────────────────────────────────────────────────────────────

void ExportDialog::gifOutputSize(int& w, int& h) const {
    w = srcW_;
    h = srcH_;
    const int sel = gifSizeCombo_.selected();
    if (sel <= 0 || sel >= (int)gifWidths_.size() || srcW_ <= 0 || srcH_ <= 0)
        return;
    w = gifWidths_[(size_t)sel];
    h = std::max(1, (int)std::lround((double)srcH_ * w / srcW_));
}

double ExportDialog::gifOutputFps() const {
    const int sel = gifFpsCombo_.selected();
    if (sel <= 0 || sel >= (int)gifRates_.size())
        return fps_;
    return gifRates_[(size_t)sel];
}

// The frames go to ffmpeg as raw RGBA at the timeline's size and rate, the same
// pipe the movie export uses; ffmpeg drops to the GIF's rate, scales, and builds
// the palette in one filter graph (see kGifQualities).
void ExportDialog::runGifExport(int64_t startFrame, int64_t endFrame) {
    std::string outPath = pathInput_.text();
    if (fs::path(outPath).extension().string().empty())
        outPath += ".gif";

    SourceCache srcCache;

    int inW = srcW_, inH = srcH_;
    if (inW <= 0 || inH <= 0) {
        for (int64_t f = startFrame; f <= endFrame; ++f) {
            if (f < (int64_t)frames_.size()) {
                if (auto fp = decodeFrame(frames_[f], srcCache)) {
                    inW = fp->width; inH = fp->height; break;
                }
            }
        }
    }
    if (inW <= 0 || inH <= 0) {
        setStatusMsg("No renderable frames in range.");
        exportError_ = true; return;
    }
    int outW, outH;
    gifOutputSize(outW, outH);
    if (outW <= 0 || outH <= 0) { outW = inW; outH = inH; }
    const double outFps = gifOutputFps();
    const GifQualityDef& q = kGifQualities[std::clamp(gifQualityCombo_.selected(), 0,
                                                      (int)std::size(kGifQualities) - 1)];

    // format=rgb24 drops alpha before the palette sees it, so a frame with alpha
    // below 1 is written opaque rather than punched through.
    std::ostringstream vf;
    if (outFps != fps_)
        vf << "fps=" << outFps << ",";
    vf << "scale=" << outW << ":" << outH << ":flags=lanczos,format=rgb24,"
       << "split[a][b];[a]" << q.palettegen << "[p];[b][p]" << q.paletteuse;

    std::ostringstream cmd;
    cmd << ffmpegCmd()
        << " -y -f rawvideo -pix_fmt rgba"
        << " -video_size " << inW << "x" << inH
        << " -framerate " << fps_
        << " -i pipe:0"
        << " -vf \"" << vf.str() << "\""
        << " -loop " << (gifLoopCombo_.selected() == 1 ? -1 : 0)
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

    const int64_t total = endFrame - startFrame + 1;
    const size_t frameBytes = (size_t)inW * inH * 4;
    std::vector<uint8_t> xfBuf; // reused across frames when a transform runs
    std::vector<uint8_t> black;
    bool pipeBroke = false;
    for (int64_t i = 0; i < total; ++i) {
        if (cancelExport_) break;
        const int64_t tlFrame = startFrame + i;
        FramePtr frame;
        if (tlFrame < (int64_t)frames_.size())
            frame = decodeFrame(frames_[tlFrame], srcCache);

        const uint8_t* px;
        if (frame && frame->width == inW && frame->height == inH) {
            px = displayPixels_(frames_[tlFrame], *frame, xfBuf);
        } else {
            black.resize(frameBytes, 0);
            px = black.data();
        }
        if (fwrite(px, 1, frameBytes, pipe) != frameBytes) {
            pipeBroke = true; // ffmpeg exited early; its exit code says why
            break;
        }

        exportProgress_ = (int)((i + 1) * 100 / total);
        char buf[64];
        SDL_snprintf(buf, sizeof(buf), "Frame %lld / %lld", (long long)(i + 1), (long long)total);
        setStatusMsg(buf);
    }
    // The global-palette qualities only start writing once the input ends, so the
    // close is where most of the encode happens.
    if (!cancelExport_ && !pipeBroke)
        setStatusMsg("Encoding GIF...");

    const int rc = PPCLOSE(pipe);
    if (cancelExport_) {
        // ffmpeg finishes the file on EOF either way; a cut-short GIF is not
        // something to leave lying around under the name the user picked.
        std::error_code ec;
        fs::remove(outPath, ec);
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
