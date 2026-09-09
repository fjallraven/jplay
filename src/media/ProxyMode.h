#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// Process-wide state for the global proxy/full media-representation mode
// (the top-bar "Proxy" dropdown). Deliberately dependency-free — no Python/
// pybind11 — because Media.cpp, which consults this to decide what file to
// actually decode, links directly into command-line tools (createproject,
// createclips, ...) that never embed the interpreter. Only the jplay GUI
// target installs a real path resolver (PythonBridge.cpp's
// jplayResolveProxyPath, wired up in App::init()); every other binary sees
// resolveProxyPath() always return false and so always decodes the nominal
// path, unaffected by whatever mode string is set.
namespace jplay {

// The active mode's value; "" is the built-in "Full" mode (no substitution).
std::string currentProxyMode();
void setProxyMode(std::string mode);

// Frames of slate at the head of the file a mode substitutes: a published
// quicktime opens on a slate card that is not part of the shot, so its frame 0
// is the shot's frame `n`. Supplied per mode by the naming config
// (list_proxy_modes' "slate_frames"), pushed here alongside the mode, and added
// to every read index by the media that actually got a substitute — see
// Media::slateOffset(). 0 for "Full" and for any mode that declares none.
int64_t proxySlateFrames();
void setProxySlateFrames(int64_t n);

// Slate carried by the file at `path` itself, asked for a media that got NO
// substitute under the active mode. proxySlateFrames() above covers the
// substituted case, where the mode picked the file and so knows what is in front
// of it; a media whose own path is already a slated representation — a published
// quicktime added directly, by a tracker drop or a version jump, rather than
// reached through the quicktime mode — decodes that path nominally under every
// mode, including Full, and only the naming config can say how much of its head
// is slate. Answers 0 when no resolver is installed, i.e. in every binary but
// the GUI, exactly as resolveProxyPath does.
using SlateFramesResolver = std::function<int64_t(const std::string& path)>;
void setSlateFramesResolver(SlateFramesResolver resolver);
int64_t pathSlateFrames(const std::string& path);

// Bumped by every setProxyMode() call (even to the same value), so Media can
// cheaply detect "the active mode may have changed, recheck" without ever
// comparing mode strings on its hot (per-frame) read path.
uint64_t proxyGeneration();

// path: the nominal path or frame-pattern Media would otherwise open, exactly
// as passed to ImageSeq::open/VideoSource::open today.
// mode: the value most recently passed to setProxyMode, including "" for the
// built-in "Full" — a nominal path may itself be a proxy representation, so
// Full is a resolution too and not merely the absence of one. Returns true and
// fills outPath with the file to open instead; false (outPath left untouched)
// means "no substitute — decode the nominal path", e.g. this source has no
// proxy under the current mode, or under Full it already is the full-res one.
using ProxyPathResolver = std::function<bool(const std::string& path,
                                              const std::string& mode,
                                              std::string& outPath)>;
void setProxyPathResolver(ProxyPathResolver resolver);

// Resolves `path` for the current mode via the installed resolver. Returns
// false immediately (no resolver call) when no resolver is installed — which is
// every binary but the GUI, so those decode the nominal path in every mode.
bool resolveProxyPath(const std::string& path, std::string& outPath);

} // namespace jplay
