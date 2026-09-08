#include "UserData.h"

#include "Preferences.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

const char kSettingsFile[] = "settings.conf";
const char kRecentSection[] = "[recentProjects]";
const char kPrefsSection[] = "[preferences]";
const size_t kMaxRecent = 20;

// Compare two project paths for identity WITHOUT touching the filesystem.
// Recent entries and the current project path are both stored absolute (via
// fs::absolute), so a lexical compare — normalize away '.'/'..', unify the
// separator, and fold case on Windows — identifies "the same file" without
// stat()ing paths that may live on a missing or slow network drive.
// (fs::equivalent used to block ~1 s across a dozen recent entries.) The
// tradeoff: two different spellings that resolve to the same file only via a
// symlink/junction or 8.3 short name won't be deduped — an acceptable rarity.
bool samePathLexical(const std::string& a, const std::string& b) {
    std::string na = fs::path(a).lexically_normal().generic_string();
    std::string nb = fs::path(b).lexically_normal().generic_string();
#ifdef _WIN32
    auto fold = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
    };
    fold(na);
    fold(nb);
#endif
    return na == nb;
}

std::string envVar(const char* name) {
#ifdef _WIN32
    // _dupenv_s avoids the deprecation warning MSVC raises on std::getenv.
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) == 0 && buf) {
        std::string v(buf);
        free(buf);
        return v;
    }
    return {};
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// Create `p` and confirm we can actually write into it (a created-but-readonly
// directory still fails the probe, so we move on to the next candidate root).
bool ensureWritableDir(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec)
        return false;
    fs::path probe = p / ".write_test";
    {
        std::ofstream os(probe, std::ios::binary | std::ios::trunc);
        if (!os)
            return false;
        os << "ok";
        if (!os)
            return false;
    }
    fs::remove(probe, ec);
    return true;
}

std::string computeBaseDir() {
    // Prefer the user's home; fall back to the temp dirs when it isn't writable
    // (e.g. locked-down or roaming-only profiles).
    std::vector<std::string> roots;
    std::string home = envVar("USERPROFILE");
    if (home.empty())
        home = envVar("HOME");
    if (!home.empty())
        roots.push_back(home);
    for (const char* e : { "TMP", "TEMP" }) {
        std::string v = envVar(e);
        if (!v.empty())
            roots.push_back(v);
    }
    for (const auto& r : roots) {
        fs::path d = fs::path(r) / ".jplay";
        if (ensureWritableDir(d))
            return d.string();
    }
    return {};
}

fs::path settingsPath() {
    return fs::path(UserData::dir()) / kSettingsFile;
}

} // namespace

namespace UserData {

const std::string& dir() {
    static const std::string d = computeBaseDir();
    return d;
}

std::string projectsDir() {
    const std::string& base = dir();
    if (base.empty())
        return {};
    fs::path p = fs::path(base) / "projects";
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec)
        return {};
    return p.string();
}

std::string newProjectId() {
    std::random_device rd;
    std::mt19937_64 gen(((uint64_t)rd() << 32) ^ rd());
    uint64_t a = gen(), b = gen();
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  (unsigned long long)a, (unsigned long long)b);
    return std::string(buf, 32);
}

std::vector<RecentProject> loadRecent() {
    std::vector<RecentProject> out;
    const std::string& base = dir();
    if (base.empty())
        return out;
    std::ifstream is(settingsPath());
    if (!is)
        return out;

    std::string line;
    bool inSection = false;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos)
            continue;
        line = line.substr(s);
        if (line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            inSection = (line.rfind(kRecentSection, 0) == 0);
            continue;
        }
        if (!inSection)
            continue;

        // id|savedUnix|path  (path is the remainder, so it may contain spaces)
        size_t p1 = line.find('|');
        if (p1 == std::string::npos)
            continue;
        size_t p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos)
            continue;
        RecentProject rp;
        rp.id = line.substr(0, p1);
        rp.savedUnix = (int64_t)std::strtoll(line.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr, 10);
        rp.path = line.substr(p2 + 1);
        if (!rp.path.empty())
            out.push_back(std::move(rp));
        if (out.size() >= kMaxRecent)
            break;
    }
    return out;
}

// Rewrite settings.conf with both sections, so neither writer clobbers the
// other's data. Best-effort: silently does nothing if the file can't be opened.
static void writeSettings(const std::vector<RecentProject>& list, const Prefs& prefs) {
    if (dir().empty())
        return;
    std::ofstream os(settingsPath(), std::ios::trunc);
    if (!os)
        return;
    os << kRecentSection << '\n';
    for (const auto& e : list)
        os << e.id << '|' << e.savedUnix << '|' << e.path << '\n';
    os << kPrefsSection << '\n';
    os << "timeFormatFrames=" << (prefs.timeFormatFrames ? 1 : 0) << '\n';
    os << "frameNumberingClip=" << (prefs.frameNumberingClip ? 1 : 0) << '\n';
    os << "pythonDebug=" << (prefs.pythonDebug ? 1 : 0) << '\n';
    os << "showFramePreview=" << (prefs.showFramePreview ? 1 : 0) << '\n';
    os << "snapPlayhead=" << (prefs.snapPlayhead ? 1 : 0) << '\n';
    os << "audioScrub=" << (prefs.audioScrub ? 1 : 0) << '\n';
    os << "clipWaveform=" << (prefs.clipWaveform ? 1 : 0) << '\n';
    os << "attachAudioToSeq=" << (prefs.attachAudioToSeq ? 1 : 0) << '\n';
    os << "warnUnsaved=" << (prefs.warnUnsaved ? 1 : 0) << '\n';
    os << "sourceThumbSize=" << prefs.sourceThumbSize << '\n';
    os << "gridThumbH=" << prefs.gridThumbH << '\n';
    os << "sourceSort=" << prefs.sourceSort << '\n';
    os << "cacheGb=" << prefs.cacheGb << '\n';
    os << "decodeThreads=" << prefs.decodeThreads << '\n';
    os << "nitRef=" << prefs.nitRef << '\n';
    os << "hdrOutput=" << (prefs.hdrOutput ? 1 : 0) << '\n';
    os << "hdrRefWhiteNits=" << prefs.hdrRefWhiteNits << '\n';
    os << "uiScale=" << prefs.uiScale << '\n';
    os << "syncNetwork=" << (prefs.syncNetwork ? 1 : 0) << '\n';
    os << "syncPort=" << prefs.syncPort << '\n';
    os << "mcpEnabled=" << (prefs.mcpEnabled ? 1 : 0) << '\n';
    os << "proxyEnabled=" << (prefs.proxyEnabled ? 1 : 0) << '\n';
    os << "volume=" << prefs.volume << '\n';
    os << "muted=" << (prefs.muted ? 1 : 0) << '\n';
    os << "frameOverlay=" << (prefs.frameOverlay ? 1 : 0) << '\n';
    os << "frameOverlayBottom=" << (prefs.frameOverlayBottom ? 1 : 0) << '\n';
    os << "overlaySize=" << prefs.overlaySize << '\n';
    os << "overlayColor=" << prefs.overlayColor << '\n';
    os << "compactTimeline=" << (prefs.compactTimeline ? 1 : 0) << '\n';
}

void recordRecent(const RecentProject& rp) {
    const std::string& base = dir();
    if (base.empty() || rp.path.empty())
        return;

    std::vector<RecentProject> list = loadRecent();
    auto samePath = [&](const RecentProject& o) { return samePathLexical(o.path, rp.path); };
    list.erase(std::remove_if(list.begin(), list.end(), samePath), list.end());
    list.insert(list.begin(), rp);
    if (list.size() > kMaxRecent)
        list.resize(kMaxRecent);

    writeSettings(list, loadPrefs()); // preserve the existing preferences section
}

void removeRecent(const std::string& path) {
    if (dir().empty() || path.empty())
        return;

    std::vector<RecentProject> list = loadRecent();
    auto samePath = [&](const RecentProject& o) { return samePathLexical(o.path, path); };
    list.erase(std::remove_if(list.begin(), list.end(), samePath), list.end());

    writeSettings(list, loadPrefs()); // preserve the existing preferences section
}

Prefs loadPrefs() {
    Prefs p;
    // The two networking switches take their starting position from the deployed
    // jplay_preferences.conf rather than from the struct, so a site can decide
    // what a machine with no saved settings does. A settings.conf key below still
    // overrides — which it always has once the user has saved anything at all.
    p.syncNetwork = Preferences::getBool("sync", "enabled", p.syncNetwork);
    p.mcpEnabled = Preferences::getBool("control", "enabled", p.mcpEnabled);

    const std::string& base = dir();
    if (base.empty())
        return p;
    std::ifstream is(settingsPath());
    if (!is)
        return p;

    std::string line;
    bool inSection = false;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos)
            continue;
        line = line.substr(s);
        if (line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            inSection = (line.rfind(kPrefsSection, 0) == 0);
            continue;
        }
        if (!inSection)
            continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        bool on = val == "1";
        if (key == "timeFormatFrames")
            p.timeFormatFrames = on;
        else if (key == "frameNumberingClip")
            p.frameNumberingClip = on;
        else if (key == "pythonDebug")
            p.pythonDebug = on;
        else if (key == "showFramePreview")
            p.showFramePreview = on;
        else if (key == "snapPlayhead")
            p.snapPlayhead = on;
        else if (key == "audioScrub")
            p.audioScrub = on;
        else if (key == "clipWaveform")
            p.clipWaveform = on;
        else if (key == "attachAudioToSeq")
            p.attachAudioToSeq = on;
        else if (key == "muted")
            p.muted = on;
        else if (key == "frameOverlay")
            p.frameOverlay = on;
        else if (key == "frameOverlayBottom")
            p.frameOverlayBottom = on;
        else if (key == "compactTimeline")
            p.compactTimeline = on;
        else if (key == "overlaySize")
            p.overlaySize = std::atoi(val.c_str());
        else if (key == "overlayColor")
            p.overlayColor = std::atoi(val.c_str());
        else if (key == "volume")
            p.volume = (float)std::atof(val.c_str());
        else if (key == "warnUnsaved")
            p.warnUnsaved = on;
        else if (key == "sourceThumbSize")
            p.sourceThumbSize = std::atoi(val.c_str());
        else if (key == "gridThumbH")
            p.gridThumbH = std::atoi(val.c_str());
        else if (key == "sourceSort")
            p.sourceSort = std::atoi(val.c_str());
        else if (key == "cacheGb")
            p.cacheGb = (float)std::atof(val.c_str());
        else if (key == "decodeThreads")
            p.decodeThreads = std::atoi(val.c_str());
        else if (key == "nitRef")
            p.nitRef = (float)std::atof(val.c_str());
        else if (key == "hdrOutput")
            p.hdrOutput = on;
        else if (key == "hdrRefWhiteNits")
            p.hdrRefWhiteNits = (float)std::atof(val.c_str());
        else if (key == "uiScale")
            p.uiScale = (float)std::atof(val.c_str());
        else if (key == "syncNetwork")
            p.syncNetwork = on;
        else if (key == "syncPort")
            p.syncPort = std::atoi(val.c_str());
        else if (key == "mcpEnabled")
            p.mcpEnabled = on;
        else if (key == "proxyEnabled")
            p.proxyEnabled = on;
    }
    return p;
}

void savePrefs(const Prefs& prefs) {
    if (dir().empty())
        return;
    writeSettings(loadRecent(), prefs); // preserve the recent-projects section
}

// <projectsDir>/<id>-<16 hex> keyed by project id and file path (see the header).
// Empty when there is no writable data dir. The FNV-1a hash keeps the name short
// and filesystem-safe; on Windows the path is lowercased first so the same file
// reached through a differently-cased path lands on one thumbnail.
static fs::path thumbnailPath(const std::string& id, const std::string& projectPath) {
    std::string pdir = projectsDir();
    if (pdir.empty())
        return {};
    std::error_code ec;
    fs::path abs = fs::absolute(projectPath, ec);
    std::string norm = (ec ? fs::path(projectPath) : abs).lexically_normal().string();
#ifdef _WIN32
    for (char& c : norm)
        c = (char)std::tolower((unsigned char)c);
#endif
    uint64_t h = 1469598103934665603ull; // FNV-1a 64
    for (unsigned char c : norm) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char suffix[24];
    std::snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)h);
    return fs::path(pdir) / (id + suffix);
}

bool writeThumbnail(const std::string& id, const std::string& projectPath,
                    const Project::Thumbnail& t) {
    if (id.empty() || !t.valid())
        return false;
    fs::path file = thumbnailPath(id, projectPath);
    if (file.empty())
        return false;
    std::ofstream os(file, std::ios::binary | std::ios::trunc);
    if (!os)
        return false;
    // Tiny self-describing header so a stale/garbled file is rejected on read.
    const char magic[4] = { 'J', 'T', 'H', '1' };
    os.write(magic, 4);
    os.write(reinterpret_cast<const char*>(&t.width), sizeof(int32_t));
    os.write(reinterpret_cast<const char*>(&t.height), sizeof(int32_t));
    os.write(reinterpret_cast<const char*>(t.rgba.data()), (std::streamsize)t.rgba.size());
    return (bool)os;
}

bool readThumbnail(const std::string& id, const std::string& projectPath,
                   Project::Thumbnail& t) {
    t = Project::Thumbnail{};
    if (id.empty())
        return false;
    fs::path file = thumbnailPath(id, projectPath);
    if (file.empty())
        return false;
    std::ifstream is(file, std::ios::binary);
    if (!is)
        return false;
    char magic[4] = {};
    is.read(magic, 4);
    if (!is || std::memcmp(magic, "JTH1", 4) != 0)
        return false;
    int32_t w = 0, h = 0;
    is.read(reinterpret_cast<char*>(&w), sizeof(int32_t));
    is.read(reinterpret_cast<char*>(&h), sizeof(int32_t));
    if (!is || w <= 0 || h <= 0 || w > 8192 || h > 8192)
        return false;
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    if (!is.read(reinterpret_cast<char*>(rgba.data()), (std::streamsize)rgba.size()))
        return false;
    t.width = w;
    t.height = h;
    t.rgba = std::move(rgba);
    return true;
}

} // namespace UserData
