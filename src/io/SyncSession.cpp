#include "SyncSession.h"

// All platform/socket code is confined to this translation unit; SyncSession.h
// stays header-only-friendly (no <winsock2.h> leaking into App.h).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>  // also typedefs socklen_t (== int) on Windows
#include <iphlpapi.h>
#else
// POSIX sockets. A thin compatibility shim maps the handful of Winsock names the
// code below uses onto their BSD-socket equivalents so the body stays identical.
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
static constexpr SOCKET INVALID_SOCKET = -1;
static inline int closesocket(SOCKET s) { return ::close(s); }
#endif

// send() must not raise SIGPIPE and kill the process when a peer has vanished;
// on Linux MSG_NOSIGNAL suppresses it, on Windows there is no such signal.
#if defined(MSG_NOSIGNAL)
static constexpr int kSendFlags = MSG_NOSIGNAL;
#else
static constexpr int kSendFlags = 0;
#endif

// select() has a hard ceiling: on Linux fd_set is a 1024-bit (FD_SETSIZE) bitmask
// and FD_SET aborts the process ("bit out of range 0 - FD_SETSIZE") the moment a
// socket fd is >= 1024 — which jplay hits easily once EXR/video/cache/pipe fds pile
// up. poll() addresses fds by value with no such ceiling; WSAPoll is the Windows
// equivalent, taking the same pollfd array shape.
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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace syncreview {

namespace {

constexpr uint16_t kDiscoveryPort = 45777;     // UDP beacon port (fixed)
constexpr char     kBeaconTag[]    = "JPLAYSYNC1";
constexpr uint32_t kMaxMsgLen      = 64u * 1024u * 1024u; // 64 MiB guard
constexpr uint64_t kBeaconTtlMs    = 5000;     // drop a beacon unseen this long

uint64_t nowMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

// Send the whole buffer; false on any error.
bool sendAll(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = (int)::send(s, data + sent, len - sent, kSendFlags);
        if (n <= 0)
            return false;
        sent += n;
    }
    return true;
}

// Receive exactly len bytes; false on error / peer close.
bool recvAll(SOCKET s, char* data, int len) {
    int got = 0;
    while (got < len) {
        int n = (int)::recv(s, data + got, len - got, 0);
        if (n <= 0)
            return false;
        got += n;
    }
    return true;
}

// Frame = [u8 type][u32 len][payload]. len is raw little-endian (x64 native).
bool sendFrame(SOCKET s, MsgType type, const char* payload, uint32_t len) {
    char hdr[5];
    hdr[0] = (char)type;
    std::memcpy(hdr + 1, &len, 4);
    if (!sendAll(s, hdr, 5))
        return false;
    return len == 0 || sendAll(s, payload, (int)len);
}

// Read one framed message; false on error / peer close. On success `payload`
// holds the raw payload bytes (empty for zero-length frames).
bool recvFrame(SOCKET s, MsgType& type, std::string& payload) {
    uint8_t t = 0;
    uint32_t len = 0;
    if (!recvAll(s, (char*)&t, 1) || !recvAll(s, (char*)&len, 4))
        return false;
    if (len > kMaxMsgLen)
        return false;
    payload.clear();
    if (len) {
        payload.resize(len);
        if (!recvAll(s, payload.data(), (int)len))
            return false;
    }
    type = (MsgType)t;
    return true;
}

// Turn a decoded frame into the Message the main thread consumes.
Message decodeMessage(MsgType type, std::string&& payload) {
    Message m;
    m.type = type;
    switch (type) {
    case MsgType::Project:
    case MsgType::Stroke:
    case MsgType::Annotations:
        m.blob = std::move(payload);
        break;
    case MsgType::Play:
    case MsgType::Pause:
    case MsgType::Seek:
    case MsgType::Clear:
        if (payload.size() >= sizeof(int64_t))
            std::memcpy(&m.i64, payload.data(), sizeof(int64_t));
        break;
    case MsgType::Sequence:
        if (payload.size() >= sizeof(int32_t))
            std::memcpy(&m.i32, payload.data(), sizeof(int32_t));
        break;
    case MsgType::View:
        if (payload.size() >= 3 * sizeof(float)) {
            std::memcpy(&m.zoom, payload.data(), sizeof(float));
            std::memcpy(&m.u, payload.data() + 4, sizeof(float));
            std::memcpy(&m.v, payload.data() + 8, sizeof(float));
        }
        break;
    case MsgType::Exposure:
        if (payload.size() >= 2 * sizeof(float)) {
            std::memcpy(&m.gain, payload.data(), sizeof(float));
            std::memcpy(&m.gamma, payload.data() + 4, sizeof(float));
        }
        break;
    case MsgType::Matte:
        if (payload.size() >= 2 * sizeof(float) + 1) {
            std::memcpy(&m.ratio, payload.data(), sizeof(float));
            std::memcpy(&m.opacity, payload.data() + 4, sizeof(float));
            m.fitView = payload[8] != 0;
        }
        break;
    }
    return m;
}

} // namespace

struct Session::Impl {
    mutable std::mutex mtx;
    Role role = Role::None;

    // Host state.
    SOCKET listenSock = INVALID_SOCKET;
    SOCKET beaconSock = INVALID_SOCKET;
    struct PeerConn {
        SOCKET sock = INVALID_SOCKET;
        std::string ip;
        std::string name;     // from the Hello handshake; empty until received
        std::string hostname; // from the Hello handshake; empty until received
    };
    std::vector<PeerConn> peers;
    std::string projectSnapshot; // bytes handed to each new joiner
    uint16_t tcpPort = 0;
    std::thread hostThread;
    std::atomic<bool> hostRun{ false };

    // Spectator state.
    SOCKET clientSock = INVALID_SOCKET;
    std::thread recvThread;
    std::atomic<bool> recvRun{ false };
    std::atomic<bool> hostLost{ false };

    // Discovery (browse) state.
    SOCKET discoverySock = INVALID_SOCKET;
    std::vector<Beacon> beacons;
    std::thread discoveryThread;
    std::atomic<bool> discoveryRun{ false };

    // Inbound decoded messages (spectator side), drained by the main thread.
    std::vector<Message> inbound;

    std::string username;
    std::string hostname;

    void hostLoop();
    void recvLoop();
    void discoveryLoop();
    void sendBeacon();
    void closeSocket(SOCKET& s) {
        if (s != INVALID_SOCKET) {
            ::closesocket(s);
            s = INVALID_SOCKET;
        }
    }
    // Broadcast one frame to every connected peer; drops peers that error out.
    void broadcast(MsgType type, const char* payload, uint32_t len) {
        std::lock_guard<std::mutex> lk(mtx);
        for (size_t i = 0; i < peers.size();) {
            if (!sendFrame(peers[i].sock, type, payload, len)) {
                ::closesocket(peers[i].sock);
                peers.erase(peers.begin() + i);
            } else {
                ++i;
            }
        }
    }
    void sendFrameI64(MsgType type, int64_t v) {
        broadcast(type, reinterpret_cast<const char*>(&v), sizeof(v));
    }
    // Spectator: send one frame to the host over the client socket. A failure is
    // ignored here — the recv loop notices the drop and latches hostLost.
    void sendToHost(MsgType type, const char* payload, uint32_t len) {
        std::lock_guard<std::mutex> lk(mtx);
        if (clientSock != INVALID_SOCKET)
            sendFrame(clientSock, type, payload, len);
    }
};

// ---- beacon ---------------------------------------------------------------

void Session::Impl::sendBeacon() {
    if (beaconSock == INVALID_SOCKET)
        return;
    char msg[256];
    int n = std::snprintf(msg, sizeof(msg), "%s|%s|%s|%u", kBeaconTag,
                          hostname.c_str(), username.c_str(), (unsigned)tcpPort);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(msg)) // truncated: send only what fits
        n = (int)sizeof(msg) - 1;

    auto sendTo = [&](uint32_t addrHost) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(kDiscoveryPort);
        dst.sin_addr.s_addr = htonl(addrHost);
        ::sendto(beaconSock, msg, n, 0, (sockaddr*)&dst, sizeof(dst));
    };

    // The limited broadcast 255.255.255.255 leaves only ONE interface (chosen by
    // routing/metric), so on a multi-homed host — e.g. a VPN or Hyper-V adapter
    // with a lower metric — the beacon never reaches the physical LAN. Enumerate
    // interfaces and send a subnet-directed broadcast (ip | ~mask) on each, which
    // the routing table delivers to the correct NIC. Keep the limited broadcast
    // as a fallback in case enumeration turns up nothing usable.
    sendTo(INADDR_BROADCAST);

#ifdef _WIN32
    ULONG bufLen = 15 * 1024; // MSDN-recommended starting size
    std::vector<char> buf(bufLen);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
    ULONG rc = ::GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      (IP_ADAPTER_ADDRESSES*)buf.data(), &bufLen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        rc = ::GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    (IP_ADAPTER_ADDRESSES*)buf.data(), &bufLen);
    }
    if (rc != NO_ERROR)
        return;

    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp)
            continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            uint32_t ip = ntohl(
                ((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
            uint8_t prefix = u->OnLinkPrefixLength;
            if (prefix == 0 || prefix > 32)
                continue;
            uint32_t mask = (prefix == 32) ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
            uint32_t bcast = ip | ~mask;
            if (bcast != INADDR_BROADCAST) // already sent above
                sendTo(bcast);
        }
    }
#else
    // POSIX: enumerate interfaces via getifaddrs and derive each subnet-directed
    // broadcast (ip | ~mask) the same way, using the kernel-provided netmask.
    struct ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) != 0)
        return;
    for (struct ifaddrs* a = ifs; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_BROADCAST))
            continue;
        uint32_t ip = ntohl(((sockaddr_in*)a->ifa_addr)->sin_addr.s_addr);
        uint32_t mask = a->ifa_netmask
            ? ntohl(((sockaddr_in*)a->ifa_netmask)->sin_addr.s_addr)
            : 0u;
        uint32_t bcast = ip | ~mask;
        if (bcast != INADDR_BROADCAST) // already sent above
            sendTo(bcast);
    }
    ::freeifaddrs(ifs);
#endif
}

// ---- host loop ------------------------------------------------------------

void Session::Impl::hostLoop() {
    uint64_t lastBeacon = 0;
    while (hostRun.load()) {
        uint64_t t = nowMs();
        if (t - lastBeacon >= 1000) {
            sendBeacon();
            lastBeacon = t;
        }

        // Slot 0 is the listener; the rest are a snapshot of peer sockets taken
        // under the lock. peers can change (broadcast() drops errored peers on
        // other threads) between polling and processing, so peer revents are
        // matched back by socket value under the lock, not by index.
        std::vector<pollfd> pfds;
        pfds.push_back({ listenSock, POLLIN, 0 });
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (const auto& p : peers)
                pfds.push_back({ p.sock, POLLIN, 0 });
        }
        int r = pollFds(pfds.data(), pfds.size(), 1000);
        if (r <= 0)
            continue;

        // Collect the peer sockets that polled readable (POLLHUP/POLLERR also mean
        // "read now" — the recv will return 0/error and the peer gets reaped).
        std::vector<SOCKET> readable;
        for (size_t i = 1; i < pfds.size(); ++i)
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))
                readable.push_back(pfds[i].fd);

        if (pfds[0].revents & POLLIN) {
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            SOCKET c = ::accept(listenSock, (sockaddr*)&from, &fromLen);
            if (c != INVALID_SOCKET) {
                char ipstr[INET_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET, &from.sin_addr, ipstr, sizeof(ipstr));

                // Push the current project to the new joiner immediately.
                std::string snap;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    snap = projectSnapshot;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[sync] host: peer connected, pushing snapshot of %zu bytes%s",
                            snap.size(), snap.empty() ? " (EMPTY — nothing to send!)" : "");
                bool ok = true;
                if (!snap.empty())
                    ok = sendFrame(c, MsgType::Project, snap.data(),
                                   (uint32_t)snap.size());
                if (ok) {
                    std::lock_guard<std::mutex> lk(mtx);
                    peers.push_back({ c, ipstr, "", "" });
                } else {
                    ::closesocket(c);
                }
            }
        }

        // A readable peer socket carries a framed message (a spectator's Hello,
        // stroke, or clear request) or signals a close. Read under the lock — the
        // same discipline broadcast()/leave() use to own peer sockets, so a socket
        // can't be closed out from under this recv. Frames are small, so the
        // blocking read returns promptly.
        std::vector<SOCKET> dead;
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto& p : peers) {
                if (std::find(readable.begin(), readable.end(), p.sock) ==
                    readable.end())
                    continue;
                MsgType type;
                std::string payload;
                if (!recvFrame(p.sock, type, payload)) {
                    dead.push_back(p.sock);
                } else if (type == MsgType::Hello) {
                    size_t sep = payload.find('|');
                    if (sep != std::string::npos) {
                        p.name = payload.substr(0, sep);
                        p.hostname = payload.substr(sep + 1);
                    }
                } else {
                    inbound.push_back(decodeMessage(type, std::move(payload)));
                }
            }
            for (SOCKET p : dead) {
                ::closesocket(p);
                peers.erase(std::remove_if(peers.begin(), peers.end(),
                                           [p](const PeerConn& e) { return e.sock == p; }),
                            peers.end());
            }
        }
    }
}

// ---- spectator recv loop --------------------------------------------------

void Session::Impl::recvLoop() {
    while (recvRun.load()) {
        MsgType type;
        std::string payload;
        if (!recvFrame(clientSock, type, payload))
            break;
        if (type == MsgType::Project)
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[sync] spectator: received Project frame, %zu bytes",
                        payload.size());
        Message m = decodeMessage(type, std::move(payload));
        std::lock_guard<std::mutex> lk(mtx);
        inbound.push_back(std::move(m));
    }
    // Stream ended. If we didn't ask to stop, the host dropped us.
    if (recvRun.load())
        hostLost.store(true);
}

// ---- discovery loop -------------------------------------------------------

void Session::Impl::discoveryLoop() {
    while (discoveryRun.load()) {
        pollfd pfd{ discoverySock, POLLIN, 0 };
        int r = pollFds(&pfd, 1, 1000);
        if (r <= 0 || !(pfd.revents & POLLIN))
            continue;

        char buf[512];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = (int)::recvfrom(discoverySock, buf, sizeof(buf) - 1, 0,
                                (sockaddr*)&from, &fromLen);
        if (n <= 0)
            continue;
        buf[n] = 0;

        // Parse "TAG|hostname|username|tcpPort".
        std::string s(buf);
        if (s.rfind(kBeaconTag, 0) != 0)
            continue;
        size_t p1 = s.find('|');
        size_t p2 = s.find('|', p1 + 1);
        size_t p3 = s.find('|', p2 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos ||
            p3 == std::string::npos)
            continue;
        Beacon b;
        b.hostname = s.substr(p1 + 1, p2 - p1 - 1);
        b.username = s.substr(p2 + 1, p3 - p2 - 1);
        b.tcpPort = (uint16_t)std::strtoul(s.substr(p3 + 1).c_str(), nullptr, 10);
        char ipstr[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &from.sin_addr, ipstr, sizeof(ipstr));
        b.ip = ipstr;
        b.lastSeenMs = nowMs();

        std::lock_guard<std::mutex> lk(mtx);
        bool merged = false;
        for (auto& e : beacons)
            if (e.ip == b.ip && e.tcpPort == b.tcpPort) {
                e = b;
                merged = true;
                break;
            }
        if (!merged)
            beacons.push_back(b);
    }
}

// ---- Session public API ---------------------------------------------------

Session::Session() : d_(new Impl) {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    char host[256] = {};
    if (::gethostname(host, sizeof(host)) == 0)
        d_->hostname = host;
    else
        d_->hostname = "host";
}

Session::~Session() {
    leave();
    stopDiscovery();
#ifdef _WIN32
    WSACleanup();
#endif
    delete d_;
}

Role Session::role() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->role;
}

bool Session::startHost(const std::string& username, uint16_t port, std::string& err) {
    leave();
    d_->username = username;
    d_->hostLost.store(false);

    // TCP listener on the configured port (SESSION panel / settings.conf).
    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        err = "socket() failed";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(ls, 8) != 0) {
        err = "port in use (another host on this machine?)";
        ::closesocket(ls);
        return false;
    }
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    ::getsockname(ls, (sockaddr*)&bound, &blen);
    d_->tcpPort = ntohs(bound.sin_port);

    // Broadcast-enabled UDP socket for the beacon.
    SOCKET bs = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (bs == INVALID_SOCKET) {
        err = "udp socket() failed";
        ::closesocket(ls);
        return false;
    }
    int yes = 1;
    ::setsockopt(bs, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));

    {
        std::lock_guard<std::mutex> lk(d_->mtx);
        d_->listenSock = ls;
        d_->beaconSock = bs;
        d_->role = Role::Host;
    }
    d_->hostRun.store(true);
    d_->hostThread = std::thread([this] { d_->hostLoop(); });
    return true;
}

// getaddrinfo covers every form the host field accepts with one call: IPv4 and
// IPv6 literals resolve locally, names go to the system resolver (DNS, and on
// both platforms the mDNS ".local" responder), and it hands back one entry per
// usable address. AF_UNSPEC lets a host that only publishes AAAA work; each
// candidate is tried in the order the resolver ranked them, so a machine with
// both records still falls back to IPv4 when v6 has no route.
bool Session::join(const std::string& host, uint16_t port, const std::string& username,
                   std::string& err) {
    leave();
    d_->username = username;
    d_->hostLost.store(false);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        err = "cannot resolve " + host;
        if (res)
            ::freeaddrinfo(res);
        return false;
    }

    SOCKET cs = INVALID_SOCKET;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        SOCKET s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (::connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) {
            cs = s;
            break;
        }
        ::closesocket(s);
    }
    ::freeaddrinfo(res);
    if (cs == INVALID_SOCKET) {
        err = "connection refused";
        return false;
    }

    // Announce who's joining so the host can list this viewer by name.
    std::string hello = username + "|" + d_->hostname;
    sendFrame(cs, MsgType::Hello, hello.data(), (uint32_t)hello.size());

    {
        std::lock_guard<std::mutex> lk(d_->mtx);
        d_->clientSock = cs;
        d_->role = Role::Spectator;
    }
    d_->recvRun.store(true);
    d_->recvThread = std::thread([this] { d_->recvLoop(); });
    return true;
}

void Session::leave() {
    // Signal threads to stop, close sockets to unblock them, then join.
    d_->hostRun.store(false);
    d_->recvRun.store(false);
    {
        std::lock_guard<std::mutex> lk(d_->mtx);
        d_->closeSocket(d_->listenSock);
        d_->closeSocket(d_->beaconSock);
        d_->closeSocket(d_->clientSock);
        for (const auto& p : d_->peers)
            ::closesocket(p.sock);
        d_->peers.clear();
    }
    if (d_->hostThread.joinable())
        d_->hostThread.join();
    if (d_->recvThread.joinable())
        d_->recvThread.join();
    {
        std::lock_guard<std::mutex> lk(d_->mtx);
        d_->role = Role::None;
        d_->inbound.clear();
        d_->projectSnapshot.clear();
    }
    d_->hostLost.store(false);
}

void Session::startDiscovery() {
    if (d_->discoveryRun.load())
        return;
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return;
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kDiscoveryPort);
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::closesocket(s);
        return;
    }
    d_->discoverySock = s;
    d_->discoveryRun.store(true);
    d_->discoveryThread = std::thread([this] { d_->discoveryLoop(); });
}

void Session::stopDiscovery() {
    d_->discoveryRun.store(false);
    {
        std::lock_guard<std::mutex> lk(d_->mtx);
        d_->closeSocket(d_->discoverySock);
    }
    if (d_->discoveryThread.joinable())
        d_->discoveryThread.join();
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->beacons.clear();
}

std::vector<Beacon> Session::beacons() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    uint64_t now = nowMs();
    std::vector<Beacon> live;
    for (const auto& b : d_->beacons)
        if (now - b.lastSeenMs <= kBeaconTtlMs)
            live.push_back(b);
    return live;
}

void Session::setProjectSnapshot(const std::string& bytes) {
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->projectSnapshot = bytes;
}

void Session::broadcastProject(const std::string& bytes) {
    setProjectSnapshot(bytes);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[sync] broadcastProject: %zu bytes to %d peer(s)", bytes.size(),
                peerCount());
    d_->broadcast(MsgType::Project, bytes.data(), (uint32_t)bytes.size());
}

void Session::sendPlay(int64_t frame)      { d_->sendFrameI64(MsgType::Play, frame); }
void Session::sendPause(int64_t frame)     { d_->sendFrameI64(MsgType::Pause, frame); }
void Session::sendSeek(int64_t frame)      { d_->sendFrameI64(MsgType::Seek, frame); }
void Session::sendSequence(int32_t seqIdx) {
    d_->broadcast(MsgType::Sequence, reinterpret_cast<const char*>(&seqIdx),
                  sizeof(seqIdx));
}
void Session::sendView(float zoom, float u, float v) {
    float p[3] = { zoom, u, v };
    d_->broadcast(MsgType::View, reinterpret_cast<const char*>(p), sizeof(p));
}
void Session::sendExposure(float gain, float gamma) {
    float p[2] = { gain, gamma };
    d_->broadcast(MsgType::Exposure, reinterpret_cast<const char*>(p), sizeof(p));
}
void Session::sendMatte(float ratio, float opacity, bool fitView) {
    char p[9];
    std::memcpy(p, &ratio, 4);
    std::memcpy(p + 4, &opacity, 4);
    p[8] = fitView ? 1 : 0;
    d_->broadcast(MsgType::Matte, p, sizeof(p));
}
void Session::broadcastAnnotations(const std::string& payload) {
    d_->broadcast(MsgType::Annotations, payload.data(), (uint32_t)payload.size());
}
void Session::sendClear(int64_t frame) { d_->sendFrameI64(MsgType::Clear, frame); }
void Session::sendStrokeToHost(const std::string& payload) {
    d_->sendToHost(MsgType::Stroke, payload.data(), (uint32_t)payload.size());
}
void Session::sendClearToHost(int64_t frame) {
    d_->sendToHost(MsgType::Clear, reinterpret_cast<const char*>(&frame), sizeof(frame));
}

std::vector<Message> Session::drain() {
    std::lock_guard<std::mutex> lk(d_->mtx);
    std::vector<Message> out = std::move(d_->inbound);
    d_->inbound.clear();
    return out;
}

bool Session::hostLost() const { return d_->hostLost.load(); }

int Session::peerCount() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return (int)d_->peers.size();
}

std::vector<PeerInfo> Session::peerList() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    std::vector<PeerInfo> out;
    out.reserve(d_->peers.size());
    for (const auto& p : d_->peers)
        out.push_back({ p.name.empty() ? "(unnamed)" : p.name, p.hostname, p.ip });
    return out;
}

} // namespace syncreview
