#include <gtest/gtest.h>

#include <qdb/kernel/thread_context.h>
#include <qdb/scheduler/settings.h>
#include <qdb/scheduler/state.h>

#include <algorithm>
#include <thread>

using namespace NQdb::NScheduler;

namespace {

class TTestTask : public ITaskNode {
public:
    ETaskResult Execute(NQdb::TThreadExecutionContext&) override {
        ++Calls_;
        return ETaskResult::OK;
    }

    int Calls() const {
        return Calls_;
    }

private:
    int Calls_ = 0;
};

TEST(SchedulerCore, TaskNodeInterfaceExecutes) {
    NQdb::TThreadExecutionContext context;
    TTestTask task;
    ITaskNode* node = &task;

    EXPECT_EQ(node->Execute(context), ETaskResult::OK);
    EXPECT_EQ(task.Calls(), 1);
}

TEST(SchedulerCore, TaskStateValuesAreStable) {
    EXPECT_EQ(static_cast<int>(ETaskState::Idle), 0);
    EXPECT_EQ(static_cast<int>(ETaskState::Queued), 1);
    EXPECT_EQ(static_cast<int>(ETaskState::Running), 2);
    EXPECT_EQ(static_cast<int>(ETaskState::Reschedule), 3);
    EXPECT_EQ(static_cast<int>(ETaskState::Finished), 4);
}

TEST(SchedulerCore, SettingsDefaultsEnableAutomaticParallelism) {
    TSettings settings;

    EXPECT_EQ(settings.Scheduler.Mode, EExecutionMode::ThreadedScheduler);
    EXPECT_EQ(settings.Scheduler.WorkerCount,
        std::max(1u, std::thread::hardware_concurrency()));
    EXPECT_EQ(settings.Queue.RowsetCapacityPerLane, 4u);
    EXPECT_EQ(settings.ScanSplit.MaxScanTasks, 0u);
    EXPECT_EQ(settings.HashShuffle.PartitionCount, 0u);
    EXPECT_EQ(settings.HashShuffle.MaxPartitionCount, 0u);
    EXPECT_EQ(settings.HashShuffle.TargetOutputBatchRows, 16u * 1024u);
    EXPECT_EQ(settings.HashShuffle.MaxOutputBatchRows, 64u * 1024u);
    EXPECT_EQ(settings.HashShuffle.TargetOutputBatchBytes, 1024u * 1024u);
    EXPECT_TRUE(settings.Aggregate.EnablePartial);
    EXPECT_EQ(settings.Aggregate.PartialMinInputRows, 1024u * 1024u);
    EXPECT_EQ(settings.Aggregate.PartialMinReductionFactor, 8u);
    EXPECT_EQ(settings.Aggregate.CascadeGlobal, EGlobalCascadeMode::Auto);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
