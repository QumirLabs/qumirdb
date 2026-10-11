#include <qdb/scheduler/scan_split.h>

#include <algorithm>

namespace NQdb {
namespace NScheduler {
namespace {

TScanSplit MakeSingleSplit(
    const std::vector<TScanRowGroup>& rowGroups,
    bool serialRead)
{
    TScanSplit split;
    split.RowGroups.reserve(rowGroups.size());
    split.SerialRead = serialRead;
    for (const auto& rowGroup : rowGroups) {
        split.RowGroups.push_back(rowGroup.RowGroup);
        split.RowCount += rowGroup.RowCount;
        split.ByteSize += rowGroup.ByteSize;
    }
    return split;
}

} // namespace

size_t ChooseScanTaskCount(
    const std::vector<TScanRowGroup>& rowGroups,
    const TSettings& settings,
    bool feedsJoin)
{
    if (settings.ScanSplit.MaxScanTasks != 0) {
        return settings.ScanSplit.MaxScanTasks;
    }
    const size_t workers = std::max<size_t>(settings.Scheduler.WorkerCount, 1);
    if (rowGroups.empty() || workers == 1 ||
        settings.Scheduler.Mode == EExecutionMode::SingleThreadedScheduler ||
        settings.ScanSplit.Strategy == EScanSplitStrategy::SerialRead)
    {
        return 1;
    }

    uint64_t rows = 0;
    uint64_t bytes = 0;
    for (const auto& group : rowGroups) {
        rows += static_cast<uint64_t>(std::max<int64_t>(group.RowCount, 0));
        bytes += static_cast<uint64_t>(std::max<int64_t>(group.ByteSize, 0));
    }
    auto tasksFor = [](uint64_t work, size_t target) -> uint64_t {
        return target == 0 ? 0 : work / target + (work % target != 0);
    };
    const uint64_t sizeTasks = std::max({
        tasksFor(rows, settings.ScanSplit.TargetRowsPerTask),
        tasksFor(bytes, settings.ScanSplit.TargetBytesPerTask),
        uint64_t{1}});

    size_t budget = std::min(workers, rowGroups.size());
    if (!feedsJoin) {
        // Saturate at the available row groups without overflowing 2 * workers.
        budget += std::min(workers, rowGroups.size() - budget);
    }
    return static_cast<size_t>(std::min<uint64_t>(sizeTasks, budget));
}

std::vector<TScanSplit> BuildScanSplits(
    const std::vector<TScanRowGroup>& rowGroups,
    const TScanSplitSettings& settings)
{
    if (rowGroups.empty()) {
        return {};
    }

    auto totalRows = int64_t{0};
    for (const auto& rowGroup : rowGroups) {
        totalRows += rowGroup.RowCount;
    }

    const auto maxTasks = std::max<size_t>(settings.MaxScanTasks, 1);
    if (
        settings.Strategy == EScanSplitStrategy::SerialRead ||
        maxTasks == 1 ||
        (
            settings.TinyInputRowsThreshold &&
            totalRows <= static_cast<int64_t>(settings.TinyInputRowsThreshold)))
    {
        return {MakeSingleSplit(rowGroups, true)};
    }

    // Split the row groups into up to maxTasks evenly-sized, contiguous chunks
    // of ceil(n / m) groups each. The last chunk may be slightly smaller, which
    // is fine — an even split keeps every worker busy. (The previous target/byte
    // heuristic degenerated to 15 one-group splits plus one giant split when no
    // target was configured, serialising the whole scan onto a single worker.)
    const size_t groupCount = rowGroups.size();
    const size_t taskCount = std::min(maxTasks, groupCount);
    const size_t groupsPerTask = (groupCount + taskCount - 1) / taskCount;

    std::vector<TScanSplit> splits;
    splits.reserve(taskCount);
    for (size_t begin = 0; begin < groupCount; begin += groupsPerTask) {
        const size_t end = std::min(begin + groupsPerTask, groupCount);
        TScanSplit split;
        split.RowGroups.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            split.RowGroups.push_back(rowGroups[i].RowGroup);
            split.RowCount += rowGroups[i].RowCount;
            split.ByteSize += rowGroups[i].ByteSize;
        }
        splits.push_back(split);
    }
    return splits;
}

} // namespace NScheduler
} // namespace NQdb
