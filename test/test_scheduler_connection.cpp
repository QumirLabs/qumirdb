#include <gtest/gtest.h>

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/connections/local.h>

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

using namespace NQdb;
using namespace NQdb::NScheduler;

namespace {

void CountDestroy(TRowSet* rowSet) {
    auto* counter = static_cast<std::atomic<int>*>(rowSet->Private);
    counter->fetch_add(1, std::memory_order_relaxed);
}

TRowSet MakeRowSet(int64_t rows, std::atomic<int>* destroyCount = nullptr) {
    return TRowSet{
        .Columns = nullptr,
        .ColumnCount = 0,
        .RowCount = rows,
        .Selection = nullptr,
        .Destroy = destroyCount ? CountDestroy : nullptr,
        .Private = destroyCount,
        .RefCount = 1,
    };
}

TEST(SchedulerConnection, OneToOneTransfersRowSets) {
    TOneToOneConnection conn(1);
    conn.Resize(2, 2);

    auto input = MakeRowSet(11);
    ASSERT_TRUE(conn.CanPush(0));
    ASSERT_TRUE(conn.Push(0, std::move(input)));
    EXPECT_EQ(input.RowCount, 0);
    EXPECT_FALSE(conn.CanPush(0));

    TRowSet out{};
    EXPECT_EQ(conn.Fetch(1, out), EFetchResult::NO_DATA);
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::OK);
    EXPECT_EQ(out.RowCount, 11);
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::NO_DATA);
}

TEST(SchedulerConnection, OneToOneReportsFinishAfterQueueIsDrained) {
    TOneToOneConnection conn(1);
    auto input = MakeRowSet(7);
    ASSERT_TRUE(conn.Push(0, std::move(input)));
    conn.Finish(0);

    TRowSet out{};
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::OK);
    EXPECT_EQ(out.RowCount, 7);
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::FINISHED);
}

TEST(SchedulerConnection, TracksDebugNameAndCounters) {
    TOneToOneConnection conn(1);
    conn.SetDebugName("test-edge");
    conn.SetStatsEnabled(true);

    auto input = MakeRowSet(7);
    ASSERT_TRUE(conn.Push(0, std::move(input)));
    conn.Finish(0);

    TRowSet out{};
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::OK);
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::FINISHED);

    auto stats = conn.Stats();
    EXPECT_EQ(conn.DebugName(), "test-edge");
    EXPECT_EQ(stats.Pushed, 1u);
    EXPECT_EQ(stats.Popped, 1u);
    EXPECT_EQ(stats.Finished, 1u);
    EXPECT_EQ(stats.FinishedFetch, 1u);
}

TEST(SchedulerConnection, OneToOnePreservesInputWhenPushIsBlocked) {
    TOneToOneConnection conn(1);
    auto first = MakeRowSet(1);
    auto second = MakeRowSet(2);

    ASSERT_TRUE(conn.Push(0, std::move(first)));
    EXPECT_FALSE(conn.Push(0, std::move(second)));
    EXPECT_EQ(second.RowCount, 2);
}

TEST(SchedulerConnection, GatherPollsProducerLanesAndFinishes) {
    TGatherConnection conn(1);
    conn.Resize(3, 1);

    auto first = MakeRowSet(10);
    auto second = MakeRowSet(20);
    ASSERT_TRUE(conn.Push(0, std::move(first)));
    ASSERT_TRUE(conn.Push(2, std::move(second)));

    TRowSet out{};
    std::vector<int64_t> rows;
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::OK);
    rows.push_back(out.RowCount);
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::OK);
    rows.push_back(out.RowCount);
    std::ranges::sort(rows);
    EXPECT_EQ(rows, (std::vector<int64_t>{10, 20}));
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::NO_DATA);

    conn.Finish(0);
    conn.Finish(1);
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::NO_DATA);
    conn.Finish(2);
    EXPECT_EQ(conn.Fetch(0, out), EFetchResult::FINISHED);
}

TEST(SchedulerConnection, QueuedRowSetsAreReleasedOnConnectionDestroy) {
    std::atomic<int> destroyCount = 0;
    {
        TOneToOneConnection conn(2);
        auto first = MakeRowSet(1, &destroyCount);
        auto second = MakeRowSet(2, &destroyCount);
        ASSERT_TRUE(conn.Push(0, std::move(first)));
        ASSERT_TRUE(conn.Push(0, std::move(second)));
    }

    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 2);
}

TEST(SchedulerConnection, FetchedRowSetOwnershipMovesToConsumer) {
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection conn(1);
    auto input = MakeRowSet(1, &destroyCount);
    ASSERT_TRUE(conn.Push(0, std::move(input)));

    TRowSet out{};
    ASSERT_EQ(conn.Fetch(0, out), EFetchResult::OK);
    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 0);

    Release(&out);
    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 1);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
