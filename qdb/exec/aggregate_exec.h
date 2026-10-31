#pragma once

#include <qdb/kernel/compiler.h>
#include <qdb/exec/instrumentation/operator.h>
#include <qdb/exec/unary_process.h>

#include <array>
#include <cstdint>
#include <vector>

namespace NQdb {

// The layouts of the aggregate hash table: the swiss table, with one array per
// state word, or the row layout (kernel/aggregation/aggregation_rows.oz),
// which keeps the key and the states of a group in one arena row.
enum class EAggregateTableLayout {
    Swiss,
    Row,
};

// How the table of one aggregate task starts and when it changes layout;
// PlanAggregateTable (planner_helpers.h) chooses it.
struct TAggregateTablePlan {
    EAggregateTableLayout Layout = EAggregateTableLayout::Swiss;
    // Swiss slots, rounded up to a power of two of at least 8.
    int64_t InitialCapacity = 8;
    // The groups a table that starts in the row layout has entries for.
    int64_t InitialGroups = 0;
    // A swiss table that must grow while it holds this many groups or more
    // moves to the row layout instead; 0 keeps it swiss.
    int64_t RowSwitchGroups = 0;
    // The groups that move sizes the row entries for, when that is more than
    // twice the groups it moves; 0 sizes them for twice the groups.
    int64_t RowExpectedGroups = 0;
};

class TAggregateProcessor {
public:
    explicit TAggregateProcessor(
        TAggregateKernels kernels,
        TAggregateTablePlan plan = {});
    ~TAggregateProcessor();

    TAggregateProcessor(const TAggregateProcessor&) = delete;
    TAggregateProcessor& operator=(const TAggregateProcessor&) = delete;

    void Add(TThreadExecutionContext& context, TRowSet& rowSet);
    void SetInstrumentation(TOperatorInstrumentation* instrumentation);
    bool Finish(TThreadExecutionContext& context, TRowSet& rowSet);
    // The groups the table holds.
    int64_t Groups() const;
    // Finalizes the groups so far into rowSet and starts an empty table, so the
    // processor keeps accepting rows. False, with nothing emitted, when the table
    // holds no groups.
    bool Flush(TThreadExecutionContext& context, TRowSet& rowSet);

private:
    void EnsureInit(TThreadExecutionContext& context);
    void Destroy(TThreadExecutionContext& context);

private:
    TAggregateKernels Kernels_;
    TOperatorInstrumentation* Instrumentation_ = nullptr;
    TAggregateTablePlan Plan_;
    std::array<uint8_t, TKernelCompiler::HashTableSize> HashTable_ = {};
    bool Initialized_ = false;
    bool Finished_ = false;
    bool Destroyed_ = false;
};

// Replays each batch once per set as a masked view (set id -> constant column, keys
// outside the set forced NULL), so all sets land in one table in a single pass.
class TGroupingSetsAggregateProcessor {
public:
    TGroupingSetsAggregateProcessor(
        TAggregateKernels kernels,
        TAggregateTablePlan plan,
        std::vector<std::vector<size_t>> sets,
        size_t numKeys);

    void Add(TThreadExecutionContext& context, TRowSet& rowSet);
    void SetInstrumentation(TOperatorInstrumentation* instrumentation);
    bool Finish(TThreadExecutionContext& context, TRowSet& rowSet);

private:
    TAggregateProcessor Inner_;
    std::vector<std::vector<size_t>> Sets_;
    size_t NumKeys_;
    std::vector<int32_t> SetIdBuf_;
    std::vector<uint8_t> NullMask_;
    std::vector<TColumn> Cols_;
};

// When a partial aggregate stops hashing and when it empties its table.
struct TPartialAggregatePolicy {
    // Input rows the table absorbs before its reduction is judged.
    uint64_t SampleRows = 128 * 1024;
    // Rows per group below which hashing does not pay for itself: the
    // remaining rows pass through as single-row partial states.
    uint64_t MinReduction = 8;
    // Groups at which the table emits its groups and restarts empty.
    int64_t MaxGroups = 1 << 20;
};

// The partial phase of a grouped aggregate cascade: a bounded table that
// reduces one lane's rows before the exchange (Larson, ICDE 2002). It watches
// its own reduction, and once hashing stops paying for itself it converts the
// rows to partial states instead (passThrough), which the final phase merges
// like any other partial output. Without a passThrough it only bounds the
// table.
class TPartialAggregateProcessor {
public:
    TPartialAggregateProcessor(
        TAggregateKernels kernels,
        TAggregateTablePlan plan,
        TPartialAggregatePolicy policy,
        TUnaryStreamProcess passThrough);
    ~TPartialAggregateProcessor();

    TPartialAggregateProcessor(const TPartialAggregateProcessor&) = delete;
    TPartialAggregateProcessor& operator=(const TPartialAggregateProcessor&) = delete;

    // Consumes rowSet; Next then yields what the batch made ready.
    void Add(TThreadExecutionContext& context, TRowSet& rowSet);
    // The partial states ready to emit: the groups of a table that was emptied,
    // or a batch that passed through.
    bool Next(TRowSet& rowSet);
    // The groups still in the table; false when it has none.
    bool Finish(TThreadExecutionContext& context, TRowSet& rowSet);
    bool PassesThrough() const { return PassingThrough_; }
    void SetInstrumentation(TOperatorInstrumentation* instrumentation);

private:
    TAggregateProcessor Table_;
    TPartialAggregatePolicy Policy_;
    TUnaryStreamProcess PassThrough_;
    TUnaryStreamingKernelState PassThroughState_;
    uint64_t HashedRows_ = 0;
    uint64_t EmittedGroups_ = 0;
    bool PassingThrough_ = false;
    bool HasPending_ = false;
    TRowSet Pending_{};
};

} // namespace NQdb
