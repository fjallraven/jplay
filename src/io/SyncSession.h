#pragma once

// Sync review networking (Phase 1). A lightweight LAN session where one peer is
// the Host and the rest are Spectators:
//   - Discovery: the host broadcasts a small UDP beacon (hostname + username +
//     TCP port); browsing spectators listen for beacons to populate a join list.
//   - Join: a spectator opens a TCP connection to the host. The host immediately
//     streams the serialized .jpproj so the spectator loads the same project.
//   - Control: the host streams small events (play / pause / seek / sequence
//     switch), plus the viewer exposure/gamma, which the host owns like zoom/pan.
//     Spectators apply them. The rest of the grade and OCIO stay local.
//   - Annotations: spectators may draw too; a spectator sends its completed
//     stroke to the host, which merges all markup and broadcasts the combined
//     set back for the frame. Spectators replace their markup with it.
//   - Disconnect: if the host drops, spectators detect it and fall back to free
//     viewing (see App::drainSync). No host election.
//
// This header is deliberately free of platform headers (no <winsock2.h>) so it
// can be included widely; all socket state lives in the .cpp behind a pimpl.

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace syncreview {

// Default TCP control port, so a spectator can join by typing just an IP (no
// port). Overridable per user in the SESSION panel; peers that browse by beacon
// pick the host's choice up automatically, a manual join needs "ip:port".
// One host per machine and port; a second local host fails to bind (reported).
constexpr uint16_t kDefaultPort = 45778;

enum class Role { None, Host, Spectator };

// Control-stream message kinds (host -> spectators). Wire framing is
// [u8 type][u32 payloadLen][payload], little-endian, in SyncSession.cpp.
enum class MsgType : uint8_t {
    Project  = 1, // payload: serialized .jpproj bytes (sent to each new joiner)
    Play     = 2, // payload: i64 start frame
    Pause    = 3, // payload: i64 stop frame
    Seek     = 4, // payload: i64 frame
    Sequence = 5, // payload: i32 sequence view index (-1 = All)
    Stroke   = 6, // payload: App-encoded completed pencil stroke (spectator -> host)
    Clear    = 7, // payload: i64 frame — clear the frame's markup (either direction)
    Annotations = 8, // payload: App-encoded combined stroke set (host -> spectators)
    Hello    = 9, // payload: "username|hostname" (spectator -> host, sent on connect)
    View     = 10, // payload: f32 zoom, f32 u, f32 v — frame zoom + normalized center
    Exposure = 11, // payload: f32 gain (f-stops), f32 gamma — the player's exposure
    Matte    = 12, // payload: f32 ratio (W/H, 0 = off), f32 opacity, u8 fitView —
                   // the letterbox matte. Part of the framing, not just decoration:
                   // Fit View fits the masked region, so View's zoom/pan only means
                   // the same thing on both ends when the matte matches.
};

// A host advertisement seen on the LAN (spectator-side browse list).
struct Beacon {
    std::string hostname;
    std::string username;
    std::string ip;          // dotted-quad of the sender
    uint16_t    tcpPort = 0;
    uint64_t    lastSeenMs = 0;
};

// A connected viewer, as known to the host (name from the Hello handshake).
struct PeerInfo {
    std::string name;
    std::string hostname;
    std::string ip;
};

// One decoded inbound control message, handed to the main thread via drain().
struct Message {
    MsgType     type = MsgType::Seek;
    int64_t     i64 = 0;     // frame (Play/Pause/Seek)
    int32_t     i32 = 0;     // sequence index (Sequence)
    float       zoom = 1.0f; // frame zoom factor (View)
    float       u = 0.5f;    // normalized image x at player center (View)
    float       v = 0.5f;    // normalized image y at player center (View)
    float       gain = 0.0f;  // exposure in f-stops (Exposure)
    float       gamma = 1.0f; // display gamma (Exposure)
    float       ratio = 0.0f;   // letterbox aspect W/H, 0 = off (Matte)
    float       opacity = 1.0f; // matte bar opacity 0..1 (Matte)
    bool        fitView = false; // fit the view to the masked region (Matte)
    std::string blob;        // project bytes (Project)
};

class Session {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- lifecycle ------------------------------------------------------
    // Host: bind a TCP listener on `port` and start broadcasting the discovery
    // beacon. The beacon carries the port, so a browsing spectator picks it up
    // even when the host isn't on kDefaultPort.
    bool startHost(const std::string& username, uint16_t port, std::string& err);
    // Spectator: connect to a host over TCP. The host may be given as an IPv4 or
    // IPv6 literal or as a resolvable name (DNS, mDNS ".local", NetBIOS); it is
    // passed through getaddrinfo, so this call blocks for as long as resolution
    // takes. On success the host will push the project, which surfaces as a
    // Project message from drain().
    bool join(const std::string& host, uint16_t port, const std::string& username,
              std::string& err);
    // Tear everything down and return to Role::None.
    void leave();

    Role role() const;
    bool active() const { return role() != Role::None; }

    // ---- discovery (spectator browse) -----------------------------------
    void startDiscovery();               // begin listening for beacons
    void stopDiscovery();
    std::vector<Beacon> beacons() const;  // snapshot of currently-live beacons

    // ---- host -> spectators ---------------------------------------------
    // Set the project bytes handed to every new joiner. Call whenever the host's
    // project changes (load / new). Cheap: just stores the buffer.
    void setProjectSnapshot(const std::string& bytes);
    void broadcastProject(const std::string& bytes); // push to all current peers now
    void sendPlay(int64_t frame);
    void sendPause(int64_t frame);
    void sendSeek(int64_t frame);
    void sendSequence(int32_t seqIdx);
    void sendView(float zoom, float u, float v); // frame zoom + normalized center
    void sendExposure(float gain, float gamma);  // player exposure (f-stops) + gamma
    void sendMatte(float ratio, float opacity, bool fitView); // letterbox matte
    void broadcastAnnotations(const std::string& payload); // combined stroke set
    void sendClear(int64_t frame);               // clear the frame's markup

    // ---- spectator -> host ----------------------------------------------
    // A spectator sends its own markup to the host, which merges it into the
    // authoritative set and re-broadcasts the combined result to all viewers.
    void sendStrokeToHost(const std::string& payload); // one completed stroke
    void sendClearToHost(int64_t frame);               // clear-frame request

    // ---- main-thread pump -----------------------------------------------
    // Return (and clear) all control messages decoded since the last call.
    std::vector<Message> drain();
    // True once the receiver thread saw the host drop (spectator only). Latches
    // until leave() is called.
    bool hostLost() const;
    // Number of connected spectators (host side).
    int peerCount() const;
    // Name + hostname/ip of each connected spectator (host side).
    std::vector<PeerInfo> peerList() const;

private:
    struct Impl;
    Impl* d_;
};

} // namespace syncreview
