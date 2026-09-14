#include "PythonBridge.h"
#include "PythonCallbackRegistry.h"
#include "PythonStartup.h"
#include "Progress.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <SDL3/SDL_log.h>

#include <cstdio>

namespace py = pybind11;

namespace {

// A picker option's color as Python may give it: "#RRGGBB" / "RRGGBB", or an
// (r, g, b) sequence of three 0-255 ints, packed to 0xRRGGBB. Anything else —
// None, a bad string, a float tuple — yields 0, meaning "the view's own color":
// a mistyped color must cost the annotation, never the option list, so nothing
// here raises. Pure black packs to 0 and so reads as undecorated too.
uint32_t parsePickerColor(py::handle h) {
    if (py::isinstance<py::str>(h)) {
        std::string s = h.cast<std::string>();
        if (!s.empty() && s[0] == '#')
            s.erase(0, 1);
        if (s.size() != 6)
            return 0;
        uint32_t v = 0;
        for (char c : s) {
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = (v << 4) | (uint32_t)d;
        }
        return v;
    }
    if (py::isinstance<py::sequence>(h)) { // str is handled above
        py::sequence seq = py::reinterpret_borrow<py::sequence>(h);
        if (py::len(seq) != 3)
            return 0;
        uint32_t v = 0;
        for (int i = 0; i < 3; ++i) {
            if (!py::isinstance<py::int_>(seq[i]))
                return 0;
            int c = seq[i].cast<int>();
            v = (v << 8) | (uint32_t)(c < 0 ? 0 : (c > 255 ? 255 : c));
        }
        return v;
    }
    return 0;
}

// One "options" entry: a plain string (its own label, undecorated) or a dict
// carrying any of value / label / color / badge / badge_color. An empty or absent
// label falls back to the value, so the option always has something to draw. A
// badge of None or "" is no badge, which is how a decoration clears one.
PickerOption parsePickerOption(py::handle h) {
    PickerOption o;
    if (py::isinstance<py::dict>(h)) {
        py::dict d = py::reinterpret_borrow<py::dict>(h);
        if (d.contains("value"))
            o.value = py::str(d["value"]).cast<std::string>();
        if (d.contains("label"))
            o.label = py::str(d["label"]).cast<std::string>();
        if (d.contains("color"))
            o.color = parsePickerColor(d["color"]);
        if (d.contains("badge") && !d["badge"].is_none())
            o.badge = py::str(d["badge"]).cast<std::string>();
        if (d.contains("badge_color"))
            o.badgeColor = parsePickerColor(d["badge_color"]);
    } else {
        o.value = py::str(h).cast<std::string>();
    }
    if (o.label.empty())
        o.label = o.value;
    return o;
}

// One {key, current_index, options} dict. current_index is optional (decoration
// ignores it).
PickerState parsePickerState(py::handle item) {
    py::dict d = item.cast<py::dict>();
    PickerState s;
    s.key = d["key"].cast<std::string>();
    if (d.contains("current_index"))
        s.currentIndex = d["current_index"].cast<int>();
    if (d.contains("options"))
        for (py::handle o : d["options"])
            s.options.push_back(parsePickerOption(o));
    return s;
}

// The [{key, current_index, options}] shape both describe_pickers and
// decorate_pickers answer in.
void parsePickerStates(py::handle res, std::vector<PickerState>& out) {
    for (py::handle item : res)
        out.push_back(parsePickerState(item));
}

// One batch yielded by a streaming decorate_pickers: normally the same list of
// picker dicts the one-shot form returns, but a callback yielding one picker at
// a time naturally hands over the bare dict — and iterating that would walk its
// keys — so take it as a one-picker batch.
void parseDecorateBatch(py::handle item, std::vector<PickerState>& out) {
    if (py::isinstance<py::dict>(item))
        out.push_back(parsePickerState(item));
    else
        parsePickerStates(item, out);
}

// Fold a decorate result into the states the caller holds. Merged per (key,
// option value), taking the label, the color and the badge only — see the header. An option
// the callback didn't mention keeps what it had, which is what makes both a
// partial return and a single streamed batch legal. True if anything changed.
bool mergeDecorated(const std::vector<PickerState>& dec, std::vector<PickerState>& states) {
    bool changed = false;
    for (PickerState& s : states) {
        const PickerState* d = nullptr;
        for (const PickerState& x : dec)
            if (x.key == s.key) { d = &x; break; }
        if (!d)
            continue;
        for (PickerOption& o : s.options) {
            for (const PickerOption& n : d->options) {
                if (n.value != o.value)
                    continue;
                if (n.label != o.label) { o.label = n.label; changed = true; }
                if (n.color != o.color) { o.color = n.color; changed = true; }
                if (n.badge != o.badge) { o.badge = n.badge; changed = true; }
                if (n.badgeColor != o.badgeColor) { o.badgeColor = n.badgeColor; changed = true; }
                break;
            }
        }
    }
    return changed;
}

// The same shape back out, for decorate_pickers' second argument. Options are
// always normalised to dicts so the callback never has to handle the bare-string
// form describe_pickers is allowed to return.
py::list pickerStatesToPy(const std::vector<PickerState>& states) {
    auto colorToPy = [](uint32_t c) -> py::object {
        if (!c)
            return py::none();
        char buf[8];
        snprintf(buf, sizeof(buf), "#%06X", c & 0xFFFFFF);
        return py::str(buf);
    };
    py::list out;
    for (const PickerState& s : states) {
        py::list opts;
        for (const PickerOption& o : s.options) {
            py::dict od;
            od["value"] = o.value;
            od["label"] = o.label;
            od["color"] = colorToPy(o.color);
            od["badge"] = o.badge.empty() ? py::object(py::none()) : py::object(py::str(o.badge));
            od["badge_color"] = colorToPy(o.badgeColor);
            opts.append(od);
        }
        py::dict d;
        d["key"] = s.key;
        d["current_index"] = s.currentIndex;
        d["options"] = opts;
        out.append(d);
    }
    return out;
}

} // namespace

bool jplayListPickers(std::vector<PickerDef>& out) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("list_pickers");
        if (it == registry.end())
            return false;
        py::object res = it->second();
        for (py::handle item : res) {
            py::dict d = item.cast<py::dict>();
            PickerDef p;
            p.key   = d["key"].cast<std::string>();
            p.label = d["label"].cast<std::string>();
            p.kind  = d["kind"].cast<std::string>();
            // Optional: a site override predating the key omits it entirely.
            if (d.contains("multi_commit"))
                p.multiCommit = d["multi_commit"].cast<bool>();
            out.push_back(std::move(p));
        }
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] list_pickers failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] list_pickers failed: %s", e.what());
        return false;
    }
}

bool jplayListProxyModes(std::vector<ProxyModeOption>& out) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("list_proxy_modes");
        if (it == registry.end())
            return false;
        py::object res = it->second();
        for (py::handle item : res) {
            ProxyModeOption o;
            if (py::isinstance<py::str>(item)) {
                o.value = item.cast<std::string>();
                o.label = o.value;
            } else {
                py::dict d = item.cast<py::dict>();
                o.value = d["value"].cast<std::string>();
                o.label = d.contains("label") ? d["label"].cast<std::string>() : o.value;
                if (d.contains("slate_frames")) {
                    int64_t n = d["slate_frames"].cast<int64_t>();
                    o.slateFrames = n > 0 ? n : 0;
                }
                if (d.contains("default"))
                    o.isDefault = d["default"].cast<bool>();
            }
            out.push_back(std::move(o));
        }
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] list_proxy_modes failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] list_proxy_modes failed: %s", e.what());
        return false;
    }
}

bool jplayDescribePickers(const std::string& path, std::vector<PickerState>& out) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("describe_pickers");
        if (it == registry.end())
            return false;
        py::dict info;
        info["path"] = path;
        parsePickerStates(it->second(info), out);
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] describe_pickers failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] describe_pickers failed: %s", e.what());
        return false;
    }
}

bool jplayHasDecoratePickers() {
    // Cached: see the header. -1 = not answered yet.
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    if (!jplayPythonReady()) // asked too early to be the final answer — don't cache it
        return false;
    py::gil_scoped_acquire gil;
    cached = callbackRegistry().count("decorate_pickers") != 0 ? 1 : 0;
    return cached != 0;
}

bool jplayDecoratePickers(const std::string& path, std::vector<PickerState>& states,
                          const std::function<void(const std::vector<PickerState>&)>& onChunk,
                          const std::function<bool()>& cancelled) {
    if (!jplayPythonReady() || states.empty())
        return false;
    bool changed = false; // kept across a mid-stream raise: the batches already merged stand
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("decorate_pickers");
        if (it == registry.end())
            return false;
        py::dict info;
        info["path"] = path;
        py::object res = it->second(info, pickerStatesToPy(states));
        if (res.is_none())
            return false;

        // A generator (or any other iterator) is the streaming form. A list or
        // tuple is iterable but not an iterator, so the one-shot form below still
        // takes the whole result at once.
        if (PyIter_Check(res.ptr())) {
            for (;;) {
                // Polled here, before the resume below runs the callback on to its
                // next yield: a cancel between batches costs nothing, one during a
                // batch has to wait for that batch.
                if (cancelled && cancelled())
                    break;
                // PyIter_Send rather than PyIter_Next so the generator's RETURN
                // value survives — PyIter_Next discards it. It matters because a
                // `yield` anywhere makes the whole callback a generator, so a
                // site's `return states` early-out (dead database, nothing to
                // annotate) arrives this way and would otherwise decorate nothing
                // at all, silently. None is the ordinary "ran out" return.
                py::object item;
                bool last = false;
#if PY_VERSION_HEX >= 0x030A0000
                PyObject* raw = nullptr;
                const PySendResult r = PyIter_Send(res.ptr(), Py_None, &raw);
                if (r == PYGEN_ERROR)
                    throw py::error_already_set();
                item = py::reinterpret_steal<py::object>(raw);
                last = (r == PYGEN_RETURN);
#else
                // Python 3.9 has no PyIter_Send. Stepping the iterator by hand
                // gets at the same value: the RETURN rides on StopIteration.
                try {
                    item = res.attr("__next__")();
                } catch (const py::error_already_set& e) {
                    if (!e.matches(PyExc_StopIteration))
                        throw;
                    item = e.value().attr("value");
                    last = true;
                }
#endif
                if (last && item.is_none())
                    break;
                std::vector<PickerState> dec;
                parseDecorateBatch(item, dec);
                if (mergeDecorated(dec, states)) {
                    changed = true;
                    if (onChunk)
                        onChunk(states);
                }
                if (last)
                    break;
            }
            return changed;
        }

        std::vector<PickerState> dec;
        parsePickerStates(res, dec);
        return mergeDecorated(dec, states);
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] decorate_pickers failed: %s", e.what());
        return changed;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] decorate_pickers failed: %s", e.what());
        return changed;
    }
}

bool jplayGetPathValues(const std::string& path,
                        std::map<std::string, std::string>& out) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("get_path_values");
        if (it == registry.end())
            return false;
        py::dict info;
        info["path"] = path;
        py::object res = it->second(info);
        out = res.cast<std::map<std::string, std::string>>();
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] get_path_values failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] get_path_values failed: %s", e.what());
        return false;
    }
}

bool jplayGetPathValues(const std::vector<std::string>& paths,
                        std::vector<std::map<std::string, std::string>>& out) {
    out.assign(paths.size(), {});
    if (!jplayPythonReady() || paths.empty())
        return false;
    try {
        py::gil_scoped_acquire gil; // once for every path, not once per path
        const auto& registry = callbackRegistry();
        auto it = registry.find("get_path_values");
        if (it == registry.end())
            return false;
        for (size_t i = 0; i < paths.size(); ++i) {
            // Per-path guard: one unrecognised path must cost its own values only,
            // not every path queued behind it.
            try {
                py::dict info;
                info["path"] = paths[i];
                out[i] = it->second(info).cast<std::map<std::string, std::string>>();
            } catch (const py::error_already_set& e) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[jplay] get_path_values failed for %s: %s",
                             paths[i].c_str(), e.what());
            }
        }
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] get_path_values failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] get_path_values failed: %s", e.what());
        return false;
    }
}

bool jplayGetPathContext(const std::string& path, std::string& sequence,
                         std::string& shot, std::string& department) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("get_path_context");
        if (it == registry.end())
            return false;
        py::dict info;
        info["path"] = path;
        py::dict d = it->second(info).cast<py::dict>();
        auto str = [&](const char* key) -> std::string {
            return d.contains(key) ? d[py::str(key)].cast<std::string>() : std::string();
        };
        // get_path_context answers with the naming convention's own term,
        // "sequence" — callers cache it on Media under the key "scene" (jplay's
        // internal name for it, from OTIO scene_name), which is not what the
        // callback returns.
        sequence   = str("sequence");
        shot       = str("shot");
        department = str("department");
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] get_path_context failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] get_path_context failed: %s", e.what());
        return false;
    }
}

// Interpret an audio-callback result: a dict {"path", "offset"} or a
// (path, offset) sequence. The caller must hold the GIL. Returns false on
// None / missing path / empty path.
static bool parseAudioResult(const py::object& res, std::string& outPath, int64_t& outOffset) {
    if (res.is_none())
        return false;
    std::string p;
    int64_t off = 0;
    if (py::isinstance<py::dict>(res)) {
        py::dict d = res.cast<py::dict>();
        if (!d.contains("path"))
            return false;
        p = d["path"].cast<std::string>();
        if (d.contains("offset"))
            off = d["offset"].cast<int64_t>();
    } else {
        // (path, offset) tuple/list.
        py::sequence seq = res.cast<py::sequence>();
        if (py::len(seq) < 1)
            return false;
        p = seq[0].cast<std::string>();
        if (py::len(seq) >= 2)
            off = seq[1].cast<int64_t>();
    }
    if (p.empty())
        return false;
    outPath = std::move(p);
    outOffset = off;
    return true;
}

// Invoke a registered audio callback by name (query_audio / derive_audio),
// passing {"path": path} and parsing the result. Shared by jplayQueryAudio and
// jplayDeriveAudio. `allowScan` adds {"allow_scan": true} for the latter.
static bool invokeAudioCallback(const char* name, const std::string& path,
                                std::string& outPath, int64_t& outOffset,
                                bool allowScan = false) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find(name);
        if (it == registry.end())
            return false;
        py::dict info;
        info["path"] = path;
        if (allowScan)
            info["allow_scan"] = true;
        py::object res = it->second(info);
        return parseAudioResult(res, outPath, outOffset);
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] %s failed: %s", name, e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[jplay] %s failed: %s", name, e.what());
        return false;
    }
}

bool jplayQueryAudio(const std::string& path, std::string& outPath, int64_t& outOffset) {
    return invokeAudioCallback("query_audio", path, outPath, outOffset);
}

bool jplayDeriveAudio(const std::string& path, std::string& outPath, int64_t& outOffset,
                      bool allowScan) {
    return invokeAudioCallback("derive_audio", path, outPath, outOffset, allowScan);
}

// True when `fn` declares a parameter called `progress`, so the host may hand it
// its ProgressReporter. Callables inspect.signature() cannot introspect are
// treated as not accepting one (call them the old way rather than risk a
// TypeError that would be indistinguishable from one raised inside the callback).
static bool acceptsProgressArg(const py::object& fn) {
    try {
        py::object sig = py::module_::import("inspect").attr("signature")(fn);
        return py::cast<bool>(sig.attr("parameters").attr("__contains__")("progress"));
    } catch (const py::error_already_set&) {
        return false;
    }
}

bool jplayInspectDirectory(const std::string& root, std::vector<DiscoveredShot>& out,
                           ProgressReporter* progress) {
    if (!jplayPythonReady()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[jplay] create_from_directory: interpreter not ready -> abort");
        return false;
    }
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("create_from_directory");
        if (it == registry.end()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[jplay] create_from_directory: no such callback registered by "
                        "jplay_init.py -> abort");
            return false;
        }
        // inspect_directory takes the root path positionally and returns a list
        // of dicts, one per shot. A callback that declares `progress` also gets
        // the host's channel, so a long walk can report and be cancelled.
        py::object res;
        if (progress && acceptsProgressArg(it->second)) {
            res = it->second(root, py::arg("progress") =
                                       py::cast(progress, py::return_value_policy::reference));
        } else {
            if (progress)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[jplay] create_from_directory takes no progress= argument; "
                            "the walk cannot report or be cancelled");
            res = it->second(root);
        }
        if (progress && progress->cancelled()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[jplay] create_from_directory(\"%s\") cancelled -> abort",
                        root.c_str());
            return false;
        }
        if (res.is_none()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[jplay] create_from_directory(\"%s\") returned None -> abort",
                        root.c_str());
            return false;
        }
        const size_t before = out.size();
        for (py::handle item : res) {
            py::dict d = item.cast<py::dict>();
            auto str = [&](const char* key) -> std::string {
                return d.contains(key) ? d[py::str(key)].cast<std::string>()
                                       : std::string();
            };
            DiscoveredShot discovered;
            discovered.sequence   = str("sequence");
            discovered.shot       = str("shot");
            discovered.department = str("department");
            discovered.asset      = str("asset");
            discovered.version    = str("version");
            discovered.path       = str("path");
            out.push_back(std::move(discovered));
        }
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] create_from_directory failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] create_from_directory failed: %s", e.what());
        return false;
    }
}

bool jplayResolveDropText(const std::string& text, const std::string& proxyMode,
                          DropResolution& out, ProgressReporter* progress) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("resolve_drop_text");
        if (it == registry.end())
            return false; // no site claims text drops; the payload is not ours
        // resolve_drop_text(text, proxy_mode) returns None for a payload it does
        // not handle, else {"sequence": str|None, "paths": [str, ...]}. A callback
        // that declares `progress` also gets the host's reporter, so a long list
        // lookup can be followed and cancelled; one that does not is called with
        // the two positional arguments alone.
        py::object res = progress && acceptsProgressArg(it->second)
                             ? it->second(text, proxyMode,
                                          py::arg("progress") =
                                              py::cast(progress,
                                                       py::return_value_policy::reference))
                             : it->second(text, proxyMode);
        if (res.is_none())
            return false;
        py::dict d = res.cast<py::dict>();
        if (d.contains("sequence") && !d["sequence"].is_none())
            out.sequence = d["sequence"].cast<std::string>();
        if (d.contains("paths") && !d["paths"].is_none())
            for (py::handle item : d["paths"])
                out.paths.push_back(item.cast<std::string>());
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] resolve_drop_text failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] resolve_drop_text failed: %s", e.what());
        return false;
    }
}

bool jplayProjectPathFromMedia(const std::string& path, std::string& outPath) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("get_project_path_from_media");
        if (it == registry.end())
            return false;
        // get_project_path_from_media(media_info) returns the project's .jpproj
        // path, else its .otio, else "" / None when neither is published.
        py::dict info;
        info["path"] = path;
        py::object res = it->second(info);
        if (res.is_none())
            return false;
        outPath = res.cast<std::string>();
        return !outPath.empty();
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] get_project_path_from_media failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] get_project_path_from_media failed: %s", e.what());
        return false;
    }
}

bool jplayResolvePath(const std::string& path, const std::string& key,
                      const std::string& value, std::string& outPath) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("resolve_path");
        if (it == registry.end())
            return false;
        py::dict info;
        info["path"] = path;
        // resolve_path(media_info, key, value): the changed picker and its value.
        py::object res = it->second(info, key, value);
        if (res.is_none())
            return false;
        outPath = res.cast<std::string>();
        return !outPath.empty();
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] resolve_path failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] resolve_path failed: %s", e.what());
        return false;
    }
}

bool jplayResolveProxyPath(const std::string& path, const std::string& mode,
                           std::string& outPath) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("resolve_proxy_path");
        if (it == registry.end())
            return false;
        py::object res = it->second(path, mode);
        if (res.is_none())
            return false;
        outPath = res.cast<std::string>();
        return !outPath.empty();
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] resolve_proxy_path failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] resolve_proxy_path failed: %s", e.what());
        return false;
    }
}

int64_t jplayPathSlateFrames(const std::string& path) {
    if (!jplayPythonReady())
        return 0;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto pathMode = registry.find("proxy_path_mode");
        auto modes = registry.find("list_proxy_modes");
        if (pathMode == registry.end() || modes == registry.end())
            return 0;
        py::object cls = pathMode->second(path);
        if (cls.is_none())
            return 0;
        // "" is the full-res representation, which carries no slate, and is also
        // the host's own Full entry -- never listed by list_proxy_modes, so the
        // scan below would not find it anyway.
        const std::string mode = cls.cast<std::string>();
        if (mode.empty())
            return 0;
        for (py::handle item : modes->second()) {
            if (py::isinstance<py::str>(item))
                continue; // a bare mode value declares no slate
            py::dict d = item.cast<py::dict>();
            if (d["value"].cast<std::string>() != mode || !d.contains("slate_frames"))
                continue;
            const int64_t n = d["slate_frames"].cast<int64_t>();
            return n > 0 ? n : 0;
        }
        return 0;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] slate lookup failed: %s", e.what());
        return 0;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] slate lookup failed: %s", e.what());
        return 0;
    }
}

bool jplayProxyPathMode(const std::string& path, std::string& outMode) {
    if (!jplayPythonReady())
        return false;
    try {
        py::gil_scoped_acquire gil;
        const auto& registry = callbackRegistry();
        auto it = registry.find("proxy_path_mode");
        if (it == registry.end())
            return false;
        py::object res = it->second(path);
        if (res.is_none())
            return false;
        // "" is a real answer here (the full-res representation), unlike in
        // resolve_proxy_path where it would mean "no substitute".
        outMode = res.cast<std::string>();
        return true;
    } catch (const py::error_already_set& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] proxy_path_mode failed: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[jplay] proxy_path_mode failed: %s", e.what());
        return false;
    }
}
