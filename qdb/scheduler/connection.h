#pragma once

#include <qdb/io/io.h>
#include <qdb/io/rowset_ops.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace NQdb {
namespace NScheduler {

enum class EConnectionKind {
    OneToOne = 0,
    Gather = 1,
    Broadcast = 2,
};

enum class EFetchResult {
    OK = 0,
    NO_DATA = 1,
    FINISHED = 2,
};

// What a consumer that asks for data does about a producer parked on a full output.
enum class EParkedDemand {
    // The lane has room again: run the producer.
    WakeProducer = 0,
    // The producer cannot push yet, but the lane holds rows: run the consumer again.
    RerunConsumer = 1,
    // Neither can go on: wait for the lane the producer is parked on to drain.
    Wait = 2,
};

struct TConnectionStats {
    uint64_t Pushed = 0;
    uint64_t Popped = 0;
    uint64_t Finished = 0;
    uint64_t BlockedPush = 0;
    uint64_t EmptyFetch = 0;
    uint64_t FinishedFetch = 0;
};

class IConnection {
public:
    virtual ~IConnection() = default;

    virtual EConnectionKind Kind() const = 0;
    virtual void Resize(size_t srcCount, size_t dstCount) = 0;
    virtual size_t SrcCount() const = 0;
    virtual size_t DstCount() const = 0;

    virtual bool CanPush(size_t srcId) const = 0;
    virtual bool Push(size_t srcId, TRowSet&& rowSet) = 0;
    virtual void Finish(size_t srcId) = 0;

    virtual EFetchResult Fetch(size_t dstId, TRowSet& rowSet) = 0;
    // The consumer of lane `dstId` reads no more: it finished or was abandoned,
    // maybe while it still runs on another thread. A broadcast then feeds only its
    // open lanes; other connections keep the lane as it is.
    virtual void Close(size_t dstId);

    // A push of `srcId` that found no room marks the lane it waits for. The
    // scheduler parks such a producer until the consumer of that lane drains it.
    virtual bool HasBlockedPush(size_t srcId) const;
    // For the consumer of the lane between `srcId` and `dstId` that asks for data
    // while its producer is parked; takes the mark when it wakes the producer. A
    // connection that keeps no marks always wakes the producer.
    virtual EParkedDemand ParkedDemand(size_t srcId, size_t dstId);

    void SetDebugName(std::string name);
    const std::string& DebugName() const;
    void SetStatsEnabled(bool enabled);
    TConnectionStats Stats() const;

    // Row descriptor, one entry per column; empty until the graph builder sets it.
    // Set while building the graph, before Seal; sinks keep spans into it, so it must not change later.
    void SetRowLayouts(std::vector<TValueLayout> layouts);
    std::span<const TValueLayout> RowLayouts() const;

protected:
    void CountPushed(uint64_t count = 1) const;
    void CountPopped(uint64_t count = 1) const;
    void CountFinished(uint64_t count = 1) const;
    void CountBlockedPush(uint64_t count = 1) const;
    void CountEmptyFetch(uint64_t count = 1) const;
    void CountFinishedFetch(uint64_t count = 1) const;

private:
    std::string DebugName_;
    std::vector<TValueLayout> RowLayouts_;
    bool StatsEnabled_ = false;
    mutable std::atomic<uint64_t> Pushed_ = 0;
    mutable std::atomic<uint64_t> Popped_ = 0;
    mutable std::atomic<uint64_t> Finished_ = 0;
    mutable std::atomic<uint64_t> BlockedPush_ = 0;
    mutable std::atomic<uint64_t> EmptyFetch_ = 0;
    mutable std::atomic<uint64_t> FinishedFetch_ = 0;
};

} // namespace NScheduler
} // namespace NQdb
