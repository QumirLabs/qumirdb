#pragma once

#include <qdb/io/io.h>
#include <qdb/io/table.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace NQdb::NScheduler {

struct TSourceCode;

// The morsels of one scan in one graph: runs of whole row groups that its
// lanes take in turn, so a lane that falls behind leaves the rest to the
// others (Leis et al., "Morsel-driven parallelism", SIGMOD 2014).
// Lanes add their morsels while the graph is built and take them once it runs.
class TScanMorselQueue {
public:
    void Add(std::vector<std::vector<int>> morsels);
    // nullopt once every morsel is taken. Safe from any worker.
    std::optional<std::vector<int>> Take();

private:
    std::vector<std::vector<int>> Morsels_;
    std::atomic<size_t> Next_ = 0;
};

// A scan lane over a morsel queue: a source of Table per morsel it takes.
struct TMorselSourceState {
    std::shared_ptr<const ITable> Table;
    std::optional<std::unordered_set<std::string>> Columns;
    std::shared_ptr<TScanMorselQueue> Queue;
    std::unique_ptr<ISource> Source;
};

// Source code that reads TMorselSourceState until its queue is empty.
std::shared_ptr<const TSourceCode> MakeMorselSourceCode();

} // namespace NQdb::NScheduler
