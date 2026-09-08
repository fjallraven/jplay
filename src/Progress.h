#pragma once

#include <atomic>
#include <mutex>
#include <string>

// A generic, thread-safe progress channel between a background worker (typically
// a call into the embedded Python interpreter) and the main-thread UI.
//
// The worker calls update()/setMessage() as it makes progress; the main thread
// polls fraction()/message() once per frame to draw the progress dialog. The
// message string is guarded by a mutex; the fraction is a plain atomic. The
// main thread never touches the GIL to read these, so there is no lock-ordering
// hazard with the worker (which holds the GIL while calling in from Python).
//
// Exposed to the embedded interpreter as `jplay.Progress` (see PythonStartup.cpp)
// so any host callback can accept one and report progress back to the host.
class ProgressReporter {
public:
    // fraction in [0,1]; a negative value means "indeterminate" (unknown total).
    // An empty message leaves the current message unchanged.
    void update(float fraction, const std::string& message = std::string()) {
        fraction_.store(fraction, std::memory_order_relaxed);
        if (!message.empty())
            setMessage(message);
    }

    void setMessage(const std::string& m) {
        std::lock_guard<std::mutex> lk(mtx_);
        message_ = m;
    }

    // Return to the initial state so a single reporter can be reused across tasks
    // (fraction indeterminate, no message, not cancelled).
    void reset() {
        fraction_.store(-1.f, std::memory_order_relaxed);
        cancel_.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mtx_);
        message_.clear();
    }

    float fraction() const { return fraction_.load(std::memory_order_relaxed); }

    std::string message() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return message_;
    }

    // Cooperative cancellation: the UI calls requestCancel(); the worker (or the
    // Python callback) polls cancelled() at its checkpoints and bails out. A
    // cancel can only take effect at the next poll — a single blocking call
    // (e.g. a large json.load) will not be interrupted mid-flight.
    void requestCancel() { cancel_.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return cancel_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> fraction_{ -1.f };
    std::atomic<bool> cancel_{ false };
    mutable std::mutex mtx_;
    std::string message_;
};
