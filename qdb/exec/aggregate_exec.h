#pragma once

#include <qdb/kernel/compiler.h>
#include <qdb/exec/diagnostics.h>

#include <array>
#include <cstdint>
#include <vector>

namespace NQdb {

class TAggregateProcessor {
public:
    explicit TAggregateProcessor(
        TAggregateKernels kernels,
        int64_t initialCapacity = 4);
    ~TAggregateProcessor();

    TAggregateProcessor(const TAggregateProcessor&) = delete;
    TAggregateProcessor& operator=(const TAggregateProcessor&) = delete;

    void Add(TRowSet& rowSet);
    void SetDiagnostics(TOperatorDiagnostics* diagnostics);
    bool Finish(TRowSet& rowSet);

private:
    void EnsureInit();
    void Destroy();

private:
    TAggregateKernels Kernels_;
    TOperatorDiagnostics* Diagnostics_ = nullptr;
    int64_t InitialCapacity_ = 4;
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
        std::vector<std::vector<size_t>> sets,
        size_t numKeys);

    void Add(TRowSet& rowSet);
    void SetDiagnostics(TOperatorDiagnostics* diagnostics);
    bool Finish(TRowSet& rowSet);

private:
    TAggregateProcessor Inner_;
    std::vector<std::vector<size_t>> Sets_;
    size_t NumKeys_;
    std::vector<int32_t> SetIdBuf_;
    std::vector<uint8_t> NullMask_;
    std::vector<TColumn> Cols_;
};

} // namespace NQdb
