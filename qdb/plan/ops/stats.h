#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>

namespace NQdb {

// Per-column statistics for the whole file (not per-row-group). Scalar values
// (Min/Max/Histogram) are stored as raw 8-byte patterns and read back with the
// column's own type via GetMinValue<T>() etc.; only 8-byte T is valid.
struct TStats {
    uint64_t RowCount = 0;
    // Planner cost in abstract work units. It is not wall-clock time; the
    // optimizer uses it to compare alternative plans with the same statistics.
    double Cost = 0.0;

    struct TColumnStats {
        std::optional<uint64_t> MinValue;
        std::optional<uint64_t> MaxValue;
        std::optional<uint64_t> NullCount;
        // Number of Distinct Values (column cardinality) over the whole file.
        // Drives hash-table sizing and join/group-by cardinality estimation.
        // NdvIsExact tells whether it is exact or an estimate (e.g. HyperLogLog).
        std::optional<uint64_t> Ndv;
        bool NdvIsExact = false;
        // Equi-depth histogram: bucket boundaries (quantiles). With N+1 entries there
        // are N buckets, each holding ~RowCount/N rows, so entry i is the value at
        // the i/N quantile (entry 0 = min, last = max). Used to estimate range-
        // predicate selectivity. Empty when unavailable (e.g. string columns).
        std::vector<uint64_t> Histogram;

        template<typename T>
        std::optional<T> GetMinValue() const {
            if (!MinValue) {
                return std::nullopt;
            }
            return std::bit_cast<T>(*MinValue);
        }

        template<typename T>
        std::optional<T> GetMaxValue() const {
            if (!MaxValue) {
                return std::nullopt;
            }
            return std::bit_cast<T>(*MaxValue);
        }

        template<typename T>
        std::optional<T> GetHistogramValue(size_t idx) const {
            if (idx >= Histogram.size()) {
                return std::nullopt;
            }
            return std::bit_cast<T>(Histogram[idx]);
        }

        // Fraction of rows with value <= c, or < c when strict. The two differ
        // only for a heavy value, which repeats as a boundary.
        template<typename T>
        std::optional<double> FractionBelow(T c, bool strict = false) const {
            if (Histogram.empty()) {
                return std::nullopt;
            }
            auto val = [&](uint64_t v) {
                return std::bit_cast<T>(v);
            };
            if (strict ? c <= val(Histogram.front()) : c < val(Histogram.front())) {
                return 0.0;
            }
            if (strict ? c > val(Histogram.back()) : c >= val(Histogram.back())) {
                return 1.0;
            }

            auto it = strict
                ? std::lower_bound(Histogram.begin(), Histogram.end(), c, [&](uint64_t a, T b) {
                    return val(a) < b;
                })
                : std::upper_bound(Histogram.begin(), Histogram.end(), c, [&](T a, uint64_t b) {
                    return a < val(b);
                });

            size_t belowCount = std::distance(Histogram.begin(), it); // # < c
            size_t bucketIdx = belowCount - 1; // bucket containing c
            T lo = val(Histogram[bucketIdx]);
            T hi = val(Histogram[bucketIdx + 1]);
            double frac = 0.0;
            if (hi != lo) {
                frac = (double)(c - lo) / (double)(hi - lo);
            }
            return (bucketIdx + frac) / (double)(Histogram.size() - 1);
        }
    };

    using TColumnStatsPtr = std::shared_ptr<TColumnStats>;
    std::map<std::string, TColumnStatsPtr> ColumnStats;
};

using TStatsPtr = std::shared_ptr<TStats>;

}
