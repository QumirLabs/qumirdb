#include <qdb/scheduler/lowering/scan_split.h>

#include <algorithm>

namespace NQdb {
namespace NScheduler {
namespace {

// Rows [firstRow, firstRow + rows) of `group`, with its share of the bytes.
void AddRows(TScanSplit& split, const TScanRowGroup& group, int64_t firstRow, int64_t rows) {
    split.Ranges.push_back({
        .RowGroup = static_cast<int32_t>(group.RowGroup),
        .FirstRow = firstRow,
        .RowCount = rows,
    });
    split.RowCount += rows;
    split.ByteSize += rows == group.RowCount
        ? group.ByteSize
        : group.ByteSize * rows / group.RowCount;
}

TScanSplit MakeSingleSplit(
    const std::vector<TScanRowGroup>& rowGroups,
    bool serialRead)
{
    TScanSplit split;
    split.Ranges.reserve(rowGroups.size());
    split.SerialRead = serialRead;
    for (const auto& rowGroup : rowGroups) {
        AddRows(split, rowGroup, 0, rowGroup.RowCount);
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

    const size_t budget = feedsJoin ? workers : 2 * workers;
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
        totalRows <= 0 ||
        (
            settings.TinyInputRowsThreshold &&
            totalRows <= static_cast<int64_t>(settings.TinyInputRowsThreshold)))
    {
        return {MakeSingleSplit(rowGroups, true)};
    }

    const size_t groupCount = rowGroups.size();
    std::vector<TScanSplit> splits;
    if (groupCount >= maxTasks) {
        // Contiguous chunks of ceil(n / m) whole row groups; the last may be
        // smaller, which still keeps every worker busy.
        const size_t groupsPerTask = (groupCount + maxTasks - 1) / maxTasks;
        splits.reserve(maxTasks);
        for (size_t begin = 0; begin < groupCount; begin += groupsPerTask) {
            const size_t end = std::min(begin + groupsPerTask, groupCount);
            TScanSplit split;
            split.Ranges.reserve(end - begin);
            for (size_t i = begin; i < end; ++i) {
                AddRows(split, rowGroups[i], 0, rowGroups[i].RowCount);
            }
            splits.push_back(std::move(split));
        }
        return splits;
    }

    // Fewer row groups than tasks: cut them, from metadata alone, into splits
    // of ceil(rows / m) rows. A split may end in one group and go on in the next.
    const auto tasks = static_cast<int64_t>(maxTasks);
    const int64_t rowsPerTask = std::max<int64_t>((totalRows + tasks - 1) / tasks, 1);
    splits.reserve(maxTasks);
    TScanSplit split;
    for (const auto& group : rowGroups) {
        for (int64_t first = 0; first < group.RowCount;) {
            const int64_t rows = std::min(group.RowCount - first, rowsPerTask - split.RowCount);
            AddRows(split, group, first, rows);
            first += rows;
            if (split.RowCount == rowsPerTask) {
                splits.push_back(std::move(split));
                split = {};
            }
        }
    }
    if (!split.Ranges.empty()) {
        splits.push_back(std::move(split));
    }
    return splits;
}

} // namespace NScheduler
} // namespace NQdb
