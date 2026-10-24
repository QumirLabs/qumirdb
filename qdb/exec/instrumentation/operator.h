#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/generated.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace NQdb {

// Logical payload of selected, non-NULL values; metadata and allocator
// overhead are excluded. Width -1 denotes variable-length string data.
std::vector<int64_t> InstrumentationColumnWidths(const NQumir::NAst::TTypePtr& type);

struct TRowSetInstrumentation {
    std::shared_ptr<TKernelSlot> Kernel;
    std::vector<int64_t> Widths;
    uint64_t Rows = 0;
    uint64_t Bytes = 0;

    void Add(const TRowSet& rowSet);
};

// A task is executed by at most one worker at a time. Its counters migrate
// with it; readers may take a snapshot only after execution has stopped.
struct TOperatorInstrumentation {
    std::shared_ptr<TKernelSlot> Kernel;
    std::vector<TRowSetInstrumentation> Inputs;
    TRowSetInstrumentation Output;
    uint64_t ExecutionCpuNanoseconds = 0;
    uint64_t Groups = 0;
    uint64_t HashCapacity = 0;
    uint64_t HashGrowthEvents = 0;
    uint64_t Rehashes = 0;
    uint64_t HashMemoryPeakBytes = 0;
    uint64_t RetainedInputBytes = 0;
    uint64_t RetainedInputPeakBytes = 0;
    uint64_t BuildMemoryPeakBytes = 0;
    bool HasHash = false;
    bool HasBuildMemory = false;
    bool MaterializesOutput = false;

    uint64_t InputPayloadBytes(const TRowSet& rowSet, size_t input) const;
    void SetRetainedInputBytes(uint64_t bytes);
    void RetainInput(const TRowSet& rowSet, size_t input, bool append = true);

    void ObserveHash(
        void* table,
        size_t side,
        bool join,
        std::span<const int64_t> stringBuffers = {});

    // A row join table (exec/row_join_exec.cpp) reports its size directly:
    // distinct keys as groups; the entry table never grows, so there are no
    // rehashes to count.
    void ObserveBuildTable(size_t side, uint64_t keys, uint64_t capacity, uint64_t bytes);

private:
    std::array<uint64_t, 2> Capacities_ = {};
    std::array<uint64_t, 2> Groups_ = {};
    std::array<uint64_t, 2> Memory_ = {};
};

} // namespace NQdb
