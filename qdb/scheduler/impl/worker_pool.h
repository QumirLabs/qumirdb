#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace NQdb {
namespace NScheduler {

// Threads that outlive the runs of a process. A run takes idle threads and starts
// new ones when none is idle, so it never waits for the threads of another run: a
// query that waits for a query on another node cannot deadlock on a full pool.
class TWorkerPool {
public:
    // The jobs of one Start.
    class TRun {
    public:
        TRun() = default;
        TRun(const TRun&) = delete;
        TRun& operator=(const TRun&) = delete;

    private:
        friend class TWorkerPool;

        std::condition_variable Finished_;
        size_t Running_ = 0;
    };

    // More idle threads than `maxIdle` exit; the default keeps two runs at full width.
    explicit TWorkerPool(size_t maxIdle = 2 * std::max(1u, std::thread::hardware_concurrency()));
    // Joins every thread; every run must have been waited for.
    ~TWorkerPool();

    TWorkerPool(const TWorkerPool&) = delete;
    TWorkerPool& operator=(const TWorkerPool&) = delete;

    // Runs `job` on `count` threads at once. On a throw the jobs already started still run.
    void Start(size_t count, const std::function<void()>& job, TRun& run);
    // Returns once each job of `run` has returned and its thread is back in the pool.
    void Wait(TRun& run);
    // The threads that run a job or are idle.
    size_t Threads() const;

private:
    struct TJob {
        std::function<void()> Work;
        TRun* Run = nullptr;
    };

    void Loop(TJob job);

    const size_t MaxIdle_;
    mutable std::mutex Mutex_;
    std::condition_variable HasJob_;
    // Jobs handed to idle threads that have not taken them yet.
    std::deque<TJob> Jobs_;
    // Idle threads less the jobs waiting for them.
    size_t Idle_ = 0;
    std::vector<std::thread> Threads_;
    // The last thread to exit; the next one to exit joins it, so Start never joins.
    std::thread Exited_;
    bool Stopping_ = false;
};

} // namespace NScheduler
} // namespace NQdb
