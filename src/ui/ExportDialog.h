#pragma once

#include "Widgets.h"
#include "TextFont.h"
#include "Timeline.h"     // ClipType
#include "OcioManager.h"  // OcioManager::CpuTransform
#include "AudioMixdown.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Modal export dialog for "Export Movie", "Export Image Sequence" and
// "Export Audio".
// The host feeds it events before other handlers and renders it last so it
// overlays everything. Export runs on a background thread; the host supplies a
// snapshot of per-frame source info before starting.
class ExportDialog {
public:
    enum class Mode { Movie, ImageSequence, Audio };

    // Per-frame source info built by the host on the main thread.
    struct FrameSource {
        std::string path;       // media file path (empty → no clip at this frame)
        ClipType    type = ClipType::Video;
        int64_t     sourceFrame = 0; // frame index within the source file
    };

    // Encoder / container option from FFmpeg query.
    struct CodecInfo  { std::string id; std::string label; };
    struct FormatInfo { std::string id; std::string label; };

    ~ExportDialog();

    bool isOpen()      const { return open_; }
    bool isExporting() const { return exporting_.load(); }

    // Open the dialog. inPoint/outPoint: outPoint = -1 means unset (use timelineLen-1).
    // width/height: resolution of the first renderable frame (for the ffmpeg command).
    void open(Mode mode, int64_t timelineLen,
              int64_t inPoint, int64_t outPoint,
              double fps, int width, int height,
              SDL_Window* win);
    void close(SDL_Window* win);

    // Set the per-frame source snapshot; call before the user can click Export.
    void setFrames(std::vector<FrameSource> frames) { frames_ = std::move(frames); }

    // The colour transform each source's frames are written through, keyed by the
    // media path FrameSource::path carries. Built by the host on the main thread
    // before the export starts, because the OCIO processors have to be resolved
    // there; the encoder thread then only applies them. A source with no entry is
    // written as it decoded, which is what colour management being off amounts to.
    //
    // Movie and PNG output takes the full transform, so the file carries the same
    // display rendering the viewer shows. EXR output takes only its first half and
    // is written in the linear working space — a display-referred EXR would be the
    // wrong thing to hand back to a pipeline.
    void setColorTransforms(std::map<std::string, OcioManager::CpuTransform> t) {
        colorXf_ = std::move(t);
    }

    // The timeline's audio mix, for Audio mode and for the movie's soundtrack.
    // Built by the host on the main thread (App::exportAudioSegments); the
    // encoder thread renders it through an AudioMixdown. Call after open(): an
    // empty mix defaults the movie's Audio option to None.
    void setAudioSegments(std::vector<AudioMixdown::Segment> segs);

    // true = event consumed (host should stop processing it).
    bool handleEvent(const SDL_Event& e, SDL_Window* win);

    // The exported file the success view's Open button asked the host to load
    // (the first frame for a sequence), or empty. One-shot: cleared on read.
    std::string takeOpenRequest() { std::string p; p.swap(openRequest_); return p; }

    // Render the modal overlay. Call last in the host's render pass.
    void render(SDL_Renderer* r, TextFont* font, float winW, float winH);

    // Query the ffmpeg binary on PATH for available video encoders.
    // Returns a curated list of available codecs. Empty on failure.
    static std::vector<CodecInfo>  queryVideoEncoders();
    static std::vector<FormatInfo> queryContainerFormats();
    static bool ffmpegAvailable();

private:
    bool   open_ = false;
    Mode   mode_ = Mode::Movie;
    int64_t timelineLen_ = 0;
    double  fps_         = 24.0;
    int     srcW_        = 0;
    int     srcH_        = 0;

    // Per-frame snapshot (set by host before export).
    std::vector<FrameSource> frames_;
    std::map<std::string, OcioManager::CpuTransform> colorXf_; // see setColorTransforms
    std::vector<AudioMixdown::Segment> audioSegs_;              // see setAudioSegments

    // The display-referred bytes to write for one decoded frame: the frame through
    // its media's colour transform, or its own 8-bit buffer when it has none.
    // `buf` is the caller's scratch, resized only when a transform runs.
    const uint8_t* displayPixels_(const FrameSource& fs, const Frame& f,
                                  std::vector<uint8_t>& buf) const;

    // ── Widgets ──────────────────────────────────────────────────────────
    TextInput pathInput_;
    RadioGroup rangeGroup_;
    TextInput startInput_, endInput_;

    // Movie-only
    Combobox containerCombo_, codecCombo_, qualityCombo_, movieAudioCombo_;
    // Image-only
    Combobox formatCombo_;
    // Audio-only. The variant combo is bit depth for PCM/lossless formats and
    // bitrate for lossy ones; both it and the sample rates are rebuilt from the
    // format whenever the format changes (see refreshAudioOptions).
    Combobox audioFormatCombo_, audioVariantCombo_, sampleRateCombo_;
    std::vector<int> audioFormats_; // indices into the format table ffmpeg can encode
    int shownAudioFormat_ = -1;     // audioFormatCombo_ selection the options match

    Button browseBtn_, exportBtn_, cancelBtn_;

    // ── Success view ─────────────────────────────────────────────────────
    // A finished export replaces the form with a small "Export Successful"
    // popup (see enterSuccessView). Copy Path and Show in File Explorer leave it
    // up; Open, Close (or Escape / Enter) dismiss it.
    bool        showSuccess_ = false;
    std::string successPath_;   // what Copy Path copies: the file, or the sequence's folder
    std::string revealPath_;    // what Show in File Explorer selects: the file, or the first frame
    std::vector<std::string> successDetails_; // settings summary under the name (format, codec, ...)
    std::string successNote_;   // feedback line under the details ("Path copied.")
    std::string openRequest_;   // set by Open, collected by the host (takeOpenRequest)
    Button      openBtn_, copyPathBtn_, revealBtn_, successCloseBtn_;
    SDL_FRect   successRect_{};

    // ── Layout rects (recomputed each render) ────────────────────────────
    SDL_FRect dialogRect_{};
    SDL_FRect progressBarOuter_{};

    // ── Export state ─────────────────────────────────────────────────────
    std::thread         exportThread_;
    std::atomic<bool>   exporting_{ false };
    std::atomic<bool>   cancelExport_{ false };
    std::atomic<int>    exportProgress_{ 0 }; // 0..100
    mutable std::mutex  statusMutex_;
    std::string         exportStatusMsg_;      // guarded by statusMutex_
    std::atomic<bool>   exportDone_{ false };
    std::atomic<bool>   exportError_{ false };
    std::string         exportedPath_;         // guarded by statusMutex_; set on success
    std::string         exportedRevealPath_;   // guarded by statusMutex_; set on success

    // ── Pending browse path (SDL dialog callback → main thread) ──────────
    std::mutex  browseMutex_;
    std::string pendingBrowsePath_;

    static void SDLCALL onBrowseChosen(void* userdata, const char* const* filelist, int filter);

    void startExport();
    void runMovieExport(int64_t startFrame, int64_t endFrame);
    void runImageExport(int64_t startFrame, int64_t endFrame);
    void runAudioExport(int64_t startFrame, int64_t endFrame);

    // Render the mix over [startFrame, endFrame] to a headerless f32le stereo
    // file at kMovieAudioRate, for the movie encoder to read as its second
    // input. Reports progress as it goes; false on cancel or a write error.
    bool renderAudioTemp(int64_t startFrame, int64_t endFrame, const std::string& path);

    // The combos the current mode shows, top to bottom.
    std::vector<Combobox*> modeCombos();
    void refreshAudioOptions();

    void setStatusMsg(const std::string& s);
    // Worker side of a successful export: record what was written, for the
    // success view to show, copy and reveal.
    void setExportedPath(const std::string& path, const std::string& revealPath);
    // Swap the form for the success view once the worker has finished without
    // error. Called from both handleEvent and render, since the export can end
    // while no events arrive.
    void enterSuccessViewIfDone();
    bool handleSuccessEvent(const SDL_Event& e, SDL_Window* win);
    void renderSuccess(SDL_Renderer* r, TextFont* font, float winW, float winH);
    std::string getStatusMsg() const;

    // Cut the dialog's rows off `body` in order, storing each widget's rect, and
    // return the height they consume. render() runs it twice: once against a
    // throwaway unbounded rect to size the dialog, then against the real content
    // rect to place the widgets. One list of rows, so the dialog's height and its
    // contents cannot drift apart.
    float layoutRows(SDL_Renderer* r, TextFont* font, SDL_FRect body);

    // Collect export range from UI fields.
    void exportRange(int64_t& outStart, int64_t& outEnd) const;

    // Bundled ffmpeg beside the executable if present, else the bare name for
    // PATH lookup. ffmpegCmd() is the same thing quoted for a popen() string.
    static std::string ffmpegPath();
    static constexpr int kMovieAudioRate = 48000;
    static std::string ffmpegCmd();
};
