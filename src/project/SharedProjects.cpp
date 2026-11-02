#include "SharedProjects.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

namespace SharedProjects {
namespace {

std::string trimWs(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// Replace every "{project}" placeholder in `tmpl` with the show code.
std::string fillProjectCode(const std::string& tmpl, const std::string& code) {
    std::string out = tmpl;
    const std::string key = "{project}";
    for (size_t p = out.find(key); p != std::string::npos; p = out.find(key, p))
        out.replace(p, key.size(), code);
    return out;
}

} // namespace

std::vector<Entry> parse(const std::string& iniPath, const std::string& pathTmpl) {
    // Every "CODE,Long Name" line, in any section (section headers are ignored —
    // all shows are collected). No existence check here; see exists().
    std::vector<Entry> entries;
    std::ifstream is(iniPath);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string t = trimWs(line);
        if (t.empty() || t[0] == '[' || t[0] == '#' || t[0] == ';')
            continue;
        size_t comma = t.find(',');
        if (comma == std::string::npos)
            continue;
        Entry e;
        e.code = trimWs(t.substr(0, comma));
        e.name = trimWs(t.substr(comma + 1));
        if (e.code.empty())
            continue;
        e.path = fillProjectCode(pathTmpl, e.code);
        entries.push_back(std::move(e));
    }
    return entries;
}

bool exists(const std::string& path, int budgetMs, const std::atomic<bool>& stop) {
    auto slot = std::make_shared<std::atomic<int>>(-1); // -1 pending, 0 no, 1 yes
    std::thread([path, slot] {
        std::error_code ec;
        bool ok = std::filesystem::exists(path, ec) && !ec;
        slot->store(ok ? 1 : 0);
    }).detach();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (slot->load() < 0) {
        if (stop.load() || std::chrono::steady_clock::now() >= deadline)
            return false; // cancelled or hung mount: treat as missing
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return slot->load() == 1;
}

} // namespace SharedProjects
