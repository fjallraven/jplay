#include "Preferences.h"

#include <SDL3/SDL_filesystem.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace {

const char kFileName[] = "jplay_preferences.conf";

std::string envVar(const char* name) {
#ifdef _WIN32
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

// Resolve the preferences file to load; see Preferences.h for the order.
std::string configPath() {
    std::string env = envVar("JPLAY_PREFERENCES");
    if (!env.empty())
        return env;

    std::string home = envVar("USERPROFILE");
    if (home.empty())
        home = envVar("HOME");
    if (!home.empty()) {
        fs::path user = fs::path(home) / ".jplay" / kFileName;
        std::error_code ec;
        if (fs::is_regular_file(user, ec))
            return user.string();
    }

    const char* base = SDL_GetBasePath(); // exe dir, trailing sep; do not free
    return std::string(base ? base : "") + kFileName;
}

// section -> (key -> value). Trimmed of surrounding whitespace.
using Config = std::map<std::string, std::map<std::string, std::string>>;

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

const Config& config() {
    static const Config cfg = [] {
        Config c;
        std::ifstream is(configPath());
        std::string line, section;
        while (std::getline(is, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::string t = trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';')
                continue;
            if (t[0] == '[') {
                size_t close = t.find(']');
                if (close != std::string::npos)
                    section = trim(t.substr(1, close - 1));
                continue;
            }
            size_t eq = t.find('=');
            if (eq == std::string::npos || section.empty())
                continue;
            c[section][trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
        }
        return c;
    }();
    return cfg;
}

} // namespace

namespace Preferences {

std::string get(const std::string& section, const std::string& key) {
    const Config& c = config();
    auto s = c.find(section);
    if (s == c.end())
        return {};
    auto k = s->second.find(key);
    return k == s->second.end() ? std::string{} : k->second;
}

bool getBool(const std::string& section, const std::string& key, bool def) {
    std::string v = get(section, key);
    for (char& c : v)
        c = (char)std::tolower((unsigned char)c);
    if (v == "1" || v == "true" || v == "yes" || v == "on")
        return true;
    if (v == "0" || v == "false" || v == "no" || v == "off")
        return false;
    return def;
}

} // namespace Preferences
