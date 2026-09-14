#include "RevealFile.h"

#include <SDL3/SDL_misc.h>

#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <SDL3/SDL_process.h>
#endif

namespace fs = std::filesystem;

namespace {

// The directory to fall back on when the exact item can't be revealed.
std::string parentDir(const std::string& path) {
    // fs::u8path, not fs::path: MSVC reads a narrow std::string in the ANSI
    // code page, and every path in jplay is UTF-8.
    return fs::u8path(path).parent_path().u8string();
}

// "file://" URL for a local path, with the characters a URL can't carry raw
// percent-encoded. Backslashes become forward slashes so a Windows path is a
// valid URL too.
std::string fileUrl(const std::string& path) {
    std::string out = "file://";
    if (!path.empty() && path[0] != '/' && path[0] != '\\')
        out += '/'; // drive-letter path: file:///C:/...
    for (unsigned char c : path) {
        if (c == '\\' || c == '/') {
            out += '/';
        } else if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~'
                   || c == ':' || c == '$' || c == '@' || c == '&' || c == '+'
                   || c == '(' || c == ')' || c == ',' || c == '=' || c == '!') {
            out += (char)c;
        } else {
            static const char* kHex = "0123456789ABCDEF";
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

} // namespace

#ifdef _WIN32

bool revealInFileManager(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::u8path(path);
    // explorer /select needs the item to exist; otherwise just open the folder.
    if (!fs::exists(p, ec)) {
        const std::string dir = parentDir(path);
        return !dir.empty() && SDL_OpenURL(fileUrl(dir).c_str());
    }
    // Native separators, and quoted so a path with spaces or commas survives
    // explorer's own argument parsing.
    std::wstring args = L"/select,\"" + p.make_preferred().wstring() + L"\"";
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
                                nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

#else

bool revealInFileManager(const std::string& path) {
    // The freedesktop file-manager interface: Nautilus, Dolphin, Nemo and Caja
    // all implement it, and it selects the item rather than just opening its
    // folder. dbus-send activates the service, so it works even with no file
    // manager already running.
    const std::string url = fileUrl(path);
    const char* args[] = {
        "dbus-send", "--session", "--print-reply", "--reply-timeout=2000",
        "--dest=org.freedesktop.FileManager1", "--type=method_call",
        "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems",
        nullptr, "string:", nullptr
    };
    const std::string urlArg = "array:string:" + url;
    args[8] = urlArg.c_str();
    if (SDL_Process* proc = SDL_CreateProcess(args, false)) {
        int code = -1;
        bool done = SDL_WaitProcess(proc, true, &code);
        SDL_DestroyProcess(proc);
        if (done && code == 0)
            return true;
    }
    // No FileManager1 (e.g. Thunar, PCManFM): open the containing folder with
    // whatever xdg-open resolves to. The item isn't selected, but it's visible.
    const std::string dir = parentDir(path);
    return !dir.empty() && SDL_OpenURL(fileUrl(dir).c_str());
}

#endif
