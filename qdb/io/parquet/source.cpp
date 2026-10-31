#include <qdb/io/parquet/source.h>
#include <qdb/io/parquet/column_scan.h>
#include <qdb/io/parquet/decimal_storage.h>
#include <qdb/io/parquet/row_reader.h>
#include <qdb/io/parquet/row_id.h>
#include <qdb/io/parquet/row_group_predicate.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <atomic>
#include <stdexcept>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/memory_pool.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>

#include <llvm/Support/JSON.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <qumir/parser/type.h>

namespace NQdb {

namespace {

using NQumir::NAst::TIntegerType;
using NQumir::NAst::TTypePtr;

TTypePtr ArrowTypeToQumir(const std::shared_ptr<arrow::DataType>& type) {
    switch (type->id()) {
        case arrow::Type::INT8: return std::make_shared<TIntegerType>(TIntegerType::I8);
        case arrow::Type::INT16: return std::make_shared<TIntegerType>(TIntegerType::I16);
        case arrow::Type::INT32: return std::make_shared<TIntegerType>(TIntegerType::I32);
        case arrow::Type::INT64: return std::make_shared<TIntegerType>(TIntegerType::I64);
        case arrow::Type::UINT8: return std::make_shared<TIntegerType>(TIntegerType::U8);
        case arrow::Type::UINT16: return std::make_shared<TIntegerType>(TIntegerType::U16);
        case arrow::Type::UINT32: return std::make_shared<TIntegerType>(TIntegerType::U32);
        case arrow::Type::UINT64: return std::make_shared<TIntegerType>(TIntegerType::U64);
        case arrow::Type::FLOAT: return std::make_shared<NQumir::NAst::TFloatType>(NQumir::NAst::TFloatType::F32);
        case arrow::Type::DOUBLE: return std::make_shared<NQumir::NAst::TFloatType>();
        case arrow::Type::BOOL: return std::make_shared<NQumir::NAst::TBoolType>();
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING: return std::make_shared<NQumir::NAst::TStringType>();
        case arrow::Type::DATE32: return std::make_shared<TIntegerType>(TIntegerType::I32);
        case arrow::Type::DATE64: return std::make_shared<TIntegerType>(TIntegerType::I64);
        case arrow::Type::DECIMAL128: {
            auto decimal = std::static_pointer_cast<arrow::Decimal128Type>(type);
            return std::make_shared<TDecimal>(decimal->precision(), decimal->scale());
        }
        case arrow::Type::DECIMAL256:
            throw std::runtime_error("DECIMAL256 parquet columns are not supported");
        default: return nullptr;
    }
}

TTypePtr ArrowFieldToQumir(const std::shared_ptr<arrow::Field>& field) {
    auto type = ArrowTypeToQumir(field->type());
    if (type && field->nullable()) {
        return std::make_shared<TNullable>(std::move(type));
    }
    return type;
}

struct TBatchData {
    std::shared_ptr<arrow::RecordBatch> Batch;
    std::vector<TColumn> Columns;
    // Decimal values copied to the width of their storage.
    std::vector<std::shared_ptr<arrow::Buffer>> DecimalValues;
};

void DestroyBatch(TRowSet* rs) {
    delete static_cast<TBatchData*>(rs->Private);
}

char* NumericData(const std::shared_ptr<arrow::Array>& arr) {
    auto fwType = static_cast<const arrow::FixedWidthType*>(arr->type().get());
    int byteWidth = fwType->bit_width() / 8;
    const uint8_t* raw = arr->data()->buffers[1]->data();
    return const_cast<char*>(reinterpret_cast<const char*>(raw)) + arr->offset() * byteWidth;
}

arrow::MemoryPool* GetMemoryPool();

// Points `col` at the values of `arr`. Decimal values copied to the width of their storage
// are kept in `decimalValues`.
void ArrayToColumn(
    const std::shared_ptr<arrow::Array>& arr,
    TColumn& col,
    std::vector<std::shared_ptr<arrow::Buffer>>& decimalValues)
{
    col.Offsets = nullptr;
    col.OffsetWidth = 0;
    col.DataBitOffset = 0;
    col.Mask = nullptr;
    col.MaskBitOffset = 0;

    if (arr->null_count() > 0) {
        col.Mask = arr->null_bitmap_data();
        col.MaskBitOffset = static_cast<int32_t>(arr->offset());
    }

    switch (arr->type_id()) {
        case arrow::Type::INT8:
        case arrow::Type::INT16:
        case arrow::Type::INT32:
        case arrow::Type::INT64:
        case arrow::Type::UINT8:
        case arrow::Type::UINT16:
        case arrow::Type::UINT32:
        case arrow::Type::UINT64:
        case arrow::Type::FLOAT:
        case arrow::Type::DOUBLE:
        case arrow::Type::DATE32:
        case arrow::Type::DATE64: {
            col.Data = NumericData(arr);
            break;
        }
        case arrow::Type::DECIMAL32:
        case arrow::Type::DECIMAL64:
        case arrow::Type::DECIMAL128: {
            auto values = DecimalStorageValues(*arr, GetMemoryPool());
            col.Data = const_cast<char*>(reinterpret_cast<const char*>(values->data()));
            decimalValues.push_back(std::move(values));
            break;
        }
        case arrow::Type::BOOL: {
            auto bitmap = arr->data()->buffers[1];
            col.Data = const_cast<char*>(reinterpret_cast<const char*>(bitmap->data()));
            col.DataBitOffset = static_cast<int32_t>(arr->offset());
            break;
        }
        case arrow::Type::STRING: {
            auto typed = std::static_pointer_cast<arrow::StringArray>(arr);
            const int32_t* src = typed->raw_value_offsets() + arr->offset();
            int32_t base = src[0];
            col.Offsets = const_cast<int32_t*>(src);
            col.OffsetWidth = 4;
            col.Data = const_cast<char*>(reinterpret_cast<const char*>(typed->raw_data()) + base);
            break;
        }
        case arrow::Type::LARGE_STRING: {
            auto typed = std::static_pointer_cast<arrow::LargeStringArray>(arr);
            const int64_t* src = typed->raw_value_offsets() + arr->offset();
            int64_t base = src[0];
            col.Offsets = const_cast<int64_t*>(src);
            col.OffsetWidth = 8;
            col.Data = const_cast<char*>(reinterpret_cast<const char*>(typed->raw_data()) + base);
            break;
        }
        default: {
            col.Data = nullptr;
            break;
        }
    }
}

void RecordBatchToRowSet(
    std::shared_ptr<arrow::RecordBatch> batch,
    TRowSet& rowSet)
{
    int32_t numCols = static_cast<int32_t>(batch->num_columns());
    int64_t len = batch->num_rows();

    auto* data = new TBatchData;
    data->Batch = std::move(batch);
    data->Columns.resize(numCols);

    for (int32_t i = 0; i < numCols; ++i) {
        ArrayToColumn(data->Batch->column(i), data->Columns[i], data->DecimalValues);
    }

    rowSet = {
        .Columns = data->Columns.data(),
        .ColumnCount = numCols,
        .RowCount = len,
        .Selection = nullptr,
        .Destroy = DestroyBatch,
        .Private = data,
        .RefCount = 1,
    };
}

class TParquetPhysicalRowReader final : public IPhysicalRowReader {
public:
    TParquetPhysicalRowReader(
        std::shared_ptr<parquet::arrow::FileReader> reader,
        std::span<const std::string> columnNames,
        arrow::MemoryPool* memoryPool)
        : Reader_(std::move(reader), columnNames, memoryPool)
    {}

    bool ReadRows(
        std::span<const TPhysicalRowId> rowIds,
        TRowSet& output,
        std::string* error) const override
    {
        auto batch = Reader_.ReadRows(rowIds);
        if (!batch.ok()) {
            if (error) {
                *error = batch.status().ToString();
            }
            return false;
        }
        RecordBatchToRowSet(std::move(*batch), output);
        return true;
    }

private:
    TParquetRowReader Reader_;
};

std::atomic<EArrowMemoryPool> ConfiguredMemoryPool{EArrowMemoryPool::Auto};

arrow::MemoryPool* GetMemoryPool() {
    static arrow::MemoryPool* pool = []() -> arrow::MemoryPool* {
        switch (ConfiguredMemoryPool.load()) {
        case EArrowMemoryPool::System:
            return arrow::system_memory_pool();
        case EArrowMemoryPool::Default:
            return arrow::default_memory_pool();
        case EArrowMemoryPool::Mimalloc: {
            arrow::MemoryPool* mimalloc = nullptr;
            if (arrow::mimalloc_memory_pool(&mimalloc).ok() && mimalloc) {
                return mimalloc;
            }
            return arrow::default_memory_pool();
        }
        case EArrowMemoryPool::Jemalloc:
        case EArrowMemoryPool::Auto:
            break;
        }
        arrow::MemoryPool* jemalloc = nullptr;
        if (arrow::jemalloc_memory_pool(&jemalloc).ok() && jemalloc) {
            return jemalloc;
        }
        return arrow::default_memory_pool();
    }();
    return pool;
}

} // namespace

std::optional<EArrowMemoryPool> ParseArrowMemoryPool(std::string_view name) {
    if (name == "auto") return EArrowMemoryPool::Auto;
    if (name == "system") return EArrowMemoryPool::System;
    if (name == "default") return EArrowMemoryPool::Default;
    if (name == "jemalloc") return EArrowMemoryPool::Jemalloc;
    if (name == "mimalloc") return EArrowMemoryPool::Mimalloc;
    return std::nullopt;
}

void SetArrowMemoryPool(EArrowMemoryPool pool) {
    ConfiguredMemoryPool.store(pool);
}

struct TParquetFileData {
    explicit TParquetFileData(const std::string& path)
        : Path(path)
    {
        Open();
    }

    void Open();
    void LoadColumnStats();
    void LoadStandardColumnStats(TStats& stats);
    void LoadRowGroupRanges();

    std::string Path;
    std::shared_ptr<parquet::arrow::FileReader> FileReader;
    std::shared_ptr<arrow::Schema> FileSchema;
    std::vector<std::string> FileNames;
    std::vector<TColumnSchema> FileColumns;
    // Per field: the leaf column a TParquetColumnScan can read, else -1 (read by Arrow).
    std::vector<int> ColumnScanLeaves;
    TSchema FullSchema;
    std::vector<NScheduler::TScanRowGroup> RowGroups;
    std::vector<std::vector<TRowGroupColumnRange>> RowGroupRanges;
    TStatsPtr Stats;
};

void TParquetFileData::Open()
{
    // Compiled physical readers share this FileReader across scheduler tasks.
    // That concurrency contract relies on a ReadableFile, disabled Arrow
    // pre-buffering, and lookup code that uses neither page-index nor bloom-
    // filter readers. Revisit sharing if any of those conditions changes.
    auto infileResult = arrow::io::ReadableFile::Open(Path);
    if (!infileResult.ok()) {
        throw std::runtime_error(infileResult.status().ToString());
    }
    auto infile = infileResult.ValueOrDie();

    parquet::ArrowReaderProperties arrowProps;
    arrowProps.set_batch_size(1 << 18);
    // Row locators assume batches arrive in row-group order.
    arrowProps.set_use_threads(false);
    // QDB schedules reads itself; Arrow pre-buffering would perform data I/O
    // while constructing the logical source, before scan pruning.
    arrowProps.set_pre_buffer(false);
    parquet::arrow::FileReaderBuilder builder;
    auto status = builder
        .memory_pool(GetMemoryPool())
        ->properties(arrowProps)
        ->Open(infile);
    if (!status.ok()) {
        throw std::runtime_error(status.ToString());
    }
    auto fileReaderResult = builder.Build();
    if (!fileReaderResult.ok()) {
        throw std::runtime_error(fileReaderResult.status().ToString());
    }
    FileReader = std::move(*fileReaderResult);

    auto schemaStatus = FileReader->GetSchema(&FileSchema);
    if (!schemaStatus.ok()) {
        throw std::runtime_error(schemaStatus.ToString());
    }
    FileNames.reserve(FileSchema->num_fields());
    for (const auto& field : FileSchema->fields()) {
        if (field->name() == InternalRowIdColumnName) {
            throw std::runtime_error(
                "parquet column name '" + field->name() + "' is reserved");
        }
        FileNames.push_back(field->name());
    }
    FileColumns.reserve(FileSchema->num_fields());
    for (int field = 0; field < FileSchema->num_fields(); ++field) {
        FileColumns.push_back({
            .Name = FileNames[static_cast<size_t>(field)],
            .Type = ArrowFieldToQumir(FileSchema->field(field)),
        });
    }
    FullSchema = TSchema{FileColumns};

    const auto& manifest = FileReader->manifest();
    ColumnScanLeaves.assign(static_cast<size_t>(FileSchema->num_fields()), -1);
    for (size_t field = 0; field < manifest.schema_fields.size() &&
        field < ColumnScanLeaves.size(); ++field)
    {
        const auto& schemaField = manifest.schema_fields[field];
        if (!schemaField.is_leaf()) {
            continue;
        }
        const auto* descriptor =
            FileReader->parquet_reader()->metadata()->schema()->Column(schemaField.column_index);
        if (TParquetColumnScan::Supports(*descriptor, FileSchema->field(static_cast<int>(field))->type())) {
            ColumnScanLeaves[field] = schemaField.column_index;
        }
    }

    LoadColumnStats();
    LoadRowGroupRanges();

    auto metadata = FileReader->parquet_reader()->metadata();
    RowGroups.reserve(static_cast<size_t>(metadata->num_row_groups()));
    for (int i = 0; i < metadata->num_row_groups(); ++i) {
        auto rowGroup = metadata->RowGroup(i);
        if (rowGroup->num_rows() < 0 ||
            static_cast<uint64_t>(rowGroup->num_rows()) >
                ParquetRowOffsetMask + 1)
        {
            throw std::runtime_error(
                "parquet row group is too large for a physical row locator");
        }
        RowGroups.push_back({
            .RowGroup = static_cast<size_t>(i),
            .RowCount = rowGroup->num_rows(),
            .ByteSize = rowGroup->total_byte_size(),
        });
    }
}

// Parquet stores a DECIMAL as its unscaled integer: INT32, INT64, or big-endian
// two's complement bytes of a (FIXED_LEN_)BYTE_ARRAY.
std::optional<std::pair<__int128, __int128>> DecimalStatisticsBounds(
    const parquet::ColumnDescriptor& descriptor,
    const parquet::Statistics& stats)
{
    auto bigEndian = [](const uint8_t* data, int32_t length)
        -> std::optional<__int128>
    {
        auto decoded = arrow::Decimal128::FromBigEndian(data, length);
        if (!decoded.ok()) {
            return std::nullopt;
        }
        const auto high = static_cast<unsigned __int128>(
            static_cast<uint64_t>(decoded->high_bits()));
        return static_cast<__int128>((high << 64) | decoded->low_bits());
    };
    switch (descriptor.physical_type()) {
        case parquet::Type::INT32: {
            const auto& typed = static_cast<const parquet::Int32Statistics&>(stats);
            return std::pair<__int128, __int128>{typed.min(), typed.max()};
        }
        case parquet::Type::INT64: {
            const auto& typed = static_cast<const parquet::Int64Statistics&>(stats);
            return std::pair<__int128, __int128>{typed.min(), typed.max()};
        }
        case parquet::Type::FIXED_LEN_BYTE_ARRAY: {
            const auto& typed = static_cast<const parquet::FLBAStatistics&>(stats);
            const int32_t length = descriptor.type_length();
            const auto lo = bigEndian(typed.min().ptr, length);
            const auto hi = bigEndian(typed.max().ptr, length);
            if (!lo || !hi) {
                return std::nullopt;
            }
            return std::pair{*lo, *hi};
        }
        case parquet::Type::BYTE_ARRAY: {
            const auto& typed = static_cast<const parquet::ByteArrayStatistics&>(stats);
            const auto lo = bigEndian(
                typed.min().ptr, static_cast<int32_t>(typed.min().len));
            const auto hi = bigEndian(
                typed.max().ptr, static_cast<int32_t>(typed.max().len));
            if (!lo || !hi) {
                return std::nullopt;
            }
            return std::pair{*lo, *hi};
        }
        default:
            return std::nullopt;
    }
}

void TParquetFileData::LoadRowGroupRanges()
{
    auto metadata = FileReader->parquet_reader()->metadata();
    const int numRowGroups = metadata->num_row_groups();
    RowGroupRanges.resize(static_cast<size_t>(numRowGroups));

    std::unordered_map<std::string_view, size_t> fieldByName;
    fieldByName.reserve(FileNames.size());
    for (size_t field = 0; field < FileNames.size(); ++field) {
        fieldByName.emplace(FileNames[field], field);
    }

    for (int rg = 0; rg < numRowGroups; ++rg) {
        const auto rowGroup = metadata->RowGroup(rg);
        auto& ranges = RowGroupRanges[static_cast<size_t>(rg)];
        ranges.resize(FileNames.size());
        for (size_t field = 0; field < FileNames.size(); ++field) {
            ranges[field].MayBeNull = FileSchema->field(static_cast<int>(field))->nullable();
            ranges[field].MayBeValue = rowGroup->num_rows() > 0;
        }

        for (int column = 0; column < metadata->num_columns(); ++column) {
            const auto* descriptor = metadata->schema()->Column(column);
            auto found = fieldByName.find(descriptor->name());
            if (found == fieldByName.end()) {
                continue; // nested/unsupported schema shape
            }
            const size_t field = found->second;
            auto& range = ranges[field];
            const auto typeId = FileSchema->field(static_cast<int>(field))->type()->id();
            range.MayBeNaN =
                typeId == arrow::Type::FLOAT || typeId == arrow::Type::DOUBLE;

            auto chunk = rowGroup->ColumnChunk(column);
            if (!chunk->is_stats_set()) {
                continue;
            }
            auto stats = chunk->statistics();
            if (!stats) {
                continue;
            }
            if (stats->HasNullCount()) {
                const int64_t nulls = stats->null_count();
                range.MayBeNull = nulls > 0;
                range.MayBeValue = nulls < rowGroup->num_rows();
            }
            if (!range.MayBeValue || !stats->HasMinMax()) {
                continue;
            }

            switch (typeId) {
                case arrow::Type::INT8:
                case arrow::Type::INT16:
                case arrow::Type::INT32:
                case arrow::Type::DATE32: {
                    if (descriptor->physical_type() != parquet::Type::INT32) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::Int32Statistics>(stats);
                    const int64_t lo = typed->min();
                    const int64_t hi = typed->max();
                    if (lo <= hi) {
                        range.MinValue = std::bit_cast<uint64_t>(lo);
                        range.MaxValue = std::bit_cast<uint64_t>(hi);
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::INT64:
                case arrow::Type::DATE64: {
                    if (descriptor->physical_type() != parquet::Type::INT64) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::Int64Statistics>(stats);
                    const int64_t lo = typed->min();
                    const int64_t hi = typed->max();
                    if (lo <= hi) {
                        range.MinValue = std::bit_cast<uint64_t>(lo);
                        range.MaxValue = std::bit_cast<uint64_t>(hi);
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::UINT8:
                case arrow::Type::UINT16:
                case arrow::Type::UINT32: {
                    if (descriptor->physical_type() != parquet::Type::INT32) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::Int32Statistics>(stats);
                    const uint64_t lo = static_cast<uint32_t>(typed->min());
                    const uint64_t hi = static_cast<uint32_t>(typed->max());
                    if (lo <= hi) {
                        range.MinValue = lo;
                        range.MaxValue = hi;
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::UINT64: {
                    if (descriptor->physical_type() != parquet::Type::INT64) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::Int64Statistics>(stats);
                    const uint64_t lo = static_cast<uint64_t>(typed->min());
                    const uint64_t hi = static_cast<uint64_t>(typed->max());
                    if (lo <= hi) {
                        range.MinValue = lo;
                        range.MaxValue = hi;
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::FLOAT: {
                    if (descriptor->physical_type() != parquet::Type::FLOAT) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::FloatStatistics>(stats);
                    const double lo = typed->min();
                    const double hi = typed->max();
                    if (!std::isnan(lo) && !std::isnan(hi) && lo <= hi) {
                        range.MinValue = std::bit_cast<uint64_t>(lo);
                        range.MaxValue = std::bit_cast<uint64_t>(hi);
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::DOUBLE: {
                    if (descriptor->physical_type() != parquet::Type::DOUBLE) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::DoubleStatistics>(stats);
                    const double lo = typed->min();
                    const double hi = typed->max();
                    if (!std::isnan(lo) && !std::isnan(hi) && lo <= hi) {
                        range.MinValue = std::bit_cast<uint64_t>(lo);
                        range.MaxValue = std::bit_cast<uint64_t>(hi);
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::DECIMAL128: {
                    auto bounds = DecimalStatisticsBounds(*descriptor, *stats);
                    if (bounds && bounds->first <= bounds->second) {
                        range.MinValue = static_cast<unsigned __int128>(bounds->first);
                        range.MaxValue = static_cast<unsigned __int128>(bounds->second);
                        range.HasBounds = true;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

TParquetFile::TParquetFile(const std::string& path)
    : Data_(std::make_shared<TParquetFileData>(path))
{}

TParquetFile::~TParquetFile() = default;

std::unique_ptr<TParquetSource> TParquetFile::MakeSource() const
{
    return std::unique_ptr<TParquetSource>(
        new TParquetSource(Data_, std::nullopt, std::nullopt));
}

TParquetSource::TParquetSource(const std::string& path)
    : TParquetSource(
        std::make_shared<TParquetFileData>(path), std::nullopt, std::nullopt)
{}

TParquetSource::TParquetSource(
    std::shared_ptr<TParquetFileData> file,
    std::optional<std::vector<int>> rowGroups,
    std::optional<std::unordered_set<std::string>> restrictedColumns)
    : File_(std::move(file))
    , RowGroups_(std::move(rowGroups))
    , RestrictedColumns_(std::move(restrictedColumns))
{
    ResetReader();
}

std::vector<int> TParquetSource::EffectiveRowGroups() const
{
    if (RowGroups_) {
        return *RowGroups_;
    }

    const int numRgs = static_cast<int>(File_->RowGroups.size());
    std::vector<int> rowGroups(numRgs);
    std::iota(rowGroups.begin(), rowGroups.end(), 0);
    return rowGroups;
}

void TParquetSource::ResetReader()
{
    auto rowGroups = EffectiveRowGroups();
    SelectedColumns_.clear();
    if (RestrictedColumns_) {
        SelectedColumns_ = EffectiveFileColumnIndexes();
    } else {
        for (int i = 0; i < static_cast<int>(File_->FileNames.size()); ++i) {
            SelectedColumns_.push_back(i);
        }
    }
    // Decimal columns bypass Arrow's per-value Decimal128 conversion (TParquetColumnScan).
    std::vector<int> arrowColumns;
    ColumnScans_.clear();
    for (size_t position = 0; position < SelectedColumns_.size(); ++position) {
        const int field = SelectedColumns_[position];
        const int leaf = File_->ColumnScanLeaves[static_cast<size_t>(field)];
        if (leaf < 0 || !IsDecimalType(File_->FileColumns[static_cast<size_t>(field)].Type)) {
            arrowColumns.push_back(field);
            continue;
        }
        ColumnScans_.push_back({
            .Position = position,
            .Scan = std::make_unique<TParquetColumnScan>(
                *File_->FileReader->parquet_reader(), leaf, File_->FileSchema->field(field)->type(),
                rowGroups, GetMemoryPool()),
        });
    }
    auto batchReaderResult = File_->FileReader->GetRecordBatchReader(rowGroups, arrowColumns);
    if (!batchReaderResult.ok()) {
        throw std::runtime_error(batchReaderResult.status().ToString());
    }
    Reader_ = std::move(*batchReaderResult);
    ReaderRowGroups_ = std::move(rowGroups);
    ReaderRowGroupIndex_ = 0;
    ReaderRowOffset_ = 0;

    RefreshSchema();
}

bool TParquetSource::RowIdRequested() const {
    return RestrictedColumns_ && RestrictedColumns_->contains(
        std::string(InternalRowIdColumnName));
}

std::vector<int> TParquetSource::EffectiveFileColumnIndexes(
    const std::unordered_set<std::string>* columnNames) const
{
    if (!columnNames && RestrictedColumns_) {
        columnNames = &*RestrictedColumns_;
    }
    if (!columnNames) {
        return {};
    }

    // Map restricted names to indices into the full file schema,
    // never the possibly-narrowed Names_, so repeated restrictions stay stable.
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(File_->FileNames.size()); ++i) {
        if (columnNames->count(File_->FileNames[i])) {
            indices.push_back(i);
        }
    }
    return indices;
}

void TParquetSource::RefreshSchema()
{
    Names_.clear();
    Columns_.clear();
    Names_.reserve(SelectedColumns_.size());
    Columns_.reserve(SelectedColumns_.size());
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(SelectedColumns_.size());
    size_t nextScan = 0;
    for (size_t position = 0; position < SelectedColumns_.size(); ++position) {
        auto field = File_->FileSchema->field(SelectedColumns_[position]);
        Names_.push_back(field->name());
        Columns_.push_back({
            .Name = Names_.back(),
            .Type = ArrowFieldToQumir(field),
        });
        if (nextScan < ColumnScans_.size() && ColumnScans_[nextScan].Position == position) {
            field = field->WithType(ColumnScans_[nextScan++].Scan->Type());
        }
        fields.push_back(std::move(field));
    }
    BatchSchema_ = arrow::schema(std::move(fields));
    if (RowIdRequested()) {
        Names_.emplace_back(InternalRowIdColumnName);
        Columns_.push_back({
            .Name = Names_.back(),
            .Type = std::make_shared<TIntegerType>(TIntegerType::U64),
        });
    }
    Schema_ = TSchema{std::span<const TColumnSchema>(Columns_)};
}

void TParquetFileData::LoadStandardColumnStats(TStats& stats)
{
    auto metadata = FileReader->parquet_reader()->metadata();
    const int numColumns = metadata->num_columns();
    const int numRowGroups = metadata->num_row_groups();
    const parquet::SchemaDescriptor* schema = metadata->schema();

    for (int c = 0; c < numColumns; ++c) {
        const parquet::ColumnDescriptor* descr = schema->Column(c);
        const parquet::Type::type physType = descr->physical_type();
        const bool isFloat =
            physType == parquet::Type::FLOAT || physType == parquet::Type::DOUBLE;

        int64_t nullCount = 0;
        bool haveNull = false;
        bool haveMinMax = false;
        double minD = 0.0;
        double maxD = 0.0;
        int64_t minI = 0;
        int64_t maxI = 0;

        for (int rg = 0; rg < numRowGroups; ++rg) {
            auto colChunk = metadata->RowGroup(rg)->ColumnChunk(c);
            if (!colChunk->is_stats_set()) {
                continue;
            }
            auto st = colChunk->statistics();
            if (!st) {
                continue;
            }
            if (st->HasNullCount()) {
                nullCount += st->null_count();
                haveNull = true;
            }
            if (!st->HasMinMax()) {
                continue;
            }
            switch (physType) {
                case parquet::Type::INT32: {
                    auto t = std::static_pointer_cast<parquet::Int32Statistics>(st);
                    const int64_t lo = t->min();
                    const int64_t hi = t->max();
                    if (!haveMinMax || lo < minI) minI = lo;
                    if (!haveMinMax || hi > maxI) maxI = hi;
                    haveMinMax = true;
                    break;
                }
                case parquet::Type::INT64: {
                    auto t = std::static_pointer_cast<parquet::Int64Statistics>(st);
                    const int64_t lo = t->min();
                    const int64_t hi = t->max();
                    if (!haveMinMax || lo < minI) minI = lo;
                    if (!haveMinMax || hi > maxI) maxI = hi;
                    haveMinMax = true;
                    break;
                }
                case parquet::Type::FLOAT: {
                    auto t = std::static_pointer_cast<parquet::FloatStatistics>(st);
                    const double lo = t->min();
                    const double hi = t->max();
                    if (!haveMinMax || lo < minD) minD = lo;
                    if (!haveMinMax || hi > maxD) maxD = hi;
                    haveMinMax = true;
                    break;
                }
                case parquet::Type::DOUBLE: {
                    auto t = std::static_pointer_cast<parquet::DoubleStatistics>(st);
                    const double lo = t->min();
                    const double hi = t->max();
                    if (!haveMinMax || lo < minD) minD = lo;
                    if (!haveMinMax || hi > maxD) maxD = hi;
                    haveMinMax = true;
                    break;
                }
                default:
                    // BYTE_ARRAY and others: no numeric min/max we store.
                    break;
            }
        }

        if (!haveNull && !haveMinMax) {
            continue;
        }

        auto& columnStats = stats.ColumnStats[descr->name()];
        if (!columnStats) {
            columnStats = std::make_shared<TStats::TColumnStats>();
        }
        if (haveNull) {
            columnStats->NullCount = static_cast<uint64_t>(nullCount);
        }
        if (haveMinMax) {
            if (isFloat) {
                columnStats->MinValue = std::bit_cast<uint64_t>(minD);
                columnStats->MaxValue = std::bit_cast<uint64_t>(maxD);
            } else {
                columnStats->MinValue = std::bit_cast<uint64_t>(minI);
                columnStats->MaxValue = std::bit_cast<uint64_t>(maxI);
            }
        }
    }
}

void TParquetFileData::LoadColumnStats()
{
    auto metadata = FileReader->parquet_reader()->metadata();

    // Row count is always available from the parquet footer, even for a plain
    // parquet with none of our custom metadata; per-column stats below are
    // best-effort on top of it.
    auto stats = std::make_shared<TStats>();
    stats->RowCount = static_cast<uint64_t>(metadata->num_rows());
    Stats = stats;

    // Base layer: min/max/null_count from the standard parquet column
    // statistics (present in most files). Our custom blob below adds NDV and
    // histograms on top.
    LoadStandardColumnStats(*stats);

    auto kv = metadata->key_value_metadata();
    if (!kv) {
        return;
    }
    const int keyIdx = kv->FindKey("stats");
    if (keyIdx < 0) {
        return;
    }

    auto parsed = llvm::json::parse(kv->value(keyIdx));
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        return;
    }
    const llvm::json::Object* root = parsed->getAsObject();
    const llvm::json::Array* columns = root ? root->getArray("columns") : nullptr;
    if (!columns) {
        return;
    }

    // Physical types decide how min/max/histogram bit patterns are stored.
    std::unordered_map<std::string_view, size_t> indexByName;
    indexByName.reserve(FileNames.size());
    for (size_t i = 0; i < FileNames.size(); ++i) {
        indexByName.emplace(FileNames[i], i);
    }

    for (const auto& colValue : *columns) {
        const llvm::json::Object* col = colValue.getAsObject();
        auto name = col ? col->getString("name") : std::nullopt;
        if (!name) {
            continue;
        }
        auto found = indexByName.find(std::string_view(name->data(), name->size()));
        if (found == indexByName.end()) {
            continue;
        }
        const size_t idx = found->second;

        const auto typeId =
            FileSchema->field(static_cast<int>(idx))->type()->id();
        const bool isFloat =
            typeId == arrow::Type::FLOAT || typeId == arrow::Type::DOUBLE;
        const bool isString =
            typeId == arrow::Type::STRING || typeId == arrow::Type::LARGE_STRING;

        // Store the raw 8-byte pattern matching the column's type: doubles keep
        // their bit representation, every other numeric is widened to int64.
        auto encode = [&](const llvm::json::Value& v) -> std::optional<uint64_t> {
            if (isFloat) {
                if (auto d = v.getAsNumber()) {
                    return std::bit_cast<uint64_t>(*d);
                }
                return std::nullopt;
            }
            if (auto i = v.getAsInteger()) {
                return std::bit_cast<uint64_t>(*i);
            }
            if (auto d = v.getAsNumber()) {
                return std::bit_cast<uint64_t>(static_cast<int64_t>(*d));
            }
            return std::nullopt;
        };

        // Merge onto the standard stats already loaded for this column: the
        // blob owns NDV/histogram; min/max/null_count only fill gaps the
        // standard parquet statistics left empty.
        auto& columnStats = stats->ColumnStats[FileNames[idx]];
        if (!columnStats) {
            columnStats = std::make_shared<TStats::TColumnStats>();
        }
        if (auto ndv = col->getInteger("ndv")) {
            columnStats->Ndv = static_cast<uint64_t>(*ndv);
        }
        columnStats->NdvIsExact = col->getBoolean("ndv_exact").value_or(false);
        if (!columnStats->NullCount) {
            if (auto nullCount = col->getInteger("null_count")) {
                columnStats->NullCount = static_cast<uint64_t>(*nullCount);
            }
        }

        if (!isString) {
            if (!columnStats->MinValue) {
                if (const auto* minValue = col->get("min")) {
                    columnStats->MinValue = encode(*minValue);
                }
            }
            if (!columnStats->MaxValue) {
                if (const auto* maxValue = col->get("max")) {
                    columnStats->MaxValue = encode(*maxValue);
                }
            }
            if (const llvm::json::Array* hist = col->getArray("histogram")) {
                columnStats->Histogram.clear();
                columnStats->Histogram.reserve(hist->size());
                for (const auto& bucket : *hist) {
                    if (auto encoded = encode(bucket)) {
                        columnStats->Histogram.push_back(*encoded);
                    }
                }
            }
        }
    }
}

TParquetSource::~TParquetSource() = default;

const TStatsPtr TParquetSource::Stats() const
{
    return File_->Stats;
}

void TParquetSource::RestrictColumns(const std::unordered_set<std::string>& names) {
    RestrictedColumns_ = names;
    ResetReader();
}

std::vector<NScheduler::TScanRowGroup> TParquetSource::ScanRowGroups() const
{
    return ScanRowGroupsForColumns(nullptr);
}

std::vector<NScheduler::TScanRowGroup> TParquetSource::ScanRowGroups(
    const std::unordered_set<std::string>& columnNames) const
{
    return ScanRowGroupsForColumns(&columnNames);
}

std::vector<NScheduler::TScanRowGroup> TParquetSource::ScanRowGroupsForColumns(
    const std::unordered_set<std::string>* columnNames) const
{
    if (!columnNames && RestrictedColumns_) {
        columnNames = &*RestrictedColumns_;
    }
    std::vector<NScheduler::TScanRowGroup> groups;
    const auto rowGroups = EffectiveRowGroups();
    const auto columns = EffectiveFileColumnIndexes(columnNames);
    const auto metadata = File_->FileReader->parquet_reader()->metadata();
    groups.reserve(rowGroups.size());
    for (int rg : rowGroups) {
        auto group = File_->RowGroups[static_cast<size_t>(rg)];
        if (columnNames) {
            group.ByteSize = 0;
            const auto rowGroup = metadata->RowGroup(rg);
            for (int column : columns) {
                group.ByteSize += rowGroup->ColumnChunk(column)
                    ->total_uncompressed_size();
            }
        }
        groups.push_back(group);
    }
    return groups;
}

std::vector<NScheduler::TScanRowGroup> TParquetSource::PruneRowGroups(
    const NQumir::NAst::TExprPtr& predicate,
    std::string_view sourceAlias,
    std::ostream* diagnostics,
    const std::unordered_set<std::string>* columnNames) const
{
    auto rowGroups = ScanRowGroupsForColumns(columnNames);
    if (!predicate) {
        return rowGroups;
    }
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, File_->FullSchema, sourceAlias, diagnostics);
    if (!evaluator || File_->RowGroupRanges.size() != File_->RowGroups.size()) {
        return rowGroups;
    }

    std::vector<NScheduler::TScanRowGroup> kept;
    kept.reserve(rowGroups.size());
    for (const auto& group : rowGroups) {
        const size_t rg = group.RowGroup;
        const bool mayBeTrue = evaluator->MayBeTrue(File_->RowGroupRanges[rg]);
        if (diagnostics) {
            *diagnostics << "row-group predicate: rg=" << rg
                         << " may_be_true=" << mayBeTrue;
            for (size_t column = 0;
                 column < File_->RowGroupRanges[rg].size(); ++column)
            {
                const auto& range = File_->RowGroupRanges[rg][column];
                *diagnostics << " col=" << File_->FileNames[column]
                             << " bounds=" << range.HasBounds
                             << " min_bits=" << static_cast<uint64_t>(range.MinValue)
                             << " max_bits=" << static_cast<uint64_t>(range.MaxValue)
                             << " null=" << range.MayBeNull
                             << " value=" << range.MayBeValue;
            }
            *diagnostics << '\n';
        }
        if (mayBeTrue) {
            kept.push_back(group);
        }
    }
    return kept;
}

std::unique_ptr<TParquetSource> TParquetSource::MakeRowGroupsSource(
    const std::vector<size_t>& selectedRowGroups) const
{
    std::vector<int> rowGroups;
    rowGroups.reserve(selectedRowGroups.size());
    for (size_t rowGroup : selectedRowGroups) {
        rowGroups.push_back(static_cast<int>(rowGroup));
    }
    return std::unique_ptr<TParquetSource>(new TParquetSource(
        File_, std::optional<std::vector<int>>(std::move(rowGroups)),
        RestrictedColumns_));
}

std::unique_ptr<TParquetGroupedScan> TParquetSource::MakeGroupedScan(
    const std::vector<size_t>& selectedRowGroups,
    const std::vector<std::vector<std::string>>& laterGroups) const
{
    const std::vector<int> rowGroups(selectedRowGroups.begin(), selectedRowGroups.end());
    std::unordered_map<std::string_view, size_t> positions;
    for (size_t position = 0; position < Names_.size(); ++position) {
        positions.emplace(Names_[position], position);
    }
    auto scan = std::unique_ptr<TParquetGroupedScan>(new TParquetGroupedScan());
    scan->ColumnCount_ = Names_.size();
    std::unordered_set<std::string> first(Names_.begin(), Names_.end());
    for (const auto& group : laterGroups) {
        auto& columns = scan->Groups_.emplace_back();
        for (const auto& name : group) {
            const size_t position = positions.at(name);
            // The row locator and a column the column scan cannot read stay in the first group.
            const int field = position < SelectedColumns_.size()
                ? SelectedColumns_[position]
                : -1;
            const int leaf = field < 0
                ? -1
                : File_->ColumnScanLeaves[static_cast<size_t>(field)];
            if (leaf < 0) {
                continue;
            }
            columns.push_back({
                .Position = position,
                .Scan = std::make_unique<TParquetColumnScan>(
                    *File_->FileReader->parquet_reader(), leaf, File_->FileSchema->field(field)->type(),
                    rowGroups, GetMemoryPool()),
            });
            first.erase(name);
        }
    }
    scan->First_ = std::unique_ptr<TParquetSource>(new TParquetSource(
        File_, std::optional<std::vector<int>>(rowGroups), std::move(first)));
    for (const auto& name : scan->First_->Names_) {
        scan->FirstPositions_.push_back(positions.at(name));
    }
    return scan;
}

struct TParquetGroupedScan::TBatch {
    TRowSet First = {};
    std::vector<TColumn> Columns;
    std::vector<std::shared_ptr<arrow::Array>> Arrays;
    std::vector<std::shared_ptr<arrow::Buffer>> DecimalValues;
};

TParquetGroupedScan::~TParquetGroupedScan() = default;

bool TParquetGroupedScan::Next(TRowSet& rowSet) {
    TRowSet first = {};
    if (!First_->Next(first)) {
        return false;
    }
    auto* batch = new TBatch{.First = first};
    batch->Columns.resize(ColumnCount_);
    for (size_t i = 0; i < FirstPositions_.size(); ++i) {
        batch->Columns[FirstPositions_[i]] = first.Columns[i];
    }
    Batch_ = batch;
    rowSet = {
        .Columns = batch->Columns.data(),
        .ColumnCount = static_cast<int64_t>(ColumnCount_),
        .RowCount = first.RowCount,
        .Selection = nullptr,
        .Destroy = [](TRowSet* destroyed) {
            auto* data = static_cast<TBatch*>(destroyed->Private);
            Release(&data->First);
            delete data;
        },
        .Private = batch,
        .RefCount = 1,
    };
    return true;
}

void TParquetGroupedScan::Read(size_t group, const uint8_t* selection) {
    for (auto& column : Groups_[group - 1]) {
        auto values = column.Scan->Read(Batch_->First.RowCount, selection);
        ArrayToColumn(values, Batch_->Columns[column.Position], Batch_->DecimalValues);
        Batch_->Arrays.push_back(std::move(values));
    }
}

void TParquetGroupedScan::Skip(size_t group) {
    for (auto& column : Groups_[group - 1]) {
        column.Scan->Skip(Batch_->First.RowCount);
    }
}

std::shared_ptr<const IPhysicalRowReader> TParquetSource::CompileReader(
    std::span<const std::string> columnNames) const
{
    return std::make_shared<TParquetPhysicalRowReader>(
        File_->FileReader, columnNames, GetMemoryPool());
}

// Parquet page sizes are not in the metadata. Writers cap a data page at 1 MiB
// uncompressed by default, so this yields a lower bound on the page count: a
// writer using smaller pages only makes the real lookup cheaper.
inline constexpr uint64_t ParquetPageSizeGuess = uint64_t{1} << 20;

std::optional<TLateMaterializationCost> TParquetSource::EstimateLookup(
    std::span<const std::string> earlyColumns,
    std::span<const std::string> fetchColumns,
    uint64_t maxRows) const
{
    auto metadata = File_->FileReader->parquet_reader()->metadata();
    std::unordered_map<std::string, int> columnIndexes;
    for (int i = 0; i < static_cast<int>(File_->FileNames.size()); ++i) {
        columnIndexes.emplace(File_->FileNames[static_cast<size_t>(i)], i);
    }

    auto normalize = [&](std::span<const std::string> columns)
        -> std::optional<std::vector<int>>
    {
        std::vector<int> result;
        std::unordered_set<int> seen;
        for (const auto& column : columns) {
            if (column == InternalRowIdColumnName) {
                continue;
            }
            auto it = columnIndexes.find(column);
            if (it == columnIndexes.end()) {
                return std::nullopt;
            }
            if (seen.insert(it->second).second) {
                result.push_back(it->second);
            }
        }
        return result;
    };

    auto early = normalize(earlyColumns);
    auto fetch = normalize(fetchColumns);
    if (!early || !fetch || fetch->empty() || maxRows == 0) {
        return std::nullopt;
    }

    std::vector<int> eager = *early;
    std::unordered_set<int> eagerSet(eager.begin(), eager.end());
    for (int column : *fetch) {
        if (eagerSet.insert(column).second) {
            eager.push_back(column);
        }
    }

    auto bytesFor = [&](int rowGroup, const std::vector<int>& columns)
        -> std::optional<uint64_t>
    {
        uint64_t total = 0;
        for (int column : columns) {
            const int64_t bytes = metadata->RowGroup(rowGroup)
                ->ColumnChunk(column)->total_compressed_size();
            if (bytes < 0) {
                return std::nullopt;
            }
            total += static_cast<uint64_t>(bytes);
        }
        return total;
    };

    TLateMaterializationCost cost;
    std::vector<uint64_t> lookupByRowGroup;
    lookupByRowGroup.reserve(static_cast<size_t>(metadata->num_row_groups()));
    std::unordered_map<int, uint64_t> fetchBytes;
    std::unordered_map<int, uint64_t> fetchPages;
    for (int rg = 0; rg < metadata->num_row_groups(); ++rg) {
        auto eagerBytes = bytesFor(rg, eager);
        auto narrowBytes = bytesFor(rg, *early);
        auto lookupBytes = bytesFor(rg, *fetch);
        if (!eagerBytes || !narrowBytes || !lookupBytes) {
            return std::nullopt;
        }
        cost.EagerBytes += *eagerBytes;
        cost.NarrowBytes += *narrowBytes;
        lookupByRowGroup.push_back(*lookupBytes);
        for (int column : *fetch) {
            const auto chunk = metadata->RowGroup(rg)->ColumnChunk(column);
            const int64_t compressed = chunk->total_compressed_size();
            const int64_t uncompressed = chunk->total_uncompressed_size();
            if (compressed < 0 || uncompressed < 0) {
                return std::nullopt;
            }
            fetchBytes[column] += static_cast<uint64_t>(compressed);
            fetchPages[column] += std::max<uint64_t>(
                1,
                (static_cast<uint64_t>(uncompressed) + ParquetPageSizeGuess - 1)
                    / ParquetPageSizeGuess);
        }
    }
    std::ranges::sort(lookupByRowGroup, std::greater<>());
    const size_t touched = std::min<size_t>(
        static_cast<size_t>(maxRows), lookupByRowGroup.size());
    const uint64_t wholeChunkBytes = std::accumulate(
        lookupByRowGroup.begin(), lookupByRowGroup.begin() + touched,
        uint64_t{0});

    // A lookup reads pages, not whole column chunks: at most one page per
    // requested row per column. Charging a chunk per row, as the whole-chunk
    // bound does, overstates a wide fetch by an order of magnitude.
    uint64_t pageBytes = 0;
    for (const auto& [column, pages] : fetchPages) {
        const uint64_t bytes = fetchBytes[column];
        const uint64_t used = std::min<uint64_t>(maxRows, pages);
        // Split the division to keep the product inside 64 bits.
        pageBytes += bytes / pages * used + bytes % pages * used / pages;
    }
    cost.LookupBytes = std::min(pageBytes, wholeChunkBytes);
    return cost;
}

const TSchema& TParquetSource::Schema() const {
    return Schema_;
}

bool TParquetSource::Next(TRowSet& rowSet) {
    std::shared_ptr<arrow::RecordBatch> batch;
    if (!Reader_->ReadNext(&batch).ok() || !batch) {
        return false;
    }
    if (!ColumnScans_.empty()) {
        // The decimal columns take their positions among the columns Arrow read.
        std::vector<std::shared_ptr<arrow::Array>> columns;
        columns.reserve(SelectedColumns_.size());
        int arrowColumn = 0;
        size_t nextScan = 0;
        for (size_t position = 0; position < SelectedColumns_.size(); ++position) {
            if (nextScan < ColumnScans_.size() && ColumnScans_[nextScan].Position == position) {
                columns.push_back(ColumnScans_[nextScan++].Scan->Read(batch->num_rows()));
            } else {
                columns.push_back(batch->column(arrowColumn++));
            }
        }
        batch = arrow::RecordBatch::Make(BatchSchema_, batch->num_rows(), std::move(columns));
    }
    if (RowIdRequested()) {
        arrow::UInt64Builder rowIds(GetMemoryPool());
        if (auto status = rowIds.Reserve(batch->num_rows()); !status.ok()) {
            throw std::runtime_error(status.ToString());
        }
        int64_t remaining = batch->num_rows();
        while (remaining > 0 && ReaderRowGroupIndex_ < ReaderRowGroups_.size()) {
            const int rg = ReaderRowGroups_[ReaderRowGroupIndex_];
            const int64_t rowGroupRows =
                File_->RowGroups[static_cast<size_t>(rg)].RowCount;
            const int64_t count = std::min(
                remaining, rowGroupRows - ReaderRowOffset_);
            for (int64_t i = 0; i < count; ++i) {
                // The file metadata check in TParquetFileData proves that the
                // row-group id and row offset fit in the locator fields.
                rowIds.UnsafeAppend(MakeParquetRowIdUnchecked(
                    static_cast<uint32_t>(rg),
                    static_cast<uint32_t>(ReaderRowOffset_ + i)));
            }
            remaining -= count;
            ReaderRowOffset_ += count;
            if (ReaderRowOffset_ == rowGroupRows) {
                ++ReaderRowGroupIndex_;
                ReaderRowOffset_ = 0;
            }
        }
        if (remaining != 0) {
            throw std::runtime_error(
                "parquet source row-group metadata ended before the batch");
        }
        auto rowIdArray = rowIds.Finish();
        if (!rowIdArray.ok()) {
            throw std::runtime_error(rowIdArray.status().ToString());
        }
        auto withRowIds = batch->AddColumn(
            batch->num_columns(),
            arrow::field(
                std::string(InternalRowIdColumnName),
                arrow::uint64(),
                false),
            *rowIdArray);
        if (!withRowIds.ok()) {
            throw std::runtime_error(withRowIds.status().ToString());
        }
        batch = std::move(*withRowIds);
    }
    RecordBatchToRowSet(std::move(batch), rowSet);
    return true;
}

} // namespace NQdb
