#pragma once

#include <Imath/half.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

// Video output to an external device (SDI card, NDI network stream, ...).
//
// The player composites its display-referred program image (post OCIO / grade /
// tech-check) into a GL-backed SDL texture. OutputManager reads that texture
// back off the GPU once per composited frame and hands the pixels to the active
// OutputDevice, which pushes them to its hardware/network target. Only NDI is
// implemented today; the interface is deliberately backend-agnostic so DeckLink
// and AJA can slot in as further OutputDevice subclasses.

// The HDR transfer function an scRGB program frame is destined for, i.e. which
// display encoding the player's colour pass decoded on the way in. `None` means the
// program is not a display-referred HDR rendering at all and only the 8-bit `rgba`
// form is meaningful. PQ is absolute — `OutputFrame::refWhiteNits` anchors it — while
// HLG is relative and ignores that value entirely.
enum class OutputTransfer { None, PQ, HLG };

// One display-referred program frame handed to an output device. Pixels are
// tightly packed, top row first (image order).
//
// Exactly one of the two pixel pointers is set. `rgba` is the SDR default (8-bit,
// display-encoded) that every device understands. `scRgb` is the HDR form: the
// untonemapped float program image, set only when the player's colour pass really
// produced an HDR (PQ or HLG) rendering AND the device asked for it via wantsHdr().
// A device that gets `scRgb` must encode it itself; see OutputNDI.cpp.
struct OutputFrame {
    const uint8_t* rgba = nullptr; // width * height * 4
    int width = 0;
    int height = 0;
    double fps = 0.0;              // timeline frame rate (for the device's frame timing)

    // HDR program pixels: width * height * 4 half, linear extended-range, Rec.709
    // primaries (scRGB), alpha unused. 1.0 is diffuse (paper) white and highlights run
    // above it. This is a straight readback of App::hdrProgramTex_ — the OCIO display
    // transform has already been applied and its display encoding decoded back to
    // linear light, so re-encoding with the matching transfer below reproduces the code
    // values OCIO authored.
    const Imath::half* scRgb = nullptr;
    OutputTransfer transfer = OutputTransfer::None;
    // Absolute luminance of scRGB 1.0, for PQ only: nits = value * refWhiteNits. It
    // must be the same value the colour pass divided by (HdrColorPass::begin) or the
    // pair stops cancelling and the transmitted signal is rescaled. Unused for HLG.
    float refWhiteNits = 100.0f;
};

// A single selectable output target. open() acquires the device, submit() pushes
// one program frame (called on the render thread), close() releases it.
class OutputDevice {
public:
    virtual ~OutputDevice() = default;
    virtual bool open() = 0;
    virtual void submit(const OutputFrame& frame) = 0;
    virtual void close() = 0;
    virtual std::string status() const { return {}; } // short line for the UI (e.g. connection count)
    // Whether this device can encode OutputFrame::scRgb. False means it only ever
    // sees `rgba`, and the player skips the float readback entirely.
    virtual bool wantsHdr() const { return false; }
};

// A backend compiled into the build. `available` reflects a runtime check (e.g.
// the NDI runtime DLL being present); an unavailable backend is still listed but
// cannot be selected. `create` spawns a fresh, unopened device.
struct OutputBackendInfo {
    std::string id;      // stable key ("ndi")
    std::string label;   // UI label ("NDI")
    bool        available = false;
    std::unique_ptr<OutputDevice> (*create)() = nullptr;
};

// Owns the GL readback resources and the currently-selected output device.
class OutputManager {
public:
    // Load the readback GL entry points from the renderer's GL context and build
    // the enumerable backend list. Returns false (and leaves output disabled) if
    // the renderer is not GL-backed; select()/present() then become no-ops.
    bool init(SDL_Renderer* renderer);

    const std::vector<OutputBackendInfo>& backends() const { return backends_; }

    // Switch the active output to backend `id` ("" = off). Closes any current
    // device, then opens the new one. Returns false if the backend is unknown,
    // unavailable, or fails to open (the output is left off in that case).
    bool select(const std::string& id);
    const std::string& activeId() const { return activeId_; }
    bool active() const { return device_ != nullptr; }
    std::string statusLine() const; // active device's status(), or "" when off

    // Read `programTex` (a w*h RGBA8 GL-backed SDL texture) back off the GPU and
    // submit it to the active device. No-op when no device is active or the GL
    // readback path is unavailable. Call once per newly-composited frame.
    void present(SDL_Texture* programTex, int w, int h, double fps);

    // Read `programTex` back to CPU (RGBA8, top row first) and return a pointer
    // into an internal buffer valid until the next readProgram() call, or nullptr
    // if the GL readback path is unavailable or the read failed. Lets one readback
    // feed several sinks (output device + the review-monitor window). Available
    // whenever the GL backend is present, independent of any output device.
    const uint8_t* readProgram(SDL_Texture* programTex, int w, int h);

    // The same read, confined to the w*h sub-rect of `tex` whose top-left texel is
    // (`x`, `y`) — for a probe that wants a handful of texels rather than the whole
    // frame. Kept in its own buffer so a probe read cannot clobber a full-frame read
    // still queued at an output device. The rect must lie inside the texture.
    const uint8_t* readProgramRect(SDL_Texture* tex, int x, int y, int w, int h);

    // Whether a readback path is usable: the GL path, or an external feed (the HDR
    // SDL_GPU path reads back itself and calls submitFrame directly).
    bool canRead() const { return glReady_ || externalFeed_; }

    // Mark that the caller supplies program pixels via submitFrame (HDR path), so a
    // device can be selected even without the GL readback path.
    void setExternalFeed(bool e) { externalFeed_ = e; }

    // Submit already-read RGBA8 pixels to the active device. No-op when off.
    void submitFrame(const uint8_t* rgba, int w, int h, double fps);

    // Whether the active device can take HDR pixels (see OutputDevice::wantsHdr).
    // The caller uses this to decide which readback to do, so it must be answerable
    // before any frame exists.
    bool wantsHdr() const;

    // Submit already-read scRGB half pixels (see OutputFrame::scRgb) to the active
    // device, to be encoded with `transfer`. No-op when off or when `transfer` is
    // None. Only call when wantsHdr() is true.
    void submitHdrFrame(const Imath::half* scRgb, int w, int h, double fps,
                        OutputTransfer transfer, float refWhiteNits);

    void shutdown(); // close the device and release GL resources

private:
    // GPU -> `dst` (RGBA8, top row first), from one sub-rect of `tex`.
    bool readback(SDL_Texture* tex, int x, int y, int w, int h, std::vector<uint8_t>& dst);

    SDL_Renderer* renderer_ = nullptr;
    bool          glReady_  = false;
    bool          externalFeed_ = false; // HDR path feeds submitFrame() itself
    unsigned      fbo_      = 0; // read framebuffer used to sample the program texture
    std::vector<uint8_t> pixels_;
    std::vector<uint8_t> rectPixels_; // readProgramRect's buffer (see above)

    std::vector<OutputBackendInfo> backends_;
    std::unique_ptr<OutputDevice>  device_;
    std::string                    activeId_;
};
