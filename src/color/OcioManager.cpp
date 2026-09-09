#include "OcioManager.h"

#include "HdrColorPass.h" // jplayDebugLogging
#include "Preferences.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <regex>
#include <utility>
#include <vector>

static OCIO::ConstConfigRcPtr loadBuiltinConfig() {
    // Try known built-in CG config names across OCIO 2.x releases (newest first).
    // Each is tried in order; the first one that loads wins.
    static const char* const kNames[] = {
        "cg-config-v2.2.0_aces-v1.3_ocio-v2.3",  // OCIO 2.3+
        "cg-config-v2.1.0_aces-v1.3_ocio-v2.1",  // OCIO 2.1-2.2
        "studio-config-v2.1.0_aces-v1.3_ocio-v2.3",
        "studio-config-v1.0.0_aces-v1.3_ocio-v2.1",
    };
    for (const char* name : kNames) {
        try {
            auto cfg = OCIO::Config::CreateFromBuiltinConfig(name);
            if (cfg) return cfg;
        } catch (...) {}
    }
    return nullptr;
}

// Translate a Python-style regex (which may carry (?P<name>...) named groups)
// into an ECMAScript pattern std::regex accepts (plain numbered groups), while
// recording each name's 1-based capture index in `names`. Non-capturing groups
// and lookarounds ((?:...), (?=...), ...) and escapes are passed through
// untouched. std::regex has no named-group support, so this bridges the gap.
static std::regex translateNamedRegex(const std::string& pat,
                                      std::map<std::string, int>& names) {
    std::string out;
    out.reserve(pat.size());
    int group = 0;
    bool inClass = false; // inside a [...] character class, where '(' is literal
    for (size_t i = 0; i < pat.size(); ++i) {
        char c = pat[i];
        if (c == '\\' && i + 1 < pat.size()) {
            out += c;
            out += pat[++i];
            continue;
        }
        if (inClass) {
            out += c;
            if (c == ']') inClass = false;
            continue;
        }
        if (c == '[') { inClass = true; out += c; continue; }
        if (c == '(') {
            if (pat.compare(i, 4, "(?P<") == 0) {
                size_t close = pat.find('>', i + 4);
                if (close != std::string::npos) {
                    names[pat.substr(i + 4, close - (i + 4))] = ++group;
                    out += '(';
                    i = close; // skip the name and its '>'
                    continue;
                }
            }
            if (pat.compare(i, 2, "(?") == 0) {
                out += c; // non-capturing / lookaround / flags: not a group
                continue;
            }
            ++group; // plain capturing group
        }
        out += c;
    }
    return std::regex(out);
}

// Replace {name} placeholders in `tmpl` with the corresponding captured group.
static std::string expandTemplate(const std::string& tmpl, const std::smatch& m,
                                  const std::map<std::string, int>& names) {
    std::string out;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '{') {
            size_t close = tmpl.find('}', i);
            if (close != std::string::npos) {
                auto it = names.find(tmpl.substr(i + 1, close - i - 1));
                if (it != names.end() && it->second < (int)m.size())
                    out += m[it->second].str();
                i = close;
                continue;
            }
        }
        out += tmpl[i];
    }
    return out;
}

// Derive an OCIO config path for `samplePath` from jplay_preferences.conf's
// [ocio] section, or "" if no rule applies (empty template/regex, no match).
static std::string resolveOcioFromPrefs(const std::string& samplePath) {
    std::string tmpl = Preferences::get("ocio", "ocio");
    std::string sourceRegex = Preferences::get("ocio", "source_regex");
    if (tmpl.empty() || sourceRegex.empty() || samplePath.empty())
        return {};
    try {
        std::map<std::string, int> names;
        std::regex re = translateNamedRegex(sourceRegex, names);
        std::string path = samplePath;
        size_t s = path.find_first_not_of('/');
        if (s != std::string::npos && s > 0) path = path.substr(s);
        std::smatch m;
        if (std::regex_search(path, m, re))
            return expandTemplate(tmpl, m, names);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OCIO prefs: bad source_regex: %s\n", e.what());
    }
    return {};
}

// The [ocio] context entry of jplay_preferences.conf, parsed into (variable,
// template) pairs: comma-separated VAR=template, each template carrying {key}
// placeholders naming naming-config metadata keys ("SCENE={scene},SHOT={shot}").
// Parsed once — the preferences file is itself parsed and cached once — so the
// per-frame cost of setContextForMedia is the substitution alone.
static const std::vector<std::pair<std::string, std::string>>& contextTemplates() {
    static const std::vector<std::pair<std::string, std::string>> tmpls = [] {
        auto trim = [](const std::string& s) {
            size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) return std::string{};
            return s.substr(b, s.find_last_not_of(" \t") - b + 1);
        };
        std::vector<std::pair<std::string, std::string>> out;
        const std::string spec = Preferences::get("ocio", "context");
        for (size_t i = 0; i < spec.size();) {
            size_t comma = spec.find(',', i);
            std::string entry = spec.substr(i, comma == std::string::npos ? comma : comma - i);
            i = (comma == std::string::npos) ? spec.size() : comma + 1;
            size_t eq = entry.find('=');
            if (eq == std::string::npos)
                continue; // not a VAR=template pair
            std::string var = trim(entry.substr(0, eq));
            std::string tmpl = trim(entry.substr(eq + 1));
            if (!var.empty() && !tmpl.empty())
                out.emplace_back(std::move(var), std::move(tmpl));
        }
        return out;
    }();
    return tmpls;
}

// Expand one context template against a media's naming metadata. Returns false —
// leaving the variable unset — as soon as a {key} the media carries no value for is
// hit, so a variable is either fully resolved or not set at all.
static bool expandContextValue(const std::string& tmpl,
                               const std::map<std::string, std::string>& meta,
                               std::string& out) {
    out.clear();
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '{') {
            size_t close = tmpl.find('}', i);
            if (close != std::string::npos) {
                auto it = meta.find(tmpl.substr(i + 1, close - i - 1));
                if (it == meta.end() || it->second.empty())
                    return false;
                out += it->second;
                i = close;
                continue;
            }
        }
        out += tmpl[i];
    }
    return true;
}

// The (working -> display, view) transform, with `look` applied first when set.
// The media colour space is not part of it: every source reaches the working space
// through its own input transform first, so this half is shared by all of them.
//
// A free function because the prewarm job builds the same transform from copies,
// off the main thread, where it can touch no members (see warmJobForMedia).
static OCIO::ConstTransformRcPtr makeDisplayTransform(const std::string& display,
                                                      const std::string& view,
                                                      const std::string& look,
                                                      const std::string& workingCS) {
    // The working space the display transform reads. A config with no scene_linear
    // role leaves this empty and the role name stands in, which OCIO resolves
    // internally (and errors on, visibly, if the config really has neither).
    const char* srcCS = workingCS.empty() ? OCIO::ROLE_SCENE_LINEAR : workingCS.c_str();

    OCIO::DisplayViewTransformRcPtr dvt = OCIO::DisplayViewTransform::Create();
    dvt->setSrc(srcCS);
    dvt->setDisplay(display.c_str());
    dvt->setView(view.c_str());
    if (look.empty())
        return dvt;

    // Chain: apply the named Look in the working space, then the display transform.
    // LookTransform converts to the look process space internally.
    OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();
    OCIO::LookTransformRcPtr lt = OCIO::LookTransform::Create();
    lt->setSrc(srcCS);
    lt->setDst(srcCS);
    lt->setLooks(look.c_str());
    group->appendTransform(lt);
    group->appendTransform(dvt);
    return group;
}

// `config`'s own context — its environment and search paths — with the resolved
// context variables layered on top. A variable we do not set keeps whatever the
// config declared for it.
static OCIO::ConstContextRcPtr makeContext(const OCIO::ConstConfigRcPtr& config,
                                           const std::map<std::string, std::string>& vars) {
    OCIO::ConstContextRcPtr base = config->getCurrentContext();
    if (vars.empty())
        return base;
    OCIO::ContextRcPtr ctx = base->createEditableCopy();
    for (const auto& kv : vars)
        ctx->setStringVar(kv.first.c_str(), kv.second.c_str());
    return ctx;
}

void OcioManager::resolveEnv(const std::string& samplePath) {
    // $OCIO already set — externally, or by an earlier call — is the whole guard, so
    // this stays repeatable: reinitOcioForFirstSource() calls init() again once real
    // media names a path the [ocio] rules can match, and must still get its chance.
    const char* env = std::getenv("OCIO");
    // Whether $OCIO came from outside can only be known before we write it ourselves,
    // so it is settled on the first call and remembered; setActiveConfigForPath needs
    // it to tell a session-wide override from our own preferences resolution.
    if (!envProbed_) {
        envProbed_ = true;
        envPreset_ = (env && env[0]);
    }
    if (env && env[0])
        return;
    // $OCIO unset: try to derive a config path from the preferences file.
    std::string resolved = resolveOcioFromPrefs(samplePath);
    if (resolved.empty())
        return;
#ifdef _WIN32
    _putenv_s("OCIO", resolved.c_str());
#else
    setenv("OCIO", resolved.c_str(), /*overwrite=*/1);
#endif
}

// A preferred view replaces a config's default only when the active display lists
// it: two configs seldom name their views alike, and a view the display does not
// offer builds no processor at all, which would leave the image untransformed.
bool OcioManager::applyPreferredView_() {
    if (!config_ || preferredView_.empty() || display_.empty() || view_ == preferredView_)
        return false;
    for (const std::string& v : views(display_)) {
        if (v == preferredView_) {
            view_ = v;
            return true;
        }
    }
    return false;
}

void OcioManager::setPreferredView(const std::string& v) {
    preferredView_ = v;
    if (applyPreferredView_())
        markDirty_();
}

void OcioManager::applyConfigDefaults_() {
    const char* disp = config_->getDefaultDisplay();
    display_ = disp ? disp : "";
    const char* view = display_.empty() ? nullptr : config_->getDefaultView(display_.c_str());
    view_ = view ? view : "";
    applyPreferredView_(); // the project's view, or the one last picked, over the default
    look_ = "";
    // The working space every media is brought into before the display transform,
    // resolved from the scene_linear role to a concrete colour space name. Left
    // empty when the config declares no such role, which the pipeline reads as
    // "no working space" and degenerates gracefully.
    auto sceneCS = config_->getColorSpace(OCIO::ROLE_SCENE_LINEAR);
    workingCS_ = sceneCS && sceneCS->getName() ? sceneCS->getName() : "";
}

bool OcioManager::prefersOcio() {
    std::string v = Preferences::get("color_management", "color_pipeline");
    for (char& c : v)
        c = (char)std::tolower((unsigned char)c);
    return v == "ocio";
}

bool OcioManager::init(const std::string& samplePath) {
    try {
        resolveEnv(samplePath);
        const char* env = std::getenv("OCIO");
        if (env && env[0]) {
            config_ = OCIO::Config::CreateFromFile(env);
            usingBuiltin_ = false;
            activeConfigPath_ = env;
        } else {
            config_ = loadBuiltinConfig();
            usingBuiltin_ = true;
            activeConfigPath_.clear();
        }
        if (!config_) {
            std::fprintf(stderr, "OCIO: no config found (set $OCIO or upgrade OCIO)\n");
            return false;
        }
        // Which config the session actually came up on is the first thing wanted when
        // an image grades wrong, and the derivation ($OCIO vs the [ocio] regex against
        // the first source) is not visible from anywhere else.
        if (jplayDebugLogging()) {
            if (usingBuiltin_)
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "OCIO: no $OCIO and no [ocio] match for '%s' - using the built-in config",
                            samplePath.c_str());
            else
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OCIO: loaded %s (%s)",
                            activeConfigPath_.c_str(),
                            envPreset_ ? "$OCIO" : "derived from [ocio] preferences");
        }
        configCache_[activeConfigPath_] = config_;
        applyConfigDefaults_();
        markDirty_();
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OCIO init: %s\n", e.what());
        config_ = nullptr;
        return false;
    }
}

bool OcioManager::setActiveConfigForPath(const std::string& mediaPath) {
    if (envPreset_ || !config_ || mediaPath.empty())
        return false;
    // The resolution is a regex match, so it is skipped while the playhead stays on
    // the same source; the path only changes at a clip boundary. Set before any of
    // the failure exits below, so an unresolvable source is not retried every frame.
    if (mediaPath == lastResolvedMedia_)
        return false;
    lastResolvedMedia_ = mediaPath;

    std::string path = resolveOcioFromPrefs(mediaPath);
    if (path.empty() || path == activeConfigPath_)
        return false;

    OCIO::ConstConfigRcPtr cfg;
    auto cached = configCache_.find(path);
    const bool fromCache = cached != configCache_.end();
    if (fromCache) {
        cfg = cached->second; // may be null: a previous load of this path failed
    } else {
        try {
            cfg = OCIO::Config::CreateFromFile(path.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "OCIO: %s: %s\n", path.c_str(), e.what());
        }
        configCache_[path] = cfg; // null included, so a bad path is opened once only
    }
    if (!cfg)
        return false; // keep the config we have rather than losing the transform

    // A config switch mid-timeline is silent otherwise, and it changes every colour
    // space name the media resolve against, so it is worth a line of its own.
    if (jplayDebugLogging())
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "OCIO: %s %s for %s",
                    fromCache ? "reusing" : "loaded", path.c_str(), mediaPath.c_str());

    selections_[activeConfigPath_] = { display_, view_, look_ };
    config_ = cfg;
    activeConfigPath_ = path;
    usingBuiltin_ = false;
    auto sel = selections_.find(path);
    if (sel != selections_.end()) {
        display_ = sel->second.display;
        view_    = sel->second.view;
        look_    = sel->second.look;
        // The working space is a property of the config, not of the selection, so it
        // is re-resolved even when a stored selection is restored — the two configs
        // need not agree on what scene_linear names.
        auto sceneCS = config_->getColorSpace(OCIO::ROLE_SCENE_LINEAR);
        workingCS_ = sceneCS && sceneCS->getName() ? sceneCS->getName() : "";
    } else {
        applyConfigDefaults_();
    }
    // The rebuild this triggers bumps version(), which is what makes OcioGpu and
    // HdrColorPass discard the shader and LUTs they built from the previous config.
    markDirty_();
    return true;
}

// The context variables one media resolves, dropping every entry whose template
// names a key the media has no value for.
static std::map<std::string, std::string> resolveContextVars(
        const std::map<std::string, std::string>& meta) {
    std::map<std::string, std::string> vars;
    for (const auto& t : contextTemplates()) {
        std::string value;
        if (expandContextValue(t.second, meta, value))
            vars[t.first] = std::move(value);
    }
    return vars;
}

bool OcioManager::setContextForMedia(const std::map<std::string, std::string>& meta) {
    if (contextTemplates().empty())
        return false;
    std::map<std::string, std::string> vars = resolveContextVars(meta);
    // Comparing the resolved variables rather than short-circuiting on the media
    // identity keeps this correct when metadata lands late: an OTIO import's media is
    // tagged asynchronously (see tagMediaPathValues), so the same source resolves
    // nothing on one frame and SCENE/SHOT on a later one.
    if (vars == contextVars_)
        return false;
    contextVars_ = std::move(vars);
    markDirty_();
    return true;
}

std::function<void()> OcioManager::warmJobForMedia(const std::string& mediaPath,
                                                   const std::map<std::string, std::string>& meta) {
    if (contextTemplates().empty() || !config_)
        return {};
    // A source from another show wants a different config, which this does not load:
    // that is the pre-existing per-source config switch, and warming this config for
    // variables it will never be asked for would only fill the cache with junk.
    if (!envPreset_) {
        std::string cfgPath = resolveOcioFromPrefs(mediaPath);
        if (!cfgPath.empty() && cfgPath != activeConfigPath_)
            return {};
    }
    std::map<std::string, std::string> vars = resolveContextVars(meta);
    if (vars == contextVars_)
        return {}; // already the live transform
    if (display_.empty() || view_.empty())
        return {};
    // The warmed transform is the selection as well as the variables, so changing
    // display / view / look / working space re-warms rather than trusting a job that
    // built a transform nobody will ask for again.
    std::string key = activeConfigPath_ + '\n' + display_ + '\n' + view_ + '\n' +
                      look_ + '\n' + workingCS_;
    for (const auto& kv : vars)
        key += '\n' + kv.first + '=' + kv.second;
    if (key == warmedKey_)
        return {}; // a job for exactly this transform has already been issued
    warmedKey_ = std::move(key);

    // Copies of everything the build touches, so the job races nothing the main
    // thread may change under it — the config handle is shared_ptr-owned and OCIO's
    // caches are internally locked, which is what makes this safe to run off-thread.
    OCIO::ConstConfigRcPtr cfg = config_;
    std::string display = display_, view = view_, look = look_, working = workingCS_;
    return [cfg, display, view, look, working, vars] {
        try {
            // The processor is discarded: the point is the LUT files it pulled in and
            // the processor itself, both now in OCIO's caches, so the rebuild this
            // media triggers at the cut is a cache hit instead of a disk read.
            cfg->getProcessor(makeContext(cfg, vars),
                              makeDisplayTransform(display, view, look, working),
                              OCIO::TRANSFORM_DIR_FORWARD);
        } catch (const std::exception& e) {
            // Warming is best-effort: the real build reports its own failure.
            std::fprintf(stderr, "OCIO prewarm: %s\n", e.what());
        }
    };
}

// ── Input colour space resolution ────────────────────────────────────────────

static std::string lowered(const std::string& s) {
    std::string out(s);
    for (char& c : out)
        c = (char)std::tolower((unsigned char)c);
    return out;
}

// The config's colour space named `name`, resolving aliases and roles; "" when the
// config has nothing by that name. Returns a copy — the name belongs to a colour
// space held only by the temporary handle here.
static std::string lookupColorSpace(const OCIO::ConstConfigRcPtr& cfg, const char* name) {
    if (!cfg || !name || !*name) return {};
    try {
        auto cs = cfg->getColorSpace(name);
        if (cs && cs->getName()) return cs->getName();
    } catch (...) {}
    return {};
}

// First colour space whose name or any alias contains every token, matched
// case-insensitively. The exact-name lists below come from the ACES and OCIO
// built-in configs; this is the net that catches a studio config spelling the
// same space its own way, which most of them do.
static std::string searchColorSpace(const OCIO::ConstConfigRcPtr& cfg,
                                    std::initializer_list<const char*> tokens) {
    if (!cfg || tokens.size() == 0) return {};
    try {
        const int n = cfg->getNumColorSpaces();
        for (int i = 0; i < n; ++i) {
            const char* name = cfg->getColorSpaceNameByIndex(i);
            if (!name) continue;
            std::string hay = lowered(name);
            if (auto cs = cfg->getColorSpace(name)) {
                for (size_t a = 0, na = cs->getNumAliases(); a < na; ++a) {
                    hay += ' ';
                    hay += lowered(cs->getAlias(a));
                }
            }
            bool all = true;
            for (const char* t : tokens) {
                if (hay.find(t) == std::string::npos) { all = false; break; }
            }
            if (all) return name;
        }
    } catch (...) {}
    return {};
}

std::string OcioManager::colorSpaceForMedia(const std::string& path,
                                            const SourceColorTags& tags) const {
    if (!config_) return {};

    // Exact names / aliases first, then the token search.
    auto pick = [this](std::initializer_list<const char*> names,
                       std::initializer_list<const char*> tokens) -> std::string {
        for (const char* n : names) {
            std::string cs = lookupColorSpace(config_, n);
            if (!cs.empty()) return cs;
        }
        return searchColorSpace(config_, tokens);
    };

    // 1. File rules, when they say something specific. filepathOnlyMatchesDefaultRule
    //    reports the catch-all case, which is exactly where a stream's own tags are
    //    worth more than the config's blanket answer.
    try {
        if (!path.empty() && !config_->filepathOnlyMatchesDefaultRule(path.c_str())) {
            const char* cs = config_->getColorSpaceFromFilepath(path.c_str());
            if (cs && *cs) return cs;
        }
    } catch (...) {}

    // 2. Scene-referred float. Kept ahead of the tag inspection (it has no tags) and
    //    resolved through the role, which is what the File Colorspace menu has always
    //    defaulted to — so EXR keeps behaving exactly as it did.
    if (tags.sceneLinear) {
        std::string cs = lookupColorSpace(config_, OCIO::ROLE_SCENE_LINEAR);
        if (!cs.empty()) return cs;
    }

    // 3. The config's own answer for video, when it states one. `default_video` is
    //    no OCIO built-in role but a studio convention (a config declaring it tends
    //    to declare default_r3d_log and friends beside it) for naming the space a
    //    show's published quicktimes are written in. A config that declares it knows
    //    more about its own dailies than any heuristic below can, so it wins — except
    //    against PQ and HLG, where the transfer tag describes this particular file
    //    rather than the show's convention, and reading an HDR master as SDR is not a
    //    subtle error.
    const std::string trc = lowered(tags.transfer);
    const bool hdrTrc = (trc == "smpte2084" || trc == "arib-std-b67");
    if (tags.video && !hdrTrc) {
        std::string cs = lookupColorSpace(config_, "default_video");
        if (!cs.empty()) return cs;
    }

    // 4. The container's transfer/primaries tags. A tagged transfer is hard evidence:
    //    PQ and HLG in particular are nothing like the sRGB assumption below, and
    //    getting them wrong is the difference between a watchable image and a
    //    blown-out one.
    const bool wide = lowered(tags.primaries) == "bt2020";
    std::string hit;
    if (trc == "smpte2084")
        hit = pick({ "rec2100_pq_display", "Rec.2100-PQ - Display", "Rec.2100-PQ" },
                   { "2100", "pq" });
    else if (trc == "arib-std-b67")
        hit = pick({ "rec2100_hlg_display", "Rec.2100-HLG - Display", "Rec.2100-HLG" },
                   { "2100", "hlg" });
    else if (trc == "linear")
        hit = lookupColorSpace(config_, OCIO::ROLE_SCENE_LINEAR);
    else if (trc == "iec61966-2-1")
        hit = pick({ "srgb_tx", "sRGB - Texture", "Utility - sRGB - Texture", "sRGB" },
                   { "srgb", "texture" });
    else if (!trc.empty() && wide)
        hit = pick({ "rec1886_rec2020_display", "Rec.1886 Rec.2020 - Display" },
                   { "1886", "2020" });
    else if (!trc.empty())
        // bt709, smpte170m, bt470bg, bt2020-10/12: camera-encoded video, reviewed
        // through the Rec.1886 EOTF, which is the convention every review tool uses.
        hit = pick({ "rec1886_rec709_display", "Rec.1886 Rec.709 - Display",
                     "Rec.709 - Display" },
                   { "1886", "709" });
    if (!hit.empty()) return hit;

    // 5. Untagged display-referred media: sRGB by convention.
    hit = pick({ "srgb_tx", "sRGB - Texture", "Utility - sRGB - Texture", "sRGB" },
               { "srgb", "texture" });
    if (hit.empty()) hit = searchColorSpace(config_, { "srgb" });
    if (!hit.empty()) return hit;

    // 6. Whatever the config itself would fall back to.
    try {
        if (!path.empty()) {
            const char* cs = config_->getColorSpaceFromFilepath(path.c_str());
            if (cs && *cs) return cs;
        }
    } catch (...) {}
    return lookupColorSpace(config_, OCIO::ROLE_DEFAULT);
}

std::vector<std::string> OcioManager::displays() const {
    std::vector<std::string> out;
    if (!config_) return out;
    int n = config_->getNumDisplays();
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const char* d = config_->getDisplay(i);
        if (d) out.emplace_back(d);
    }
    return out;
}

std::vector<std::string> OcioManager::views(const std::string& display) const {
    std::vector<std::string> out;
    if (!config_ || display.empty()) return out;
    int n = config_->getNumViews(display.c_str());
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const char* v = config_->getView(display.c_str(), i);
        if (v) out.emplace_back(v);
    }
    return out;
}

std::vector<std::string> OcioManager::looks() const {
    std::vector<std::string> out;
    if (!config_) return out;
    int n = config_->getNumLooks();
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const char* l = config_->getLookNameByIndex(i);
        if (l) out.emplace_back(l);
    }
    return out;
}

std::vector<std::string> OcioManager::colorSpaces() const {
    std::vector<std::string> out;
    if (!config_) return out;
    int n = config_->getNumColorSpaces();
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const char* cs = config_->getColorSpaceNameByIndex(i);
        if (cs) out.emplace_back(cs);
    }
    return out;
}

void OcioManager::setDisplay(const std::string& d) {
    display_ = d;
    // Reset view to the new display's default; keep look as-is.
    const char* defView = config_ ? config_->getDefaultView(d.c_str()) : nullptr;
    view_ = defView ? defView : "";
    markDirty_();
}

std::string OcioManager::sdrDisplay() const {
    if (!config_) return {};
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    for (const std::string& d : displays())
        if (lower(d).find("srgb") != std::string::npos) return d;
    // Studio configs that ship no sRGB display (GUI on a Rec.1886 monitor, say)
    // still have a default, which is by convention their SDR rendering.
    const char* def = config_->getDefaultDisplay();
    return def ? def : std::string{};
}

OCIO::ConstProcessorRcPtr OcioManager::buildDisplayProcessor_(const std::string& display,
                                                              const std::string& view) const {
    if (!config_ || display.empty() || view.empty()) return nullptr;
    try {
        return config_->getProcessor(makeContext(config_, contextVars_),
                                     makeDisplayTransform(display, view, look_, workingCS_),
                                     OCIO::TRANSFORM_DIR_FORWARD);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OCIO processor (display %s): %s\n", display.c_str(), e.what());
        return nullptr;
    }
}

// Both display halves are rebuilt together: they share a selection, and the SDR
// companion follows the selected view, so a change to either invalidates both.
// Called with displayMtx_ held (see the header) -- every caller takes it first.
void OcioManager::rebuildDisplay_() const {
    displayDirty_ = false;
    displayProc_ = nullptr;
    sdrDisplayProc_ = nullptr;
    version_ = ++versionSeq_;
    displayProc_ = buildDisplayProcessor_(display_, view_);

    sdrDisplay_ = sdrDisplay();
    sdrView_.clear();
    if (sdrDisplay_.empty() || !config_)
        return;
    // Follow the selected view by name when this display offers it too; else let the
    // config choose (see the header for why we don't map view names ourselves).
    for (const std::string& v : views(sdrDisplay_)) {
        if (v == view_) { sdrView_ = v; break; }
    }
    if (sdrView_.empty()) {
        const char* defView = config_->getDefaultView(sdrDisplay_.c_str());
        sdrView_ = defView ? defView : "";
    }
    sdrDisplayProc_ = buildDisplayProcessor_(sdrDisplay_, sdrView_);
}

// The media colour space -> the working space. Null when the two name the same
// space, which is the common EXR case and saves the renderers a whole no-op
// shader function and its resources.
OCIO::ConstProcessorRcPtr OcioManager::inputProcessor_(const std::string& inputCS) const {
    if (!config_ || workingCS_.empty())
        return nullptr;
    const std::string src = inputCS.empty() ? workingCS_ : inputCS;

    auto cached = inputProcs_.find(src);
    if (cached != inputProcs_.end())
        return cached->second;

    OCIO::ConstProcessorRcPtr proc;
    try {
        // Compare resolved names, not the strings as given: an alias, a role and the
        // canonical name all denote one space and must not produce a real transform.
        auto srcCS = config_->getColorSpace(src.c_str());
        auto dstCS = config_->getColorSpace(workingCS_.c_str());
        const bool identity = srcCS && dstCS && srcCS->getName() && dstCS->getName() &&
                              std::string(srcCS->getName()) == dstCS->getName();
        if (!identity) {
            OCIO::ColorSpaceTransformRcPtr cst = OCIO::ColorSpaceTransform::Create();
            cst->setSrc(src.c_str());
            cst->setDst(workingCS_.c_str());
            proc = config_->getProcessor(makeContext(config_, contextVars_), cst,
                                         OCIO::TRANSFORM_DIR_FORWARD);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OCIO input transform (%s -> %s): %s\n",
                     src.c_str(), workingCS_.c_str(), e.what());
        proc = nullptr;
    }
    inputProcs_[src] = proc;
    return proc;
}

void OcioManager::warmDisplay() const {
    std::lock_guard<std::mutex> lk(displayMtx_);
    if (displayDirty_)
        rebuildDisplay_();
}

OcioManager::Transform OcioManager::transformFor(const std::string& inputCS) const {
    std::lock_guard<std::mutex> lk(displayMtx_);
    if (displayDirty_) rebuildDisplay_();
    auto it = transforms_.find(inputCS);
    if (it != transforms_.end())
        return it->second;
    Transform t;
    t.toWorking = inputProcessor_(inputCS);
    t.toDisplay = displayProc_;
    t.version = ++versionSeq_;
    transforms_[inputCS] = t;
    return t;
}

OcioManager::Transform OcioManager::sdrTransformFor(const std::string& inputCS) const {
    std::lock_guard<std::mutex> lk(displayMtx_);
    if (displayDirty_) rebuildDisplay_();
    auto it = sdrTransforms_.find(inputCS);
    if (it != sdrTransforms_.end())
        return it->second;
    Transform t;
    t.toWorking = inputProcessor_(inputCS);
    t.toDisplay = sdrDisplayProc_;
    t.version = ++versionSeq_;
    sdrTransforms_[inputCS] = t;
    return t;
}

OcioManager::CpuTransform OcioManager::cpuTransformFor(const std::string& inputCS,
                                                       float exposureEV) const {
    CpuTransform ct;
    ct.gain_ = std::pow(2.0f, exposureEV);
    Transform t = transformFor(inputCS);
    try {
        if (t.toWorking) ct.toWorking_ = t.toWorking->getDefaultCPUProcessor();
        if (t.toDisplay) ct.toDisplay_ = t.toDisplay->getDefaultCPUProcessor();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "OCIO CPU processor: %s\n", e.what());
        ct.toWorking_ = nullptr;
        ct.toDisplay_ = nullptr;
    }
    return ct;
}

// ── CpuTransform ────────────────────────────────────────────────────────────
//
// Both entry points share a front half: widen whichever buffer the frame carries
// into float RGB, run the input transform, then the exposure. What differs is
// where they stop — one goes on through the display transform and quantises, the
// other hands back the working-space floats.

namespace {

// The frame widened into `dst` (w*h*3 floats) in its own colour space. The
// integer buffers normalise to 0..1, which is what an integer-encoded OCIO colour
// space expects; scene-linear half values pass through unscaled.
void widenFrame(const Frame& f, std::vector<float>& dst) {
    const size_t n = (size_t)f.width * f.height;
    dst.resize(n * 3);
    float* d = dst.data();
    if (f.linearRgb.size() >= n * 3) {
        const Imath::half* s = f.linearRgb.data();
        for (size_t i = 0; i < n * 3; ++i)
            d[i] = (float)s[i];
    } else if (f.rgba16.size() >= n * 4) {
        const uint16_t* s = f.rgba16.data();
        for (size_t i = 0; i < n; ++i, s += 4, d += 3) {
            d[0] = s[0] * (1.0f / 65535.0f);
            d[1] = s[1] * (1.0f / 65535.0f);
            d[2] = s[2] * (1.0f / 65535.0f);
        }
    } else if (f.rgba.size() >= n * 4) {
        const uint8_t* s = f.rgba.data();
        for (size_t i = 0; i < n; ++i, s += 4, d += 3) {
            d[0] = s[0] * (1.0f / 255.0f);
            d[1] = s[1] * (1.0f / 255.0f);
            d[2] = s[2] * (1.0f / 255.0f);
        }
    } else {
        std::fill(dst.begin(), dst.end(), 0.0f);
    }
}

} // namespace

void OcioManager::CpuTransform::applyToWorking(const Frame& f, float* dst) const {
    if (f.width <= 0 || f.height <= 0 || !dst)
        return;
    const size_t n = (size_t)f.width * f.height;
    widenFrame(f, scratch_);
    if (toWorking_) {
        OCIO::PackedImageDesc img(scratch_.data(), f.width, f.height, 3);
        toWorking_->apply(img);
    }
    if (gain_ != 1.0f)
        for (size_t i = 0; i < n * 3; ++i)
            scratch_[i] *= gain_;
    std::copy(scratch_.begin(), scratch_.begin() + (ptrdiff_t)(n * 3), dst);
}

void OcioManager::CpuTransform::apply(const Frame& f, uint8_t* dst) const {
    if (f.width <= 0 || f.height <= 0 || !dst)
        return;
    const size_t n = (size_t)f.width * f.height;
    widenFrame(f, scratch_);
    if (toWorking_) {
        OCIO::PackedImageDesc img(scratch_.data(), f.width, f.height, 3);
        toWorking_->apply(img);
    }
    if (gain_ != 1.0f)
        for (size_t i = 0; i < n * 3; ++i)
            scratch_[i] *= gain_;
    if (toDisplay_) {
        OCIO::PackedImageDesc img(scratch_.data(), f.width, f.height, 3);
        toDisplay_->apply(img);
    }
    auto u8 = [](float v) {
        return (uint8_t)std::clamp((int)(v * 255.0f + 0.5f), 0, 255);
    };
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = u8(scratch_[i * 3 + 0]);
        dst[i * 4 + 1] = u8(scratch_[i * 3 + 1]);
        dst[i * 4 + 2] = u8(scratch_[i * 3 + 2]);
        dst[i * 4 + 3] = 255;
    }
}
