#include "graph_cleaner.h"

#include <utility>

namespace NQdb {
namespace NScheduler {

TGraphCleaner::TGraphCleaner(size_t limit)
    : Limit_(limit)
    , Thread_([this] {
        Loop();
    })
{}

TGraphCleaner::~TGraphCleaner()
{
    {
        std::lock_guard lock(Mutex_);
        Stopping_ = true;
    }
    HasWork_.notify_one();
    Thread_.join();
}

void TGraphCleaner::Retire(std::shared_ptr<void> graph)
{
    {
        std::unique_lock lock(Mutex_);
        HasRoom_.wait(lock, [this] { return Pending_ < Limit_; });
        ++Pending_;
        Queue_.push_back(std::move(graph));
    }
    HasWork_.notify_one();
}

void TGraphCleaner::Loop()
{
    std::unique_lock lock(Mutex_);
    for (;;) {
        HasWork_.wait(lock, [this] { return !Queue_.empty() || Stopping_; });
        if (Queue_.empty()) {
            return;
        }
        auto graph = std::move(Queue_.front());
        Queue_.pop_front();
        lock.unlock();
        graph.reset();
        lock.lock();
        --Pending_;
        HasRoom_.notify_all();
    }
}

} // namespace NScheduler
} // namespace NQdb
