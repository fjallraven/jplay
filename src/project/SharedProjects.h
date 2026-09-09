#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace SharedProjects {

// A studio show with a candidate shared project: its short code, long name, and
// resolved project path (a .otio or a .jpproj — the extension picks the loader).
// Existence is checked separately, per show, via exists().
struct Entry {
    std::string code; // short show code, e.g. "TON"
    std::string name; // long name, e.g. "Positano"
    std::string path; // resolved project path (candidate; not yet verified)
};

// Parse the studio shows config at `iniPath` — every "CODE,Long Name" line in
// any section (section headers are ignored) — and resolve each show's project
// path from `pathTmpl` (a template with a "{project}" placeholder). Returns
// every candidate show; does no disk I/O beyond reading the config. Safe off the
// main thread.
std::vector<Entry> parse(const std::string& iniPath, const std::string& pathTmpl);

// Existence check for a single project `path` under a `budgetMs` wall-clock budget.
// The stat runs on a detached thread so a hung network mount can't block past the
// budget — std::filesystem::exists can hang indefinitely on a dead NFS mount. The
// wait polls `stop` so a cooperative caller (WorkQueue::reset) is released
// promptly. Returns false on missing, timeout, or stop. Safe off the main thread.
bool exists(const std::string& path, int budgetMs, const std::atomic<bool>& stop);

} // namespace SharedProjects
