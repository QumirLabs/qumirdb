#include <qdb/io/parquet/row_reader.h>

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/compute/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/column_reader.h>
#include <parquet/file_reader.h>
#include <parquet/level_conversion.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {

namespace {

struct TLocator {
    uint32_t RowGroup;
    int64_t RowOffset;
    size_t OriginalIndex;
};

enum class EColumnRead {
    Unsupported,
    Primitive,
    Binary,
};

struct TCompiledColumn {
    int FileIndex;
    std::shared_ptr<arrow::Field> Field;
    const parquet::ColumnDescriptor* Descriptor = nullptr;
    parquet::internal::LevelInfo Levels;
    EColumnRead Read = EColumnRead::Unsupported;
    std::shared_ptr<arrow::DataType> StorageType;
    bool ReinterpretStorage = false;
    bool CastStorage = false;
    // The storage holds the unscaled integer of a DECIMAL column.
    bool DecimalStorage = false;
};

arrow::MemoryPool* MemoryPoolOrDefault(arrow::MemoryPool* memoryPool) {
    return memoryPool ? memoryPool : arrow::default_memory_pool();
}

TCompiledColumn CompileColumn(
    int fileIndex,
    const std::shared_ptr<arrow::Field>& field,
    const parquet::ColumnDescriptor* descriptor)
{
    TCompiledColumn column{
        .FileIndex = fileIndex,
        .Field = field,
        .Descriptor = descriptor,
    };
    if (!descriptor || descriptor->max_repetition_level() != 0) {
        return column;
    }
    column.Levels = parquet::internal::LevelInfo::ComputeLevelInfo(descriptor);

    const auto type = field->type()->id();
    switch (descriptor->physical_type()) {
        case parquet::Type::BOOLEAN:
            if (type == arrow::Type::BOOL) {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::uint8();
                column.CastStorage = true;
            }
            break;
        case parquet::Type::INT32:
            if (type == arrow::Type::INT8 ||
                type == arrow::Type::INT16 ||
                type == arrow::Type::INT32 ||
                type == arrow::Type::UINT8 ||
                type == arrow::Type::UINT16 ||
                type == arrow::Type::UINT32 ||
                type == arrow::Type::DATE32)
            {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::int32();
                column.ReinterpretStorage =
                    type == arrow::Type::UINT32 ||
                    type == arrow::Type::DATE32;
                column.CastStorage =
                    type != arrow::Type::INT32 &&
                    !column.ReinterpretStorage;
            } else if (type == arrow::Type::DECIMAL128) {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::int32();
                column.DecimalStorage = true;
            }
            break;
        case parquet::Type::INT64:
            if (type == arrow::Type::INT64 ||
                type == arrow::Type::UINT64 ||
                type == arrow::Type::DATE64)
            {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::int64();
                column.ReinterpretStorage =
                    type == arrow::Type::UINT64 ||
                    type == arrow::Type::DATE64;
            } else if (type == arrow::Type::DECIMAL128) {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::int64();
                column.DecimalStorage = true;
            }
            break;
        case parquet::Type::FLOAT:
            if (type == arrow::Type::FLOAT) {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::float32();
            }
            break;
        case parquet::Type::DOUBLE:
            if (type == arrow::Type::DOUBLE) {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::float64();
            }
            break;
        case parquet::Type::BYTE_ARRAY:
            if (type == arrow::Type::STRING ||
                type == arrow::Type::LARGE_STRING)
            {
                column.Read = EColumnRead::Binary;
            } else if (type == arrow::Type::DECIMAL128) {
                column.Read = EColumnRead::Binary;
                column.DecimalStorage = true;
            }
            break;
        case parquet::Type::FIXED_LEN_BYTE_ARRAY:
            // Decimal128 holds at most 16 bytes; wider storage falls back.
            if (type == arrow::Type::DECIMAL128 &&
                descriptor->type_length() <= 16)
            {
                column.Read = EColumnRead::Binary;
                column.DecimalStorage = true;
            }
            break;
        default:
            break;
    }
    return column;
}

template <typename TArray>
arrow::Status AppendBigEndian(
    const TArray& unscaled,
    arrow::Decimal128Builder& builder)
{
    for (int64_t i = 0; i < unscaled.length(); ++i) {
        if (unscaled.IsNull(i)) {
            builder.UnsafeAppendNull();
            continue;
        }
        const auto bytes = unscaled.GetView(i);
        ARROW_ASSIGN_OR_RAISE(
            auto value,
            arrow::Decimal128::FromBigEndian(
                reinterpret_cast<const uint8_t*>(bytes.data()),
                static_cast<int32_t>(bytes.size())));
        builder.UnsafeAppend(value);
    }
    return arrow::Status::OK();
}

template <typename TArray>
void AppendIntegers(
    const TArray& unscaled,
    arrow::Decimal128Builder& builder)
{
    for (int64_t i = 0; i < unscaled.length(); ++i) {
        if (unscaled.IsNull(i)) {
            builder.UnsafeAppendNull();
        } else {
            builder.UnsafeAppend(
                arrow::Decimal128(static_cast<int64_t>(unscaled.Value(i))));
        }
    }
}

// Parquet stores a DECIMAL as its unscaled integer: INT32/INT64, or big-endian
// two's complement bytes of a (FIXED_LEN_)BYTE_ARRAY. Widen it to Decimal128.
arrow::Result<std::shared_ptr<arrow::Array>> DecimalFromUnscaled(
    const std::shared_ptr<arrow::Array>& unscaled,
    const std::shared_ptr<arrow::DataType>& type,
    arrow::MemoryPool* memoryPool)
{
    arrow::Decimal128Builder builder(type, memoryPool);
    ARROW_RETURN_NOT_OK(builder.Reserve(unscaled->length()));
    switch (unscaled->type_id()) {
        case arrow::Type::INT32:
            AppendIntegers(
                static_cast<const arrow::Int32Array&>(*unscaled), builder);
            break;
        case arrow::Type::INT64:
            AppendIntegers(
                static_cast<const arrow::Int64Array&>(*unscaled), builder);
            break;
        case arrow::Type::BINARY:
            ARROW_RETURN_NOT_OK(AppendBigEndian(
                static_cast<const arrow::BinaryArray&>(*unscaled), builder));
            break;
        case arrow::Type::FIXED_SIZE_BINARY:
            ARROW_RETURN_NOT_OK(AppendBigEndian(
                static_cast<const arrow::FixedSizeBinaryArray&>(*unscaled),
                builder));
            break;
        default:
            return arrow::Status::NotImplemented(
                "parquet page lookup: decimal storage ",
                unscaled->type()->ToString());
    }
    return builder.Finish();
}

struct TPageSelection {
    explicit TPageSelection(std::span<const int64_t> rows)
        : Rows(rows)
    {
        DenseRowsByTarget.reserve(rows.size());
    }

    bool Skip(const parquet::DataPageStats& stats) {
        const int64_t pageRows = stats.num_rows
            ? static_cast<int64_t>(*stats.num_rows)
            : static_cast<int64_t>(stats.num_values);
        if (pageRows < 0) {
            Invalid = true;
            return false;
        }

        const int64_t pageBegin = SourceRows;
        const int64_t pageEnd = pageBegin + pageRows;
        SourceRows = pageEnd;

        while (NextRow < Rows.size() && Rows[NextRow] < pageBegin) {
            Invalid = true;
            ++NextRow;
        }
        const size_t first = NextRow;
        while (NextRow < Rows.size() && Rows[NextRow] < pageEnd) {
            DenseRowsByTarget.push_back(
                DenseRows + Rows[NextRow] - pageBegin);
            ++NextRow;
        }
        const bool keep = NextRow != first;
        if (keep) {
            DenseRows += pageRows;
        }
        return !keep;
    }

    std::span<const int64_t> Rows;
    std::vector<int64_t> DenseRowsByTarget;
    size_t NextRow = 0;
    int64_t SourceRows = 0;
    int64_t DenseRows = 0;
    bool Invalid = false;
};

arrow::Result<std::shared_ptr<arrow::Array>> ReadPageSelectedColumn(
    parquet::RowGroupReader& rowGroup,
    int64_t rowGroupRows,
    const TCompiledColumn& column,
    std::span<const int64_t> rows,
    arrow::MemoryPool* memoryPool)
{
    auto pageReader = rowGroup.GetColumnPageReader(column.FileIndex);
    if (!pageReader) {
        return arrow::Status::Invalid(
            "parquet page lookup: cannot create page reader");
    }

    TPageSelection selection(rows);
    pageReader->set_data_page_filter(
        [&selection](const parquet::DataPageStats& stats) {
            return selection.Skip(stats);
        });

    // Arrow rejects a decimal type over BYTE_ARRAY; read the raw bytes instead.
    const auto readerType =
        column.DecimalStorage &&
            column.Descriptor->physical_type() == parquet::Type::BYTE_ARRAY
        ? arrow::binary()
        : column.Field->type();
    auto recordReader = parquet::internal::RecordReader::Make(
        column.Descriptor,
        column.Levels,
        memoryPool,
        false,
        false,
        readerType);
    recordReader->SetPageReader(std::move(pageReader));
    const int64_t recordsRead = recordReader->ReadRecords(rowGroupRows);

    if (selection.Invalid || selection.NextRow != rows.size() ||
        selection.SourceRows != rowGroupRows ||
        selection.DenseRowsByTarget.size() != rows.size() ||
        recordsRead != selection.DenseRows)
    {
        return arrow::Status::Invalid(
            "parquet page lookup: inconsistent page row boundaries");
    }

    std::shared_ptr<arrow::Array> retained;
    if (column.Read == EColumnRead::Binary) {
        auto* binary = dynamic_cast<parquet::internal::BinaryRecordReader*>(
            recordReader.get());
        if (!binary) {
            return arrow::Status::Invalid(
                "parquet page lookup: binary record reader is unavailable");
        }
        auto chunks = binary->GetBuilderChunks();
        if (chunks.empty()) {
            return arrow::Status::Invalid(
                "parquet page lookup: binary reader returned no chunks");
        }
        if (chunks.size() == 1) {
            retained = std::move(chunks.front());
        } else {
            ARROW_ASSIGN_OR_RAISE(
                retained, arrow::Concatenate(chunks, memoryPool));
        }
    } else {
        const int64_t valuesWritten = recordReader->values_written();
        const int64_t nullCount = recordReader->null_count();
        auto values = recordReader->ReleaseValues();
        auto validity = recordReader->ReleaseIsValid();
        if (valuesWritten != selection.DenseRows || !values) {
            return arrow::Status::Invalid(
                "parquet page lookup: inconsistent primitive value count");
        }

        auto data = arrow::ArrayData::Make(
            column.StorageType,
            valuesWritten,
            {std::move(validity), std::move(values)},
            nullCount);
        retained = arrow::MakeArray(std::move(data));

        if (column.ReinterpretStorage) {
            retained = arrow::MakeArray(arrow::ArrayData::Make(
                column.Field->type(),
                valuesWritten,
                retained->data()->buffers,
                nullCount));
        } else if (column.CastStorage) {
            ARROW_ASSIGN_OR_RAISE(
                auto casted,
                arrow::compute::Cast(
                    arrow::Datum(retained), column.Field->type()));
            retained = casted.make_array();
        }
    }
    if (column.DecimalStorage) {
        ARROW_ASSIGN_OR_RAISE(
            retained,
            DecimalFromUnscaled(retained, column.Field->type(), memoryPool));
    }

    if (retained->length() != selection.DenseRows) {
        return arrow::Status::Invalid(
            "parquet page lookup: decoded page length mismatch");
    }

    arrow::Int64Builder takeBuilder(memoryPool);
    ARROW_RETURN_NOT_OK(takeBuilder.AppendValues(selection.DenseRowsByTarget));
    ARROW_ASSIGN_OR_RAISE(auto takeIndexes, takeBuilder.Finish());
    ARROW_ASSIGN_OR_RAISE(
        auto taken,
        arrow::compute::Take(
            arrow::Datum(retained), arrow::Datum(takeIndexes)));
    return taken.make_array();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadPageSelectedRows(
    parquet::arrow::FileReader& reader,
    std::span<const TLocator> locators,
    std::span<const TCompiledColumn> columns,
    std::span<const size_t> outputToReadColumn,
    const std::shared_ptr<arrow::Schema>& outputSchema,
    arrow::MemoryPool* memoryPool)
{
    auto* parquetReader = reader.parquet_reader();
    auto metadata = parquetReader->metadata();
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> chunks(
        columns.size());
    try {
        size_t groupBegin = 0;
        while (groupBegin < locators.size()) {
            size_t groupEnd = groupBegin + 1;
            while (
                groupEnd < locators.size() &&
                locators[groupEnd].RowGroup == locators[groupBegin].RowGroup)
            {
                ++groupEnd;
            }

            const int rowGroupIndex =
                static_cast<int>(locators[groupBegin].RowGroup);
            const int64_t rowGroupRows =
                metadata->RowGroup(rowGroupIndex)->num_rows();
            std::vector<int64_t> rows;
            rows.reserve(groupEnd - groupBegin);
            for (size_t i = groupBegin; i < groupEnd; ++i) {
                rows.push_back(locators[i].RowOffset);
            }

            for (size_t column = 0; column < columns.size(); ++column) {
                auto rowGroup = parquetReader->RowGroup(rowGroupIndex);
                ARROW_ASSIGN_OR_RAISE(
                    auto selected,
                    ReadPageSelectedColumn(
                        *rowGroup,
                        rowGroupRows,
                        columns[column],
                        rows,
                        memoryPool));
                chunks[column].push_back(std::move(selected));
            }
            groupBegin = groupEnd;
        }
    } catch (const std::exception& e) {
        return arrow::Status::IOError(e.what());
    }

    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(columns.size());
    for (auto& columnChunks : chunks) {
        if (columnChunks.size() == 1) {
            arrays.push_back(std::move(columnChunks.front()));
        } else {
            ARROW_ASSIGN_OR_RAISE(
                auto concatenated,
                arrow::Concatenate(columnChunks, memoryPool));
            arrays.push_back(std::move(concatenated));
        }
    }

    bool alreadyOrdered = true;
    for (size_t i = 0; i < locators.size(); ++i) {
        if (locators[i].OriginalIndex != i) {
            alreadyOrdered = false;
            break;
        }
    }
    if (!alreadyOrdered) {
        std::vector<int64_t> originalToSorted(locators.size());
        for (size_t sorted = 0; sorted < locators.size(); ++sorted) {
            originalToSorted[locators[sorted].OriginalIndex] =
                static_cast<int64_t>(sorted);
        }
        arrow::Int64Builder reorderBuilder(memoryPool);
        ARROW_RETURN_NOT_OK(reorderBuilder.AppendValues(originalToSorted));
        ARROW_ASSIGN_OR_RAISE(auto reorder, reorderBuilder.Finish());
        for (auto& array : arrays) {
            ARROW_ASSIGN_OR_RAISE(
                auto reordered,
                arrow::compute::Take(
                    arrow::Datum(array), arrow::Datum(reorder)));
            array = reordered.make_array();
        }
    }

    if (outputToReadColumn.empty()) {
        return arrow::RecordBatch::Make(
            outputSchema,
            static_cast<int64_t>(locators.size()),
            std::move(arrays));
    }
    std::vector<std::shared_ptr<arrow::Array>> outputArrays;
    outputArrays.reserve(outputToReadColumn.size());
    for (size_t readColumn : outputToReadColumn) {
        outputArrays.push_back(arrays[readColumn]);
    }
    return arrow::RecordBatch::Make(
        outputSchema,
        static_cast<int64_t>(locators.size()),
        std::move(outputArrays));
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadWholeRowGroups(
    parquet::arrow::FileReader& reader,
    const parquet::FileMetaData& metadata,
    std::span<const TLocator> locators,
    const std::vector<int>& fileColumnIndexes,
    std::span<const size_t> outputToReadColumn,
    const std::shared_ptr<arrow::Schema>& outputSchema,
    arrow::MemoryPool* memoryPool)
{
    std::vector<int> rowGroups;
    std::vector<int64_t> takePositions(locators.size());
    int64_t prefix = 0;
    size_t groupBegin = 0;
    while (groupBegin < locators.size()) {
        size_t groupEnd = groupBegin + 1;
        while (
            groupEnd < locators.size() &&
            locators[groupEnd].RowGroup == locators[groupBegin].RowGroup)
        {
            ++groupEnd;
        }

        const int rowGroup = static_cast<int>(locators[groupBegin].RowGroup);
        rowGroups.push_back(rowGroup);
        for (size_t i = groupBegin; i < groupEnd; ++i) {
            takePositions[locators[i].OriginalIndex] =
                prefix + locators[i].RowOffset;
        }
        prefix += metadata.RowGroup(rowGroup)->num_rows();
        groupBegin = groupEnd;
    }

    auto readerResult = reader.GetRecordBatchReader(
        rowGroups, fileColumnIndexes);
    if (!readerResult.ok()) {
        return readerResult.status();
    }
    auto batchReader = std::move(*readerResult);
    ARROW_ASSIGN_OR_RAISE(
        auto table,
        arrow::Table::FromRecordBatchReader(batchReader.get()));

    arrow::Int64Builder takeBuilder(memoryPool);
    ARROW_RETURN_NOT_OK(takeBuilder.AppendValues(takePositions));
    ARROW_ASSIGN_OR_RAISE(auto takeIndexes, takeBuilder.Finish());
    ARROW_ASSIGN_OR_RAISE(
        auto taken,
        arrow::compute::Take(
            arrow::Datum(table), arrow::Datum(takeIndexes)));
    ARROW_ASSIGN_OR_RAISE(
        auto uniqueBatch,
        taken.table()->CombineChunksToBatch(memoryPool));
    if (outputToReadColumn.empty()) {
        return uniqueBatch;
    }
    std::vector<std::shared_ptr<arrow::Array>> outputArrays;
    outputArrays.reserve(outputToReadColumn.size());
    for (size_t readColumn : outputToReadColumn) {
        outputArrays.push_back(uniqueBatch->column(
            static_cast<int>(readColumn)));
    }
    return arrow::RecordBatch::Make(
        outputSchema,
        uniqueBatch->num_rows(),
        std::move(outputArrays));
}

} // namespace

struct TParquetRowReader::TImpl {
    TImpl(
        std::shared_ptr<parquet::arrow::FileReader> reader,
        std::span<const std::string> columnNames,
        arrow::MemoryPool* memoryPool)
        : MemoryPool(memoryPool)
        , Reader(std::move(reader))
    {
        if (!Reader) {
            throw std::invalid_argument("parquet row reader is null");
        }
        if (columnNames.empty()) {
            throw std::invalid_argument(
                "parquet row reader requires at least one column name");
        }

        Metadata = Reader->parquet_reader()->metadata();
        const auto& manifest = Reader->manifest();
        if (manifest.schema_fields.size() !=
            static_cast<size_t>(Metadata->num_columns()))
        {
            throw std::invalid_argument(
                "parquet row reader requires a flat file schema");
        }

        std::unordered_map<
            std::string_view,
            const parquet::arrow::SchemaField*> fieldsByName;
        fieldsByName.reserve(manifest.schema_fields.size());
        for (const auto& field : manifest.schema_fields) {
            if (!field.is_leaf()) {
                throw std::invalid_argument(
                    "parquet row reader requires a flat file schema");
            }
            if (!fieldsByName.emplace(field.field->name(), &field).second) {
                throw std::invalid_argument(
                    "parquet row reader requires unique column names");
            }
        }

        FileColumnIndexes.reserve(columnNames.size());
        Columns.reserve(columnNames.size());
        OutputToReadColumn.reserve(columnNames.size());
        std::unordered_map<int, size_t> uniqueColumns;
        uniqueColumns.reserve(columnNames.size());
        std::vector<std::shared_ptr<arrow::Field>> outputFields;
        outputFields.reserve(columnNames.size());
        SupportsPageLookup = true;
        for (const auto& columnName : columnNames) {
            auto found = fieldsByName.find(columnName);
            if (found == fieldsByName.end()) {
                throw std::invalid_argument(
                    "parquet row reader: unknown column '" +
                    columnName + "'");
            }
            const auto* schemaField = found->second;
            const int fileIndex = schemaField->column_index;
            if (fileIndex < 0 || fileIndex >= Metadata->num_columns()) {
                throw std::runtime_error(
                    "parquet schema manifest has an invalid column index");
            }
            auto [uniqueColumn, inserted] = uniqueColumns.emplace(
                fileIndex, FileColumnIndexes.size());
            if (inserted) {
                FileColumnIndexes.push_back(fileIndex);
                const parquet::ColumnDescriptor* descriptor =
                    Metadata->schema()->Column(fileIndex);
                auto column = CompileColumn(
                    fileIndex, schemaField->field, descriptor);
                SupportsPageLookup &=
                    column.Read != EColumnRead::Unsupported;
                Columns.push_back(std::move(column));
            }
            OutputToReadColumn.push_back(uniqueColumn->second);
            outputFields.push_back(schemaField->field);
        }
        if (uniqueColumns.size() == columnNames.size()) {
            OutputToReadColumn.clear();
        }
        OutputSchema = arrow::schema(std::move(outputFields));
    }

    arrow::MemoryPool* MemoryPool;
    std::shared_ptr<parquet::arrow::FileReader> Reader;
    std::shared_ptr<parquet::FileMetaData> Metadata;
    std::vector<int> FileColumnIndexes;
    std::vector<TCompiledColumn> Columns;
    std::vector<size_t> OutputToReadColumn;
    std::shared_ptr<arrow::Schema> OutputSchema;
    bool SupportsPageLookup = false;
};

TParquetRowReader::TParquetRowReader(
    std::shared_ptr<parquet::arrow::FileReader> reader,
    std::span<const std::string> columnNames,
    arrow::MemoryPool* memoryPool)
    : Impl_(std::make_unique<TImpl>(
        std::move(reader),
        columnNames,
        MemoryPoolOrDefault(memoryPool)))
{}

TParquetRowReader::~TParquetRowReader() = default;

bool TParquetRowReader::SupportsPageLookup() const {
    return Impl_->SupportsPageLookup;
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> TParquetRowReader::ReadRows(std::span<const TPhysicalRowId> rowIds) const
{
    if (rowIds.empty()) {
        return arrow::Status::Invalid(
            "parquet row reader requires at least one row locator");
    }
    std::vector<TLocator> locators;
    locators.reserve(rowIds.size());
    for (size_t i = 0; i < rowIds.size(); ++i) {
        locators.push_back({
            .RowGroup = ParquetRowGroup(rowIds[i]),
            .RowOffset = ParquetRowOffset(rowIds[i]),
            .OriginalIndex = i,
        });
    }
    std::ranges::sort(locators, [](const TLocator& left, const TLocator& right) {
        if (left.RowGroup != right.RowGroup) {
            return left.RowGroup < right.RowGroup;
        }
        if (left.RowOffset != right.RowOffset) {
            return left.RowOffset < right.RowOffset;
        }
        return left.OriginalIndex < right.OriginalIndex;
    });

    const auto& metadata = Impl_->Metadata;
    size_t groupBegin = 0;
    while (groupBegin < locators.size()) {
        size_t groupEnd = groupBegin + 1;
        while (
            groupEnd < locators.size() &&
            locators[groupEnd].RowGroup == locators[groupBegin].RowGroup)
        {
            ++groupEnd;
        }
        const uint32_t rowGroup = locators[groupBegin].RowGroup;
        if (rowGroup >= static_cast<uint32_t>(metadata->num_row_groups())) {
            return arrow::Status::IndexError(
                "parquet row locator is outside the file");
        }
        const int64_t rowCount =
            metadata->RowGroup(static_cast<int>(rowGroup))->num_rows();
        if (locators[groupEnd - 1].RowOffset >= rowCount) {
            return arrow::Status::IndexError(
                "parquet row locator is outside the file");
        }
        groupBegin = groupEnd;
    }

    if (Impl_->SupportsPageLookup) {
        auto selected = ReadPageSelectedRows(
            *Impl_->Reader,
            locators,
            Impl_->Columns,
            Impl_->OutputToReadColumn,
            Impl_->OutputSchema,
            Impl_->MemoryPool);
        if (selected.ok()) {
            return selected;
        }
        if (!selected.status().IsInvalid() &&
            !selected.status().IsNotImplemented())
        {
            return selected.status();
        }
    }
    return ReadWholeRowGroups(
        *Impl_->Reader,
        *metadata,
        locators,
        Impl_->FileColumnIndexes,
        Impl_->OutputToReadColumn,
        Impl_->OutputSchema,
        Impl_->MemoryPool);
}

} // namespace NQdb
