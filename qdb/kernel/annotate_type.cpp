#include <qdb/kernel/annotate_type.h>

#include <qdb/catalog/external_module.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/utils/module_path.h>

#include <qumir/error.h>
#include <qumir/parser/core/printer.h>
#include <qumir/frontend/source_module_loader.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NQdb::NKernel {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

thread_local const TExternalCatalogSnapshot* CurrentExternalCatalog = nullptr;
thread_local const TAnnotationContext* CurrentAnnotationContext = nullptr;

class TExternalCatalogScope {
public:
    explicit TExternalCatalogScope(const TExternalCatalogSnapshot* catalog)
        : Previous_(CurrentExternalCatalog)
    {
        CurrentExternalCatalog = catalog;
    }

    ~TExternalCatalogScope() {
        CurrentExternalCatalog = Previous_;
    }

private:
    const TExternalCatalogSnapshot* Previous_;
};

class TAnnotationScope {
public:
    explicit TAnnotationScope(const TAnnotationContext* context)
        : Previous_(CurrentAnnotationContext)
    {
        CurrentAnnotationContext = context;
    }

    ~TAnnotationScope() {
        CurrentAnnotationContext = Previous_;
    }

private:
    const TAnnotationContext* Previous_;
};

TTypePtr ExternalReturnType(
    const std::string& name,
    const std::vector<TTypePtr>& argTypes);

// TError carries its text in Location/Children, not Msg, so what() is empty for the
// generic std::exception catch. Re-throw with the full ToString() flattened into Msg
// so both what() and ToString() are informative wherever the error is caught.
[[noreturn]] void ThrowTyping(std::string_view stage, const NQumir::TError& cause) {
    throw NQumir::TError(
        "project type inference [" + std::string(stage) + "]: " + cause.ToString());
}

[[noreturn]] void ThrowTyping(std::string_view stage, const std::string& cause) {
    throw NQumir::TError(
        "project type inference [" + std::string(stage) + "]: " + cause);
}

// Planner column type -> the qumir type the annotator should see. String columns are
// StringView in kernels; nullable columns use qumirdb.oz's Nullable[T] alias so
// operator result types come from the same source-module overloads kernels use.
TTypePtr ToQumirType(const TTypePtr& type) {
    if (!type) {
        return type;
    }
    if (IsNullableType(type)) {
        return std::make_shared<TNamedType>(
            "Nullable",
            nullptr,
            std::vector<TGenericArg>{
                TGenericArg::TypeArg(ToQumirType(UnwrapNullableType(type)))});
    }
    if (IsDecimalType(type)) {
        return DecimalStorageType();
    }
    if (TMaybeType<TStringType>(type)) {
        return std::make_shared<TNamedType>("StringView", nullptr);
    }
    return type;
}

// Annotator result -> planner type. Map qumirdb.oz source-module aliases back to
// planner types.
TTypePtr FromQumirType(const TTypePtr& type) {
    TTypePtr t = type;
    if (IsDecimalType(t)) {
        return t;
    }
    if (auto named = TMaybeType<TNamedType>(t);
        named && named.Cast()->Name == "Nullable" && !named.Cast()->TypeArgs.empty())
    {
        return std::make_shared<TNullable>(
            FromQumirType(named.Cast()->TypeArgs.front().Type));
    }
    if (auto named = TMaybeType<TNamedType>(t); named && named.Cast()->Name == "StringView") {
        return std::make_shared<TStringType>();
    }
    // DATE/TIMESTAMP are erased to i32 at the schema boundary (parquet DATE32 -> i32);
    // a `cast(... as date)` target carries no underlying type, so map it explicitly.
    if (auto named = TMaybeType<TNamedType>(t);
        named && (named.Cast()->Name == "DATE" || named.Cast()->Name == "TIMESTAMP"))
    {
        return std::make_shared<TIntegerType>(TIntegerType::I32);
    }
    return UnwrapNamedType(UnwrapNullableType(t));
}

// Finds the __result__ var in the annotated AST and returns its inferred type. The
// pipeline may rewrite/replace nodes, so we locate it by name rather than keeping a
// pointer to the pre-annotation node.
TTypePtr FindResultType(const TExprPtr& node, const std::string& resultName) {
    if (!node) {
        return nullptr;
    }
    if (auto var = TMaybeNode<TVarStmt>(node); var && var.Cast()->Name == resultName) {
        if (var.Cast()->Type) {
            return var.Cast()->Type;
        }
        if (var.Cast()->Init && var.Cast()->Init->Type) {
            return var.Cast()->Init->Type;
        }
    }
    for (auto* child : node->MutableChildren()) {
        if (auto found = FindResultType(*child, resultName)) {
            return found;
        }
    }
    return nullptr;
}

// Runs the real qumir annotator on a throwaway function whose params are the input
// columns, so external-function and operator return types come from qumirdb.oz.
class TTypingContext {
public:
    explicit TTypingContext(
        std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog = nullptr)
        : ExternalCatalog_(std::move(externalCatalog))
    {
        if (ExternalCatalog_) {
            OwnedFrontend_ = std::make_unique<TVmFrontendContext>(
                std::vector<std::string>{
                    NUtils::ModuleFile("qumirdb.oz"),
                });
            Frontend_ = OwnedFrontend_.get();
        } else {
            Frontend_ = &QumirdbVmContext();
        }
        if (ExternalCatalog_) {
            ExternalModuleNames_ = ExternalCatalog_->ModuleNames();
            if (auto error = Frontend_->RegisterExternalDeclarations(
                    *ExternalCatalog_))
            {
                LoadError_ = std::move(*error);
            }
        }
    }

    // Planner return type of an extern function declared in qumirdb.oz (e.g. qdb_substring
    // -> string), or nullptr if unknown. Built once from the resolver's external-function
    // set - a single qumirdb.oz compose, not a per-node one.
    TTypePtr ExternReturnType(const std::string& name) {
        EnsureExterns();
        auto it = ExternReturns_.find(name);
        return it == ExternReturns_.end() ? nullptr : it->second;
    }

    TTypePtr Annotate(const TExprPtr& expr, const TStructType& inputType) {
        if (LoadError_) {
            ThrowTyping("load qumirdb.oz", *LoadError_);
        }
        NQumir::TLocation loc{};

        std::vector<TParam> params;
        params.reserve(inputType.Fields.size());
        for (const auto& [name, type] : inputType.Fields) {
            params.push_back(std::make_shared<TVarStmt>(loc, name, ToQumirType(type)));
        }

        // Annotation writes ->Type in place, so type a clone, not the plan's node.
        auto cloned = CloneExpr(expr);
        // Unique names also make diagnostics and any resolver-owned generated
        // symbols unambiguous across independent annotation calls.
        const std::string functionName =
            Frontend_->UniqueName("__annotate_");
        const std::string resultName = "__result_" + functionName;
        auto resultVar = std::make_shared<TVarStmt>(loc, resultName, nullptr);
        resultVar->Init = cloned;
        auto body = std::make_shared<TBlockExpr>(
            loc, std::vector<TExprPtr>{resultVar});
        auto fn = std::make_shared<TFunDecl>(
            loc, functionName, std::vector<TGenericParam>{},
            std::move(params), body, std::make_shared<TVoidType>());
        std::vector<TExprPtr> statements{
            std::make_shared<TUseExpr>(loc, "qumirdb")};
        for (const auto& moduleName : ExternalModuleNames_) {
            statements.push_back(std::make_shared<TUseExpr>(loc, moduleName));
        }
        statements.push_back(fn);
        TExprPtr module = std::make_shared<TBlockExpr>(loc, std::move(statements));

        auto transformed = Frontend_->Transform(std::move(module));
        if (!transformed) {
            ThrowTyping("annotate", transformed.error());
        }
        auto type = FindResultType(*transformed, resultName);
        if (!type) {
            throw NQumir::TError(
                "project type inference: annotator produced no type for '" +
                NQumir::NAst::NCore::PrintAst(expr) + "'");
        }
        return type;
    }

    TTypePtr ResolveCall(
        const std::string& name,
        const std::vector<TTypePtr>& argTypes)
    {
        NQumir::TLocation loc{};
        std::vector<std::pair<std::string, TTypePtr>> fields;
        std::vector<TExprPtr> args;
        fields.reserve(argTypes.size());
        args.reserve(argTypes.size());
        for (size_t i = 0; i < argTypes.size(); ++i) {
            const std::string argName = "__external_arg_" + std::to_string(i);
            fields.emplace_back(argName, UnwrapNullableType(argTypes[i]));
            args.push_back(std::make_shared<TIdentExpr>(loc, argName));
        }
        auto call = std::make_shared<TCallExpr>(
            loc, std::make_shared<TIdentExpr>(loc, name), std::move(args));
        return FromQumirType(Annotate(call, TStructType(std::move(fields))));
    }

private:
    // Compose qumirdb.oz once and snapshot every function's (planner) return type by
    // name. Extern decls carry an empty `(block)` body (not nullptr), so we walk the
    // composed AST rather than TNameResolver::GetExternalFunctions (which filters on a
    // null body). We only ever look names up, so collecting non-extern fns too is benign.
    void EnsureExterns() {
        if (ExternsBuilt_ || LoadError_) {
            return;
        }
        ExternsBuilt_ = true;
        NQumir::TLocation loc{};
        TExprPtr module = std::make_shared<TBlockExpr>(loc,
            std::vector<TExprPtr>{std::make_shared<TUseExpr>(loc, "qumirdb")});
        auto composed = Frontend_->Compose(std::move(module));
        if (!composed) {
            return;
        }
        CollectReturns(*composed);
    }

    void CollectReturns(const TExprPtr& node) {
        if (!node) {
            return;
        }
        if (auto fn = TMaybeNode<TFunDecl>(node);
            fn && fn.Cast()->RetType && !fn.Cast()->Name.empty())
        {
            ExternReturns_.emplace(fn.Cast()->Name, FromQumirType(fn.Cast()->RetType));
        }
        for (auto* child : node->MutableChildren()) {
            CollectReturns(*child);
        }
    }

    std::unique_ptr<TVmFrontendContext> OwnedFrontend_;
    TVmFrontendContext* Frontend_ = nullptr;
    std::optional<std::string> LoadError_;
    bool ExternsBuilt_ = false;
    std::unordered_map<std::string, TTypePtr> ExternReturns_;
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog_;
    std::vector<std::string> ExternalModuleNames_;
};

TTypingContext& Context() {
    static TTypingContext context;
    return context;
}

} // namespace

struct TExternalTypingResolver::TImpl {
    explicit TImpl(std::shared_ptr<const TExternalCatalogSnapshot> catalog)
        : ExternalCatalog(std::move(catalog))
        , Context(ExternalCatalog)
    {
    }

    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog;
    TTypingContext Context;
};

TExternalTypingResolver::TExternalTypingResolver(
    std::shared_ptr<const TExternalCatalogSnapshot> catalog)
    : Impl_(std::make_shared<TImpl>(std::move(catalog)))
{
}

TExternalTypingResolver::~TExternalTypingResolver() = default;

TTypePtr TExternalTypingResolver::ResolveCall(
    const std::string& name,
    const std::vector<TTypePtr>& argTypes)
{
    auto returnType = Impl_->Context.ResolveCall(name, argTypes);
    if (Impl_->ExternalCatalog) {
        Impl_->ExternalCatalog->RememberResolvedReturnType(
            name, argTypes, returnType);
    }
    return returnType;
}

namespace {

TTypePtr ExternalReturnType(
    const std::string& name,
    const std::vector<TTypePtr>& argTypes)
{
    if (CurrentAnnotationContext && CurrentAnnotationContext->ExternalCatalog
        && CurrentAnnotationContext->ExternalCatalog->HasFunction(name))
    {
        if (!CurrentAnnotationContext->Resolver) {
            CurrentAnnotationContext->Resolver =
                std::make_shared<TExternalTypingResolver>(
                    CurrentAnnotationContext->ExternalCatalog);
        }
        return CurrentAnnotationContext->Resolver->ResolveCall(name, argTypes);
    }
    if (!CurrentExternalCatalog) {
        return nullptr;
    }
    auto resolved = CurrentExternalCatalog->ResolveReturnType(name, argTypes);
    if (!resolved) {
        throw resolved.error();
    }
    return *resolved ? **resolved : nullptr;
}

// A call is typed by a compiled-module function from the external catalog or by a
// qumirdb.oz function. Any other name is an unknown function: left untyped, the call
// would read as a bare NULL and turn the expression around it into NULL.
TTypePtr CallReturnType(
    const TCallExpr& call,
    const std::string& name,
    const std::vector<TTypePtr>& argTypes)
{
    if (auto type = ExternalReturnType(name, argTypes)) {
        return type;
    }
    if (auto type = Context().ExternReturnType(name)) {
        return type;
    }
    throw NQumir::TError(call.Location, "unknown function: " + name);
}

using NQumir::TLocation;

// --- small AST builders ---------------------------------------------------------
TExprPtr Ident(const std::string& n, TLocation loc) {
    return std::make_shared<TIdentExpr>(loc, n);
}
// Access `.Value`/`.Valid` of a (freshly cloned) nullable operand. Cloning keeps the
// AST a tree - the operand is referenced from both the guard and the result, so a shared
// node would be annotated/lowered twice.
TExprPtr FieldOf(const TExprPtr& obj, const char* field, TLocation loc) {
    return std::make_shared<TFieldAccessExpr>(loc, CloneExpr(obj), field);
}
TExprPtr BoolLit(bool v, TLocation loc) {
    return std::make_shared<TNumberExpr>(loc, v);
}
TExprPtr Bin(const char* op, TExprPtr a, TExprPtr b, TLocation loc) {
    return std::make_shared<TBinaryExpr>(loc, TOperator(std::string_view(op)),
        std::move(a), std::move(b));
}
TExprPtr Not(TExprPtr a, TLocation loc) {
    return std::make_shared<TUnaryExpr>(loc, TOperator(std::string_view("!")), std::move(a));
}
TExprPtr MakeNull(TLocation loc) {
    return std::make_shared<TCallExpr>(loc, Ident("qdb_sql_null", loc), std::vector<TExprPtr>{});
}

// A bare SQL NULL desugars to `qdb_sql_null()`; a missing CASE ELSE is a null child.
bool IsNullLiteral(const TExprPtr& e) {
    if (!e) {
        return true;
    }
    if (auto call = TMaybeNode<TCallExpr>(e)) {
        if (auto id = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
            return id.Cast()->Name == "qdb_sql_null";
        }
    }
    return false;
}

// Calls whose result is NOT null-strict (never wrapped by the pass): they return a
// non-null bool or have bespoke NULL semantics handled in qumirdb.oz / elsewhere.
bool IsExcludedCall(const std::string& name) {
    return name == "qdb_is_null" || name == "qdb_is_true" ||
           name == "coalesce" || name == "nullif" || name == "nvl" || name == "ifnull";
}

bool IsComparison(TOperator op) {
    return op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=";
}
bool IsLogical(TOperator op) { return op == "&&" || op == "||"; }

TTypePtr ValueType(const TTypePtr& t) { return t ? UnwrapNullableType(t) : t; }
TTypePtr NullableQ(const TTypePtr& value) {
    auto nullable = std::make_shared<TNullable>(value);
    return IsDecimalType(value)
        ? std::static_pointer_cast<TType>(nullable)
        : ToQumirType(nullable);
}

// A predicate is a boolean context, so its bare NULL is NULL[bool] (`WHERE NULL`,
// `CASE WHEN NULL`): the kernel then tests it like any other nullable predicate.
TExprPtr TypeNullPredicate(const TExprPtr& predicate) {
    if (!IsNullLiteral(predicate)) {
        return predicate;
    }
    return std::make_shared<TCastExpr>(predicate->Location, predicate,
        NullableQ(std::make_shared<TBoolType>()));
}

std::string PlanTypeKey(const TTypePtr& type) {
    if (!type) {
        return "unknown";
    }
    if (auto decimal = DecimalSpecOfValueType(type)) {
        return "Decimal::" + std::to_string(decimal->Precision) + "," +
            std::to_string(decimal->Scale);
    }
    if (IsNullableType(type)) {
        return "Nullable::" + PlanTypeKey(UnwrapNullableType(type));
    }
    return TypeKey(type);
}

// SQL null propagation: any nullable (or bare-NULL) operand makes the result Nullable.
TTypePtr Propagate(std::initializer_list<TTypePtr> operands, const TTypePtr& R) {
    if (!R) {
        return R;
    }
    bool nullable = IsNullableType(R);
    for (const auto& t : operands) {
        nullable = nullable || !t || IsNullableType(t);
    }
    TTypePtr base = UnwrapNullableType(R);
    return nullable ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(base)) : base;
}

bool IsInt(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TIntegerType>(t)); }
bool IsFloat(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TFloatType>(t)); }
// Whether `e` is an integer literal whose value `type` can represent (Qumir's
// IntegerLiteralFits). Such a literal adopts the other operand's integer type; one that
// does not fit keeps i64 and widens the operation.
bool IntLiteralFits(const TExprPtr& e, const TTypePtr& type) {
    auto n = TMaybeNode<TNumberExpr>(e);
    auto integer = TMaybeType<TIntegerType>(type);
    if (!n || n.Cast()->IsFloat() || !integer) {
        return false;
    }
    const int64_t value = n.Cast()->IntValue;
    const int bits = integer.Cast()->BitWidth();
    if (!integer.Cast()->IsSigned()) {
        return value >= 0 && (bits >= 64 || value < (int64_t{1} << bits));
    }
    return bits >= 64 ||
        (value >= -(int64_t{1} << (bits - 1)) && value < (int64_t{1} << (bits - 1)));
}

TTypePtr NumericPreservingUnaryCall(
    const std::string& name,
    const std::vector<TTypePtr>& argTypes)
{
    if (name != "abs" || argTypes.size() != 1) {
        return nullptr;
    }
    TTypePtr value = ValueType(argTypes[0]);
    if (!value) {
        return nullptr;
    }
    if (DecimalSpecOfValueType(value)) {
        return value;
    }
    value = UnwrapNamedType(value);
    if (IsInt(value) || IsFloat(value)) {
        return value;
    }
    return nullptr;
}

// Mirrors qumir NTypeAnnotation::CommonNumericType + WideningIntOK for the types qdb sees
// (all ints signed). Float dominates; two ints widen to the wider. TODO(reuse): call
// qumir's own BinaryNumericResultType once it is exposed (see NULL_PROPAGATION_PASS_PLAN.md).
TTypePtr CommonNumeric(const TTypePtr& a, const TTypePtr& b) {
    if (IsFloat(a) || IsFloat(b)) {
        return std::make_shared<TFloatType>();
    }
    auto ai = TMaybeType<TIntegerType>(a);
    auto bi = TMaybeType<TIntegerType>(b);
    if (ai && bi) {
        return bi.Cast()->BitWidth() >= ai.Cast()->BitWidth() ? b : a;
    }
    return a;
}

[[noreturn]] void ThrowDecimalUnsupported(TOperator op, const char* detail) {
    throw NQumir::TError(
        "decimal operator '" + op.ToString() + "' is not supported: " + detail);
}

TDecimalSpec DecimalDivisionSpec(const TDecimalSpec& left, const TDecimalSpec& right) {
    const int32_t scale = std::max(6, left.Scale + right.Precision + 1);
    const int32_t intDigits = left.Precision - left.Scale + right.Scale;
    const int32_t precision = intDigits + scale;
    if (precision <= MaxDecimalPrecision) {
        return {.Precision = precision, .Scale = scale};
    }

    // SQL decimal division keeps integral digits first and reduces fractional scale
    // when precision would exceed DECIMAL(38, s).
    return {
        .Precision = MaxDecimalPrecision,
        .Scale = std::max(0, MaxDecimalPrecision - intDigits),
    };
}

// An integer operand of a decimal remainder counts as DECIMAL(w, 0), w being the digits
// of its SQL integer type (INTEGER is DECIMAL(10, 0)). An integer literal is INTEGER when
// its value fits, as SQL types integer literals.
TDecimalSpec IntegerDecimalSpec(const TExprPtr& e, const TTypePtr& type) {
    int bits = TMaybeType<TIntegerType>(type).Cast()->BitWidth();
    if (auto n = TMaybeNode<TNumberExpr>(e); n && !n.Cast()->IsFloat()) {
        const int64_t value = n.Cast()->IntValue;
        const bool fitsInteger = value >= std::numeric_limits<int32_t>::min()
            && value <= std::numeric_limits<int32_t>::max();
        bits = fitsInteger
            ? 32
            : 64;
    }
    switch (bits) {
        case 8:
            return {.Precision = 3, .Scale = 0};
        case 16:
            return {.Precision = 5, .Scale = 0};
        case 32:
            return {.Precision = 10, .Scale = 0};
        case 64:
            return {.Precision = 19, .Scale = 0};
    }
    return {.Precision = MaxDecimalPrecision, .Scale = 0};
}

TTypePtr DecimalBinaryValueType(TOperator op, const TExprPtr& le, const TExprPtr& re,
    const TTypePtr& vl, const TTypePtr& vr)
{
    auto dl = DecimalSpecOfValueType(vl);
    auto dr = DecimalSpecOfValueType(vr);
    if (!dl && !dr) {
        return nullptr;
    }
    if (IsComparison(op)) {
        return std::make_shared<TBoolType>();
    }
    if (op == "/") {
        if (dl && dr) {
            return MakeDecimalType(DecimalDivisionSpec(*dl, *dr));
        }
        if (dl && !dr && IsInt(vr)) {
            return MakeDecimalType(*dl);
        }
        ThrowDecimalUnsupported(op, "only decimal / decimal and decimal / integer are implemented in qdb v1");
    }
    if (op == "%") {
        if ((!dl && !IsInt(vl)) || (!dr && !IsInt(vr))) {
            ThrowDecimalUnsupported(op, "only decimal % decimal and decimal % integer are implemented");
        }
        // DuckDB's rule: the common scale and the larger integer part of the operands.
        const TDecimalSpec l = dl ? *dl : IntegerDecimalSpec(le, vl);
        const TDecimalSpec r = dr ? *dr : IntegerDecimalSpec(re, vr);
        const int32_t scale = std::max(l.Scale, r.Scale);
        const int32_t intDigits = std::max(l.Precision - l.Scale, r.Precision - r.Scale);
        return MakeDecimalType({
            .Precision = std::min(MaxDecimalPrecision, intDigits + scale),
            .Scale = scale,
        });
    }
    if (op == "*" && dl && dr) {
        ThrowDecimalUnsupported(op, "decimal * decimal is intentionally absent in v1");
    }
    if (op == "*") {
        auto decimal = dl ? *dl : *dr;
        const TTypePtr other = dl ? vr : vl;
        if (!IsInt(other)) {
            ThrowDecimalUnsupported(op, "only decimal * integer is implemented in v1");
        }
        return MakeDecimalType({
            .Precision = std::min(MaxDecimalPrecision, decimal.Precision + 1),
            .Scale = decimal.Scale,
        });
    }
    if (op == "+" || op == "-") {
        if (!dl) {
            if (!IsInt(vl) && !IsFloat(vl)) {
                ThrowDecimalUnsupported(op, "left operand cannot be converted to decimal");
            }
            return MakeDecimalType({
                .Precision = std::min(MaxDecimalPrecision, dr->Precision + 1),
                .Scale = dr->Scale,
            });
        }
        if (!dr) {
            if (!IsInt(vr) && !IsFloat(vr)) {
                ThrowDecimalUnsupported(op, "right operand cannot be converted to decimal");
            }
            return MakeDecimalType({
                .Precision = std::min(MaxDecimalPrecision, dl->Precision + 1),
                .Scale = dl->Scale,
            });
        }
        const int32_t scale = std::max(dl->Scale, dr->Scale);
        const int32_t intDigits = std::max(
            dl->Precision - dl->Scale,
            dr->Precision - dr->Scale);
        const int32_t precision = std::min(MaxDecimalPrecision, intDigits + scale + 1);
        return MakeDecimalType({.Precision = precision, .Scale = scale});
    }
    ThrowDecimalUnsupported(op, "operator is not implemented for decimal values");
}

// Common value type of two CASE/`if` branches (SQL branch unification): numeric promotion
// (i32 then, i64 else -> i64), otherwise assume the branches already agree.
TTypePtr UnifyBranchTypes(const TTypePtr& a, const TTypePtr& b) {
    if (!a) return b ? UnwrapNullableType(b) : nullptr;
    if (!b) return UnwrapNullableType(a);
    TTypePtr va = UnwrapNullableType(a), vb = UnwrapNullableType(b);
    auto da = DecimalSpecOfValueType(va);
    auto db = DecimalSpecOfValueType(vb);
    if (da || db) {
        if (da && db) {
            const int32_t scale = std::max(da->Scale, db->Scale);
            const int32_t intDigits = std::max(
                da->Precision - da->Scale,
                db->Precision - db->Scale);
            return MakeDecimalType({
                .Precision = std::min(MaxDecimalPrecision, intDigits + scale),
                .Scale = scale,
            });
        }
        return MakeDecimalType(da ? *da : *db);
    }
    if ((IsInt(va) || IsFloat(va)) && (IsInt(vb) || IsFloat(vb))) {
        return CommonNumeric(va, vb);
    }
    return va;
}

// COALESCE result: value type (unified over non-NULL args) and whether every arg is
// nullable (a bare NULL arg is ignored). Non-nullable arg => the whole COALESCE is
// non-nullable (it is guaranteed to return that arg's value at worst).
std::pair<TTypePtr, bool> CoalesceValueType(const std::vector<TTypePtr>& argTypes) {
    TTypePtr v = nullptr;
    bool allNullable = true;
    for (const auto& t : argTypes) {
        if (!t) continue; // bare NULL - never chosen
        v = v ? UnifyBranchTypes(v, t) : UnwrapNullableType(t);
        if (!IsNullableType(t)) allNullable = false;
    }
    return {v, allNullable};
}

// The plain (non-null-propagated) result value type of a binary op, matching qumir's
// AnnotateBinary: `/` -> float, `+ - * %`/bitwise -> CommonNumeric, comparisons/logical
// -> bool. An integer-literal operand adopts the other operand's int type when its value
// fits it (qumir's RetypeIntegerLiteralOperands).
TTypePtr BinaryValueType(TOperator op, const TExprPtr& le, const TExprPtr& re,
    const TTypePtr& lt, const TTypePtr& rt)
{
    if (IsComparison(op) || IsLogical(op)) {
        return std::make_shared<TBoolType>();
    }
    TTypePtr vl = UnwrapNamedType(ValueType(lt));
    TTypePtr vr = UnwrapNamedType(ValueType(rt));
    if (auto decimal = DecimalBinaryValueType(op, le, re, ValueType(lt), ValueType(rt))) {
        return decimal;
    }
    if (op == "+" && (TMaybeType<TStringType>(vl) || TMaybeType<TStringType>(vr))) {
        return std::make_shared<TStringType>(); // string concatenation
    }
    if (IsInt(vl) && IsInt(vr)) {
        if (IntLiteralFits(re, vl)) vr = vl;
        else if (IntLiteralFits(le, vr)) vl = vr;
    }
    if (op == "/") {
        return std::make_shared<TFloatType>();
    }
    return CommonNumeric(vl, vr);
}

// SQL `%` on exact numbers and decimal `/` have no value for a zero divisor, and unlike
// DOUBLE (NaN, infinity) their result types cannot stand for one. They yield NULL, as in
// DuckDB. PostgreSQL raises "division by zero"; qdb has no runtime error channel yet.
bool IsNullOnZeroDivisor(TOperator op, const TTypePtr& result) {
    const TTypePtr value = ValueType(result);
    if (DecimalSpecOfValueType(value)) {
        return op == "%" || op == "/";
    }
    return op == "%" && IsInt(UnwrapNamedType(value));
}

// Anything but a non-zero numeric literal can be a zero divisor.
bool MayBeZero(const TExprPtr& divisor) {
    auto n = TMaybeNode<TNumberExpr>(divisor);
    if (!n) {
        return true;
    }
    return n.Cast()->IsFloat()
        ? n.Cast()->FloatValue == 0.0
        : n.Cast()->IntValue == 0;
}

// Lightweight, rule-based SQL type inference - no AST rewrite and no per-node qumirdb.oz
// compose. Types operators/calls "as if" ExpandNullable had run: a nullable operand makes
// the result Nullable[R]. Extern-call returns come from the resolver (ExternReturnType).
TTypePtr InferType(const TExprPtr& e, const TStructType& schema) {
    if (!e || IsNullLiteral(e)) {
        return nullptr; // bare/typed NULL - the enclosing node supplies the value type
    }
    if (TMaybeNode<TStringLiteralExpr>(e)) {
        return std::make_shared<TStringType>();
    }
    if (auto num = TMaybeNode<TNumberExpr>(e)) {
        // The ctor sets ->Type (bool / i64 / float); i32_literal overrides to i32.
        return num.Cast()->Type ? num.Cast()->Type
            : std::static_pointer_cast<TType>(std::make_shared<TIntegerType>());
    }
    if (auto id = TMaybeNode<TIdentExpr>(e)) {
        const std::string& n = id.Cast()->Name;
        for (const auto& [fn, ft] : schema.Fields) {
            if (fn == n) return ft;
        }
        return nullptr; // unknown here - tolerated when nested (null propagates)
    }
    if (auto bin = TMaybeNode<TBinaryExpr>(e)) {
        auto node =bin.Cast();
        TTypePtr lt = InferType(node->Left, schema);
        TTypePtr rt = InferType(node->Right, schema);
        // A bare-NULL operand takes the other operand's type, as Expand does.
        TTypePtr R = BinaryValueType(node->Operator, node->Left, node->Right,
            lt ? lt : rt, rt ? rt : lt);
        TTypePtr type = Propagate({lt, rt}, R);
        if (type && IsNullOnZeroDivisor(node->Operator, R) && MayBeZero(node->Right)) {
            return std::make_shared<TNullable>(UnwrapNullableType(type));
        }
        return type;
    }
    if (auto un = TMaybeNode<TUnaryExpr>(e)) {
        auto node =un.Cast();
        TTypePtr ot = InferType(node->Operand, schema);
        TTypePtr R = node->Operator == "!"
            ? std::static_pointer_cast<TType>(std::make_shared<TBoolType>())
            : ValueType(ot);
        return Propagate({ot}, R);
    }
    if (auto cst = TMaybeNode<TCastExpr>(e)) {
        auto node =cst.Cast();
        TTypePtr ot = InferType(node->Operand, schema);
        // A cast whose target is itself nullable (e.g. the UNION-branch coercion
        // `cast(i32, Nullable[i32])`) is Nullable[base] regardless of the operand.
        if (IsNullableType(node->Type)) {
            return std::make_shared<TNullable>(UnwrapNullableType(node->Type));
        }
        return Propagate({ot}, FromQumirType(node->Type));
    }
    if (auto call = TMaybeNode<TCallExpr>(e)) {
        auto node =call.Cast();
        std::string name;
        if (auto cid = TMaybeNode<TIdentExpr>(node->Callee)) {
            name = cid.Cast()->Name;
        }
        std::vector<TTypePtr> at;
        for (const auto& a : node->Args) {
            at.push_back(InferType(a, schema));
        }
        if (name == "qdb_is_null" || name == "qdb_is_true") {
            return std::make_shared<TBoolType>(); // non-null bool regardless of args
        }
        if (name == "coalesce") {
            auto [v, allNullable] = CoalesceValueType(at);
            if (!v) return nullptr;
            return allNullable
                ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(v)) : v;
        }
        if (name == "qdb_in_list") {
            bool nullable = false;
            for (const auto& t : at) {
                nullable = nullable || !t || IsNullableType(t);
            }
            auto result = std::make_shared<TBoolType>();
            return nullable
                ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(result))
                : std::static_pointer_cast<TType>(result);
        }
        if (name == "strcat") {
            bool nullable = false;
            for (const auto& t : at) nullable = nullable || !t || IsNullableType(t);
            TTypePtr s = std::make_shared<TStringType>();
            return nullable
                ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(s)) : s;
        }
        if (auto value = NumericPreservingUnaryCall(name, at)) {
            return Propagate({at[0]}, value);
        }
        TTypePtr ret = CallReturnType(*node, name, at);
        bool nullable = false;
        for (const auto& t : at) nullable = nullable || !t || IsNullableType(t);
        TTypePtr base = UnwrapNullableType(ret);
        return (nullable || IsNullableType(ret))
            ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(base)) : base;
    }
    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node =ifp.Cast();
        // The condition does not shape the result type, but it is typed too: it may
        // call an unknown function.
        InferType(node->Cond, schema);
        TTypePtr tt = InferType(node->Then, schema);
        TTypePtr et = node->Else ? InferType(node->Else, schema) : nullptr;
        TTypePtr base = UnifyBranchTypes(tt, et);
        if (!base) {
            return nullptr;
        }
        bool nullable = !tt || !et || IsNullableType(tt) || IsNullableType(et);
        return nullable ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(base)) : base;
    }
    if (auto fa = TMaybeNode<TFieldAccessExpr>(e)) {
        auto node =fa.Cast();
        TTypePtr ot = InferType(node->Object, schema);
        if (ot && IsNullableType(ot)) {
            if (node->FieldName == "Value") return UnwrapNullableType(ot);
            if (node->FieldName == "Valid") return std::make_shared<TBoolType>();
        }
        return nullptr;
    }
    if (auto blk = TMaybeNode<TBlockExpr>(e)) {
        const auto& s = blk.Cast()->Stmts;
        return s.empty() ? nullptr : InferType(s.back(), schema);
    }
    return nullptr;
}

TTypePtr ResultTypeUnary(TOperator op, const TTypePtr& ot) {
    return op == "!" ? std::static_pointer_cast<TType>(std::make_shared<TBoolType>())
                     : ValueType(ot);
}

// Build `if(all-valid) cast(<apply unwrapped>, Nullable[R]) else null[R]`, guarding on
// every nullable operand's `.Valid` and applying the op to their `.Value`s. `R` is the
// plain result value type. Uses only `if`/`cast` (no block-local vars) - the shape the
// annotator and kernel lowering already handle; operands are cloned per reference.
// Bind `expr` to a fresh temp var (appended to `stmts`) and return an ident referencing
// it. `counter` is per-query (owned by the ExpandNullable call) - names must be unique
// within one expansion. Reading a temp's fields many times can't re-expand a compound
// operand, so this keeps the rewrite linear instead of 2^depth.
TExprPtr BindTemp(const TExprPtr& expr, std::vector<TExprPtr>& stmts, uint64_t& counter,
    TLocation loc)
{
    std::string name = "__nx" + std::to_string(counter++);
    auto var = std::make_shared<TVarStmt>(loc, name, nullptr);
    var->Init = expr;
    stmts.push_back(var);
    return Ident(name, loc);
}

TExprPtr BuildNullStrict(const std::vector<std::pair<TExprPtr, bool>>& operands,
    const std::function<TExprPtr(std::vector<TExprPtr>)>& apply,
    const TTypePtr& R, uint64_t& counter, TLocation loc)
{
    std::vector<TExprPtr> stmts;
    std::vector<TExprPtr> unwrapped;
    std::vector<TExprPtr> valids;
    for (const auto& [expr, nullable] : operands) {
        if (nullable) {
            auto ref = BindTemp(expr, stmts, counter, loc);
            unwrapped.push_back(FieldOf(ref, "Value", loc));
            valids.push_back(FieldOf(ref, "Valid", loc));
        } else {
            unwrapped.push_back(CloneExpr(expr));
        }
    }
    auto nq = NullableQ(R);
    TExprPtr guard = valids.front();
    for (size_t i = 1; i < valids.size(); ++i) {
        guard = Bin("&&", guard, valids[i], loc);
    }
    TExprPtr thenE = std::make_shared<TCastExpr>(loc, apply(unwrapped), nq);
    TExprPtr elseE = std::make_shared<TCastExpr>(loc, MakeNull(loc), nq);
    stmts.push_back(std::make_shared<TIfExpr>(loc, guard, thenE, elseE));
    return std::make_shared<TBlockExpr>(loc, std::move(stmts));
}

// SQL 3VL for AND/OR: evaluates both operands; a valid dominating value (FALSE for AND,
// TRUE for OR) wins over NULL. Each operand contributes (Valid, Value) plain bools;
// `ref == nullptr` is a bare NULL, non-nullable is always valid.
TExprPtr OpValid(const TExprPtr& ref, const TTypePtr& t, TLocation loc) {
    if (!t) return BoolLit(false, loc);
    if (IsNullableType(t)) return FieldOf(ref, "Valid", loc);
    return BoolLit(true, loc);
}
TExprPtr OpValue(const TExprPtr& ref, const TTypePtr& t, TLocation loc) {
    if (!t) return BoolLit(false, loc);
    if (IsNullableType(t)) return FieldOf(ref, "Value", loc);
    return CloneExpr(ref);
}
TExprPtr Build3VL(bool isAnd, const TExprPtr& a, const TTypePtr& at,
    const TExprPtr& b, const TTypePtr& bt, uint64_t& counter, TLocation loc)
{
    std::vector<TExprPtr> stmts;
    // Bind nullable operands to temps (read .Valid/.Value repeatedly below).
    TExprPtr ra = (at && IsNullableType(at)) ? BindTemp(a, stmts, counter, loc) : a;
    TExprPtr rb = (bt && IsNullableType(bt)) ? BindTemp(b, stmts, counter, loc) : b;
    auto boolN = NullableQ(std::make_shared<TBoolType>());
    auto nTrue = std::make_shared<TCastExpr>(loc, BoolLit(true, loc), boolN);
    auto nFalse = std::make_shared<TCastExpr>(loc, BoolLit(false, loc), boolN);
    auto nNull = std::make_shared<TCastExpr>(loc, MakeNull(loc), boolN);
    auto bothValid = Bin("&&", OpValid(ra, at, loc), OpValid(rb, bt, loc), loc);
    TExprPtr result;
    if (isAnd) {
        auto dominant = Bin("||",
            Bin("&&", OpValid(ra, at, loc), Not(OpValue(ra, at, loc), loc), loc),
            Bin("&&", OpValid(rb, bt, loc), Not(OpValue(rb, bt, loc), loc), loc), loc);
        result = std::make_shared<TIfExpr>(loc, dominant, nFalse,
            std::make_shared<TIfExpr>(loc, bothValid, nTrue, nNull));
    } else {
        auto dominant = Bin("||",
            Bin("&&", OpValid(ra, at, loc), OpValue(ra, at, loc), loc),
            Bin("&&", OpValid(rb, bt, loc), OpValue(rb, bt, loc), loc), loc);
        result = std::make_shared<TIfExpr>(loc, dominant, nTrue,
            std::make_shared<TIfExpr>(loc, bothValid, nFalse, nNull));
    }
    stmts.push_back(result);
    return std::make_shared<TBlockExpr>(loc, std::move(stmts));
}

// NULLIF(divisor, 0) as Nullable[value]: a zero divisor becomes one more NULL operand of
// the division, so the null-strict guard skips computing it.
TExprPtr NullIfZero(const TExprPtr& divisor, const TTypePtr& type, uint64_t& counter,
    TLocation loc)
{
    std::vector<TExprPtr> stmts;
    TExprPtr ref = BindTemp(divisor, stmts, counter, loc);
    auto nq = NullableQ(ValueType(type));
    TExprPtr nonZero = Bin("!=", OpValue(ref, type, loc),
        std::make_shared<TNumberExpr>(loc, int64_t{0}), loc);
    TExprPtr kept = CloneExpr(ref);
    if (IsNullableType(type)) {
        nonZero = Bin("&&", FieldOf(ref, "Valid", loc), nonZero, loc);
    } else {
        kept = std::make_shared<TCastExpr>(loc, kept, nq);
    }
    stmts.push_back(std::make_shared<TIfExpr>(loc, nonZero, kept,
        std::make_shared<TCastExpr>(loc, MakeNull(loc), nq)));
    return std::make_shared<TBlockExpr>(loc, std::move(stmts));
}

// Coerce an `if`/CASE branch to Nullable[value] so all branches share one type: NULL ->
// null[value]; a plain branch is widened if needed, then wrapped via nullable_from_value;
// an already-nullable branch of a narrower type is widened through a null-strict cast.
TExprPtr CoerceBranch(const TExprPtr& branch, const TTypePtr& branchType, bool isNull,
    const TTypePtr& value, uint64_t& counter, TLocation loc)
{
    auto nq = NullableQ(value);
    if (isNull) {
        return std::make_shared<TCastExpr>(loc, branch ? branch : MakeNull(loc), nq);
    }
    auto tv = ToQumirType(value);
    if (!branchType || !IsNullableType(branchType)) {
        TExprPtr v = branch;
        if (branchType && PlanTypeKey(branchType) != PlanTypeKey(value)) {
            v = std::make_shared<TCastExpr>(loc, branch, tv); // widen plain value
        }
        return std::make_shared<TCastExpr>(v->Location, v, nq); // nullable_from_value
    }
    if (PlanTypeKey(UnwrapNullableType(branchType)) == PlanTypeKey(value)) {
        return branch; // already Nullable[value]
    }
    TExprPtr b = branch; // Nullable but narrower: widen the value under the validity guard
    return BuildNullStrict({{b, true}},
        [tv, loc](std::vector<TExprPtr> a) {
            return std::make_shared<TCastExpr>(loc, a[0], tv);
        }, value, counter, loc);
}

TTypePtr Expand(TExprPtr& e, const TStructType& inputType, uint64_t& counter);

// qdb_in_list(lhs, item...) preserves the SQL IN boundary until operand types are
// known. Hoist lhs validity once, compare its plain value to every item, and only
// retain per-item 3VL when an item itself can be NULL.
std::pair<TExprPtr, TTypePtr> ExpandInList(const TCallExpr& node,
    const TStructType& inputType, uint64_t& counter)
{
    const TLocation loc = node.Location;
    auto boolType = std::make_shared<TBoolType>();
    auto nullableBool = std::make_shared<TNullable>(boolType);
    if (node.Args.size() < 2) {
        throw NQumir::TError("qdb_in_list expects an lhs and at least one item");
    }

    TExprPtr lhs = node.Args.front();
    TTypePtr lhsType = Expand(lhs, inputType, counter);
    if (!lhsType) {
        return {
            std::make_shared<TCastExpr>(loc, MakeNull(loc), NullableQ(boolType)),
            nullableBool,
        };
    }

    std::vector<TExprPtr> stmts;
    TExprPtr lhsRef = BindTemp(lhs, stmts, counter, loc);
    TExprPtr lhsValue = IsNullableType(lhsType)
        ? FieldOf(lhsRef, "Value", loc)
        : lhsRef;

    TExprPtr result;
    TTypePtr resultType;
    for (size_t i = 1; i < node.Args.size(); ++i) {
        TExprPtr item = node.Args[i];
        TTypePtr itemType = Expand(item, inputType, counter);

        TExprPtr equal;
        TTypePtr equalType;
        if (!itemType) {
            equal = std::make_shared<TCastExpr>(
                loc, MakeNull(loc), NullableQ(boolType));
            equalType = nullableBool;
        } else if (IsNullableType(itemType)) {
            equal = BuildNullStrict({{item, true}},
                [lhsValue, loc](std::vector<TExprPtr> args) {
                    return Bin("==", CloneExpr(lhsValue), std::move(args[0]), loc);
                }, boolType, counter, loc);
            equalType = nullableBool;
        } else {
            equal = Bin("==", CloneExpr(lhsValue), std::move(item), loc);
            equalType = boolType;
        }

        if (!result) {
            result = std::move(equal);
            resultType = equalType;
        } else if (IsNullableType(resultType) || IsNullableType(equalType)) {
            result = Build3VL(
                /*isAnd=*/false, result, resultType, equal, equalType, counter, loc);
            resultType = nullableBool;
        } else {
            result = Bin("||", std::move(result), std::move(equal), loc);
            resultType = boolType;
        }
    }

    if (IsNullableType(lhsType)) {
        result = std::make_shared<TIfExpr>(
            loc,
            FieldOf(lhsRef, "Valid", loc),
            CoerceBranch(result, resultType, /*isNull=*/false, boolType, counter, loc),
            std::make_shared<TCastExpr>(loc, MakeNull(loc), NullableQ(boolType)));
        resultType = nullableBool;
    }

    stmts.push_back(std::move(result));
    return {
        std::make_shared<TBlockExpr>(loc, std::move(stmts)),
        resultType,
    };
}

// COALESCE(a, b, ...) -> an `if`-chain: return the first arg with `.Valid`; once a
// non-nullable arg is reached it is returned directly (result becomes non-nullable).
// Bare-NULL args are dropped. Args are bound to temps (referenced for both `.Valid` and
// value). Returns {rewritten expr, planner type}.
std::pair<TExprPtr, TTypePtr> ExpandCoalesce(const TCallExpr& node,
    const TStructType& inputType, uint64_t& counter)
{
    const TLocation loc = node.Location;
    std::vector<std::pair<TExprPtr, TTypePtr>> args; // (expr, type); bare NULLs dropped
    std::vector<TTypePtr> types;
    for (const auto& a : node.Args) {
        TExprPtr arg = a;
        TTypePtr t = Expand(arg, inputType, counter);
        if (t) { args.emplace_back(arg, t); types.push_back(t); }
    }
    auto [T, allNullable] = CoalesceValueType(types);
    if (args.empty() || !T) {
        return {MakeNull(loc), nullptr}; // degenerate (all NULL) - untyped
    }
    const bool resultNullable = allNullable;
    auto tq = ToQumirType(T);

    // Terminator = first non-nullable arg (guaranteed value), else the last arg.
    size_t m = args.size() - 1;
    for (size_t i = 0; i < args.size(); ++i) {
        if (!IsNullableType(args[i].second)) { m = i; break; }
    }

    // The value contributed by a (temp-bound) arg when it is the chosen one.
    auto pick = [&](const TExprPtr& ref, const TTypePtr& t) -> TExprPtr {
        if (resultNullable) {
            return CoerceBranch(ref, t, /*isNull*/false, T, counter, loc); // -> Nullable[T]
        }
        TExprPtr val = IsNullableType(t) ? FieldOf(ref, "Value", loc) : CloneExpr(ref);
        if (PlanTypeKey(UnwrapNullableType(t)) != PlanTypeKey(T)) {
            val = std::make_shared<TCastExpr>(loc, val, tq); // widen to T
        }
        return val;
    };

    std::vector<TExprPtr> stmts;
    // Terminator is returned unconditionally (no `.Valid` guard).
    TExprPtr acc = pick(args[m].first, args[m].second);
    // Chain args[0..m-1] (all nullable) from the inside out.
    for (int i = static_cast<int>(m) - 1; i >= 0; --i) {
        TExprPtr ref = BindTemp(args[i].first, stmts, counter, loc);
        acc = std::make_shared<TIfExpr>(loc,
            FieldOf(ref, "Valid", loc), pick(ref, args[i].second), acc);
    }
    if (stmts.empty()) {
        return {acc, resultNullable ? std::static_pointer_cast<TType>(
            std::make_shared<TNullable>(T)) : T};
    }
    stmts.push_back(acc);
    return {std::make_shared<TBlockExpr>(loc, std::move(stmts)),
        resultNullable ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(T)) : T};
}

struct TLikeFastPath {
    const char* Func;
    std::string Literal;
};

TStringLiteralExpr* MutableStringLiteral(const TExprPtr& expr) {
    TExprPtr current = expr;
    while (auto cast = TMaybeNode<TCastExpr>(current)) {
        current = cast.Cast()->Operand;
    }
    auto literal = TMaybeNode<TStringLiteralExpr>(current);
    return literal ? literal.Cast().get() : nullptr;
}

std::optional<TLikeFastPath> ClassifyLikePattern(const std::string& pattern) {
    if (pattern.find('_') != std::string::npos) {
        return std::nullopt;
    }
    if (pattern.find('%') == std::string::npos) {
        return TLikeFastPath{"qdb_like_equals", pattern};
    }
    size_t begin = 0;
    while (begin < pattern.size() && pattern[begin] == '%') {
        ++begin;
    }
    size_t end = pattern.size();
    while (end > begin && pattern[end - 1] == '%') {
        --end;
    }
    std::string middle = pattern.substr(begin, end - begin);
    if (middle.find('%') != std::string::npos) {
        return std::nullopt;
    }
    const bool leading = begin > 0;
    const bool trailing = end < pattern.size();
    if (leading && trailing) {
        return TLikeFastPath{"qdb_like_contains", std::move(middle)};
    }
    return TLikeFastPath{trailing ? "qdb_like_prefix" : "qdb_like_suffix",
        std::move(middle)};
}

// Bottom-up rewrite. Mutates `e` in place, returns its planner type (`TNullable`-aware),
// or nullptr for a bare NULL leaf (the enclosing `if`/op supplies the type).
TTypePtr Expand(TExprPtr& e, const TStructType& inputType, uint64_t& counter) {
    if (IsNullLiteral(e)) {
        return nullptr;
    }
    // A string literal is a non-null `string` leaf. It is always the operand of a
    // parser-emitted `cast(lit, StringView)`; annotating the bare literal would wrap it in
    // a var (owned storage) and fail the lifetime validator, so type it directly.
    if (TMaybeNode<TStringLiteralExpr>(e)) {
        return std::make_shared<TStringType>();
    }

    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node = ifp.Cast();
        Expand(node->Cond, inputType, counter);
        if (!node->Then) node->Then = MakeNull(node->Location);
        if (!node->Else) node->Else = MakeNull(node->Location);
        TTypePtr thenT = Expand(node->Then, inputType, counter);
        TTypePtr elseT = Expand(node->Else, inputType, counter);
        const bool thenNull = !thenT, elseNull = !elseT;
        const bool thenNullable = thenNull || IsNullableType(thenT);
        const bool elseNullable = elseNull || IsNullableType(elseT);
        if (!thenNullable && !elseNullable) {
            return UnifyBranchTypes(thenT, elseT); // no null - unify (i32/i64 -> i64)
        }
        TTypePtr value = UnifyBranchTypes(thenT, elseT);
        if (!value) {
            return nullptr; // both branches NULL - untyped, leave for the parent
        }
        node->Then = CoerceBranch(node->Then, thenT, thenNull, value, counter, node->Location);
        node->Else = CoerceBranch(node->Else, elseT, elseNull, value, counter, node->Location);
        return std::make_shared<TNullable>(value);
    }

    if (auto bp = TMaybeNode<TBinaryExpr>(e)) {
        auto node = bp.Cast();
        TTypePtr lt = Expand(node->Left, inputType, counter);
        TTypePtr rt = Expand(node->Right, inputType, counter);
        TOperator op = node->Operator;
        const bool lNull = !lt || IsNullableType(lt);
        bool rNull = !rt || IsNullableType(rt);
        if (IsLogical(op)) {
            if (!lNull && !rNull) {
                return std::make_shared<TBoolType>();
            }
            e = Build3VL(op == "&&", node->Left, lt, node->Right, rt, counter, node->Location);
            return std::make_shared<TNullable>(std::make_shared<TBoolType>());
        }
        if (lt && rt && MayBeZero(node->Right)) {
            TTypePtr R = BinaryValueType(op, node->Left, node->Right, lt, rt);
            if (IsNullOnZeroDivisor(op, R)) {
                if (TMaybeNode<TNumberExpr>(node->Right)) {
                    // `x % 0`: NULL on every row.
                    e = std::make_shared<TCastExpr>(node->Location, MakeNull(node->Location), NullableQ(R));
                    return std::make_shared<TNullable>(R);
                }
                node->Right = NullIfZero(node->Right, rt, counter, node->Location);
                rt = std::make_shared<TNullable>(ValueType(rt));
                rNull = true;
            }
        }
        if (!lNull && !rNull) {
            return BinaryValueType(op, node->Left, node->Right, lt, rt);
        }
        // Bare-NULL operand => the whole op is unconditionally NULL[R].
        if (!lt || !rt) {
            TTypePtr R = BinaryValueType(op, node->Left, node->Right,
                lt ? lt : rt, rt ? rt : lt);
            e = std::make_shared<TCastExpr>(node->Location, MakeNull(node->Location), NullableQ(R));
            return std::make_shared<TNullable>(R);
        }
        TTypePtr R = BinaryValueType(op, node->Left, node->Right, lt, rt);
        TExprPtr l = node->Left, r = node->Right;
        e = BuildNullStrict({{l, lNull}, {r, rNull}},
            [op, node](std::vector<TExprPtr> a) {
                return std::make_shared<TBinaryExpr>(node->Location, op, a[0], a[1]);
            }, R, counter, node->Location);
        return std::make_shared<TNullable>(R);
    }

    if (auto up = TMaybeNode<TUnaryExpr>(e)) {
        auto node = up.Cast();
        TTypePtr ot = Expand(node->Operand, inputType, counter);
        TOperator op = node->Operator;
        TTypePtr R = ResultTypeUnary(op, ot);
        if (IsNullLiteral(node->Operand) && R) {
            // `!` fixes the type of a bare-NULL operand: NOT NULL is NULL[bool].
            e = std::make_shared<TCastExpr>(node->Location, MakeNull(node->Location), NullableQ(R));
            return std::make_shared<TNullable>(R);
        }
        if (!ot || !IsNullableType(ot)) {
            return ot ? R : nullptr;
        }
        TExprPtr operand = node->Operand;
        e = BuildNullStrict({{operand, true}},
            [op, node](std::vector<TExprPtr> a) {
                return std::make_shared<TUnaryExpr>(node->Location, op, a[0]);
            }, R, counter, node->Location);
        return std::make_shared<TNullable>(R);
    }

    if (auto cp = TMaybeNode<TCastExpr>(e)) {
        auto node = cp.Cast();
        TTypePtr ot = Expand(node->Operand, inputType, counter);
        // `target` is the (non-nullable) value type; a nullable cast target contributes
        // nullability separately (BuildNullStrict / nullable_from_value wrap it once).
        TTypePtr target = IsNullableType(node->Type)
            ? UnwrapNullableType(node->Type)
            : FromQumirType(node->Type);
        if (IsNullLiteral(node->Operand)) {
            // CAST(NULL AS T) is the NULL of T.
            e = std::make_shared<TCastExpr>(node->Location, node->Operand, NullableQ(target));
            return std::make_shared<TNullable>(target);
        }
        if (!ot || !IsNullableType(ot)) {
            // Non-nullable operand cast to a nullable target (e.g. a UNION-branch
            // coercion `cast(i32, Nullable[i32])`): emit nullable_from_value. The cast
            // target must be the qumir Nullable type - a raw planner TNullable does not
            // compile - and the value is widened to the target base if needed. (Checked
            // on the raw node->Type: FromQumirType strips the qdb TNullable.)
            if (ot && IsNullableType(node->Type)) {
                TTypePtr base = UnwrapNullableType(node->Type);
                TExprPtr val = node->Operand;
                if (PlanTypeKey(UnwrapNullableType(ot)) != PlanTypeKey(base)) {
                    // Base conversion targets the planner base type so ExpandDecimal can
                    // still turn a float/int -> decimal cast into qdb_decimal_from_*.
                    val = std::make_shared<TCastExpr>(node->Location, val, base);
                }
                e = std::make_shared<TCastExpr>(node->Location, val, ToQumirType(node->Type));
                return std::make_shared<TNullable>(base);
            }
            return ot ? target : nullptr;
        }
        TExprPtr operand = node->Operand;
        // BuildNullStrict already re-wraps the result in Nullable[target], so the inner
        // cast targets the planner base value type (kept planner-typed so ExpandDecimal
        // can still lower a decimal conversion).
        TTypePtr tt = target;
        e = BuildNullStrict({{operand, true}},
            [tt, node](std::vector<TExprPtr> a) {
                return std::make_shared<TCastExpr>(node->Location, a[0], tt);
            }, target, counter, node->Location);
        return std::make_shared<TNullable>(target);
    }

    if (auto cl = TMaybeNode<TCallExpr>(e)) {
        auto node = cl.Cast();
        std::string name;
        if (auto id = TMaybeNode<TIdentExpr>(node->Callee)) {
            name = id.Cast()->Name;
        }
        if (name == "qdb_string_view_sql_like" && node->Args.size() == 2) {
            if (auto* literal = MutableStringLiteral(node->Args[1])) {
                if (auto fast = ClassifyLikePattern(literal->Value)) {
                    literal->Value = std::move(fast->Literal);
                    node->Callee = std::make_shared<TIdentExpr>(
                        node->Location, fast->Func);
                    name = fast->Func;
                }
            }
        }
        if (name == "coalesce") {
            auto [expr, type] = ExpandCoalesce(*node, inputType, counter);
            e = expr;
            return type;
        }
        if (name == "qdb_in_list") {
            auto [expr, type] = ExpandInList(*node, inputType, counter);
            e = expr;
            return type;
        }
        if (name == "strcat") {
            // `strcat(a, b)` (parser lowering of `a || b`) -> the runtime call
            // `qdb_string_concat(__arena__, a, b)`. __arena__ is a placeholder the
            // codegen binds to the kernel's scratch arena. Null-strict: NULL if either
            // operand is NULL (SQL `NULL || x = NULL`).
            std::vector<std::pair<TExprPtr, bool>> ops;
            for (auto& arg : node->Args) {
                auto t = Expand(arg, inputType, counter);
                ops.emplace_back(arg, !t || IsNullableType(t));
            }
            TLocation loc = node->Location;
            auto apply = [loc](std::vector<TExprPtr> a) -> TExprPtr {
                std::vector<TExprPtr> callArgs = { std::make_shared<TIdentExpr>(loc, "__arena__") };
                for (auto& x : a) callArgs.push_back(std::move(x));
                return std::make_shared<TCallExpr>(loc,
                    std::make_shared<TIdentExpr>(loc, "qdb_string_concat"), std::move(callArgs));
            };
            TTypePtr R = std::make_shared<TStringType>();
            bool anyNull = false;
            for (const auto& [expr, nullable] : ops) anyNull = anyNull || nullable;
            if (!anyNull) {
                e = apply({node->Args[0], node->Args[1]});
                return R;
            }
            e = BuildNullStrict(ops, apply, R, counter, loc);
            return std::make_shared<TNullable>(R);
        }
        std::vector<TTypePtr> argTypes;
        for (auto& arg : node->Args) {
            argTypes.push_back(Expand(arg, inputType, counter));
        }
        if (name == "qdb_is_true" && node->Args.size() == 1) {
            node->Args[0] = TypeNullPredicate(node->Args[0]);
        }
        if (name == "qdb_is_null" || name == "qdb_is_true") {
            // Non-null bool. The arguments are expanded already: typing them again with
            // InferType would not see the temporaries the expansion introduced.
            return std::make_shared<TBoolType>();
        }
        if (IsExcludedCall(name)) {
            return InferType(e, inputType);
        }
        bool anyNull = false;
        for (const auto& t : argTypes) {
            anyNull = anyNull || !t || IsNullableType(t);
        }
        if (auto value = NumericPreservingUnaryCall(name, argTypes)) {
            if (!anyNull) {
                return value;
            }
            std::vector<std::pair<TExprPtr, bool>> operands;
            for (size_t i = 0; i < node->Args.size(); ++i) {
                operands.emplace_back(node->Args[i], !argTypes[i] || IsNullableType(argTypes[i]));
            }
            e = BuildNullStrict(operands,
                [node](std::vector<TExprPtr> a) {
                    return std::make_shared<TCallExpr>(
                        node->Location, node->Callee, std::move(a));
                }, value, counter, node->Location);
            return std::make_shared<TNullable>(value);
        }
        TTypePtr ret = CallReturnType(*node, name, argTypes);
        TTypePtr R = UnwrapNullableType(ret);
        if (!anyNull) {
            return ret;
        }
        std::vector<std::pair<TExprPtr, bool>> operands;
        for (size_t i = 0; i < node->Args.size(); ++i) {
            operands.emplace_back(node->Args[i], !argTypes[i] || IsNullableType(argTypes[i]));
        }
        e = BuildNullStrict(operands,
            [node](std::vector<TExprPtr> a) {
                return std::make_shared<TCallExpr>(node->Location, node->Callee, std::move(a));
            }, R, counter, node->Location);
        return std::make_shared<TNullable>(R);
    }

    if (TMaybeNode<TIdentExpr>(e) || TMaybeNode<TNumberExpr>(e) ||
        TMaybeNode<TStringLiteralExpr>(e))
    {
        return InferType(e, inputType); // leaves - no rewrite
    }

    // Anything else (field access already produced by this pass, etc.): recurse into
    // children to rewrite nested nullables, then type by rule.
    for (auto* child : e->MutableChildren()) {
        Expand(*child, inputType, counter);
    }
    return InferType(e, inputType);
}

using TTypeEnv = std::unordered_map<std::string, TTypePtr>;

TTypePtr LookupIdentType(const std::string& name, const TStructType& schema,
    const TTypeEnv& env)
{
    if (auto it = env.find(name); it != env.end()) {
        return it->second;
    }
    for (const auto& [fieldName, type] : schema.Fields) {
        if (fieldName == name) {
            return type;
        }
    }
    return nullptr;
}

TExprPtr Int64(TLocation loc, int64_t value) {
    auto n = std::make_shared<TNumberExpr>(loc, value);
    n->Type = std::make_shared<TIntegerType>();
    return n;
}

TExprPtr Call(const std::string& name, std::vector<TExprPtr> args, TLocation loc) {
    return std::make_shared<TCallExpr>(loc, Ident(name, loc), std::move(args));
}

TExprPtr CastToI64(TExprPtr expr, TLocation loc) {
    return std::make_shared<TCastExpr>(loc, std::move(expr),
        std::make_shared<TIntegerType>());
}

TExprPtr DecimalScaleLiteral(TLocation loc, int32_t scale) {
    return Int64(loc, static_cast<int64_t>(scale));
}

TExprPtr DecimalValueAtScale(TExprPtr expr, const TTypePtr& type, int32_t targetScale,
    TLocation loc)
{
    TTypePtr valueType = ValueType(type);
    if (auto decimal = DecimalSpecOfValueType(valueType)) {
        if (decimal->Scale == targetScale) {
            return expr;
        }
        if (decimal->Scale > targetScale) {
            throw NQumir::TError(
                "decimal scale down cast is not implemented: " +
                std::to_string(decimal->Scale) + " -> " + std::to_string(targetScale));
        }
        return Call("qdb_decimal_scale_up", {
            std::move(expr),
            DecimalScaleLiteral(loc, targetScale - decimal->Scale),
        }, loc);
    }
    valueType = UnwrapNamedType(valueType);
    if (IsInt(valueType)) {
        return Call("qdb_decimal_from_i64", {
            CastToI64(std::move(expr), loc),
            DecimalScaleLiteral(loc, targetScale),
        }, loc);
    }
    if (IsFloat(valueType)) {
        return Call("qdb_decimal_from_f64", {
            std::move(expr),
            DecimalScaleLiteral(loc, targetScale),
        }, loc);
    }
    throw NQumir::TError(
        "cannot convert type '" + (type ? TypeDiagnosticName(type) : std::string("unknown")) +
        "' to decimal");
}

int32_t DecimalCommonScale(const TTypePtr& left, const TTypePtr& right,
    const TDecimalSpec& result)
{
    auto dl = DecimalSpecOfValueType(ValueType(left));
    auto dr = DecimalSpecOfValueType(ValueType(right));
    int32_t scale = result.Scale;
    if (dl) {
        scale = std::max(scale, dl->Scale);
    }
    if (dr) {
        scale = std::max(scale, dr->Scale);
    }
    return scale;
}

TTypePtr ExpandDecimalNode(TExprPtr& e, const TStructType& inputType, TTypeEnv& env);

TTypePtr ExpandDecimalBlock(TBlockExpr& node, const TStructType& inputType, TTypeEnv& env) {
    TTypePtr last;
    TTypeEnv local = env;
    for (auto& stmt : node.Stmts) {
        if (auto var = TMaybeNode<TVarStmt>(stmt)) {
            TTypePtr initType;
            if (var.Cast()->Init) {
                initType = ExpandDecimalNode(var.Cast()->Init, inputType, local);
            }
            TTypePtr varType = var.Cast()->Type ? FromQumirType(var.Cast()->Type) : initType;
            if (varType) {
                local[var.Cast()->Name] = varType;
            }
            last = varType;
            continue;
        }
        if (auto assign = TMaybeNode<TAssignExpr>(stmt)) {
            last = ExpandDecimalNode(assign.Cast()->Value, inputType, local);
            if (last) {
                local[assign.Cast()->Name] = last;
            }
            continue;
        }
        last = ExpandDecimalNode(stmt, inputType, local);
    }
    return last;
}

TTypePtr ExpandDecimalNode(TExprPtr& e, const TStructType& inputType, TTypeEnv& env) {
    if (!e || IsNullLiteral(e)) {
        return nullptr;
    }
    if (TMaybeNode<TStringLiteralExpr>(e)) {
        return std::make_shared<TStringType>();
    }
    if (auto num = TMaybeNode<TNumberExpr>(e)) {
        return num.Cast()->Type ? num.Cast()->Type
            : std::static_pointer_cast<TType>(std::make_shared<TIntegerType>());
    }
    if (auto id = TMaybeNode<TIdentExpr>(e)) {
        return LookupIdentType(id.Cast()->Name, inputType, env);
    }
    if (auto fa = TMaybeNode<TFieldAccessExpr>(e)) {
        auto node = fa.Cast();
        TTypePtr objectType = ExpandDecimalNode(node->Object, inputType, env);
        if (objectType && IsNullableType(objectType)) {
            if (node->FieldName == "Value") {
                return UnwrapNullableType(objectType);
            }
            if (node->FieldName == "Valid") {
                return std::make_shared<TBoolType>();
            }
        }
        auto structType = TMaybeType<TStructType>(UnwrapNamedType(objectType));
        if (structType) {
            for (const auto& [name, type] : structType.Cast()->Fields) {
                if (name == node->FieldName) {
                    return type;
                }
            }
        }
        return nullptr;
    }
    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node = ifp.Cast();
        ExpandDecimalNode(node->Cond, inputType, env);
        TTypePtr thenT = ExpandDecimalNode(node->Then, inputType, env);
        TTypePtr elseT = node->Else ? ExpandDecimalNode(node->Else, inputType, env) : nullptr;
        TTypePtr result = UnifyBranchTypes(thenT, elseT);
        if (auto decimal = DecimalSpecOfValueType(result)) {
            node->Then = DecimalValueAtScale(node->Then, thenT, decimal->Scale, node->Location);
            if (node->Else) {
                node->Else = DecimalValueAtScale(node->Else, elseT, decimal->Scale, node->Location);
            }
        }
        bool nullable = !thenT || !elseT || IsNullableType(thenT) || IsNullableType(elseT);
        return result && nullable
            ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(result))
            : result;
    }
    if (auto bp = TMaybeNode<TBinaryExpr>(e)) {
        auto node = bp.Cast();
        TTypePtr lt = ExpandDecimalNode(node->Left, inputType, env);
        TTypePtr rt = ExpandDecimalNode(node->Right, inputType, env);
        const auto dl = DecimalSpecOfValueType(ValueType(lt));
        const auto dr = DecimalSpecOfValueType(ValueType(rt));
        if (!dl && !dr) {
            return Propagate({lt, rt},
                BinaryValueType(node->Operator, node->Left, node->Right, lt, rt));
        }
        TTypePtr result = BinaryValueType(node->Operator, node->Left, node->Right, lt, rt);
        if (IsComparison(node->Operator)) {
            TDecimalSpec spec = dl ? *dl : *dr;
            const int32_t scale = DecimalCommonScale(lt, rt, spec);
            node->Left = DecimalValueAtScale(node->Left, lt, scale, node->Location);
            node->Right = DecimalValueAtScale(node->Right, rt, scale, node->Location);
            return Propagate({lt, rt}, result);
        }
        auto resultDecimal = DecimalSpecOfValueType(result);
        if (!resultDecimal) {
            return Propagate({lt, rt}, result);
        }
        if (node->Operator == "+"_op || node->Operator == "-"_op || node->Operator == "%"_op) {
            const int32_t scale = DecimalCommonScale(lt, rt, *resultDecimal);
            node->Left = DecimalValueAtScale(node->Left, lt, scale, node->Location);
            node->Right = DecimalValueAtScale(node->Right, rt, scale, node->Location);
            return Propagate({lt, rt}, result);
        }
        if (node->Operator == "*") {
            if (dl && !dr) {
                node->Left = DecimalValueAtScale(node->Left, lt, dl->Scale, node->Location);
                node->Right = CastToI64(node->Right, node->Location);
                e = Call("qdb_decimal_mul_i64", {
                    std::move(node->Left),
                    std::move(node->Right),
                }, node->Location);
                return Propagate({lt, rt}, result);
            }
            if (!dl && dr) {
                node->Right = DecimalValueAtScale(node->Right, rt, dr->Scale, node->Location);
                node->Left = CastToI64(node->Left, node->Location);
                e = Call("qdb_decimal_mul_i64", {
                    std::move(node->Right),
                    std::move(node->Left),
                }, node->Location);
                return Propagate({lt, rt}, result);
            }
        }
        if (node->Operator == "/") {
            if (dl && dr) {
                node->Left = DecimalValueAtScale(node->Left, lt, dl->Scale, node->Location);
                node->Right = DecimalValueAtScale(node->Right, rt, dr->Scale, node->Location);
                const int32_t dividendScaleDelta =
                    resultDecimal->Scale + dr->Scale - dl->Scale;
                if (dividendScaleDelta < 0) {
                    throw NQumir::TError(
                        "decimal / decimal produced negative dividend scale delta");
                }
                if (dividendScaleDelta > 0) {
                    node->Left = Call("qdb_decimal_scale_up", {
                        std::move(node->Left),
                        DecimalScaleLiteral(node->Location, dividendScaleDelta),
                    }, node->Location);
                }
                e = Call("qdb_decimal_div", {
                    std::move(node->Left),
                    std::move(node->Right),
                }, node->Location);
                return Propagate({lt, rt}, result);
            }
            if (dl && !dr) {
                node->Left = DecimalValueAtScale(node->Left, lt, dl->Scale, node->Location);
                node->Right = CastToI64(node->Right, node->Location);
                e = Call("qdb_decimal_div_i64", {
                    std::move(node->Left),
                    std::move(node->Right),
                }, node->Location);
                return Propagate({lt, rt}, result);
            }
        }
        return Propagate({lt, rt}, result);
    }
    if (auto up = TMaybeNode<TUnaryExpr>(e)) {
        auto node = up.Cast();
        TTypePtr ot = ExpandDecimalNode(node->Operand, inputType, env);
        if (auto decimal = DecimalSpecOfValueType(ValueType(ot))) {
            if (node->Operator == "-") {
                node->Operand = DecimalValueAtScale(
                    node->Operand, ot, decimal->Scale, node->Location);
                e = Call("qdb_decimal_neg", {std::move(node->Operand)}, node->Location);
                return Propagate({ot}, MakeDecimalType(*decimal));
            }
            ThrowDecimalUnsupported(node->Operator, "unary operator is not implemented");
        }
        return Propagate({ot}, ResultTypeUnary(node->Operator, ot));
    }
    if (auto cp = TMaybeNode<TCastExpr>(e)) {
        auto node = cp.Cast();
        TTypePtr ot = ExpandDecimalNode(node->Operand, inputType, env);
        TTypePtr target = FromQumirType(node->Type);
        if (auto nullable = TMaybeType<TNullable>(target)) {
            if (auto decimal = DecimalSpecOfValueType(nullable.Cast()->UnderlyingType)) {
                if (ot && !IsBinIntStorageType(ot)) {
                    node->Operand = DecimalValueAtScale(
                        node->Operand, ot, decimal->Scale, node->Location);
                }
                node->Type = ToQumirType(target);
                return target;
            }
        }
        if (auto decimal = DecimalSpecOfValueType(target)) {
            e = DecimalValueAtScale(node->Operand, ot, decimal->Scale, node->Location);
            return MakeDecimalType(*decimal);
        }
        if (ot && IsFloat(UnwrapNamedType(ValueType(target)))) {
            if (auto decimal = DecimalSpecOfValueType(ValueType(ot))) {
                e = Call("qdb_decimal_to_f64", {
                    std::move(node->Operand),
                    DecimalScaleLiteral(node->Location, decimal->Scale),
                }, node->Location);
            }
        }
        return Propagate({ot}, target);
    }
    if (auto cl = TMaybeNode<TCallExpr>(e)) {
        auto node = cl.Cast();
        std::vector<TTypePtr> argTypes;
        argTypes.reserve(node->Args.size());
        for (auto& arg : node->Args) {
            argTypes.push_back(ExpandDecimalNode(arg, inputType, env));
        }
        if (auto cid = TMaybeNode<TIdentExpr>(node->Callee)) {
            if (cid.Cast()->Name == "qdb_is_null" || cid.Cast()->Name == "qdb_is_true") {
                return std::make_shared<TBoolType>();
            }
            if (cid.Cast()->Name == "coalesce") {
                auto [value, allNullable] = CoalesceValueType(argTypes);
                return value && allNullable
                    ? std::static_pointer_cast<TType>(std::make_shared<TNullable>(value))
                    : value;
            }
            if (auto value = NumericPreservingUnaryCall(cid.Cast()->Name, argTypes)) {
                if (auto decimal = DecimalSpecOfValueType(value)) {
                    node->Args[0] = DecimalValueAtScale(
                        std::move(node->Args[0]), argTypes[0], decimal->Scale, node->Location);
                    e = Call(cid.Cast()->Name, {std::move(node->Args[0])}, node->Location);
                }
                return Propagate({argTypes[0]}, value);
            }
            if (cid.Cast()->Name.rfind("qdb_decimal_", 0) == 0) {
                if (cid.Cast()->Name == "qdb_decimal_lt" ||
                    cid.Cast()->Name == "qdb_decimal_le" ||
                    cid.Cast()->Name == "qdb_decimal_gt" ||
                    cid.Cast()->Name == "qdb_decimal_ge" ||
                    cid.Cast()->Name == "qdb_decimal_eq" ||
                    cid.Cast()->Name == "qdb_decimal_ne")
                {
                    return std::make_shared<TBoolType>();
                }
                return DecimalStorageType();
            }
        }
        TTypePtr ret = InferType(e, inputType);
        return ret;
    }
    if (auto blk = TMaybeNode<TBlockExpr>(e)) {
        return ExpandDecimalBlock(*blk.Cast(), inputType, env);
    }
    if (auto var = TMaybeNode<TVarStmt>(e)) {
        TTypePtr initType;
        if (var.Cast()->Init) {
            initType = ExpandDecimalNode(var.Cast()->Init, inputType, env);
        }
        TTypePtr varType = var.Cast()->Type ? FromQumirType(var.Cast()->Type) : initType;
        if (varType) {
            env[var.Cast()->Name] = varType;
        }
        return varType;
    }
    if (auto assign = TMaybeNode<TAssignExpr>(e)) {
        TTypePtr valueType = ExpandDecimalNode(assign.Cast()->Value, inputType, env);
        if (valueType) {
            env[assign.Cast()->Name] = valueType;
        }
        return valueType;
    }
    for (auto* child : e->MutableChildren()) {
        ExpandDecimalNode(*child, inputType, env);
    }
    return InferType(e, inputType);
}

std::pair<TExprPtr, TTypePtr> ExpandDecimal(const TExprPtr& expr,
    const TStructType& inputType)
{
    TExprPtr e = CloneExpr(expr);
    TTypeEnv env;
    TTypePtr t = ExpandDecimalNode(e, inputType, env);
    return {e, t ? t : InferType(e, inputType)};
}

} // namespace

TTypePtr AnnotateExprType(
    const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    TAnnotationScope scope(context);
    if (!expr) {
        throw NQumir::TError("project type inference: null expression");
    }
    TTypePtr t = InferType(expr, inputType);
    if (!t) {
        throw NQumir::TError("project type inference: could not infer type of '" +
            NQumir::NAst::NCore::PrintAst(expr) + "'");
    }
    return t;
}

TTypePtr EffectiveBinaryNumericType(
    const TExprPtr& left,
    const TExprPtr& right)
{
    if (!left || !right) {
        return nullptr;
    }
    TTypePtr leftType = UnwrapNamedType(ValueType(left->Type));
    TTypePtr rightType = UnwrapNamedType(ValueType(right->Type));
    if (IsInt(leftType) && IsInt(rightType)) {
        if (IntLiteralFits(right, leftType)) {
            rightType = leftType;
        } else if (IntLiteralFits(left, rightType)) {
            leftType = rightType;
        }
    }
    const bool leftNumeric = IsInt(leftType) || IsFloat(leftType);
    const bool rightNumeric = IsInt(rightType) || IsFloat(rightType);
    return leftNumeric && rightNumeric
        ? CommonNumeric(leftType, rightType)
        : nullptr;
}

TTypePtr AnnotateExprTreeTypes(
    const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    TAnnotationScope scope(context);
    if (!expr) {
        throw NQumir::TError("expression type inference: null expression");
    }

    std::function<TTypePtr(const TExprPtr&)> annotate =
        [&](const TExprPtr& node) -> TTypePtr
    {
        for (auto* child : node->MutableChildren()) {
            if (*child) {
                annotate(*child);
            }
        }
        if (auto binary = TMaybeNode<TBinaryExpr>(node)) {
            auto leftType = UnwrapNamedType(ValueType(binary.Cast()->Left->Type));
            auto rightType = UnwrapNamedType(ValueType(binary.Cast()->Right->Type));
            if (IsInt(leftType) && IsInt(rightType)) {
                if (IntLiteralFits(binary.Cast()->Right, leftType)) {
                    binary.Cast()->Right->Type = leftType;
                } else if (IntLiteralFits(binary.Cast()->Left, rightType)) {
                    binary.Cast()->Left->Type = rightType;
                }
            }
        }
        TTypePtr type;
        try {
            type = InferType(node, inputType);
        } catch (const NQumir::TError&) {
            // Row-group pruning types its predicate without the external catalog,
            // where a compiled-module call is unknown. Such a node keeps its type,
            // and pruning treats what it cannot type as unknown.
        }
        if (type) {
            node->Type = type;
        }
        return type ? type : node->Type;
    };
    return annotate(expr);
}

namespace {
// Structural check for a bare NULL (qdb_sql_null) anywhere in the tree.
bool ContainsNull(const TExprPtr& e) {
    if (IsNullLiteral(e)) {
        return true;
    }
    for (const auto& child : e->Children()) {
        if (ContainsNull(child)) return true;
    }
    return false;
}

// Constructs Expand always rewrites regardless of nullability - the fast path must not
// skip Expand when one is present: coalesce -> if-chain, qdb_in_list -> typed
// comparisons, strcat -> qdb_string_concat, qdb_string_view_sql_like -> a constant-pattern
// fast path, a cast to a nullable target -> nullable_from_value (the raw planner
// TNullable won't compile), and a `%` or decimal `/` by what may be zero -> a NULLIF
// guard. Float division keeps the fast path: it has no such rewrite.
bool ContainsMustExpandCall(const TExprPtr& e, const TStructType& inputType) {
    if (!e) return false;
    if (auto call = TMaybeNode<TCallExpr>(e)) {
        if (auto id = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
            const std::string& name = id.Cast()->Name;
            if (name == "coalesce" || name == "qdb_in_list" || name == "strcat" || name == "qdb_string_view_sql_like") {
                return true;
            }
        }
    }
    if (auto cast = TMaybeNode<TCastExpr>(e)) {
        if (IsNullableType(cast.Cast()->Type)) return true;
    }
    if (auto bin = TMaybeNode<TBinaryExpr>(e)) {
        const TOperator op = bin.Cast()->Operator;
        if ((op == "%" || op == "/") && MayBeZero(bin.Cast()->Right)) {
            if (TTypePtr type = InferType(e, inputType); type && IsNullOnZeroDivisor(op, type)) {
                return true;
            }
        }
    }
    for (const auto& child : e->Children()) {
        if (ContainsMustExpandCall(child, inputType)) return true;
    }
    return false;
}

bool ContainsDecimalTypeRef(const TExprPtr& e) {
    if (!e) {
        return false;
    }
    if (IsDecimalType(e->Type)) {
        return true;
    }
    if (auto cast = TMaybeNode<TCastExpr>(e); cast && IsDecimalType(cast.Cast()->Type)) {
        return true;
    }
    for (const auto& child : e->Children()) {
        if (ContainsDecimalTypeRef(child)) {
            return true;
        }
    }
    return false;
}

bool SchemaHasDecimal(const TStructType& inputType) {
    for (const auto& [_, type] : inputType.Fields) {
        if (IsDecimalType(type)) {
            return true;
        }
    }
    return false;
}
} // namespace

std::pair<TExprPtr, TTypePtr> ExpandNullable(
    const TExprPtr& expr,
    const TStructType& inputType,
    const TExternalCatalogSnapshot* externalCatalog)
{
    TExternalCatalogScope scope(externalCatalog);
    // Fast path: with no nullable column and no bare NULL there is nothing to propagate,
    // so leave the expression untouched (non-nullable path stays ordinary).
    bool anyNullable = false;
    for (const auto& [name, type] : inputType.Fields) {
        if (IsNullableType(type)) { anyNullable = true; break; }
    }
    if (!anyNullable && !ContainsNull(expr) && !ContainsMustExpandCall(expr, inputType)) {
        return {expr, InferType(expr, inputType)};
    }
    // Rewrite a clone - this runs at kernel build on the plan's shared expression, which
    // must stay intact (a node may be lowered into more than one kernel).
    TExprPtr e = CloneExpr(expr);
    uint64_t counter = 0; // per-query temp-name counter (unique within this expansion)
    TTypePtr t = Expand(e, inputType, counter);
    return {e, t};
}

std::pair<TExprPtr, TTypePtr> ExpandKernelExpr(const TExprPtr& expr,
    const TStructType& inputType,
    const TExternalCatalogSnapshot* externalCatalog)
{
    TExternalCatalogScope scope(externalCatalog);
    auto nullable = ExpandNullable(expr, inputType, externalCatalog);
    if (!SchemaHasDecimal(inputType) && !ContainsDecimalTypeRef(nullable.first)) {
        return nullable;
    }
    return ExpandDecimal(nullable.first, inputType);
}

std::pair<TExprPtr, TTypePtr> ExpandKernelPredicate(const TExprPtr& expr,
    const TStructType& inputType,
    const TExternalCatalogSnapshot* externalCatalog)
{
    auto expanded = ExpandKernelExpr(expr, inputType, externalCatalog);
    if (IsNullLiteral(expanded.first)) {
        expanded = {
            TypeNullPredicate(expanded.first),
            std::make_shared<TNullable>(std::make_shared<TBoolType>()),
        };
    }
    return expanded;
}

} // namespace NQdb::NKernel
