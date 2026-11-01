#pragma once

#include <qdb/exec/runtime_filter.h>
#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace NQdb {

class TParquetGroupedScan;

// The steps of a staged scan after the first column group of a batch is read: filter kernels,
// each on the rows the earlier steps keep, and runtime filter probes.
struct TStagedScanPlan {
    struct TStep {
        // Reads the next column group before the step.
        bool ReadsGroup = false;
        // A filter kernel, or else a runtime filter probe.
        std::optional<TKernelCompiler::TFilterDispatch> Filter;
        std::shared_ptr<const TRuntimeFilterConsumer> RuntimeFilter;
    };

    std::vector<TStep> Steps;
    // Per column group after the first: read sparsely while at most this share of the batch
    // is selected. The groups no step reads are the payload, read last.
    std::vector<double> MaxSparseShares;
    // The runtime filter step whose key hashes the output carries for the next consumer.
    std::optional<size_t> HashStep;
    // The scan columns of the output, in output order; every column when unset.
    std::optional<std::vector<int32_t>> KeptColumns;
};

enum class EStagedScanResult {
    Batch,
    // A runtime filter is not published yet, and enough batches wait for it.
    Pending,
    Finished,
};

// One lane of a filter and runtime filters fused into their Parquet scan. A batch leaves with
// the selection of the last step; a batch no row of which passes is not emitted.
class TStagedScan {
public:
    TStagedScan(
        std::unique_ptr<TParquetGroupedScan> scan,
        std::shared_ptr<const TStagedScanPlan> plan);
    ~TStagedScan();

    EStagedScanResult Next(TRowSet& rowSet);

private:
    struct TBatch;

    bool Advance(TBatch& batch, bool first);
    bool Emit(TBatch& batch, TRowSet& rowSet);
    void ReadGroup(TBatch& batch, size_t group);

    std::unique_ptr<TParquetGroupedScan> Scan_;
    std::shared_ptr<const TStagedScanPlan> Plan_;
    // Batches read ahead while a runtime filter is unpublished, in scan order.
    std::deque<std::unique_ptr<TBatch>> Batches_;
    std::unique_ptr<TBatch> Spare_;
    bool Exhausted_ = false;
    // The selection the next step writes.
    std::vector<uint8_t> Next_;
    // Key hashes of a runtime filter step whose hashes the output does not carry.
    std::vector<uint64_t> Hashes_;
    // Per step: the probe state of a runtime filter, created once it is published.
    std::vector<std::shared_ptr<TRuntimeFilterProbeState>> ProbeStates_;
};

} // namespace NQdb
