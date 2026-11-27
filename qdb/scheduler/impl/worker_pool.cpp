#include <qdb/scheduler/impl/worker_pool.h>

#include <utility>

namespace NQdb {
namespace NScheduler {

TWorkerPool::TWorkerPool(size_t maxIdle)
    : MaxIdle_(maxIdle)
{}

TWorkerPool::~TWorkerPool()
{
    {
        std::lock_guard lock(Mutex_);
        Stopping_ = true;
    }
    HasJob_.notify_all();
    // Every thread is idle, so none changes Threads_ or Exited_ any more.
    for (auto& thread : Threads_) {
        thread.join();
    }
    if (Exited_.joinable()) {
        Exited_.join();
    }
}

void TWorkerPool::Start(size_t count, const std::function<void()>& job, TRun& run)
{
    std::lock_guard lock(Mutex_);
    for (size_t i = 0; i < count; ++i) {
        if (Idle_ > 0) {
            --Idle_;
            Jobs_.push_back({.Work = job, .Run = &run});
            HasJob_.notify_one();
        } else {
            Threads_.emplace_back([this, first = TJob{.Work = job, .Run = &run}]() mutable {
                // libc++ copies a small std::function on move; exchange empties `first`.
                Loop(std::exchange(first, {}));
            });
        }
        ++run.Running_;
    }
}

void TWorkerPool::Wait(TRun& run)
{
    std::unique_lock lock(Mutex_);
    run.Finished_.wait(lock, [&run] {
        return run.Running_ == 0;
    });
}

size_t TWorkerPool::Threads() const
{
    std::lock_guard lock(Mutex_);
    return Threads_.size();
}

void TWorkerPool::Loop(TJob job)
{
    for (;;) {
        job.Work();
        // Drops what the job captured before its run can end.
        job.Work = nullptr;
        std::unique_lock lock(Mutex_);
        // Back in the pool in the same step as the run counts it, so the next
        // Start after Wait finds this thread idle.
        if (--job.Run->Running_ == 0) {
            job.Run->Finished_.notify_all();
        }
        if (Idle_ >= MaxIdle_) {
            const auto self = std::ranges::find(Threads_, std::this_thread::get_id(), &std::thread::get_id);
            auto previous = std::exchange(Exited_, std::move(*self));
            Threads_.erase(self);
            lock.unlock();
            if (previous.joinable()) {
                previous.join();
            }
            return;
        }
        ++Idle_;
        HasJob_.wait(lock, [this] {
            return !Jobs_.empty() || Stopping_;
        });
        if (Jobs_.empty()) {
            return;
        }
        job = std::move(Jobs_.front());
        Jobs_.pop_front();
    }
}

} // namespace NScheduler
} // namespace NQdb
