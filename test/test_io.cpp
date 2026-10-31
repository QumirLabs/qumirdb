#include <gtest/gtest.h>

#include <qdb/io/parquet/column_scan.h>
#include <qdb/io/parquet/decimal_storage.h>
#include <qdb/io/parquet/row_id.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/parquet/row_id.h>
#include <qdb/io/text/sink.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/schema.h>
#include <parquet/arrow/writer.h>
#include <parquet/column_writer.h>
#include <parquet/file_reader.h>
#include <parquet/file_writer.h>

using namespace NQdb;

namespace {

void WriteParquet(
    const std::string& path,
    std::shared_ptr<arrow::RecordBatch> batch,
    int64_t chunkSize = 1024)
{
    auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    auto status = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, chunkSize);
    ASSERT_TRUE(status.ok()) << status.ToString();
}

// Scans the first column of a parquet file, a decimal, at the width of its storage.
std::vector<std::optional<__int128>> ScanDecimalStorage(const std::string& path, NQdb::TDecimalSpec spec) {
    NQdb::TParquetFile file(path);
    auto source = file.MakeSource();
    const int32_t width = NQdb::DecimalStorageBytes(spec);
    std::vector<std::optional<__int128>> scanned;
    TRowSet rowSet{};
    while (source->Next(rowSet)) {
        const auto& column = rowSet.Columns[0];
        for (int64_t row = 0; row < rowSet.RowCount; ++row) {
            if (column.Mask && !(column.Mask[(column.MaskBitOffset + row) / 8] >> ((column.MaskBitOffset + row) % 8) & 1)) {
                scanned.push_back(std::nullopt);
                continue;
            }
            __int128 value = 0;
            if (width == 8) {
                int64_t narrow;
                std::memcpy(&narrow, column.Data + row * 8, sizeof(narrow));
                value = narrow;
            } else {
                std::memcpy(&value, column.Data + row * 16, sizeof(value));
            }
            scanned.push_back(value);
        }
        NQdb::Release(&rowSet);
    }
    return scanned;
}

} // namespace

TEST(IOTest, ParquetRoundtrip) {
    const std::string path = "/tmp/test_io_qdb.parquet";

    arrow::Int64Builder ids;
    arrow::StringBuilder names;
    arrow::DoubleBuilder values;
    arrow::BooleanBuilder flags;

    (void)ids.AppendValues({1, 2, 3});
    (void)names.AppendValues({"alice", "bob", "carol"});
    (void)values.AppendValues({1.5, 2.5, 3.5});
    (void)flags.AppendValues(std::vector<bool>{true, false, true});

    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), false),
        arrow::field("name", arrow::utf8()),
        arrow::field("value", arrow::float64()),
        arrow::field("flag", arrow::boolean()),
    });
    auto batch = arrow::RecordBatch::Make(schema, 3, {
        ids.Finish().ValueOrDie(),
        names.Finish().ValueOrDie(),
        values.Finish().ValueOrDie(),
        flags.Finish().ValueOrDie(),
    });
    WriteParquet(path, batch);

    NQdb::TParquetSource source(path);
    const auto& s = source.Schema();

    ASSERT_EQ(s.Columns.size(), 4u);
    EXPECT_EQ(s.Columns[0].Name, "id");
    EXPECT_EQ(s.Columns[1].Name, "name");
    EXPECT_EQ(s.Columns[2].Name, "value");
    EXPECT_EQ(s.Columns[3].Name, "flag");
    EXPECT_FALSE(IsNullableType(s.Columns[0].Type));
    EXPECT_TRUE(IsNullableType(s.Columns[1].Type));
    EXPECT_TRUE(IsNullableType(s.Columns[2].Type));
    EXPECT_TRUE(IsNullableType(s.Columns[3].Type));
    EXPECT_EQ(UnwrapNullableType(s.Columns[1].Type)->TypeName(),
        NQumir::NAst::TStringType::TypeId);

    auto structType = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(
        StructTypeFromSchema(s));
    ASSERT_TRUE(structType);
    EXPECT_EQ(structType.Cast()->Fields[1].second, s.Columns[1].Type);

    TRowSet rowSet = {};
    ASSERT_TRUE(source.Next(rowSet));
    EXPECT_EQ(rowSet.RowCount, 3);
    EXPECT_EQ(rowSet.ColumnCount, 4);

    std::ostringstream out;
    NQdb::TConsoleSink sink(s, out);
    sink.Write(rowSet);
    sink.Flush();

    auto result = out.str();
    EXPECT_NE(result.find("alice"), std::string::npos);
    EXPECT_NE(result.find("1.5"), std::string::npos);
    EXPECT_NE(result.find("true"), std::string::npos);

    NQdb::Release(&rowSet);

    ASSERT_FALSE(source.Next(rowSet));
}

TEST(IOTest, NullsInColumn) {
    const std::string path = "/tmp/test_io_qdb_nulls.parquet";

    arrow::Int64Builder ids;
    (void)ids.Append(10);
    (void)ids.AppendNull();
    (void)ids.Append(30);

    auto schema = arrow::schema({arrow::field("id", arrow::int64())});
    auto batch = arrow::RecordBatch::Make(schema, 3, {ids.Finish().ValueOrDie()});
    WriteParquet(path, batch);

    NQdb::TParquetSource source(path);

    TRowSet rowSet = {};
    ASSERT_TRUE(source.Next(rowSet));

    const auto& col = rowSet.Columns[0];
    ASSERT_NE(col.Mask, nullptr);
    // Arrow validity bitmap: 1 = valid, 0 = null, bit-packed within each byte
    auto isValid = [&](int row) {
        return (col.Mask[row / 8] >> (row % 8)) & 1;
    };
    EXPECT_NE(isValid(0), 0); // row 0: valid
    EXPECT_EQ(isValid(1), 0); // row 1: null
    EXPECT_NE(isValid(2), 0); // row 2: valid

    std::ostringstream out;
    NQdb::TConsoleSink sink(source.Schema(), out);
    sink.Write(rowSet);
    sink.Flush();
    EXPECT_NE(out.str().find("NULL"), std::string::npos);

    NQdb::Release(&rowSet);
}

TEST(IOTest, ParquetRowGroupRangeSource) {
    const std::string path = "/tmp/test_io_qdb_rowgroups.parquet";

    arrow::Int64Builder ids;
    arrow::Int64Builder payload;
    (void)ids.AppendValues({1, 2, 3, 4, 5});
    (void)payload.AppendValues({10, 20, 30, 40, 50});

    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), false),
        arrow::field("payload", arrow::int64(), false),
    });
    auto batch = arrow::RecordBatch::Make(schema, 5, {
        ids.Finish().ValueOrDie(),
        payload.Finish().ValueOrDie(),
    });
    WriteParquet(path, batch, 2);

    NQdb::TParquetFile file(path);
    auto source = file.MakeSource();
    auto rowGroups = source->ScanRowGroups();
    ASSERT_EQ(rowGroups.size(), 3u);
    EXPECT_EQ(rowGroups[0].RowCount, 2);
    EXPECT_EQ(rowGroups[1].RowCount, 2);
    EXPECT_EQ(rowGroups[2].RowCount, 1);

    const auto plannedGroups = source->ScanRowGroups(
        std::unordered_set<std::string>{"id"});
    // Planning metadata must not alter a reader shared by other scan aliases.
    EXPECT_EQ(source->Schema().Columns.size(), 2u);
    EXPECT_EQ(source->ScanRowGroups()[0].ByteSize, rowGroups[0].ByteSize);
    source->RestrictColumns({"id"});
    ASSERT_EQ(source->Schema().Columns.size(), 1u);
    const auto projectedGroups = source->ScanRowGroups();
    ASSERT_EQ(projectedGroups.size(), rowGroups.size());
    for (size_t rg = 0; rg < rowGroups.size(); ++rg) {
        EXPECT_EQ(projectedGroups[rg].RowGroup, rowGroups[rg].RowGroup);
        EXPECT_EQ(projectedGroups[rg].RowCount, rowGroups[rg].RowCount);
        EXPECT_GT(projectedGroups[rg].ByteSize, 0);
        EXPECT_LT(projectedGroups[rg].ByteSize, rowGroups[rg].ByteSize);
        EXPECT_EQ(projectedGroups[rg].ByteSize, plannedGroups[rg].ByteSize);
    }
    const std::vector<std::string> columnNames{
        "payload",
        "id",
        "payload",
    };
    auto reader = source->CompileReader(columnNames);
    const std::vector<TPhysicalRowId> rowIds{
        MakeParquetRowId(2, 0),
        MakeParquetRowId(0, 1),
        MakeParquetRowId(1, 0),
    };
    TRowSet lookupRows{};
    std::string error;
    ASSERT_TRUE(reader->ReadRows(rowIds, lookupRows, &error)) << error;
    ASSERT_EQ(lookupRows.RowCount, 3);
    ASSERT_EQ(lookupRows.ColumnCount, 3);
    const auto* outPayload =
        reinterpret_cast<const int64_t*>(lookupRows.Columns[0].Data);
    const auto* outIds =
        reinterpret_cast<const int64_t*>(lookupRows.Columns[1].Data);
    const auto* outPayloadAgain =
        reinterpret_cast<const int64_t*>(lookupRows.Columns[2].Data);
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + 3),
        (std::vector<int64_t>{50, 20, 30}));
    EXPECT_EQ(std::vector<int64_t>(outIds, outIds + 3),
        (std::vector<int64_t>{5, 2, 3}));
    EXPECT_EQ(std::vector<int64_t>(outPayloadAgain, outPayloadAgain + 3),
        (std::vector<int64_t>{50, 20, 30}));
    Release(&lookupRows);

    auto split = source->MakeRowGroupsSource({1});
    EXPECT_EQ(split->Stats().get(), source->Stats().get());
    const auto splitGroups = split->ScanRowGroups();
    ASSERT_EQ(splitGroups.size(), 1u);
    EXPECT_EQ(splitGroups[0].RowGroup, 1u);
    EXPECT_EQ(splitGroups[0].RowCount, projectedGroups[1].RowCount);
    EXPECT_EQ(splitGroups[0].ByteSize, projectedGroups[1].ByteSize);
    TRowSet rowSet = {};
    ASSERT_TRUE(split->Next(rowSet));
    ASSERT_EQ(rowSet.RowCount, 2);
    ASSERT_EQ(rowSet.ColumnCount, 1);
    ASSERT_EQ(split->Schema().Columns.size(), 1u);
    EXPECT_EQ(split->Schema().Columns[0].Name, "id");
    const auto* values = reinterpret_cast<const int64_t*>(rowSet.Columns[0].Data);
    EXPECT_EQ(values[0], 3);
    EXPECT_EQ(values[1], 4);
    NQdb::Release(&rowSet);
    EXPECT_FALSE(split->Next(rowSet));

    source->RestrictColumns({});
    for (const auto& group : source->ScanRowGroups()) {
        EXPECT_EQ(group.ByteSize, 0);
    }
    int64_t rowCount = 0;
    while (source->Next(rowSet)) {
        EXPECT_EQ(rowSet.ColumnCount, 0);
        rowCount += rowSet.RowCount;
        NQdb::Release(&rowSet);
    }
    EXPECT_EQ(rowCount, 5);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

namespace {

// Writes 7 rows in row groups of 3: (0,0..2) (1,0..2) (2,0).
std::string WriteRowIdFixture() {
    const std::string path = "/tmp/test_io_qdb_row_id.parquet";
    arrow::Int64Builder ids;
    arrow::Int64Builder payload;
    (void)ids.AppendValues({1, 2, 3, 4, 5, 6, 7});
    (void)payload.AppendValues({10, 20, 30, 40, 50, 60, 70});
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({
            arrow::field("id", arrow::int64(), false),
            arrow::field("payload", arrow::int64(), false),
        }),
        7,
        {ids.Finish().ValueOrDie(), payload.Finish().ValueOrDie()});
    WriteParquet(path, batch, 3);
    return path;
}

// Drains a source, returning the row-id column of every row in scan order.
std::vector<TPhysicalRowId> DrainRowIds(
    ISource& source,
    int32_t rowIdColumn,
    std::vector<int64_t>* firstColumn = nullptr)
{
    std::vector<TPhysicalRowId> rowIds;
    TRowSet rowSet = {};
    while (source.Next(rowSet)) {
        const auto* ids = reinterpret_cast<const TPhysicalRowId*>(
            rowSet.Columns[rowIdColumn].Data);
        const auto* values = firstColumn
            ? reinterpret_cast<const int64_t*>(rowSet.Columns[0].Data)
            : nullptr;
        for (int64_t row = 0; row < rowSet.RowCount; ++row) {
            rowIds.push_back(ids[row]);
            if (values) {
                firstColumn->push_back(values[row]);
            }
        }
        Release(&rowSet);
    }
    return rowIds;
}

const std::vector<TPhysicalRowId> ExpectedRowIds = {
    MakeParquetRowId(0, 0), MakeParquetRowId(0, 1), MakeParquetRowId(0, 2),
    MakeParquetRowId(1, 0), MakeParquetRowId(1, 1), MakeParquetRowId(1, 2),
    MakeParquetRowId(2, 0),
};

} // namespace

TEST(IOTest, RowIdColumnIsAppendedAfterFileColumns) {
    NQdb::TParquetFile file(WriteRowIdFixture());
    auto source = file.MakeSource();

    // Without the request the synthetic column does not exist.
    source->RestrictColumns({"id"});
    ASSERT_EQ(source->Schema().Columns.size(), 1u);
    EXPECT_EQ(source->Schema().Columns[0].Name, "id");

    source->RestrictColumns({"id", std::string(InternalRowIdColumnName)});
    ASSERT_EQ(source->Schema().Columns.size(), 2u);
    EXPECT_EQ(source->Schema().Columns[0].Name, "id");
    EXPECT_EQ(source->Schema().Columns[1].Name, InternalRowIdColumnName);

    // Arrow coalesces row groups into one record batch, so this also covers
    // a batch whose rows straddle several row groups.
    std::vector<int64_t> ids;
    const auto rowIds = DrainRowIds(*source, 1, &ids);
    EXPECT_EQ(rowIds, ExpectedRowIds);
    EXPECT_EQ(ids, (std::vector<int64_t>{1, 2, 3, 4, 5, 6, 7}));

    // The locator halves decode back to the row group and the offset in it.
    for (size_t row = 0; row < rowIds.size(); ++row) {
        const auto rowGroup = ParquetRowGroup(rowIds[row]);
        const auto offset = ParquetRowOffset(rowIds[row]);
        EXPECT_EQ(rowGroup, static_cast<uint32_t>(row / 3));
        EXPECT_EQ(offset, static_cast<uint32_t>(row % 3));
    }
}

TEST(IOTest, RowIdColumnWithoutAnyFileColumn) {
    NQdb::TParquetFile file(WriteRowIdFixture());
    auto source = file.MakeSource();
    source->RestrictColumns({std::string(InternalRowIdColumnName)});
    ASSERT_EQ(source->Schema().Columns.size(), 1u);
    EXPECT_EQ(source->Schema().Columns[0].Name, InternalRowIdColumnName);

    TRowSet rowSet = {};
    ASSERT_TRUE(source->Next(rowSet));
    EXPECT_EQ(rowSet.ColumnCount, 1);
    Release(&rowSet);

    auto fresh = file.MakeSource();
    fresh->RestrictColumns({std::string(InternalRowIdColumnName)});
    EXPECT_EQ(DrainRowIds(*fresh, 0), ExpectedRowIds);
}

TEST(IOTest, RowIdColumnKeepsFileWideLocatorsInSplitScans) {
    NQdb::TParquetFile file(WriteRowIdFixture());
    auto source = file.MakeSource();
    source->RestrictColumns({"id", std::string(InternalRowIdColumnName)});

    // A split source scans one row group but must still report file-wide
    // locators, otherwise a later lookup would read the wrong rows.
    auto split = source->MakeRowGroupsSource({1});
    std::vector<int64_t> ids;
    const auto rowIds = DrainRowIds(*split, 1, &ids);
    EXPECT_EQ(rowIds, (std::vector<TPhysicalRowId>{
        MakeParquetRowId(1, 0),
        MakeParquetRowId(1, 1),
        MakeParquetRowId(1, 2),
    }));
    EXPECT_EQ(ids, (std::vector<int64_t>{4, 5, 6}));

    auto tail = source->MakeRowGroupsSource({2});
    EXPECT_EQ(DrainRowIds(*tail, 1),
        (std::vector<TPhysicalRowId>{MakeParquetRowId(2, 0)}));

    auto empty = source->MakeRowGroupsSource({});
    EXPECT_TRUE(DrainRowIds(*empty, 1).empty());
}

TEST(IOTest, RowIdColumnRestartsAfterRestrictColumns) {
    NQdb::TParquetFile file(WriteRowIdFixture());
    auto source = file.MakeSource();
    source->RestrictColumns({"id", std::string(InternalRowIdColumnName)});

    TRowSet rowSet = {};
    ASSERT_TRUE(source->Next(rowSet));
    ASSERT_GT(rowSet.RowCount, 0);
    Release(&rowSet);

    // Re-restricting rebuilds the reader, so locators must start over instead
    // of continuing from the partially consumed scan.
    source->RestrictColumns({"payload", std::string(InternalRowIdColumnName)});
    std::vector<int64_t> payload;
    EXPECT_EQ(DrainRowIds(*source, 1, &payload), ExpectedRowIds);
    EXPECT_EQ(payload, (std::vector<int64_t>{10, 20, 30, 40, 50, 60, 70}));
}

TEST(IOTest, ScannedRowIdsResolveBackToTheirRows) {
    const auto path = WriteRowIdFixture();
    NQdb::TParquetFile file(path);
    auto source = file.MakeSource();
    source->RestrictColumns({"id", std::string(InternalRowIdColumnName)});
    std::vector<int64_t> ids;
    const auto rowIds = DrainRowIds(*source, 1, &ids);
    ASSERT_EQ(rowIds.size(), 7u);

    // Feed the locators the scan produced back into a lookup and check that
    // every row comes back with the payload of the row the scan saw.
    const std::vector<std::string> columns{"payload", "id"};
    auto reader = source->CompileReader(columns);
    TRowSet lookup = {};
    std::string error;
    ASSERT_TRUE(reader->ReadRows(rowIds, lookup, &error)) << error;
    ASSERT_EQ(lookup.RowCount, 7);
    ASSERT_EQ(lookup.ColumnCount, 2);
    const auto* payload = reinterpret_cast<const int64_t*>(lookup.Columns[0].Data);
    const auto* lookedUpIds = reinterpret_cast<const int64_t*>(lookup.Columns[1].Data);
    for (size_t row = 0; row < rowIds.size(); ++row) {
        EXPECT_EQ(lookedUpIds[row], ids[row]) << "row " << row;
        EXPECT_EQ(payload[row], ids[row] * 10) << "row " << row;
    }
    Release(&lookup);
}

TEST(IOTest, RejectsFileWithReservedRowIdColumnName) {
    const std::string path = "/tmp/test_io_qdb_reserved_row_id.parquet";
    arrow::Int64Builder values;
    (void)values.AppendValues({1, 2});
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({
            arrow::field(std::string(InternalRowIdColumnName),
                arrow::int64(), false),
        }),
        2,
        {values.Finish().ValueOrDie()});
    WriteParquet(path, batch);

    EXPECT_THROW(NQdb::TParquetFile{path}, std::runtime_error);
}

TEST(IOTest, EstimateLookupChargesPagesNotWholeChunks) {
    const std::string path = "/tmp/test_io_qdb_estimate.parquet";
    constexpr int64_t rowCount = 30000;

    arrow::Int64Builder ids;
    arrow::StringBuilder payload;
    for (int64_t row = 0; row < rowCount; ++row) {
        (void)ids.Append(row);
        // Unique so the writer cannot shrink the chunk with a dictionary, but
        // compressible, so the fixture stays small on disk.
        (void)payload.Append(std::string(110, 'z') + std::to_string(row));
    }
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({
            arrow::field("id", arrow::int64(), false),
            arrow::field("payload", arrow::utf8(), false),
        }),
        rowCount,
        {ids.Finish().ValueOrDie(), payload.Finish().ValueOrDie()});

    const std::vector<std::string> early{"id"};
    const std::vector<std::string> fetch{"payload"};

    // One row group whose fetch chunk spans several pages.
    WriteParquet(path, batch, rowCount);
    NQdb::TParquetFile file(path);
    auto source = file.MakeSource();

    auto few = source->EstimateLookup(early, fetch, 2);
    auto many = source->EstimateLookup(early, fetch, 1000);
    ASSERT_TRUE(few);
    ASSERT_TRUE(many);
    EXPECT_GT(few->LookupBytes, 0u);
    // The whole-chunk bound cannot tell these apart -- one row group is touched
    // either way. Charging pages can.
    EXPECT_LT(few->LookupBytes, many->LookupBytes);

    // Past the page count the estimate saturates at the whole chunk, and then
    // the eager cost is exactly the narrow scan plus the fetched columns.
    auto saturated = source->EstimateLookup(early, fetch, 1000000);
    ASSERT_TRUE(saturated);
    EXPECT_EQ(saturated->LookupBytes, many->LookupBytes);
    EXPECT_EQ(saturated->EagerBytes,
        saturated->NarrowBytes + saturated->LookupBytes);
    EXPECT_GT(saturated->EagerBytes, saturated->NarrowBytes);

    // The synthetic locator is not a file column and costs nothing.
    const std::vector<std::string> earlyWithLocator{
        "id", std::string(InternalRowIdColumnName)};
    auto withLocator = source->EstimateLookup(earlyWithLocator, fetch, 2);
    ASSERT_TRUE(withLocator);
    EXPECT_EQ(withLocator->NarrowBytes, few->NarrowBytes);
    EXPECT_EQ(withLocator->LookupBytes, few->LookupBytes);

    EXPECT_FALSE(source->EstimateLookup(early, fetch, 0));
    EXPECT_FALSE(source->EstimateLookup(early, std::vector<std::string>{}, 2));
    EXPECT_FALSE(source->EstimateLookup(
        early, std::vector<std::string>{"nosuch"}, 2));

    // Small row groups hold a single page each, so the page estimate falls back
    // to the whole-chunk bound.
    const std::string smallPath = "/tmp/test_io_qdb_estimate_small.parquet";
    WriteParquet(smallPath, batch, 500);
    NQdb::TParquetFile smallFile(smallPath);
    auto smallSource = smallFile.MakeSource();
    auto everyGroup = smallSource->EstimateLookup(early, fetch, rowCount);
    ASSERT_TRUE(everyGroup);
    EXPECT_EQ(everyGroup->EagerBytes,
        everyGroup->NarrowBytes + everyGroup->LookupBytes);
}

namespace {

// Collects one f32 column as raw bits, so NaN payloads and -0 are compared exactly;
// NULL cells come back as std::nullopt.
std::vector<std::optional<uint32_t>> FloatCells(const TColumn& column, int64_t rowCount) {
    std::vector<std::optional<uint32_t>> cells;
    const auto* values = reinterpret_cast<const uint32_t*>(column.Data);
    for (int64_t row = 0; row < rowCount; ++row) {
        const auto bit = column.MaskBitOffset + row;
        const bool valid = !column.Mask || ((column.Mask[bit / 8] >> (bit % 8)) & 1);
        cells.push_back(valid ? std::optional<uint32_t>(values[row]) : std::nullopt);
    }
    return cells;
}

} // namespace

// A Parquet FLOAT column is a 4-byte f32 column in a scan and in a late lookup:
// NaN, -0, infinities and denormals keep their bits, in required and optional
// columns, in dictionary and PLAIN pages, across row groups. The 40K-row case
// reads past the first batch and would show a read beyond the decoded buffer as
// values that change from run to run.
TEST(IOTest, FloatColumnIsReadAsFloat) {
    const std::vector<float> specials{
        0.1f, -0.0f, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(), 16777216.0f};
    for (const bool nullable : {false, true}) {
        for (const bool dictionary : {false, true}) {
            for (const int64_t size : {int64_t{9}, int64_t{40000}}) {
                SCOPED_TRACE("nullable=" + std::to_string(nullable) + " dictionary=" +
                    std::to_string(dictionary) + " rows=" + std::to_string(size));
                std::vector<std::optional<float>> values;
                for (int64_t row = 0; row < size; ++row) {
                    if (nullable && row % 5 == 4) {
                        values.push_back(std::nullopt);
                    } else if (row < static_cast<int64_t>(specials.size())) {
                        values.push_back(specials[row]);
                    } else {
                        values.push_back(static_cast<float>(row % 1000) * 0.37f - 100.0f);
                    }
                }
                arrow::FloatBuilder builder;
                for (const auto& value : values) {
                    ASSERT_TRUE((value ? builder.Append(*value) : builder.AppendNull()).ok());
                }
                auto batch = arrow::RecordBatch::Make(
                    arrow::schema({arrow::field("f", arrow::float32(), nullable)}),
                    size, {builder.Finish().ValueOrDie()});
                const std::string path = "/tmp/test_io_qdb_float.parquet";
                {
                    parquet::WriterProperties::Builder properties;
                    if (!dictionary) {
                        properties.disable_dictionary();
                    }
                    auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
                    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
                    ASSERT_TRUE(parquet::arrow::WriteTable(
                        *table, arrow::default_memory_pool(), outfile, 4, properties.build()).ok());
                }

                auto expectRead = [&](const std::vector<std::optional<uint32_t>>& cells) {
                    ASSERT_EQ(cells.size(), values.size());
                    for (size_t row = 0; row < values.size(); ++row) {
                        ASSERT_EQ(cells[row].has_value(), values[row].has_value()) << "row " << row;
                        if (values[row]) {
                            EXPECT_EQ(*cells[row], std::bit_cast<uint32_t>(*values[row])) << "row " << row;
                        }
                    }
                };

                NQdb::TParquetFile file(path);
                auto source = file.MakeSource();
                source->RestrictColumns({"f", std::string(InternalRowIdColumnName)});
                auto type = NQumir::NAst::TMaybeType<NQumir::NAst::TFloatType>(
                    UnwrapNullableType(source->Schema().Columns[0].Type));
                ASSERT_TRUE(type);
                EXPECT_EQ(type.Cast()->Kind, NQumir::NAst::TFloatType::F32);

                std::vector<std::optional<uint32_t>> scanned;
                std::vector<TPhysicalRowId> rowIds;
                TRowSet rowSet = {};
                while (source->Next(rowSet)) {
                    auto cells = FloatCells(rowSet.Columns[0], rowSet.RowCount);
                    scanned.insert(scanned.end(), cells.begin(), cells.end());
                    const auto* ids = reinterpret_cast<const TPhysicalRowId*>(rowSet.Columns[1].Data);
                    rowIds.insert(rowIds.end(), ids, ids + rowSet.RowCount);
                    Release(&rowSet);
                }
                expectRead(scanned);

                const std::vector<std::string> columns{"f"};
                TRowSet lookup = {};
                std::string error;
                ASSERT_TRUE(source->CompileReader(columns)->ReadRows(rowIds, lookup, &error)) << error;
                expectRead(FloatCells(lookup.Columns[0], lookup.RowCount));
                Release(&lookup);
            }
        }
    }
}

// A decimal array of any Arrow width comes out at the width of the qdb storage of its
// precision, sign-extended or truncated, and a slice keeps its offset.
TEST(IOTest, DecimalStorageValuesTakeTheStorageWidth) {
    auto check = [](const std::shared_ptr<arrow::DataType>& type, const std::vector<__int128>& values) {
        std::shared_ptr<arrow::Array> array;
        const auto& decimal = static_cast<const arrow::DecimalType&>(*type);
        auto builder = arrow::MakeBuilder(type).ValueOrDie();
        for (const auto value : values) {
            arrow::Status status;
            switch (type->id()) {
                case arrow::Type::DECIMAL32:
                    status = static_cast<arrow::Decimal32Builder&>(*builder).Append(
                        arrow::Decimal32(static_cast<int32_t>(value)));
                    break;
                case arrow::Type::DECIMAL64:
                    status = static_cast<arrow::Decimal64Builder&>(*builder).Append(
                        arrow::Decimal64(static_cast<int64_t>(value)));
                    break;
                default:
                    status = static_cast<arrow::Decimal128Builder&>(*builder).Append(
                        arrow::Decimal128(static_cast<int64_t>(value >> 64), static_cast<uint64_t>(value)));
                    break;
            }
            ASSERT_TRUE(status.ok()) << status.ToString();
        }
        array = builder->Finish().ValueOrDie()->Slice(1);
        const int32_t width = NQdb::DecimalStorageBytes({
            .Precision = decimal.precision(),
            .Scale = decimal.scale(),
        });
        auto storage = NQdb::DecimalStorageValues(*array, arrow::default_memory_pool());
        ASSERT_EQ(storage->size(), array->length() * width) << type->ToString();
        for (int64_t i = 0; i < array->length(); ++i) {
            __int128 actual = 0;
            if (width == 8) {
                int64_t narrow;
                std::memcpy(&narrow, storage->data() + i * width, sizeof(narrow));
                actual = narrow;
            } else {
                std::memcpy(&actual, storage->data() + i * width, sizeof(actual));
            }
            EXPECT_TRUE(actual == values[static_cast<size_t>(i) + 1]) << type->ToString() << " row " << i;
        }
    };
    const __int128 big = static_cast<__int128>(std::numeric_limits<int64_t>::max()) * 1000;
    check(arrow::decimal32(9, 2), {7, 999999999, -999999999, 0, -1});
    check(arrow::decimal64(15, 2), {7, 999999999999999, -999999999999999, 0, -1});
    check(arrow::decimal64(18, 0), {7, 999999999999999999, -999999999999999999, -1});
    check(arrow::decimal128(15, 2), {7, 999999999999999, -999999999999999, -1});
    check(arrow::decimal128(38, 2), {7, big, -big, -1});
}

// An f32 column holds 4-byte values and prints as the shortest text that reads back as
// the same float; the same bits read as f64 would print differently.
TEST(IOTest, SinkPrintsF32AtFloatPrecision) {
    const std::vector<float> values{
        0.1f, 16777216.0f, -0.0f,
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
    };
    TColumn column{.Data = reinterpret_cast<char*>(const_cast<float*>(values.data()))};
    TRowSet rowSet{
        .Columns = &column,
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(values.size()),
        .RefCount = 1,
    };
    const std::vector<TColumnSchema> columns{
        {"f", std::make_shared<NQumir::NAst::TFloatType>(NQumir::NAst::TFloatType::F32)},
    };
    const TSchema schema{columns};

    std::ostringstream csv;
    TCsvSink csvSink(schema, csv);
    csvSink.Write(rowSet);
    EXPECT_EQ(csv.str(), "f\n0.1\n16777216\n-0\ninf\nnan\n");

    std::ostringstream console;
    TConsoleSink consoleSink(schema, console);
    consoleSink.Write(rowSet);
    consoleSink.Flush();
    EXPECT_NE(console.str().find("0.1"), std::string::npos);
    EXPECT_EQ(console.str().find("0.10000000149"), std::string::npos);
}

// A scan reads decimal columns itself (TParquetColumnScan): INT32, INT64 and
// FIXED_LEN_BYTE_ARRAY decimals with NULLs come out at their storage width, also
// when a batch spans several row groups.
TEST(IOTest, ScanDecimalColumnsInTheirStorage) {
    struct TCase {
        std::shared_ptr<arrow::DataType> Type;
        bool AsInteger;
    };
    const __int128 big = static_cast<__int128>(std::numeric_limits<int64_t>::max()) * 1000;
    const std::vector<std::optional<__int128>> base = {
        7, -1, std::nullopt, 999999999, -999999999, 0, 12345, std::nullopt, -42, 1, 2,
    };
    for (const auto& testCase : std::vector<TCase>{
        {arrow::decimal128(9, 2), true},
        {arrow::decimal128(18, 0), true},
        {arrow::decimal128(15, 2), false},
        {arrow::decimal128(30, 4), false},
    }) {
        const auto& decimal = static_cast<const arrow::DecimalType&>(*testCase.Type);
        auto values = base;
        if (decimal.precision() >= 18) {
            values.push_back(decimal.precision() > 18 ? big : std::numeric_limits<int64_t>::max() / 10);
            values.push_back(decimal.precision() > 18 ? -big : -(std::numeric_limits<int64_t>::max() / 10));
        }
        arrow::Decimal128Builder builder(testCase.Type);
        for (const auto& value : values) {
            ASSERT_TRUE((value
                ? builder.Append(arrow::Decimal128(static_cast<int64_t>(*value >> 64), static_cast<uint64_t>(*value)))
                : builder.AppendNull()).ok());
        }
        auto batch = arrow::RecordBatch::Make(
            arrow::schema({arrow::field("d", testCase.Type)}),
            static_cast<int64_t>(values.size()), {builder.Finish().ValueOrDie()});
        const std::string path = "/tmp/test_io_qdb_decimal_scan.parquet";
        {
            auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
            parquet::WriterProperties::Builder properties;
            if (testCase.AsInteger) {
                properties.enable_store_decimal_as_integer();
            }
            auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
            ASSERT_TRUE(parquet::arrow::WriteTable(
                *table, arrow::default_memory_pool(), output, 3, properties.build()).ok());
        }

        const auto scanned = ScanDecimalStorage(path, {.Precision = decimal.precision(), .Scale = decimal.scale()});
        ASSERT_EQ(scanned.size(), values.size()) << testCase.Type->ToString();
        for (size_t row = 0; row < values.size(); ++row) {
            EXPECT_TRUE(scanned[row] == values[row]) << testCase.Type->ToString() << " row " << row;
        }
    }
}

// Every physical type of a decimal, PLAIN and dictionary encoded, REQUIRED and with NULLs,
// read through many pages, page-sized and chunk-sized reads, and row groups in one batch.
// Fixed-length values are sign-extended to their length, BYTE_ARRAY values come in minimal,
// sign-padded and empty (zero) forms. A dictionary chunk has several pages and falls back to PLAIN.
TEST(IOTest, ScanDecimalPagesOfEveryPhysicalType) {
    struct TCase {
        parquet::Type::type Physical;
        int Length;
        int Precision;
        bool Nullable;
    };
    constexpr int64_t rows = 20011;
    constexpr int64_t rowGroupRows = 9001;
    auto bigEndian = [](__int128 value, int length) {
        std::string bytes(static_cast<size_t>(length), '\0');
        for (int i = 0; i < length; ++i) {
            bytes[static_cast<size_t>(length - 1 - i)] = static_cast<char>(value >> (8 * i));
        }
        return bytes;
    };
    // A BYTE_ARRAY decimal may carry any number of redundant sign bytes, and zero may be empty.
    auto byteArrayLength = [&](__int128 value, int64_t row) {
        int length = 1;
        while (length < 16 && (value >= (static_cast<__int128>(1) << (8 * length - 1)) ||
            value < -(static_cast<__int128>(1) << (8 * length - 1))))
        {
            ++length;
        }
        if (value == 0 && row % 3 == 0) {
            return 0;
        }
        return std::min(16, length + static_cast<int>(row % 4));
    };
    for (const bool dictionary : {true, false}) {
        for (const auto& testCase : std::vector<TCase>{
            {parquet::Type::INT32, 0, 9, true},
            {parquet::Type::INT64, 0, 18, true},
            {parquet::Type::INT64, 0, 15, false},
            {parquet::Type::FIXED_LEN_BYTE_ARRAY, 1, 2, true},
            {parquet::Type::FIXED_LEN_BYTE_ARRAY, 7, 15, false},
            {parquet::Type::FIXED_LEN_BYTE_ARRAY, 8, 18, true},
            {parquet::Type::FIXED_LEN_BYTE_ARRAY, 9, 18, true},
            {parquet::Type::FIXED_LEN_BYTE_ARRAY, 16, 15, true},
            {parquet::Type::FIXED_LEN_BYTE_ARRAY, 13, 30, true},
            {parquet::Type::FIXED_LEN_BYTE_ARRAY, 16, 38, false},
            {parquet::Type::BYTE_ARRAY, 0, 15, true},
            {parquet::Type::BYTE_ARRAY, 0, 38, true},
        }) {
            const std::string label = std::string(parquet::TypeToString(testCase.Physical)) + "(" +
                std::to_string(testCase.Length) + ") p=" + std::to_string(testCase.Precision) +
                (dictionary ? " dictionary" : " plain");
            __int128 limit = 1;
            for (int digit = 0; digit < testCase.Precision; ++digit) {
                limit *= 10;
            }
            limit -= 1;
            std::vector<std::optional<__int128>> values;
            for (int64_t row = 0; row < rows; ++row) {
                if (testCase.Nullable && row % 13 == 5) {
                    values.push_back(std::nullopt);
                    continue;
                }
                const __int128 edges[] = {limit, -limit, 0, -1, 1};
                values.push_back(row % 11 < 5
                    ? edges[row % 11]
                    : (limit / (row % 997 + 2) + row % 7) * (row % 2 ? -1 : 1));
            }

            const std::string path = "/tmp/test_io_qdb_decimal_pages.parquet";
            {
                using parquet::schema::GroupNode;
                using parquet::schema::PrimitiveNode;
                auto node = PrimitiveNode::Make(
                    "d",
                    testCase.Nullable ? parquet::Repetition::OPTIONAL : parquet::Repetition::REQUIRED,
                    parquet::LogicalType::Decimal(testCase.Precision, 2),
                    testCase.Physical,
                    testCase.Physical == parquet::Type::FIXED_LEN_BYTE_ARRAY ? testCase.Length : -1);
                auto schema = std::static_pointer_cast<GroupNode>(
                    GroupNode::Make("schema", parquet::Repetition::REQUIRED, {node}));
                parquet::WriterProperties::Builder properties;
                if (dictionary) {
                    properties.enable_dictionary();
                    properties.data_pagesize(1000);
                    properties.dictionary_pagesize_limit(2000);
                    properties.write_batch_size(128);
                } else {
                    properties.disable_dictionary();
                    properties.data_pagesize(1000);
                }
                auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
                auto writer = parquet::ParquetFileWriter::Open(output, schema, properties.build());
                for (int64_t begin = 0; begin < rows; begin += rowGroupRows) {
                    const int64_t end = std::min(rows, begin + rowGroupRows);
                    std::vector<int16_t> defLevels;
                    std::vector<int32_t> ints;
                    std::vector<int64_t> longs;
                    std::vector<std::string> bytes;
                    for (int64_t row = begin; row < end; ++row) {
                        defLevels.push_back(values[row] ? 1 : 0);
                        if (!values[row]) {
                            continue;
                        }
                        ints.push_back(static_cast<int32_t>(*values[row]));
                        longs.push_back(static_cast<int64_t>(*values[row]));
                        bytes.push_back(bigEndian(*values[row],
                            testCase.Length ? testCase.Length : byteArrayLength(*values[row], row)));
                    }
                    auto* column = writer->AppendRowGroup()->NextColumn();
                    const auto count = static_cast<int64_t>(defLevels.size());
                    const int16_t* levels = testCase.Nullable ? defLevels.data() : nullptr;
                    switch (testCase.Physical) {
                        case parquet::Type::INT32:
                            static_cast<parquet::Int32Writer*>(column)->WriteBatch(count, levels, nullptr, ints.data());
                            break;
                        case parquet::Type::INT64:
                            static_cast<parquet::Int64Writer*>(column)->WriteBatch(count, levels, nullptr, longs.data());
                            break;
                        case parquet::Type::FIXED_LEN_BYTE_ARRAY: {
                            std::vector<parquet::FixedLenByteArray> flba;
                            for (const auto& value : bytes) {
                                flba.emplace_back(reinterpret_cast<const uint8_t*>(value.data()));
                            }
                            static_cast<parquet::FixedLenByteArrayWriter*>(column)->WriteBatch(count, levels, nullptr, flba.data());
                            break;
                        }
                        default: {
                            std::vector<parquet::ByteArray> binary(bytes.begin(), bytes.end());
                            static_cast<parquet::ByteArrayWriter*>(column)->WriteBatch(count, levels, nullptr, binary.data());
                            break;
                        }
                    }
                }
                writer->Close();
                ASSERT_TRUE(output->Close().ok());
            }

            if (dictionary && testCase.Precision >= 9) {
                // Thousands of distinct values overflow the dictionary page limit mid-chunk.
                auto metadata = parquet::ParquetFileReader::OpenFile(path)->metadata();
                for (int group = 0; group < metadata->num_row_groups(); ++group) {
                    int64_t dictionaryPages = 0;
                    int64_t plainPages = 0;
                    for (const auto& stats : metadata->RowGroup(group)->ColumnChunk(0)->encoding_stats()) {
                        if (stats.page_type == parquet::PageType::DATA_PAGE) {
                            (stats.encoding == parquet::Encoding::PLAIN ? plainPages : dictionaryPages) += stats.count;
                        }
                    }
                    EXPECT_GT(dictionaryPages, 0) << label << " group " << group;
                    EXPECT_GT(plainPages, 0) << label << " group " << group;
                }
            }

            const auto scanned = ScanDecimalStorage(path, {.Precision = testCase.Precision, .Scale = 2});
            ASSERT_EQ(scanned.size(), values.size()) << label;
            int64_t mismatches = 0;
            for (size_t row = 0; row < values.size(); ++row) {
                if (scanned[row] != values[row] && mismatches++ < 5) {
                    ADD_FAILURE() << label << " row " << row;
                }
            }
            EXPECT_EQ(mismatches, 0) << label;
        }
    }
}

// A column scan reads only the rows a selection marks, through many pages and row groups:
// every selected row has its value, an unselected string is empty, a read of a smaller
// selection follows a dense one, and skipped rows leave the next read aligned.
TEST(IOTest, ColumnScanReadsSelectedRows) {
    constexpr int64_t rows = 20000;
    arrow::Int64Builder ids;
    arrow::Int16Builder smalls;
    arrow::StringBuilder strings;
    arrow::BooleanBuilder flags;
    arrow::DoubleBuilder doubles;
    arrow::Decimal128Builder decimals(arrow::decimal128(9, 2));
    for (int64_t row = 0; row < rows; ++row) {
        ASSERT_TRUE(ids.Append(row).ok());
        ASSERT_TRUE(smalls.Append(static_cast<int16_t>(row % 1000 - 500)).ok());
        ASSERT_TRUE((row % 7 == 3
            ? strings.AppendNull()
            : strings.Append("v" + std::to_string(row % 50) + std::string(row % 3 == 0 ? row % 40 : 0, 'x'))).ok());
        ASSERT_TRUE((row % 11 == 0 ? flags.AppendNull() : flags.Append(row % 3 == 0)).ok());
        ASSERT_TRUE(doubles.Append(static_cast<double>(row) * 0.5).ok());
        ASSERT_TRUE((row % 13 == 5 ? decimals.AppendNull() : decimals.Append(arrow::Decimal128(row * 7 - 1000))).ok());
    }
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({
            arrow::field("id", arrow::int64(), false),
            arrow::field("small", arrow::int16(), false),
            arrow::field("s", arrow::utf8()),
            arrow::field("b", arrow::boolean()),
            arrow::field("d", arrow::float64(), false),
            arrow::field("dec", arrow::decimal128(9, 2)),
        }),
        rows,
        {
            ids.Finish().ValueOrDie(),
            smalls.Finish().ValueOrDie(),
            strings.Finish().ValueOrDie(),
            flags.Finish().ValueOrDie(),
            doubles.Finish().ValueOrDie(),
            decimals.Finish().ValueOrDie(),
        });
    const std::string path = "/tmp/test_io_qdb_column_scan.parquet";
    {
        parquet::WriterProperties::Builder properties;
        properties.data_pagesize(128);
        properties.write_batch_size(64);
        auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
        auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
        ASSERT_TRUE(parquet::arrow::WriteTable(
            *table, arrow::default_memory_pool(), outfile, 6000, properties.build()).ok());
    }

    // Selections of one read: every row, none, sparse, one run, and a dense middle.
    auto select = [](int64_t pattern, int64_t count) {
        std::vector<uint8_t> selection(static_cast<size_t>(count));
        for (int64_t i = 0; i < count; ++i) {
            switch (pattern % 5) {
                case 0: selection[i] = 1; break;
                case 1: selection[i] = 0; break;
                case 2: selection[i] = i % 997 == 13; break;
                case 3: selection[i] = i >= count / 3 && i < count / 2; break;
                default: selection[i] = (i * 7 + pattern) % 10 < 3; break;
            }
        }
        return selection;
    };
    auto reader = parquet::ParquetFileReader::OpenFile(path);
    std::shared_ptr<arrow::Schema> schema;
    ASSERT_TRUE(parquet::arrow::FromParquetSchema(reader->metadata()->schema(), &schema).ok());
    for (int column = 0; column < batch->num_columns(); ++column) {
        int64_t pages = 0;
        for (const auto& stats : reader->metadata()->RowGroup(0)->ColumnChunk(column)->encoding_stats()) {
            if (stats.page_type == parquet::PageType::DATA_PAGE) {
                pages += stats.count;
            }
        }
        ASSERT_GE(pages, 4) << column;
    }
    for (const std::vector<int>& rowGroups : {std::vector<int>{0, 1, 2, 3}, std::vector<int>{1, 3}}) {
        std::vector<int64_t> fileRows;
        for (int group : rowGroups) {
            for (int64_t row = group * 6000; row < std::min<int64_t>(rows, (group + 1) * 6000); ++row) {
                fileRows.push_back(row);
            }
        }
        for (int column = 0; column < batch->num_columns(); ++column) {
            SCOPED_TRACE(batch->schema()->field(column)->name() + " groups=" + std::to_string(rowGroups.size()));
            const auto& type = schema->field(column)->type();
            ASSERT_TRUE(TParquetColumnScan::Supports(*reader->metadata()->schema()->Column(column), type));
            TParquetColumnScan scan(*reader, column, type, rowGroups, arrow::default_memory_pool());
            const auto expected = batch->column(column);
            int64_t position = 0;
            for (int64_t read = 0; position < static_cast<int64_t>(fileRows.size()); ++read) {
                const int64_t count = std::min<int64_t>(
                    static_cast<int64_t>(fileRows.size()) - position, 1000 + (read * 1777) % 3000);
                if (read % 6 == 5) {
                    scan.Skip(count);
                    position += count;
                    continue;
                }
                const auto selection = select(read, count);
                const bool dense = read % 5 == 0;
                auto values = scan.Read(count, dense ? nullptr : selection.data());
                ASSERT_EQ(values->length(), count);
                for (int64_t i = 0; i < count; ++i) {
                    const int64_t fileRow = fileRows[position + i];
                    if (!dense && selection[i] == 0) {
                        if (values->type_id() == arrow::Type::LARGE_STRING && values->IsValid(i)) {
                            EXPECT_EQ(static_cast<const arrow::LargeStringArray&>(*values).value_length(i), 0);
                        }
                        continue;
                    }
                    ASSERT_EQ(values->IsValid(i), expected->IsValid(fileRow)) << "row " << fileRow;
                    if (expected->IsValid(fileRow)) {
                        ASSERT_EQ(values->GetScalar(i).ValueOrDie()->ToString(),
                            expected->GetScalar(fileRow).ValueOrDie()->ToString()) << "row " << fileRow;
                    }
                }
                position += count;
            }
        }
    }
}
