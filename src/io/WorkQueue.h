#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// A background work/job queue for the App.
//
// - submit() enqueues work that runs on one of N pool threads. When the work
//   finishes, its completion callback is queued and runs on the MAIN thread
//   (drainCompletions(), called once per frame) so it can safely touch SDL/UI
//   state without extra locking.
// - Tasks run concurrently across the pool and may complete out of order.
// - setPaused(true) stops NEW tasks from starting (e.g. while playing media);
//   any in-flight task runs to completion. setPaused(false) resumes.
// - reset() drops all pending (not-yet-started) tasks — their completion
//   callbacks never fire — signals the shared stop flag so cooperative in-flight
//   tasks can bail, waits until the pool is idle, then discards the in-flight
//   tasks' completions. The queue stays usable afterwards (used on project
//   load / quit).
// - The destructor stops the pool threads.
//
// Submission, reset() and drainCompletions() are expected to be called only
// from the main thread (single producer); the pool threads are the consumers.
class WorkQueue {
public:
    // Work receives a stop flag it may poll to bail out early on reset/shutdown.
    using Work = std::function<void(const std::atomic<bool>& stop)>;
    using Done = std::function<void()>; // runs on the main thread

    explicit WorkQueue(int threads = 0); // 0 => derive from hardware_concurrency
    ~WorkQueue();

    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;

    // Enqueue work. When it finishes (and was not cancelled by reset()), `done`
    // is queued to run on the main thread via drainCompletions().
    void submit(Work work, Done done = {});

    // Main thread: run completion callbacks for finished work. Call per frame.
    void drainCompletions();

    // Hold / resume dequeuing of new tasks. In-flight tasks are unaffected.
    void setPaused(bool paused);
    bool paused() const;

    // Cancel pending tasks, signal + wait for in-flight, discard completions.
    void reset();

private:
    struct Job {
        Work work;
        Done done;
    };

    void workerLoop();

    std::vector<std::thread> threads_;
    std::deque<Job> pending_;
    std::vector<Done> completed_; // finished work waiting to run on the main thread

    mutable std::mutex mtx_;
    std::condition_variable cv_;     // wakes workers: new job / unpause / quit
    std::condition_variable idleCv_; // wakes reset() when active_ hits 0

    bool paused_ = false;
    bool quitting_ = false;          // permanent: tear the pool down
    std::atomic<bool> stop_{ false };// per-reset cancel flag handed to tasks
    int active_ = 0;                 // tasks currently executing
};
