#include "ClipboardFiles.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>

#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace {

bool existsUtf8(const std::string& path) {
    // fs::u8path, not fs::path: MSVC reads a narrow std::string in the ANSI
    // code page, and every path in jplay is UTF-8.
    std::error_code ec;
    return !path.empty() && fs::exists(fs::u8path(path), ec);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "file:///C:/a%20b.exr" -> "C:/a b.exr"; "file:///mnt/a.exr" -> "/mnt/a.exr".
std::string pathFromFileUrl(const std::string& url) {
    std::string rest = url.substr(7); // past "file://"
    // An optional host ("localhost") precedes the path.
    if (!rest.empty() && rest[0] != '/') {
        size_t slash = rest.find('/');
        if (slash == std::string::npos)
            return {};
        rest = rest.substr(slash);
    }
    std::string out;
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            int hi = hexValue(rest[i + 1]), lo = hexValue(rest[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += rest[i];
    }
#ifdef _WIN32
    // "/C:/..." -> "C:/..."
    if (out.size() >= 3 && out[0] == '/' && std::isalpha((unsigned char)out[1]) && out[2] == ':')
        out.erase(0, 1);
#endif
    return out;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// One path per line, each optionally quoted or a file:// URL. All-or-nothing: a
// single line that is not an existing path means the text is not a path list
// (a sentence that happens to start with a path is not a paste of that file).
std::vector<std::string> pathsFromText(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty())
            continue;
        if (line.size() >= 2 && (line.front() == '"' || line.front() == '\'')
            && line.back() == line.front())
            line = line.substr(1, line.size() - 2);
        if (line.rfind("file://", 0) == 0)
            line = pathFromFileUrl(line);
        if (!existsUtf8(line))
            return {};
        out.push_back(line);
    }
    return out;
}

#ifdef _WIN32
// The file list Explorer puts on the clipboard for a Ctrl+C on files (CF_HDROP).
std::vector<std::string> pathsFromFileDrop() {
    std::vector<std::string> out;
    if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(nullptr))
        return out;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        if (HDROP drop = (HDROP)GlobalLock(h)) {
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring w(len, L'\0');
                DragQueryFileW(drop, i, w.data(), len + 1);
                std::string path = fs::path(w).u8string();
                if (existsUtf8(path))
                    out.push_back(std::move(path));
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}
#endif

} // namespace

std::vector<std::string> clipboardFilePaths() {
#ifdef _WIN32
    if (auto files = pathsFromFileDrop(); !files.empty())
        return files;
#endif
    std::vector<std::string> out;
    if (!SDL_HasClipboardText())
        return out;
    if (char* text = SDL_GetClipboardText()) {
        out = pathsFromText(text);
        SDL_free(text);
    }
    return out;
}
