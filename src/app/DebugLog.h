#pragma once

#include <atomic>

// The Settings panel's "Debug Logging" checkbox, as a process-wide flag.
//
// Verbose diagnostics (HDR / OCIO / display state, sync traffic, project load,
// timeline and review details) are only wanted while investigating a problem, so
// they are logged only when this is on. Genuine failures — shader compile errors,
// a null processor, a renderer that could not be created — stay unconditional at
// warning/error level.
//
// Written from the main thread, read wherever logging happens, hence the atomic.
inline std::atomic<bool>& jplayDebugLogFlag() {
    static std::atomic<bool> enabled{ false };
    return enabled;
}
inline bool jplayDebugLogging() {
    return jplayDebugLogFlag().load(std::memory_order_relaxed);
}
inline void jplaySetDebugLogging(bool enabled) {
    jplayDebugLogFlag().store(enabled, std::memory_order_relaxed);
}
