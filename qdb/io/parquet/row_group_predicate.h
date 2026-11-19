#pragma once

#include <qdb/io/schema.h>

#include <qumir/parser/ast.h>

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

// A source predicate lowered to oz-lang over the intervals of the columns it
// reads: the form a scan node carries to the process that runs the scan.
struct TRowGroupPredicateProgram {
    // (block (return <truth>)) over the interval parameters of Columns.
    std::string Body;
    // Schema columns the body reads, ascending.
    std::vector<int32_t> Columns;

    bool operator==(const TRowGroupPredicateProgram&) const = default;

    void Fields(this auto& self, auto& v)
    {
        v(self.Body);
        v(self.Columns);
    }
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

    // nullopt when the predicate bounds no column, so no rows can be ruled out
    // by their values.
    static std::optional<TRowGroupPredicateProgram> Program(
        const NQumir::NAst::TExprPtr& predicate,
        const TSchema& schema,
        std::string_view sourceAlias);

    static std::unique_ptr<TRowGroupPredicateEvaluator> Compile(
        const TRowGroupPredicateProgram& program,
        const TSchema& schema,
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
