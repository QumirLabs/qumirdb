#pragma once

#include <qdb/io/schema.h>

#include <qumir/parser/ast.h>

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace NQdb {

// Raw scalar bounds for one source column in one Parquet row group. The low 64
// bits of MinValue and MaxValue contain the native i64/u64/f64 bit pattern
// selected by the source schema; a DECIMAL column holds its unscaled i128. The
// remaining flags deliberately describe possibilities, rather than facts, so
// missing metadata stays conservative.
struct TRowGroupColumnRange {
    unsigned __int128 MinValue = 0;
    unsigned __int128 MaxValue = 0;
    bool HasBounds = false;
    bool MayBeNull = true;
    bool MayBeValue = true;
    bool MayBeNaN = false;
};

// Compiles a typed source predicate into a Qumir VM function over interval
// arguments. Unsupported AST fragments become an unknown truth value. The
// compiled function is reusable for every row group of the source.
class TRowGroupPredicateEvaluator {
public:
    static std::unique_ptr<TRowGroupPredicateEvaluator> Compile(
        const NQumir::NAst::TExprPtr& predicate,
        const TSchema& schema,
        std::string_view sourceAlias,
        std::ostream* diagnostics = nullptr);

    ~TRowGroupPredicateEvaluator();

    TRowGroupPredicateEvaluator(const TRowGroupPredicateEvaluator&) = delete;
    TRowGroupPredicateEvaluator& operator=(const TRowGroupPredicateEvaluator&) = delete;

    // False is returned only when TRUE is absent from the VM's over-approximate
    // set of possible SQL results. Any VM/runtime failure returns true.
    bool MayBeTrue(std::span<const TRowGroupColumnRange> columns) const noexcept;

private:
    struct TImpl;
    explicit TRowGroupPredicateEvaluator(std::unique_ptr<TImpl> impl);

    std::unique_ptr<TImpl> Impl_;
};

} // namespace NQdb
