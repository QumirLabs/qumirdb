#include <gtest/gtest.h>

#include <qdb/io/parquet/row_reader.h>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace NQdb;

namespace {

void WriteParquet(
    const std::string& path,
    const std::shared_ptr<arrow::RecordBatch>& batch,
    int64_t rowGroupSize,
    bool storeDecimalAsInteger = false)
{
    auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    parquet::WriterProperties::Builder properties;
    properties
        .max_rows_per_page(16)
        ->data_pagesize(256)
        ->disable_write_page_index();
    if (storeDecimalAsInteger) {
        properties.enable_store_decimal_as_integer();
    }
    auto status = parquet::arrow::WriteTable(
        *table,
        arrow::default_memory_pool(),
        output,
        rowGroupSize,
        properties.build());
    ASSERT_TRUE(status.ok()) << status.ToString();
}

std::shared_ptr<parquet::arrow::FileReader> OpenParquet(
    const std::string& path)
{
    auto input = arrow::io::ReadableFile::Open(path).ValueOrDie();
    parquet::arrow::FileReaderBuilder builder;
    auto status = builder.Open(input);
    if (!status.ok()) {
        throw std::runtime_error(status.ToString());
    }
    auto reader = builder.Build().ValueOrDie();
    return std::shared_ptr<parquet::arrow::FileReader>(std::move(reader));
}

} // namespace

TEST(ParquetRowReaderTest, LocatorUsesHighAndLow32Bits) {
    const auto locator = MakeParquetRowId(UINT32_MAX, UINT32_MAX);
    EXPECT_EQ(ParquetRowGroup(locator), UINT32_MAX);
    EXPECT_EQ(ParquetRowOffset(locator), UINT32_MAX);
    EXPECT_THROW(
        MakeParquetRowId(uint64_t{UINT32_MAX} + 1, 0),
        std::out_of_range);
    EXPECT_THROW(
        MakeParquetRowId(0, uint64_t{UINT32_MAX} + 1),
        std::out_of_range);
}

TEST(ParquetRowReaderTest, ReadsSelectedPagesInLocatorOrder) {
    const std::string path = "/tmp/test_parquet_row_reader_qdb.parquet";
    constexpr int64_t rowGroupSize = 256;
    constexpr int64_t rowCount = rowGroupSize * 2;

    arrow::Int64Builder integers;
    arrow::StringBuilder strings;
    arrow::BooleanBuilder booleans;
    arrow::BinaryBuilder binary;
    for (int64_t row = 0; row < rowCount; ++row) {
        if (row % 7 == 0) {
            ASSERT_TRUE(integers.AppendNull().ok());
        } else {
            ASSERT_TRUE(integers.Append(row * 10).ok());
        }
        if (row % 11 == 0) {
            ASSERT_TRUE(strings.AppendNull().ok());
        } else {
            ASSERT_TRUE(strings.Append("value-" + std::to_string(row)).ok());
        }
        if (row % 5 == 0) {
            ASSERT_TRUE(booleans.AppendNull().ok());
        } else {
            ASSERT_TRUE(booleans.Append((row & 1) != 0).ok());
        }
        ASSERT_TRUE(binary.Append("raw").ok());
    }

    auto schema = arrow::schema({
        arrow::field("i", arrow::int64(), true),
        arrow::field("s", arrow::utf8(), true),
        arrow::field("b", arrow::boolean(), true),
        arrow::field("raw", arrow::binary(), false),
    });
    auto batch = arrow::RecordBatch::Make(schema, rowCount, {
        integers.Finish().ValueOrDie(),
        strings.Finish().ValueOrDie(),
        booleans.Finish().ValueOrDie(),
        binary.Finish().ValueOrDie(),
    });
    WriteParquet(path, batch, rowGroupSize);

    const std::vector<TPhysicalRowId> locators{
        MakeParquetRowId(1, 200),
        MakeParquetRowId(0, 7),
        MakeParquetRowId(1, 22),
        MakeParquetRowId(0, 3),
        MakeParquetRowId(1, 255),
        MakeParquetRowId(0, 3),
        MakeParquetRowId(0, 127),
    };
    const std::vector<std::string> columnNames{"s", "i", "b"};
    auto fileReader = OpenParquet(path);
    TParquetRowReader reader(fileReader, columnNames);
    auto result = reader.ReadRows(locators);
    ASSERT_TRUE(result.ok()) << result.status().ToString();

    const auto& output = *result;
    ASSERT_EQ(output->num_rows(), static_cast<int64_t>(locators.size()));
    ASSERT_EQ(output->num_columns(), 3);
    auto outputStrings =
        std::static_pointer_cast<arrow::StringArray>(output->column(0));
    auto outputIntegers =
        std::static_pointer_cast<arrow::Int64Array>(output->column(1));
    auto outputBooleans =
        std::static_pointer_cast<arrow::BooleanArray>(output->column(2));

    for (size_t i = 0; i < locators.size(); ++i) {
        const int64_t row =
            static_cast<int64_t>(ParquetRowGroup(locators[i])) * rowGroupSize +
            ParquetRowOffset(locators[i]);
        EXPECT_EQ(outputStrings->IsValid(static_cast<int64_t>(i)), row % 11 != 0);
        if (row % 11 != 0) {
            EXPECT_EQ(
                outputStrings->GetString(static_cast<int64_t>(i)),
                "value-" + std::to_string(row));
        }
        EXPECT_EQ(outputIntegers->IsValid(static_cast<int64_t>(i)), row % 7 != 0);
        if (row % 7 != 0) {
            EXPECT_EQ(outputIntegers->Value(static_cast<int64_t>(i)), row * 10);
        }
        EXPECT_EQ(outputBooleans->IsValid(static_cast<int64_t>(i)), row % 5 != 0);
        if (row % 5 != 0) {
            EXPECT_EQ(
                outputBooleans->Value(static_cast<int64_t>(i)),
                (row & 1) != 0);
        }
    }

    auto badLocator = reader.ReadRows(
        std::vector<TPhysicalRowId>{MakeParquetRowId(2, 0)});
    EXPECT_FALSE(badLocator.ok());

    const std::vector<std::string> fallbackColumnNames{"i", "raw", "i"};
    TParquetRowReader fallbackReader(fileReader, fallbackColumnNames);
    EXPECT_FALSE(fallbackReader.SupportsPageLookup());
    auto fallbackResult = fallbackReader.ReadRows(locators);
    ASSERT_TRUE(fallbackResult.ok()) << fallbackResult.status().ToString();
    const auto& fallbackOutput = *fallbackResult;
    ASSERT_EQ(fallbackOutput->num_rows(),
        static_cast<int64_t>(locators.size()));
    ASSERT_EQ(fallbackOutput->num_columns(), 3);
    EXPECT_EQ(fallbackOutput->schema()->field(0)->name(), "i");
    EXPECT_EQ(fallbackOutput->schema()->field(1)->name(), "raw");
    EXPECT_EQ(fallbackOutput->schema()->field(2)->name(), "i");
    EXPECT_TRUE(fallbackOutput->column(0)->Equals(
        fallbackOutput->column(2)));

    EXPECT_THROW(
        TParquetRowReader(fileReader, std::vector<std::string>{"missing"}),
        std::invalid_argument);
}

// DECIMAL is stored as INT32/INT64 or as big-endian FIXED_LEN_BYTE_ARRAY; the
// page lookup must decode every representation into the Decimal128 column.
TEST(ParquetRowReaderTest, ReadsDecimalColumnsFromEveryStorage) {
    constexpr int64_t rowGroupSize = 128;
    constexpr int64_t rowCount = rowGroupSize * 2;

    auto d9 = arrow::decimal128(9, 2);
    auto d15 = arrow::decimal128(15, 2);
    auto d18 = arrow::decimal128(18, 4);
    auto d38 = arrow::decimal128(38, 6);
    arrow::Decimal128Builder b9(d9);
    arrow::Decimal128Builder b15(d15);
    arrow::Decimal128Builder b18(d18);
    arrow::Decimal128Builder b38(d38);
    const arrow::Decimal128 wide =
        arrow::Decimal128::FromString("123456789012345678901234567890").ValueOrDie();
    for (int64_t row = 0; row < rowCount; ++row) {
        if (row % 7 == 0) {
            ASSERT_TRUE(b9.AppendNull().ok());
            ASSERT_TRUE(b15.AppendNull().ok());
            ASSERT_TRUE(b18.AppendNull().ok());
            ASSERT_TRUE(b38.AppendNull().ok());
            continue;
        }
        const int64_t sign = row % 3 == 0 ? -1 : 1;
        ASSERT_TRUE(b9.Append(arrow::Decimal128(sign * (row * 101 + 1))).ok());
        ASSERT_TRUE(b15.Append(arrow::Decimal128(sign * (row * 1000003 + 7))).ok());
        ASSERT_TRUE(b18.Append(arrow::Decimal128(
            sign * (row * 100000000000LL + 3))).ok());
        ASSERT_TRUE(b38.Append(
            (wide + arrow::Decimal128(row)) * arrow::Decimal128(sign)).ok());
    }
    auto schema = arrow::schema({
        arrow::field("d9", d9, true),
        arrow::field("d15", d15, true),
        arrow::field("d18", d18, true),
        arrow::field("d38", d38, true),
    });
    auto batch = arrow::RecordBatch::Make(schema, rowCount, {
        b9.Finish().ValueOrDie(),
        b15.Finish().ValueOrDie(),
        b18.Finish().ValueOrDie(),
        b38.Finish().ValueOrDie(),
    });

    const std::vector<TPhysicalRowId> locators{
        MakeParquetRowId(1, 100),
        MakeParquetRowId(0, 7),
        MakeParquetRowId(0, 3),
        MakeParquetRowId(1, 127),
        MakeParquetRowId(0, 66),
    };
    arrow::Int64Builder rowsBuilder;
    for (const auto locator : locators) {
        ASSERT_TRUE(rowsBuilder.Append(
            static_cast<int64_t>(ParquetRowGroup(locator)) * rowGroupSize +
            ParquetRowOffset(locator)).ok());
    }
    auto rows = rowsBuilder.Finish().ValueOrDie();
    auto expected = arrow::compute::Take(
        arrow::Datum(batch), arrow::Datum(rows)).ValueOrDie().record_batch();

    struct TStorage {
        bool AsInteger;
        std::vector<parquet::Type::type> PhysicalTypes;
        std::vector<int> TypeLengths;
    };
    const std::vector<TStorage> storages{
        {true,
            {parquet::Type::INT32, parquet::Type::INT64, parquet::Type::INT64,
                parquet::Type::FIXED_LEN_BYTE_ARRAY},
            {0, 0, 0, 16}},
        {false,
            {parquet::Type::FIXED_LEN_BYTE_ARRAY,
                parquet::Type::FIXED_LEN_BYTE_ARRAY,
                parquet::Type::FIXED_LEN_BYTE_ARRAY,
                parquet::Type::FIXED_LEN_BYTE_ARRAY},
            {4, 7, 8, 16}},
    };
    for (const auto& storage : storages) {
        const std::string path = storage.AsInteger
            ? "/tmp/test_parquet_row_reader_decimal_int_qdb.parquet"
            : "/tmp/test_parquet_row_reader_decimal_flba_qdb.parquet";
        WriteParquet(path, batch, rowGroupSize, storage.AsInteger);
        auto fileReader = OpenParquet(path);
        const auto* fileSchema =
            fileReader->parquet_reader()->metadata()->schema();
        for (int i = 0; i < 4; ++i) {
            EXPECT_EQ(fileSchema->Column(i)->physical_type(),
                storage.PhysicalTypes[static_cast<size_t>(i)]);
            EXPECT_EQ(fileSchema->Column(i)->type_length(),
                storage.TypeLengths[static_cast<size_t>(i)]);
        }

        const std::vector<std::string> columnNames{"d9", "d15", "d18", "d38"};
        TParquetRowReader reader(fileReader, columnNames);
        EXPECT_TRUE(reader.SupportsPageLookup());
        auto result = reader.ReadRows(locators);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        EXPECT_TRUE((*result)->Equals(*expected))
            << (*result)->ToString() << "\nexpected\n" << expected->ToString();
    }
}

TEST(ParquetRowReaderTest, RejectsNestedFileSchema) {
    const std::string path = "/tmp/test_parquet_row_reader_nested_qdb.parquet";

    arrow::Int64Builder values;
    ASSERT_TRUE(values.AppendValues({10, 20}).ok());
    auto valueArray = values.Finish().ValueOrDie();
    auto valueField = arrow::field("value", arrow::int64(), false);
    auto nested = arrow::StructArray::Make(
        {valueArray}, {valueField}).ValueOrDie();
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("nested", nested->type(), false)}),
        2,
        {nested});
    WriteParquet(path, batch, 2);

    EXPECT_THROW(
        TParquetRowReader(
            OpenParquet(path), std::vector<std::string>{"nested"}),
        std::invalid_argument);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
