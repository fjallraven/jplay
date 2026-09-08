#include "ControlServer.h"

#include "Preferences.h"

// All platform/socket code is confined to this translation unit; ControlServer.h
// stays free of <winsock2.h>. Mirrors the shim in SyncSession.cpp.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>  // also typedefs socklen_t (== int) on Windows
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
static constexpr SOCKET INVALID_SOCKET = -1;
static inline int closesocket(SOCKET s) { return ::close(s); }
#endif

// send() must not raise SIGPIPE and kill the process when the client vanished.
#if defined(MSG_NOSIGNAL)
static constexpr int kSendFlags = MSG_NOSIGNAL;
#else
static constexpr int kSendFlags = 0;
#endif

// select()'s FD_SETSIZE ceiling aborts once a socket fd is >= 1024, which jplay
// reaches easily with EXR/video/cache fds in flight; poll()/WSAPoll address fds
// by value instead. Same reasoning as SyncSession.cpp.
#ifdef _WIN32
static inline int pollFds(pollfd* fds, size_t n, int timeoutMs) {
    return ::WSAPoll(fds, (ULONG)n, timeoutMs);
}
#else
static inline int pollFds(pollfd* fds, size_t n, int timeoutMs) {
    return ::poll(fds, (nfds_t)n, timeoutMs);
}
#endif

#include <SDL3/SDL_log.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace control {

uint16_t configuredPort() {
    const std::string v = Preferences::get("control", "port");
    if (!v.empty()) {
        const long n = std::strtol(v.c_str(), nullptr, 10);
        if (n > 0 && n <= 65535)
            return (uint16_t)n;
        SDL_Log("control: ignoring invalid [control] port '%s'", v.c_str());
    }
    return kDefaultPort;
}

namespace {

constexpr int    kMaxRequestBytes = 1 << 16; // 64 KiB guard on one request line
constexpr int    kRecvTimeoutMs   = 5000;    // a silent client can't wedge the thread
constexpr int    kRebindDelayMs   = 2000;    // retry cadence while another instance owns the port

// Send the whole buffer; false on any error.
bool sendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = (int)::send(s, data + sent, (int)(len - sent), kSendFlags);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

// Read up to the first '\n'. Returns false on error, timeout, peer close before
// any newline, or an over-long line.
bool recvLine(SOCKET s, std::string& out) {
    out.clear();
    char buf[4096];
    for (;;) {
        int n = (int)::recv(s, buf, sizeof(buf), 0);
        if (n <= 0)
            return false;
        out.append(buf, (size_t)n);
        size_t nl = out.find('\n');
        if (nl != std::string::npos) {
            out.resize(nl); // drop the newline and anything after it
            if (!out.empty() && out.back() == '\r')
                out.pop_back();
            return true;
        }
        if (out.size() > (size_t)kMaxRequestBytes)
            return false;
    }
}

// ---- flat JSON parsing ----------------------------------------------------

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

bool parseHex4(const std::string& s, size_t i, uint32_t& out) {
    if (i + 4 > s.size())
        return false;
    out = 0;
    for (size_t k = i; k < i + 4; ++k) {
        char c = s[k];
        int v;
        if (c >= '0' && c <= '9')      v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        out = (out << 4) | (uint32_t)v;
    }
    return true;
}

void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
        ++i;
}

// Parse a double-quoted JSON string starting at s[i] == '"'.
bool parseString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"')
        return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"')
            return true;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (i >= s.size())
            return false;
        char e = s[i++];
        switch (e) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'u': {
            // Python's json.dumps escapes non-ASCII by default, so paths with
            // accented characters arrive as \uXXXX and must be decoded.
            uint32_t cp = 0;
            if (!parseHex4(s, i, cp))
                return false;
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate: expect the low one
                uint32_t lo = 0;
                if (i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u' &&
                    parseHex4(s, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            appendUtf8(out, cp);
            break;
        }
        default:
            return false;
        }
    }
    return false; // unterminated
}

} // namespace

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) { // other control characters need \u00XX
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out += (char)c; // includes UTF-8 continuation bytes, passed through
            }
        }
    }
    return out;
}

bool parseFlatObject(const std::string& json, std::map<std::string, std::string>& out) {
    out.clear();
    size_t i = 0;
    skipWs(json, i);
    if (i >= json.size() || json[i] != '{')
        return false;
    ++i;
    skipWs(json, i);
    if (i < json.size() && json[i] == '}')
        return true; // empty object
    for (;;) {
        skipWs(json, i);
        std::string key;
        if (!parseString(json, i, key))
            return false;
        skipWs(json, i);
        if (i >= json.size() || json[i] != ':')
            return false;
        ++i;
        skipWs(json, i);
        if (i >= json.size())
            return false;

        std::string value;
        if (json[i] == '"') {
            if (!parseString(json, i, value))
                return false;
        } else if (json[i] == '{' || json[i] == '[') {
            return false; // flat objects only
        } else {
            // Number or true/false/null: take the literal run of characters.
            size_t start = i;
            while (i < json.size() && json[i] != ',' && json[i] != '}' &&
                   json[i] != ' ' && json[i] != '\t' && json[i] != '\r' && json[i] != '\n')
                ++i;
            if (i == start)
                return false;
            value = json.substr(start, i - start);
        }
        out[key] = std::move(value);

        skipWs(json, i);
        if (i >= json.size())
            return false;
        if (json[i] == ',') { ++i; continue; }
        if (json[i] == '}') { ++i; break; }
        return false;
    }
    skipWs(json, i);
    return i == json.size(); // trailing garbage is malformed
}

// ---- Server ---------------------------------------------------------------

struct Server::Impl {
    mutable std::mutex mtx;
    SOCKET listenSock = INVALID_SOCKET;
    uint16_t port = kDefaultPort;
    std::atomic<bool> run{ false };
    std::atomic<bool> bound{ false };
    std::thread thread;
    std::vector<Request> inbound;
    bool loggedBindFailure = false; // keep the retry loop from spamming the log

    void loop();
    bool tryBind();
    void serveOne(SOCKET c);
};

// Bind the loopback listener. False when another instance already owns the port.
bool Server::Impl::tryBind() {
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET)
        return false;
    // Deliberately NOT SO_REUSEADDR: a failed bind is how a second instance
    // discovers it isn't the owner, so the collision must be real.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // local only, never INADDR_ANY
    addr.sin_port = htons(port);
    if (::bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(ls, 4) != 0) {
        ::closesocket(ls);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mtx);
        listenSock = ls;
    }
    bound.store(true);
    return true;
}

// Read one request, hand it to the main thread, write the reply back.
void Server::Impl::serveOne(SOCKET c) {
#ifdef _WIN32
    DWORD tv = kRecvTimeoutMs;
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv{ kRecvTimeoutMs / 1000, (kRecvTimeoutMs % 1000) * 1000 };
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    std::string line;
    if (!recvLine(c, line))
        return; // timeout / closed / over-long: nothing sensible to reply to

    Request req;
    req.json = std::move(line);
    std::future<std::string> fut = req.reply.get_future();
    {
        std::lock_guard<std::mutex> lk(mtx);
        inbound.push_back(std::move(req));
    }

    std::string response;
    if (fut.wait_for(kReplyTimeout) == std::future_status::ready) {
        try {
            response = fut.get();
        } catch (...) {
            // Promise destroyed without a value: the app is shutting down, or
            // drainControl() dropped the request.
            response = R"json({"ok":false,"error":"jplay did not answer (shutting down?)"})json";
        }
    } else {
        // The command was NOT cancelled — the main thread is still working (a
        // large directory add opens each source synchronously). Say so, so the
        // caller polls state instead of assuming nothing happened.
        response = R"({"ok":false,"error":"timeout after 120s - command may still be running, poll state"})";
    }
    response += '\n';
    sendAll(c, response.data(), response.size());
}

void Server::Impl::loop() {
    while (run.load()) {
        if (!bound.load()) {
            // Another instance owns the channel. Retry so that if it exits, this
            // instance takes over rather than staying unreachable forever.
            if (!tryBind()) {
                if (!loggedBindFailure) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[control] port %u in use - not the first instance; "
                                "retrying every %dms", (unsigned)port, kRebindDelayMs);
                    loggedBindFailure = true;
                }
                for (int slept = 0; slept < kRebindDelayMs && run.load(); slept += 100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[control] listening on 127.0.0.1:%u", (unsigned)port);
            loggedBindFailure = false;
        }

        pollfd pfd{ listenSock, POLLIN, 0 };
        int r = pollFds(&pfd, 1, 500);
        if (r <= 0 || !(pfd.revents & POLLIN))
            continue;
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        SOCKET c = ::accept(listenSock, (sockaddr*)&from, &fromLen);
        if (c == INVALID_SOCKET)
            continue;
        serveOne(c);
        ::closesocket(c);
    }
}

Server::Server() : d_(new Impl) {
#ifdef _WIN32
    // Refcounted, so this is safe alongside SyncSession's own WSAStartup.
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

Server::~Server() {
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
    delete d_;
}

void Server::start(uint16_t port) {
    if (d_->run.load())
        return;
    d_->port = port;
    d_->run.store(true);
    d_->thread = std::thread([this] { d_->loop(); });
}

void Server::stop() {
    d_->run.store(false);
    if (d_->thread.joinable())
        d_->thread.join();
    std::lock_guard<std::mutex> lk(d_->mtx);
    if (d_->listenSock != INVALID_SOCKET) {
        ::closesocket(d_->listenSock);
        d_->listenSock = INVALID_SOCKET;
    }
    d_->bound.store(false);
    d_->inbound.clear(); // pending promises break; the socket thread is already gone
}

bool Server::listening() const { return d_->bound.load(); }
uint16_t Server::port() const { return d_->port; }

std::vector<Request> Server::drain() {
    std::lock_guard<std::mutex> lk(d_->mtx);
    std::vector<Request> out = std::move(d_->inbound);
    d_->inbound.clear();
    return out;
}

} // namespace control
