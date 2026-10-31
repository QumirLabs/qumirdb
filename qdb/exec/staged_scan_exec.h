#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace NQdb {

class TParquetGroupedScan;

// The filter kernels of a staged scan. Stage k runs once column group k is read, on the rows
// the earlier stages keep; a last payload group is read for the rows every stage keeps.
struct TStagedScanPlan {
    std::vector<TKernelCompiler::TFilterDispatch> Stages;
    bool Payload = false;
    // The scan columns of the output, in output order; every column when unset.
    std::optional<std::vector<int32_t>> KeptColumns;
};

// One lane of a filter fused into its Parquet scan. A batch leaves with the selection of the
// last stage; a batch no row of which passes is not emitted.
class TStagedScan {
public:
    TStagedScan(
        std::unique_ptr<TParquetGroupedScan> scan,
        std::shared_ptr<const TStagedScanPlan> plan);
    ~TStagedScan();

    bool Next(TRowSet& rowSet);

private:
    std::unique_ptr<TParquetGroupedScan> Scan_;
    std::shared_ptr<const TStagedScanPlan> Plan_;
    // The selection the next stage reads.
    std::vector<uint8_t> Previous_;
};

} // namespace NQdb
