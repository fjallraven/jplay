#pragma once

// The format tables and the ffmpeg probe that ExportDialog's two halves share:
// ExportDialog.cpp fills its combos from them, ExportDialog_Encode.cpp turns the
// chosen entries into ffmpeg arguments. Internal to the export dialog.

#include <string>
#include <vector>

namespace ExportFormats {

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

const std::vector<AudioFormatDef>& audioFormatTable();

struct SampleRateDef { const char* label; int hz; };
inline constexpr SampleRateDef kSampleRates[] = {
    { "44.1 kHz", 44100 }, { "48 kHz", 48000 }, { "96 kHz", 96000 },
};

// ---- GIF export
// One entry per choice in the Quality combo, as the palettegen / paletteuse
// halves of the filter graph. GIF holds 256 colours, so quality is how the
// palette is chosen and how the error is dithered back in:
//   Best   - a palette per frame (stats_mode=single + new=1). Nothing is
//            buffered, but every frame carries its own colour table: biggest file.
//   High   - one palette from every pixel of the range, error-diffusion dither.
//   Medium - palette weighted to what moves (stats_mode=diff), ordered dither
//            and only the changed rectangle re-dithered, which LZW packs better.
//   Low    - as Medium with 128 colours and a coarser dither.
// The global-palette entries hold every (scaled) frame in ffmpeg until the
// palette is known, so a long range at a big size costs that much memory.
// reserve_transparent=0 everywhere: the frames are opaque, and the default
// spends one of the 256 entries on transparency.
struct GifQualityDef { const char* label; const char* palettegen; const char* paletteuse; };
inline constexpr GifQualityDef kGifQualities[] = {
    { "Best (palette per frame)",
      "palettegen=stats_mode=single:reserve_transparent=0",
      "paletteuse=new=1:dither=sierra2_4a" },
    { "High",
      "palettegen=stats_mode=full:reserve_transparent=0",
      "paletteuse=dither=sierra2_4a" },
    { "Medium",
      "palettegen=stats_mode=diff:reserve_transparent=0",
      "paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle" },
    { "Low (smallest file)",
      "palettegen=stats_mode=diff:max_colors=128:reserve_transparent=0",
      "paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle" },
};
inline constexpr int kGifDefaultQuality = 1;

// Width choices offered below the source's own; a GIF at plate resolution is
// rarely what anyone wants to send.
inline constexpr int kGifWidths[] = { 1920, 1280, 960, 720, 640, 480, 320 };
inline constexpr int kGifDefaultMaxWidth = 960;
// Rates offered below the timeline's. GIF delays are in hundredths of a second,
// so 24 fps plays back at 25; the rates here that divide 100 evenly are exact.
inline constexpr double kGifRates[] = { 30, 25, 20, 15, 12.5, 10 };

// The raw probe output, captured once per session: the binary is resolved once
// (ffmpegPath) and cannot change under us, while each probe is a process launch
// that cost the dialog a visible stall on every open. Probed on the first open
// (not at startup, so a session that never exports never runs ffmpeg), with the
// three in parallel.
struct FfmpegInfo {
    std::string version;
    std::string encoders;
    std::string formats;
};

// `exe` is always ExportDialog::ffmpegCmd(), itself fixed for the session.
const FfmpegInfo& ffmpegInfo(const std::string& exe);

} // namespace ExportFormats
