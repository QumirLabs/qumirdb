#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace NQdb {
namespace NScheduler {

// Destroys the graphs of finished queries on its own thread: a large graph
// takes seconds to free, which the actor thread spends on the reply and the
// next query instead. At most `limit` retired graphs hold memory at a time.
class TGraphCleaner {
public:
    // `limit` is at least 1.
    explicit TGraphCleaner(size_t limit);
    // Destroys the graphs still queued.
    ~TGraphCleaner();

    TGraphCleaner(const TGraphCleaner&) = delete;
    TGraphCleaner& operator=(const TGraphCleaner&) = delete;

    // Takes the last reference to a graph nothing runs any more; it must not own
    // the cleaner. Waits while `limit` graphs retired earlier are not destroyed yet.
    void Retire(std::shared_ptr<void> graph);

private:
    void Loop();

    const size_t Limit_;
    std::mutex Mutex_;
    std::condition_variable HasWork_;
    std::condition_variable HasRoom_;
    std::deque<std::shared_ptr<void>> Queue_;
    // The queued graphs and the one being destroyed.
    size_t Pending_ = 0;
    bool Stopping_ = false;
    // Started last, once the members it uses exist.
    std::thread Thread_;
};

} // namespace NScheduler
} // namespace NQdb
