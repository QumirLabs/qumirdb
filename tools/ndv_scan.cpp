#include <arrow/api.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

uint64_t Mix(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

uint64_t HashBytes(std::string_view value) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : value) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return Mix(hash);
}

class THyperLogLog {
public:
    static constexpr int Precision = 18;
    static constexpr size_t RegisterCount = size_t{1} << Precision;

    THyperLogLog() : Registers_(RegisterCount, 0) {}

    void Add(uint64_t hash) {
        const auto index = static_cast<size_t>(hash >> (64 - Precision));
        const auto remaining = (hash << Precision) | (uint64_t{1} << (Precision - 1));
        const auto rank = static_cast<uint8_t>(std::countl_zero(remaining) + 1);
        Registers_[index] = std::max(Registers_[index], rank);
        ++NonNullRows_;
    }

    uint64_t Estimate() const {
        double inverseSum = 0.0;
        size_t zeroRegisters = 0;
        for (uint8_t rank : Registers_) {
            inverseSum += std::ldexp(1.0, -rank);
            zeroRegisters += rank == 0;
        }
        const double m = static_cast<double>(RegisterCount);
        const double alpha = 0.7213 / (1.0 + 1.079 / m);
        double estimate = alpha * m * m / inverseSum;
        if (estimate <= 2.5 * m && zeroRegisters) {
            estimate = m * std::log(m / zeroRegisters);
        }
        return std::min<uint64_t>(std::llround(estimate), NonNullRows_);
    }

    uint64_t NonNullRows() const { return NonNullRows_; }

private:
    std::vector<uint8_t> Registers_;
    uint64_t NonNullRows_ = 0;
};

void AddArray(const arrow::Array& array, THyperLogLog& hll) {
    const bool hasNulls = array.null_count() != 0;
    switch (array.type_id()) {
    case arrow::Type::INT32: {
        const auto& values = static_cast<const arrow::Int32Array&>(array);
        for (int64_t i = 0; i < array.length(); ++i) {
            if (!hasNulls || !array.IsNull(i)) {
                hll.Add(Mix(static_cast<uint64_t>(values.Value(i))));
            }
        }
        return;
    }
    case arrow::Type::INT64: {
        const auto& values = static_cast<const arrow::Int64Array&>(array);
        for (int64_t i = 0; i < array.length(); ++i) {
            if (!hasNulls || !array.IsNull(i)) {
                hll.Add(Mix(static_cast<uint64_t>(values.Value(i))));
            }
        }
        return;
    }
    case arrow::Type::DOUBLE: {
        const auto& values = static_cast<const arrow::DoubleArray&>(array);
        for (int64_t i = 0; i < array.length(); ++i) {
            if (!hasNulls || !array.IsNull(i)) {
                const double value = values.Value(i);
                const double canonical = value == 0.0 ? 0.0 : value;
                hll.Add(Mix(std::bit_cast<uint64_t>(canonical)));
            }
        }
        return;
    }
    case arrow::Type::STRING: {
        const auto& values = static_cast<const arrow::StringArray&>(array);
        for (int64_t i = 0; i < array.length(); ++i) {
            if (!hasNulls || !array.IsNull(i)) {
                hll.Add(HashBytes(values.GetView(i)));
            }
        }
        return;
    }
    case arrow::Type::LARGE_STRING: {
        const auto& values = static_cast<const arrow::LargeStringArray&>(array);
        for (int64_t i = 0; i < array.length(); ++i) {
            if (!hasNulls || !array.IsNull(i)) {
                hll.Add(HashBytes(values.GetView(i)));
            }
        }
        return;
    }
    default:
        throw std::runtime_error("unsupported NDV column type: "
            + array.type()->ToString());
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::cerr << "usage: ndv_scan file.parquet column [column ...]\n";
            return 2;
        }
        parquet::arrow::FileReaderBuilder builder;
        auto status = builder.OpenFile(argv[1]);
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
        parquet::ArrowReaderProperties properties;
        properties.set_batch_size(65'536);
        properties.set_use_threads(true);
        builder.properties(properties);
        auto result = builder.Build();
        if (!result.ok()) {
            throw std::runtime_error(result.status().ToString());
        }
        auto reader = std::move(*result);
        std::shared_ptr<arrow::Schema> schema;
        status = reader->GetSchema(&schema);
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }

        std::vector<int> columns;
        std::vector<std::string> names;
        for (int i = 2; i < argc; ++i) {
            const int index = schema->GetFieldIndex(argv[i]);
            if (index < 0) {
                throw std::runtime_error("column not found: " + std::string(argv[i]));
            }
            columns.push_back(index);
            names.emplace_back(argv[i]);
        }
        std::vector<int> rowGroups(reader->num_row_groups());
        std::iota(rowGroups.begin(), rowGroups.end(), 0);
        auto batchResult = reader->GetRecordBatchReader(rowGroups, columns);
        if (!batchResult.ok()) {
            throw std::runtime_error(batchResult.status().ToString());
        }
        auto batches = std::move(*batchResult);
        std::vector<THyperLogLog> sketches(names.size());
        std::shared_ptr<arrow::RecordBatch> batch;
        while (true) {
            status = batches->ReadNext(&batch);
            if (!status.ok()) {
                throw std::runtime_error(status.ToString());
            }
            if (!batch) {
                break;
            }
            for (size_t i = 0; i < sketches.size(); ++i) {
                AddArray(*batch->column(static_cast<int>(i)), sketches[i]);
            }
        }
        for (size_t i = 0; i < sketches.size(); ++i) {
            std::cout << names[i] << '\t' << sketches[i].Estimate() << '\t'
                << sketches[i].NonNullRows() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
