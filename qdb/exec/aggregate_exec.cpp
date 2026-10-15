#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/kernel_rowset.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace NQdb {

namespace {

// Mirrors the HashTable C layout from qdb/modules/qumirdb.cpp.
struct THashTable {
    uint8_t* Keys = nullptr;
    uint8_t* Ctrl = nullptr;
    int64_t* SlotId = nullptr;
    uint8_t* GroupKeys = nullptr;
    int64_t** AggBuffers = nullptr;
    uint8_t** OwnedBlocks = nullptr;
    int64_t OwnedBlockCount = 0;
    int64_t OwnedBlockCapacity = 0;
    int64_t Capacity = 0;
    int64_t Size = 0;
    int64_t NumAggs = 0;
    int64_t NumKeys = 0;
    int64_t KeySize = 0;
};
static_assert(sizeof(THashTable) == TKernelCompiler::HashTableSize);

// SwissTable groups are 8 slots wide, so capacity starts at 8.
constexpr int64_t InitialCapacity = 8;
constexpr int64_t OpInit = 0;
constexpr int64_t OpUpdate = 1;
constexpr int64_t OpDestroy = 2;

} // namespace

TAggregateProcessor::TAggregateProcessor(
    TAggregateKernels kernels,
    int64_t initialCapacity)
    : Kernels_(std::move(kernels))
    // aht_init only accepts a power-of-two capacity of at least 8.
    , InitialCapacity_(std::bit_ceil(static_cast<uint64_t>(
          std::max(initialCapacity, InitialCapacity))))
{
}

TAggregateProcessor::~TAggregateProcessor()
{
    Destroy();
}

// Deferred so a processor can be constructed before its kernels are finalized.
void TAggregateProcessor::EnsureInit()
{
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    if (Kernels_.Dispatch(
            HashTable_.data(), nullptr, InitialCapacity_, OpInit) == 0)
    {
        Destroyed_ = true;
        throw std::runtime_error("aggregate initialization failed");
    }
    if (Diagnostics_) {
        Diagnostics_->ObserveHash(HashTable_.data(), 0, false, Kernels_.StringMetadataBuffers);
    }
}

void TAggregateProcessor::SetDiagnostics(TOperatorDiagnostics* diagnostics) {
    Diagnostics_ = diagnostics;
}

void TGroupingSetsAggregateProcessor::SetDiagnostics(TOperatorDiagnostics* diagnostics) {
    Inner_.SetDiagnostics(diagnostics);
}

void TAggregateProcessor::Add(TRowSet& rowSet)
{
    if (Finished_) {
        throw std::runtime_error("aggregate processor is already finished");
    }
    EnsureInit();
    if (Kernels_.Dispatch(HashTable_.data(), &rowSet, 0, OpUpdate) < 0) {
        throw std::runtime_error("aggregate update failed");
    }
    if (Diagnostics_) {
        Diagnostics_->ObserveHash(HashTable_.data(), 0, false, Kernels_.StringMetadataBuffers);
    }
}

bool TAggregateProcessor::Finish(TRowSet& rowSet)
{
    if (Finished_) {
        return false;
    }
    Finished_ = true;
    EnsureInit();

    const int64_t size = reinterpret_cast<THashTable*>(HashTable_.data())->Size;
    const int64_t finalized = Kernels_.FinishRowSet(HashTable_.data(), &rowSet);
    Destroyed_ = true;
    if (finalized != size) {
        throw std::runtime_error("aggregate finalize returned an unexpected row count");
    }

    rowSet.Destroy = DestroyKernelOwnedRowSet;
    return true;
}

void TAggregateProcessor::Destroy()
{
    if (Initialized_ && !Destroyed_) {
        Kernels_.Dispatch(HashTable_.data(), nullptr, 0, OpDestroy);
        Destroyed_ = true;
    }
}

TGroupingSetsAggregateProcessor::TGroupingSetsAggregateProcessor(
    TAggregateKernels kernels,
    std::vector<std::vector<size_t>> sets,
    size_t numKeys)
    : Inner_(std::move(kernels))
    , Sets_(std::move(sets))
    , NumKeys_(numKeys)
{
}

void TGroupingSetsAggregateProcessor::Add(TRowSet& batch)
{
    const int64_t rows = batch.RowCount;
    if (static_cast<int64_t>(SetIdBuf_.size()) < rows) {
        SetIdBuf_.resize(rows);
    }
    const size_t maskBytes = static_cast<size_t>((rows + 7) / 8);
    if (NullMask_.size() < maskBytes) {
        NullMask_.assign(maskBytes, 0); // all bits 0 => every row NULL
    }
    Cols_.resize(static_cast<size_t>(batch.ColumnCount) + 1);

    for (size_t si = 0; si < Sets_.size(); ++si) {
        std::fill_n(SetIdBuf_.begin(), rows, static_cast<int32_t>(si));
        Cols_[0] = TColumn{};
        Cols_[0].Data = reinterpret_cast<char*>(SetIdBuf_.data());
        Cols_[0].Mask = nullptr;

        const auto& set = Sets_[si];
        for (int64_t c = 0; c < batch.ColumnCount; ++c) {
            TColumn col = batch.Columns[c];
            if (c < static_cast<int64_t>(NumKeys_) &&
                std::find(set.begin(), set.end(), static_cast<size_t>(c)) == set.end())
            {
                col.Mask = NullMask_.data();
                col.MaskBitOffset = 0;
            }
            Cols_[static_cast<size_t>(c) + 1] = col;
        }

        TRowSet masked = batch;
        masked.Columns = Cols_.data();
        masked.ColumnCount = batch.ColumnCount + 1;
        masked.Destroy = nullptr;
        masked.Private = nullptr;
        masked.RefCount = 1;
        Inner_.Add(masked);
    }
}

bool TGroupingSetsAggregateProcessor::Finish(TRowSet& rowSet)
{
    return Inner_.Finish(rowSet);
}

} // namespace NQdb
