#include "WorkQueue.h"

#include <algorithm>

WorkQueue::WorkQueue(int threads) {
    int n = threads > 0
                ? threads
                : std::clamp((int)std::thread::hardware_concurrency() - 1, 1, 8);
    threads_.reserve(n);
    for (int i = 0; i < n; ++i)
        threads_.emplace_back([this] { workerLoop(); });
}

WorkQueue::~WorkQueue() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        quitting_ = true;
        stop_.store(true);
        pending_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
}

void WorkQueue::submit(Work work, Done done) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.push_back(Job{ std::move(work), std::move(done) });
    }
    cv_.notify_one();
}

void WorkQueue::drainCompletions() {
    std::vector<Done> ready;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ready.swap(completed_);
    }
    for (auto& d : ready)
        if (d)
            d();
}

void WorkQueue::setPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (paused_ == paused)
            return;
        paused_ = paused;
    }
    if (!paused)
        cv_.notify_all(); // let idle workers pick up held tasks
}

bool WorkQueue::paused() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return paused_;
}

void WorkQueue::reset() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.clear();  // drop not-yet-started tasks; their `done` never fires
        stop_.store(true); // ask in-flight tasks to bail early
    }
    cv_.notify_all();
    {
        std::unique_lock<std::mutex> lk(mtx_);
        idleCv_.wait(lk, [this] { return active_ == 0; });
        completed_.clear(); // discard in-flight tasks' completion callbacks
        stop_.store(false); // ready to accept new work again
    }
}

void WorkQueue::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] {
                return quitting_ || (!paused_ && !pending_.empty());
            });
            if (quitting_)
                return;
            job = std::move(pending_.front());
            pending_.pop_front();
            ++active_;
        }

        if (job.work)
            job.work(stop_);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (job.done && !stop_.load())
                completed_.push_back(std::move(job.done));
            if (--active_ == 0)
                idleCv_.notify_all();
        }
    }
}
