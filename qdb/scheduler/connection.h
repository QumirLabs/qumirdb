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
    HashShuffle = 2,
    Broadcast = 3,
};

enum class EFetchResult {
    OK = 0,
    NO_DATA = 1,
    FINISHED = 2,
};

struct TConnectionStats {
    uint64_t Pushed = 0;
    uint64_t Popped = 0;
    uint64_t Finished = 0;
    uint64_t BlockedPush = 0;
    uint64_t EmptyFetch = 0;
    uint64_t FinishedFetch = 0;
};

// Physical contract of a hash repartition exchange. The local scheduler uses
// an in-process many-to-many connection; a distributed scheduler can replace
// that transport while preserving the key and destination-partition contract.
struct TRepartitionSpec {
    std::vector<std::string> Keys;
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

    void SetDebugName(std::string name);
    const std::string& DebugName() const;
    void SetStatsEnabled(bool enabled);
    TConnectionStats Stats() const;

    // Row descriptor, one entry per column; empty until the graph builder sets it.
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
