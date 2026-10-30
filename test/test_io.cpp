#include <gtest/gtest.h>

#include <qdb/io/parquet/row_id.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/parquet/row_id.h>
#include <qdb/io/text/sink.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/types/nullable.h>

#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

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

// Collects one f64 column; NULL cells come back as std::nullopt.
std::vector<std::optional<double>> DoubleCells(const TColumn& column, int64_t rowCount) {
    std::vector<std::optional<double>> cells;
    const auto* values = reinterpret_cast<const double*>(column.Data);
    for (int64_t row = 0; row < rowCount; ++row) {
        const auto bit = column.MaskBitOffset + row;
        const bool valid = !column.Mask || ((column.Mask[bit / 8] >> (bit % 8)) & 1);
        cells.push_back(valid ? std::optional<double>(values[row]) : std::nullopt);
    }
    return cells;
}

} // namespace

// The engine has one float width: a Parquet FLOAT column is scanned and looked
// up as f64, and every value, including NaN, -0 and denormals, widens exactly.
TEST(IOTest, FloatColumnIsReadAsDouble) {
    const std::string path = "/tmp/test_io_qdb_float.parquet";
    const std::vector<float> values{
        0.1f, -0.0f, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(), 16777216.0f};
    arrow::FloatBuilder builder;
    (void)builder.AppendValues(values);
    (void)builder.AppendNull();
    const auto rowCount = static_cast<int64_t>(values.size()) + 1;
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("f", arrow::float32())}), rowCount, {builder.Finish().ValueOrDie()});
    WriteParquet(path, batch, 4);

    auto expectWidened = [&](const std::vector<std::optional<double>>& cells) {
        ASSERT_EQ(cells.size(), values.size() + 1);
        for (size_t row = 0; row < values.size(); ++row) {
            ASSERT_TRUE(cells[row]) << "row " << row;
            const double expected = values[row];
            EXPECT_TRUE(std::isnan(expected)
                ? std::isnan(*cells[row])
                : std::bit_cast<uint64_t>(*cells[row]) == std::bit_cast<uint64_t>(expected))
                << "row " << row << ": " << *cells[row];
        }
        EXPECT_FALSE(cells.back());
    };

    NQdb::TParquetFile file(path);
    auto source = file.MakeSource();
    source->RestrictColumns({"f", std::string(InternalRowIdColumnName)});
    auto type = NQumir::NAst::TMaybeType<NQumir::NAst::TFloatType>(
        UnwrapNullableType(source->Schema().Columns[0].Type));
    ASSERT_TRUE(type);
    EXPECT_EQ(type.Cast()->Kind, NQumir::NAst::TFloatType::F64);

    std::vector<std::optional<double>> scanned;
    std::vector<TPhysicalRowId> rowIds;
    TRowSet rowSet = {};
    while (source->Next(rowSet)) {
        auto cells = DoubleCells(rowSet.Columns[0], rowSet.RowCount);
        scanned.insert(scanned.end(), cells.begin(), cells.end());
        const auto* ids = reinterpret_cast<const TPhysicalRowId*>(rowSet.Columns[1].Data);
        rowIds.insert(rowIds.end(), ids, ids + rowSet.RowCount);
        Release(&rowSet);
    }
    expectWidened(scanned);

    const std::vector<std::string> columns{"f"};
    TRowSet lookup = {};
    std::string error;
    ASSERT_TRUE(source->CompileReader(columns)->ReadRows(rowIds, lookup, &error)) << error;
    expectWidened(DoubleCells(lookup.Columns[0], lookup.RowCount));
    Release(&lookup);
}
