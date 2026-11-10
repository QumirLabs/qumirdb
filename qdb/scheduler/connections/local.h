#pragma once

#include <qdb/scheduler/connection.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace NQdb {
namespace NScheduler {

struct TConnectionLane;

class TLocalConnection : public IConnection {
protected:
    // Push/can-push against a single lane, updating counters. Shared by the
    // single-lane connections (one-to-one, gather). On a failed push the rowset
    // is left untouched for a retry.
    bool LaneCanPush(TConnectionLane& lane) const;
    bool LanePush(TConnectionLane& lane, TRowSet& rowSet) const;
};

class TOneToOneConnection : public TLocalConnection {
public:
    explicit TOneToOneConnection(size_t capacity = 1);
    ~TOneToOneConnection() override;

    EConnectionKind Kind() const override;
    void Resize(size_t srcCount, size_t dstCount) override;
    size_t SrcCount() const override;
    size_t DstCount() const override;

    bool CanPush(size_t srcId) const override;
    bool Push(size_t srcId, TRowSet&& rowSet) override;
    void Finish(size_t srcId) override;

    EFetchResult Fetch(size_t dstId, TRowSet& rowSet) override;

    size_t Capacity() const;

private:
    size_t Capacity_;
    size_t Size_ = 0;
    std::vector<std::unique_ptr<TConnectionLane>> Lanes_;
};

class TGatherConnection : public TLocalConnection {
public:
    explicit TGatherConnection(size_t capacity = 1);
    ~TGatherConnection() override;

    EConnectionKind Kind() const override;
    void Resize(size_t srcCount, size_t dstCount) override;
    size_t SrcCount() const override;
    size_t DstCount() const override;

    bool CanPush(size_t srcId) const override;
    bool Push(size_t srcId, TRowSet&& rowSet) override;
    void Finish(size_t srcId) override;

    EFetchResult Fetch(size_t dstId, TRowSet& rowSet) override;

    size_t Capacity() const;

private:
    size_t Capacity_;
    size_t Size_ = 0;
    size_t FetchId_ = 0;
    std::atomic<size_t> FinishedCount_ = 0;
    std::vector<std::unique_ptr<TConnectionLane>> Lanes_;
};

// One producer, N consumers: every pushed rowset is replicated (shared) to all
// destination lanes. Used to broadcast a small side (e.g. a scalar) to every
// partition of the vector side in a cross join.
class TBroadcastConnection : public TLocalConnection {
public:
    explicit TBroadcastConnection(size_t capacity = 1);
    ~TBroadcastConnection() override;

    EConnectionKind Kind() const override;
    void Resize(size_t srcCount, size_t dstCount) override;
    size_t SrcCount() const override;
    size_t DstCount() const override;

    bool CanPush(size_t srcId) const override;
    bool Push(size_t srcId, TRowSet&& rowSet) override;
    void Finish(size_t srcId) override;

    EFetchResult Fetch(size_t dstId, TRowSet& rowSet) override;
    void Close(size_t dstId) override;

    size_t Capacity() const;

private:
    size_t Capacity_;
    size_t DstCount_ = 0;
    std::atomic<bool> Finished_ = false;
    std::vector<std::unique_ptr<TConnectionLane>> Lanes_;
};

} // namespace NScheduler
} // namespace NQdb
