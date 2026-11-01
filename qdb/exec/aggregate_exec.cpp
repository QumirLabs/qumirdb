#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/kernel_rowset.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace NQdb {

namespace {

// Mirrors the HashTable C layout from qdb/modules/qumirdb.cpp. The processor
// reads only Size; the row layout's structs are the kernel's.
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
    std::array<uint8_t, TKernelCompiler::RowArenaSize + TKernelCompiler::RowTableSize
        + TKernelCompiler::RowScratchSize> RowLayout = {};
    int64_t RowSwitchGroups = 0;
    int64_t RowExpectedGroups = 0;
    int64_t RowRebuilds = 0;
    int64_t RowMovedFromSlots = 0;
};
static_assert(sizeof(THashTable) == TKernelCompiler::HashTableSize);

// SwissTable groups are 8 slots wide, so capacity starts at 8.
constexpr int64_t InitialCapacity = 8;

int64_t Dispatch(
    const TAggregateKernels& kernels, void* table, TRowSet* batch, int64_t arg, EAggregateKernelOp op)
{
    return kernels.Dispatch(table, batch, arg, static_cast<int64_t>(op));
}

} // namespace

TAggregateProcessor::TAggregateProcessor(
    TAggregateKernels kernels,
    TAggregateTablePlan plan)
    : Kernels_(std::move(kernels))
    , Plan_(plan)
{
    // aht_init only accepts a power-of-two capacity of at least 8.
    Plan_.InitialCapacity = static_cast<int64_t>(std::bit_ceil(static_cast<uint64_t>(
        std::max(Plan_.InitialCapacity, InitialCapacity))));
    if (Kernels_.RowBytes == 0
        && (Plan_.Layout == EAggregateTableLayout::Row || Plan_.RowSwitchGroups != 0
            || Plan_.RowExpectedGroups != 0))
    {
        throw std::invalid_argument("aggregate has no row layout");
    }
}

// Deferred so a processor can be constructed before its kernels are finalized.
void TAggregateProcessor::EnsureInit()
{
    if (Initialized_) {
        return;
    }
    void* table = HashTable_.data();
    const bool initialized = Plan_.Layout == EAggregateTableLayout::Row
        ? Dispatch(Kernels_, table, nullptr, Plan_.InitialGroups, EAggregateKernelOp::InitRows) != 0
        : Dispatch(Kernels_, table, nullptr, Plan_.InitialCapacity, EAggregateKernelOp::Init) != 0;
    if (!initialized) {
        // A failing init frees what it allocated.
        throw std::runtime_error("aggregate initialization failed");
    }
    Initialized_ = true;
    // Both only set a field; the constructor checked that the kernel serves them.
    if (Plan_.RowSwitchGroups != 0) {
        Dispatch(Kernels_, table, nullptr, Plan_.RowSwitchGroups, EAggregateKernelOp::SwitchToRowsAt);
    }
    if (Plan_.RowExpectedGroups != 0) {
        Dispatch(Kernels_, table, nullptr, Plan_.RowExpectedGroups, EAggregateKernelOp::ExpectRowGroups);
    }
    if (Instrumentation_) {
        Instrumentation_->ObserveHash(HashTable_.data(), 0, false, Kernels_.StringMetadataBuffers);
    }
}

void TAggregateProcessor::SetInstrumentation(TOperatorInstrumentation* instrumentation) {
    Instrumentation_ = instrumentation;
}

void TGroupingSetsAggregateProcessor::SetInstrumentation(TOperatorInstrumentation* instrumentation) {
    Inner_.SetInstrumentation(instrumentation);
}

void TAggregateProcessor::Add(TRowSet& rowSet)
{
    TKernelHeapScope scope(StandaloneHeap_);
    if (Finished_) {
        throw std::runtime_error("aggregate processor is already finished");
    }
    EnsureInit();
    if (Dispatch(Kernels_, HashTable_.data(), &rowSet, 0, EAggregateKernelOp::Update) < 0) {
        throw std::runtime_error("aggregate update failed");
    }
    if (Instrumentation_) {
        Instrumentation_->ObserveHash(HashTable_.data(), 0, false, Kernels_.StringMetadataBuffers);
    }
}

bool TAggregateProcessor::Finish(TRowSet& rowSet)
{
    TKernelHeapScope scope(StandaloneHeap_);
    if (Finished_) {
        return false;
    }
    Finished_ = true;
    EnsureInit();

    const int64_t size = reinterpret_cast<THashTable*>(HashTable_.data())->Size;
    const int64_t finalized = Kernels_.FinishRowSet(HashTable_.data(), &rowSet);
    if (finalized != size) {
        throw std::runtime_error("aggregate finalize returned an unexpected row count");
    }

    AdoptKernelOwnedRowSet(rowSet);
    return true;
}

int64_t TAggregateProcessor::Groups() const
{
    if (!Initialized_ || Finished_) {
        return 0;
    }
    return reinterpret_cast<const THashTable*>(HashTable_.data())->Size;
}

bool TAggregateProcessor::Flush(TRowSet& rowSet)
{
    if (Groups() == 0) {
        return false;
    }
    Finish(rowSet);
    // Finish destroyed the table; the next Add initializes a new one.
    Initialized_ = false;
    Finished_ = false;
    return true;
}

TPartialAggregateProcessor::TPartialAggregateProcessor(
    TAggregateKernels kernels,
    TAggregateTablePlan plan,
    TPartialAggregatePolicy policy,
    TUnaryStreamProcess passThrough)
    : Table_(std::move(kernels), plan)
    , Policy_(policy)
    , PassThrough_(std::move(passThrough))
{
}

TPartialAggregateProcessor::~TPartialAggregateProcessor()
{
    if (HasPending_) {
        Release(&Pending_);
    }
}

void TPartialAggregateProcessor::SetInstrumentation(TOperatorInstrumentation* instrumentation)
{
    Table_.SetInstrumentation(instrumentation);
}

void TPartialAggregateProcessor::Add(TRowSet& rowSet)
{
    if (HasPending_) {
        throw std::runtime_error("partial aggregate output was not taken");
    }
    if (PassingThrough_) {
        // The projection takes the batch over and leaves the partial states.
        PassThrough_(rowSet, PassThroughState_);
        Pending_ = rowSet;
        rowSet = {};
        HasPending_ = true;
        return;
    }

    HashedRows_ += static_cast<uint64_t>(rowSet.RowCount);
    Table_.Add(rowSet);
    Release(&rowSet);
    rowSet = {};
    const int64_t groups = Table_.Groups();
    // The reduction counts every group the lane produced, emitted or held: a
    // table that is emptied often has not reduced its rows.
    const double groupsSoFar = static_cast<double>(EmittedGroups_ + static_cast<uint64_t>(groups));
    if (PassThrough_ && HashedRows_ >= Policy_.SampleRows
        && static_cast<double>(HashedRows_) < static_cast<double>(Policy_.MinReduction) * groupsSoFar)
    {
        PassingThrough_ = true;
        HasPending_ = Table_.Flush(Pending_);
        return;
    }
    if (groups >= Policy_.MaxGroups) {
        EmittedGroups_ += static_cast<uint64_t>(groups);
        HasPending_ = Table_.Flush(Pending_);
    }
}

bool TPartialAggregateProcessor::Next(TRowSet& rowSet)
{
    if (!HasPending_) {
        return false;
    }
    rowSet = Pending_;
    Pending_ = {};
    HasPending_ = false;
    return true;
}

bool TPartialAggregateProcessor::Finish(TRowSet& rowSet)
{
    if (PassingThrough_) {
        return false;
    }
    return Table_.Finish(rowSet);
}

TGroupingSetsAggregateProcessor::TGroupingSetsAggregateProcessor(
    TAggregateKernels kernels,
    TAggregateTablePlan plan,
    std::vector<std::vector<size_t>> sets,
    size_t numKeys)
    : Inner_(std::move(kernels), plan)
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
