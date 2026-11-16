#include <gtest/gtest.h>

#include <numeric>
#include <algorithm>
#include <unordered_map>

namespace {

using TColumn = std::vector<int32_t>;

struct TRowSet {
    std::vector<TColumn> Columns;
    int RowCount;
};

int RandomInt(int* seed) {
    *seed = (static_cast<uint32_t>(*seed) * 1103515245u + 12345u) & 0x7fffffff;
    return *seed;
}

void GenerateColumn(uint32_t* column, int n, int* seed) {
    for (int i = 0; i < n; i++) {
        column[i] = RandomInt(seed) % 15;
    }
}

void GenerateRowSet(TRowSet& rowSet, int rowCount, int colCount, int* seed) {
    rowSet.RowCount = rowCount;
    rowSet.Columns.resize(colCount);
    for (int col = 0; col < colCount; ++col) {
        rowSet.Columns[col].resize(rowCount);
        GenerateColumn(reinterpret_cast<uint32_t*>(rowSet.Columns[col].data()), rowCount, seed);
    }
}

struct TWindowSpec {
    int Column; // column name to apply window function on
    std::optional<int> PartitionBy; // column name to partition by
    std::optional<int> OrderBy; // column name to order by
    std::optional<int64_t> FrameStart; // nullopt => unbounded preceding
    std::optional<int64_t> FrameEnd; // nullopt => unbounded following
};

std::vector<int32_t> ApplyWindowSumFunc(TRowSet& rowSet, int col, int from, int to, const std::vector<int>& indices) {
    int left = 0;
    int right = 0;
    int n = indices.size();
    int32_t sum = 0;
    std::vector<int32_t> result;
    result.resize(n);
    for (int i = 0; i < n; ++i) {
        int frameLeft = std::clamp(i + from, 0, n);
        int frameRight = std::clamp(i + to + 1, 0, n);
        while (left < frameLeft) {
            sum -= rowSet.Columns[col][indices[left]];
            left ++;
        }
        while (right < frameRight) {
            sum += rowSet.Columns[col][indices[right]];
            right ++;
        }

        // result: rowSet.Columns[{cols}][indices[i]] and sum
        result[i] = sum;
    }
    return result;
}

} // namespace

TEST(Window, Basic) {
    int seed = 42;
    int rowCount = 100;
    int colCount = 3;
    TRowSet rowSet;
    GenerateRowSet(rowSet, rowCount, colCount, &seed);
    TWindowSpec spec;
    spec.FrameStart = -5;
    spec.FrameEnd = 5;
    spec.Column = 0;

    std::vector<int> indices(rowCount);
    std::iota(indices.begin(), indices.end(), 0);

    auto result = ApplyWindowSumFunc(rowSet, spec.Column, *spec.FrameStart, *spec.FrameEnd, indices);

    for (int i = 0; i < rowCount; ++i) {
        for (int j = 0; j < colCount; ++j) {
            std::cerr << rowSet.Columns[j][i] << ", ";
        }
        std::cerr << result[i];
        std::cerr << "\n";
    }
}

TEST(Window, Sorted) {
    int seed = 42;
    int rowCount = 100;
    int colCount = 3;
    TRowSet rowSet;
    GenerateRowSet(rowSet, rowCount, colCount, &seed);
    TWindowSpec spec;

    spec.OrderBy = 1;
    spec.FrameStart = -5;
    spec.FrameEnd = 5;
    spec.Column = 0;

    std::vector<int> indices(rowCount);
    std::iota(indices.begin(), indices.end(), 0);

    std::stable_sort(indices.begin(), indices.end(), [&](auto i, auto j) {
        return rowSet.Columns[*spec.OrderBy][i] < rowSet.Columns[*spec.OrderBy][j];
    });

    auto result = ApplyWindowSumFunc(rowSet, spec.Column, *spec.FrameStart, *spec.FrameEnd, indices);

    for (int i = 0; i < rowCount; ++i) {
        for (int j = 0; j < colCount; ++j) {
            std::cerr << rowSet.Columns[j][indices[i]] << ", ";
        }
        std::cerr << result[indices[i]];
        std::cerr << "\n";
    }
}

TEST(Window, Partitioned) {
    int seed = 42;
    int rowCount = 100;
    int colCount = 3;
    TRowSet rowSet;
    GenerateRowSet(rowSet, rowCount, colCount, &seed);
    TWindowSpec spec;

    spec.PartitionBy = 1;
    spec.FrameStart = -5;
    spec.FrameEnd = 5;
    spec.Column = 0;

    std::unordered_map<int32_t, std::vector<int>> partitions;
    for (int i = 0; i < rowCount; ++i) {
        auto value = rowSet.Columns[*spec.PartitionBy][i];
        partitions[value].push_back(i);
    }

    for (auto& [_, indices] : partitions) {
        auto result = ApplyWindowSumFunc(rowSet, spec.Column, *spec.FrameStart, *spec.FrameEnd, indices);

        int n = indices.size();
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < colCount; ++j) {
                std::cerr << rowSet.Columns[j][indices[i]] << ", ";
            }
            std::cerr << result[i];
            std::cerr << "\n";
        }
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
