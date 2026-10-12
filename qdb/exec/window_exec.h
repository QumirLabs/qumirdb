#pragma once

#include <qdb/exec/join_exec.h>
#include <qdb/exec/sort_exec.h>

#include <qumir/parser/type.h>

#include <cstdint>
#include <vector>

namespace NQdb {

// Blocking window processor. Buffers all input, then in a single Next() sorts
// the row-id permutation by (partition ++ order) and materializes the input
// columns followed by one computed column per window function - all inside the
// JIT window kernel (qdb_window_run). The whole result is emitted at once
// because the running/prefix compute spans the full partition.
class TWindowProcessor {
public:
    TWindowProcessor(
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        TSortRadixKernel kernel);

    void Add(TRowSet& rowSet);
    void Finish();
    bool Next(TRowSet& rowSet);

private:
    NQumir::NAst::TTypePtr OutputType_;
    std::vector<TSortKey> Keys_;
    std::vector<TSortColumnRef> KeyColumns_;
    TSortRadixKernel Kernel_;

    TRowStore Store_;
    std::vector<TRowId> Rows_;
    bool Emitted_ = false;
};

} // namespace NQdb
