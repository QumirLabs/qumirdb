#pragma once

#include <qdb/plan/types/decimal.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NQdb {
class TExternalCatalogSnapshot;
}

namespace NQdb::NKernel {

// Planner return types of qumirdb.oz functions and of external catalog functions.
// Nothing is built until the first lookup, which composes qumirdb.oz (with the
// catalog's declarations) in `types`, the query's factory.
class TExternalTypingResolver {
public:
    TExternalTypingResolver(
        NQumir::NAst::TTypeFactory& types,
        std::shared_ptr<const TExternalCatalogSnapshot> catalog);
    ~TExternalTypingResolver();

    NQumir::NAst::TTypePtr ResolveCall(
        const std::string& name,
        const std::vector<NQumir::NAst::TTypePtr>& argTypes);

    // The return type of a qumirdb.oz function, or null for an unknown name.
    NQumir::NAst::TTypePtr QumirdbReturnType(const std::string& name);

private:
    struct TImpl;
    std::shared_ptr<TImpl> Impl_;
};

// The typing state of one query. Copies share the resolver, so a query composes
// qumirdb.oz for typing at most once.
struct TAnnotationContext {
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog;
    std::shared_ptr<TExternalTypingResolver> Resolver;
};

TAnnotationContext MakeAnnotationContext(
    NQumir::NAst::TTypeFactory& types,
    std::shared_ptr<const TExternalCatalogSnapshot> catalog);

// Kernels read a DATE as its storage integer, as qumirdb.oz declares DATE columns, so a
// plan AST is erased before Qumir compiles it: every kernel and planning-time function.
NQumir::NAst::TTypePtr EraseDateType(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TTypePtr& type);
void EraseDateTypes(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TExprPtr& ast);

// Lightweight SQL type inference for a Project/filter/aggregate expression over
// `inputType`. Operators use local promotion rules; catalog calls use the reusable
// resolver in TAnnotationContext. No AST rewrite is performed, and NULL propagates as
// if ExpandNullable had already run. The logical AST therefore stays clean for passes
// such as equi-join extraction. An untyped NULL value (`SELECT NULL AS x`) is
// NULL[INTEGER], as in ExpandKernelValue. Throws on a null expression.
NQumir::NAst::TTypePtr AnnotateExprType(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

// Annotates every node in an expression tree with the same rule-based type
// inference used by kernels. Integer literals adopt the adjacent integer
// operand type when their value fits it, matching Qumir overload resolution. A
// node that cannot be typed, such as a call to a function outside the catalog,
// keeps its type.
NQumir::NAst::TTypePtr AnnotateExprTreeTypes(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

// A fractional literal beside a DECIMAL is a decimal of its own digits, as in DuckDB
// (0.125 is DECIMAL(3,3)), instead of being rounded to the other operand's scale. The
// lexeme is gone by now, so the digits are the shortest decimal form that round-trips
// the double. Literals needing more than 18 digits stay doubles (nullopt). A unary
// minus around the literal is part of it.
struct TDecimalLiteral {
    int64_t Unscaled;
    TDecimalSpec Spec;
};

std::optional<TDecimalLiteral> DecimalLiteralOf(const NQumir::NAst::TExprPtr& e);

// Effective common type used by a numeric binary operator/comparison after
// AnnotateExprTreeTypes. Returns nullptr for a non-numeric or incompatible pair.
NQumir::NAst::TTypePtr EffectiveBinaryNumericType(
    const NQumir::NAst::TExprPtr& left,
    const NQumir::NAst::TExprPtr& right);

// Heavy rewrite, run once per kernel just before compilation (never during logical
// planning). Rewrites null-strict ops/calls/casts, AND/OR (SQL 3VL) and CASE/`if`
// branches so nullability is explicit in the AST (guards on `.Valid`, results wrapped
// Nullable[T]); the non-nullable path is untouched. Operates on a clone - the plan's
// shared expression is left intact. Returns the rewritten expression and its planner type.
std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TTypePtr> ExpandNullable(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

// Whether ExpandNullable makes a call NULL whenever its argument `index` is NULL.
// Calls with their own NULL semantics (`qdb_is_null`, `coalesce`, ...) are not
// strict; an IN list is NULL for a NULL operand, but a NULL item does not decide it.
bool IsNullStrictCallArgument(const std::string& name, size_t index);

// Final expression rewrite for kernel compilation. Applies the existing nullable
// normalization and then qdb-only decimal erasure (Decimal -> qumirdb.oz BinInt).
std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TTypePtr> ExpandKernelExpr(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

// Whether `expr` is an untyped NULL value (`NULL`, `CASE WHEN c THEN NULL END`): AnnotateExprType
// types it NULL[INTEGER] only because nothing else gives it a type. A UNION takes the type of
// such a column from its other branches.
bool IsUntypedNullValue(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

// ExpandKernelExpr for a filter or join residual predicate. A predicate is a boolean
// context: an untyped NULL predicate (`WHERE NULL`) becomes NULL[bool], which selects no rows.
std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TTypePtr> ExpandKernelPredicate(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

// ExpandKernelExpr for a projected column or an aggregate argument. Nothing there types
// an untyped NULL (`SELECT NULL AS x`), so it becomes NULL[INTEGER], the type
// AnnotateExprType gives the column.
std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TTypePtr> ExpandKernelValue(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

} // namespace NQdb::NKernel
