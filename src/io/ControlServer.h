#pragma once

// Local control channel. A loopback-only TCP listener that lets another process
// on this machine drive jplay: open a project / OTIO / media source, and query
// application state. Built for an MCP server to talk to, but the wire format is
// plain enough for any language (or a hand-typed netcat session).
//
//   - Transport: TCP on 127.0.0.1 only (never INADDR_ANY — this is not a LAN
//     service). One newline-terminated JSON request per connection, one
//     newline-terminated JSON response back, then the connection closes.
//   - Single instance: binding the port IS the instance lock. The first jplay
//     to start owns the channel; later instances fail to bind and simply don't
//     listen. The bind is retried every ~2s, so if the owner exits a surviving
//     instance takes the channel over.
//   - Threading: the socket thread only reads bytes and queues a Request. All
//     application state is touched by the main thread in App::drainControl(),
//     which fulfils the request's promise with the response text. The socket
//     thread waits on that promise (bounded — see kReplyTimeout) and writes it.
//   - Serialization: requests are handled one at a time on the socket thread, so
//     a second client waits behind the first. Fine for a single-user tool.
//
// All platform/socket code is confined to the .cpp behind a pimpl, so this
// header stays free of <winsock2.h> (the same discipline as SyncSession.h).

#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <string>
#include <vector>

namespace control {

// Fallback port, used when jplay_preferences.conf names none. Kept clear of
// 45777 (UDP sync beacon) and 45778 (sync TCP), which sync review already owns.
constexpr uint16_t kDefaultPort = 45125;

// The port to listen on: `port` under [control] in jplay_preferences.conf, or
// kDefaultPort when that option is absent or not a valid port number.
uint16_t configuredPort();

// How long the socket thread waits for the main thread to produce a response.
// Generous because a command may open media off a network share synchronously
// (see App::ensureMedia); a timeout does NOT cancel the command, it only gives
// up on reporting its result.
constexpr std::chrono::seconds kReplyTimeout{ 120 };

// One inbound command awaiting the main thread. Move-only (it owns a promise).
struct Request {
    std::string json;                // the raw request line
    std::promise<std::string> reply; // main thread: set to the response line
};

class Server {
public:
    Server();
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Start the socket thread. Returns immediately; the initial bind may fail
    // (another instance owns the port) and is retried in the background, so
    // there is no failure to report here — check listening().
    void start(uint16_t port = configuredPort());
    void stop();

    bool listening() const;  // true while this instance owns the port
    uint16_t port() const;

    // Main thread: take every command received since the last call.
    std::vector<Request> drain();

private:
    struct Impl;
    Impl* d_;
};

// ---- minimal JSON helpers -------------------------------------------------
// Requests are flat objects of scalars, and responses are assembled by hand, so
// a full JSON library isn't warranted. rapidjson is in vcpkg.json but is not
// wired into the jplay target and isn't present in the Linux prebuilt deps —
// tools/create_otio_project carries its own reader for the same reason.

// Escape a string for embedding in a JSON double-quoted literal. Bytes >= 0x80
// pass through unchanged (output is UTF-8, not \u-escaped ASCII).
std::string jsonEscape(const std::string& s);

// Parse a flat JSON object into key -> value. Every value is returned as text:
// strings unquoted and unescaped (including \uXXXX, decoded to UTF-8), numbers
// and true/false/null as their literal spelling. Nested objects/arrays are
// rejected. Returns false on malformed input.
bool parseFlatObject(const std::string& json, std::map<std::string, std::string>& out);

} // namespace control
