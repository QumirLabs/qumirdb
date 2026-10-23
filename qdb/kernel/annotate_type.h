#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <memory>
#include <string>
#include <vector>

namespace NQdb {
class TExternalCatalogSnapshot;
}

namespace NQdb::NKernel {

class TExternalTypingResolver {
public:
    explicit TExternalTypingResolver(
        std::shared_ptr<const TExternalCatalogSnapshot> catalog);
    ~TExternalTypingResolver();

    NQumir::NAst::TTypePtr ResolveCall(
        const std::string& name,
        const std::vector<NQumir::NAst::TTypePtr>& argTypes);

private:
    struct TImpl;
    std::shared_ptr<TImpl> Impl_;
};

struct TAnnotationContext {
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog;
    // Filled lazily on the first external annotation and then reused by all
    // repeated AnnotateTypes passes for this context.
    mutable std::shared_ptr<TExternalTypingResolver> Resolver;
};

// Lightweight SQL type inference for a Project/filter/aggregate expression over
// `inputType`. Operators use local promotion rules; catalog calls use the reusable
// resolver in TAnnotationContext. No AST rewrite is performed, and NULL propagates as
// if ExpandNullable had already run. The logical AST therefore stays clean for passes
// such as equi-join extraction. Throws on a null expression.
NQumir::NAst::TTypePtr AnnotateExprType(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

// Annotates every node in an expression tree with the same rule-based type
// inference used by kernels. Integer literals adopt the adjacent integer
// operand type when their value fits it, matching Qumir overload resolution. A
// node that cannot be typed, such as a call to a function outside the catalog,
// keeps its type.
NQumir::NAst::TTypePtr AnnotateExprTreeTypes(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TAnnotationContext* context = nullptr);

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
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TExternalCatalogSnapshot* externalCatalog = nullptr);

// Final expression rewrite for kernel compilation. Applies the existing nullable
// normalization and then qdb-only decimal erasure (Decimal -> qumirdb.oz BinInt).
std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TTypePtr> ExpandKernelExpr(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TExternalCatalogSnapshot* externalCatalog = nullptr);

// ExpandKernelExpr for a filter or join residual predicate. A predicate is a boolean
// context: a bare NULL predicate (`WHERE NULL`) becomes NULL[bool], which selects no rows.
std::pair<NQumir::NAst::TExprPtr, NQumir::NAst::TTypePtr> ExpandKernelPredicate(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType,
    const TExternalCatalogSnapshot* externalCatalog = nullptr);

} // namespace NQdb::NKernel
