#include <qdb/kernel/annotate_type.h>

#include <qdb/catalog/external_module.h>
#include <qdb/kernel/cast_rules.h>
#include <qdb/kernel/vm_function.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/plan/types/date.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/type_text.h>
#include <qdb/utils/module_path.h>
#include <qdb/utils/nesting.h>

#include <qumir/error.h>
#include <qumir/parser/core/printer.h>
#include <qumir/frontend/source_module_loader.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace NQdb::NKernel {

using namespace NQumir::NAst;
using namespace NQumir::NAst::NLiterals;

namespace {

thread_local const TAnnotationContext* CurrentAnnotationContext = nullptr;

// A call without a context gets one of its own, and a nested call without a context
// keeps the enclosing one.
class TAnnotationScope {
public:
    TAnnotationScope(NQumir::NAst::TTypeFactory& types, const TAnnotationContext* context)
        : Previous_(CurrentAnnotationContext)
    {
        if (!context && Previous_) {
            return;
        }
        if (!context || !context->Resolver) {
            Owned_ = MakeAnnotationContext(types, context ? context->ExternalCatalog : nullptr);
            context = &Owned_;
        }
        CurrentAnnotationContext = context;
    }

    ~TAnnotationScope() {
        CurrentAnnotationContext = Previous_;
    }

private:
    const TAnnotationContext* Previous_;
    TAnnotationContext Owned_;
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

class TDateEraser {
public:
    explicit TDateEraser(NQumir::NAst::TTypeFactory& types)
        : Types_(types)
    {}

    TTypePtr Erase(const TTypePtr& type) {
        if (!type) {
            return type;
        }
        if (auto it = Erased_.find(type); it != Erased_.end()) {
            return it->second;
        }
        const TTypePtr erased = EraseNew(type);
        Erased_.emplace(type, erased);
        return erased;
    }

    void Erase(const TExprPtr& expr) {
        if (!expr) {
            return;
        }
        expr->Type = Erase(expr->Type);
        if (auto maybeFun = TMaybeNode<TFunDecl>(expr)) {
            auto fun = maybeFun.Cast();
            fun->RetType = Erase(fun->RetType);
            for (auto& generic : fun->GenericParams) {
                generic.ValueType = Erase(generic.ValueType);
            }
        }
        for (auto* child : expr->MutableChildren()) {
            Erase(*child);
        }
    }

    // Whether erasing would change a type of `expr`.
    bool Holds(const TExprPtr& expr) {
        if (!expr) {
            return false;
        }
        if (Erase(expr->Type) != expr->Type) {
            return true;
        }
        if (auto maybeFun = TMaybeNode<TFunDecl>(expr)) {
            auto fun = maybeFun.Cast();
            if (Erase(fun->RetType) != fun->RetType) {
                return true;
            }
        }
        for (auto* child : expr->MutableChildren()) {
            if (Holds(*child)) {
                return true;
            }
        }
        return false;
    }

private:
    // Named types other than Nullable are the kernels' own and hold no DATE.
    TTypePtr EraseNew(const TTypePtr& type) {
        if (const auto* storage = DateStorageType(type)) {
            return storage;
        }
        if (IsNullableType(type)) {
            return MakeNullableType(Types_, Erase(UnwrapNullableType(type)));
        }
        if (auto structure = TMaybeType<TStructType>(type)) {
            auto fields = structure.Cast()->Fields;
            for (auto& [_, field] : fields) {
                field = Erase(field);
            }
            return Types_.MakeStruct(std::move(fields));
        }
        if (auto maybeFunction = TMaybeType<TFunctionType>(type)) {
            auto function = maybeFunction.Cast();
            auto params = function->ParamTypes;
            for (auto& param : params) {
                param = Erase(param);
            }
            return Types_.MakeFunction(std::move(params), Erase(function->ReturnType));
        }
        if (auto pointer = TMaybeType<TPointerType>(type)) {
            return Types_.MakePointer(Erase(pointer.Cast()->PointeeType));
        }
        if (auto reference = TMaybeType<TReferenceType>(type)) {
            return Types_.MakeReference(Erase(reference.Cast()->ReferencedType));
        }
        if (auto maybeArray = TMaybeType<TArrayType>(type)) {
            auto array = maybeArray.Cast();
            return Types_.MakeArray(Erase(array->ElementType), array->Arity);
        }
        return type;
    }

    NQumir::NAst::TTypeFactory& Types_;
    std::unordered_map<TTypePtr, TTypePtr> Erased_;
};

// Planner column type -> the qumir type the annotator should see. String columns are
// StringView in kernels; nullable columns use qumirdb.oz's Nullable[T] alias so
// operator result types come from the same source-module overloads kernels use.
TTypePtr ToQumirType(NQumir::NAst::TTypeFactory& types, const TTypePtr& type) {
    if (!type) {
        return type;
    }
    if (IsNullableType(type)) {
        return types.MakeNamed(
            "Nullable",
            nullptr,
            std::vector<TGenericArg>{
                TGenericArg::TypeArg(ToQumirType(types, UnwrapNullableType(type)))});
    }
    if (auto decimal = DecimalSpecOfValueType(type)) {
        return DecimalStorageType(types, *decimal);
    }
    if (const auto* date = DateStorageType(type)) {
        return date;
    }
    if (TMaybeType<TStringType>(type)) {
        return NQumir::NRegistry::QumirDbNamedType(types, "StringView");
    }
    return type;
}

// Annotator result -> planner type. Map qumirdb.oz source-module aliases back to
// planner types.
TTypePtr FromQumirType(NQumir::NAst::TTypeFactory& types, const TTypePtr& type) {
    TTypePtr t = type;
    if (IsDecimalType(t) || DateStorageType(t)) {
        return t;
    }
    if (auto named = TMaybeType<TNamedType>(t);
        named && named.Cast()->Name == "Nullable" && !named.Cast()->TypeArgs.empty())
    {
        return MakeNullableType(types, FromQumirType(types, named.Cast()->TypeArgs.front().Type));
    }
    if (auto named = TMaybeType<TNamedType>(t); named && named.Cast()->Name == "StringView") {
        return types.Make<TStringType>();
    }
    // A `cast(... as date)` target and the qumirdb.oz DATE carry no argument, so map
    // them explicitly. DATE is an i32 to kernels; TIMESTAMP is plain i32.
    if (auto named = TMaybeType<TNamedType>(t); named && named.Cast()->Name == "DATE") {
        return MakeDateType(types);
    }
    if (auto named = TMaybeType<TNamedType>(t); named && named.Cast()->Name == "TIMESTAMP") {
        return types.MakeInteger(TIntegerType::I32);
    }
    return UnwrapNamedValueType(UnwrapNullableType(t));
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
    TTypingContext(
        NQumir::NAst::TTypeFactory& types,
        std::shared_ptr<const TExternalCatalogSnapshot> externalCatalog)
        : Frontend_(types, {NUtils::ModuleFile("qumirdb.oz")})
        , ExternalCatalog_(std::move(externalCatalog))
    {
        if (ExternalCatalog_) {
            ExternalModuleNames_ = ExternalCatalog_->ModuleNames();
            if (auto error = Frontend_.RegisterExternalDeclarations(
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
            params.push_back(NQumir::MakeIntrusive<TVarStmt>(loc, name, ToQumirType(Frontend_.Types(), type)));
        }

        // Annotation writes ->Type in place, so type a clone, not the plan's node.
        auto cloned = CloneExpr(expr);
        EraseDateTypes(Frontend_.Types(), cloned);
        // Unique names also make diagnostics and any resolver-owned generated
        // symbols unambiguous across independent annotation calls.
        const std::string functionName =
            Frontend_.UniqueName("__annotate_");
        const std::string resultName = "__result_" + functionName;
        auto resultVar = NQumir::MakeIntrusive<TVarStmt>(loc, resultName, nullptr);
        resultVar->Init = cloned;
        auto body = NQumir::MakeIntrusive<TBlockExpr>(
            loc, std::vector<TExprPtr>{resultVar});
        auto fn = NQumir::MakeIntrusive<TFunDecl>(
            loc, functionName, std::vector<TGenericParam>{},
            std::move(params), body, Frontend_.Types().Make<TVoidType>());
        std::vector<TExprPtr> statements{
            NQumir::MakeIntrusive<TUseExpr>(loc, "qumirdb")};
        for (const auto& moduleName : ExternalModuleNames_) {
            statements.push_back(NQumir::MakeIntrusive<TUseExpr>(loc, moduleName));
        }
        statements.push_back(fn);
        TExprPtr module = NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(statements));

        auto transformed = Frontend_.Transform(std::move(module));
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
            args.push_back(NQumir::MakeIntrusive<TIdentExpr>(loc, argName));
        }
        auto call = NQumir::MakeIntrusive<TCallExpr>(
            loc, NQumir::MakeIntrusive<TIdentExpr>(loc, name), std::move(args));
        return FromQumirType(Frontend_.Types(), Annotate(call, *Frontend_.Types().MakeStruct(std::move(fields))));
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
        TExprPtr module = NQumir::MakeIntrusive<TBlockExpr>(loc,
            std::vector<TExprPtr>{NQumir::MakeIntrusive<TUseExpr>(loc, "qumirdb")});
        auto composed = Frontend_.Compose(std::move(module));
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
            ExternReturns_.emplace(fn.Cast()->Name, FromQumirType(Frontend_.Types(), fn.Cast()->RetType));
        }
        for (auto* child : node->MutableChildren()) {
            CollectReturns(*child);
        }
    }

    TVmFrontendContext Frontend_;
    std::optional<std::string> LoadError_;
    bool ExternsBuilt_ = false;
    std::unordered_map<std::string, TTypePtr> ExternReturns_;
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog_;
    std::vector<std::string> ExternalModuleNames_;
};

} // namespace

struct TExternalTypingResolver::TImpl {
    TImpl(NQumir::NAst::TTypeFactory& types, std::shared_ptr<const TExternalCatalogSnapshot> catalog)
        : Types(types)
        , ExternalCatalog(std::move(catalog))
    {
    }

    TTypingContext& Typing() {
        if (!Context) {
            Context.emplace(Types, ExternalCatalog);
        }
        return *Context;
    }

    NQumir::NAst::TTypeFactory& Types;
    std::shared_ptr<const TExternalCatalogSnapshot> ExternalCatalog;
    std::optional<TTypingContext> Context;
};

TExternalTypingResolver::TExternalTypingResolver(
    NQumir::NAst::TTypeFactory& types,
    std::shared_ptr<const TExternalCatalogSnapshot> catalog)
    : Impl_(std::make_shared<TImpl>(types, std::move(catalog)))
{
}

TExternalTypingResolver::~TExternalTypingResolver() = default;

TTypePtr TExternalTypingResolver::ResolveCall(
    const std::string& name,
    const std::vector<TTypePtr>& argTypes)
{
    auto returnType = Impl_->Typing().ResolveCall(name, argTypes);
    if (Impl_->ExternalCatalog) {
        Impl_->ExternalCatalog->RememberResolvedReturnType(
            name, argTypes, returnType);
    }
    return returnType;
}

TTypePtr TExternalTypingResolver::QumirdbReturnType(const std::string& name) {
    return Impl_->Typing().ExternReturnType(name);
}

TTypePtr EraseDateType(NQumir::NAst::TTypeFactory& types, const TTypePtr& type) {
    return TDateEraser(types).Erase(type);
}

void EraseDateTypes(NQumir::NAst::TTypeFactory& types, const TExprPtr& ast) {
    TDateEraser(types).Erase(ast);
}

TAnnotationContext MakeAnnotationContext(
    NQumir::NAst::TTypeFactory& types,
    std::shared_ptr<const TExternalCatalogSnapshot> catalog)
{
    auto resolver = std::make_shared<TExternalTypingResolver>(types, catalog);
    return {.ExternalCatalog = std::move(catalog), .Resolver = std::move(resolver)};
}

namespace {

// Remembered and exact catalog declarations first; otherwise the Qumir annotator
// resolves the overload, as for a coercion.
TTypePtr ExternalReturnType(
    const std::string& name,
    const std::vector<TTypePtr>& argTypes)
{
    const auto& catalog = CurrentAnnotationContext->ExternalCatalog;
    if (!catalog || !catalog->HasFunction(name)) {
        return nullptr;
    }
    if (auto resolved = catalog->ResolveReturnType(name, argTypes); resolved && *resolved) {
        return **resolved;
    }
    return CurrentAnnotationContext->Resolver->ResolveCall(name, argTypes);
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
    if (auto type = CurrentAnnotationContext->Resolver->QumirdbReturnType(name)) {
        return type;
    }
    throw NQumir::TError(call.Location, "unknown function: " + name);
}

using NQumir::TLocation;

// --- small AST builders ---------------------------------------------------------
TExprPtr Ident(const std::string& n, TLocation loc) {
    return NQumir::MakeIntrusive<TIdentExpr>(loc, n);
}
// Access `.Value`/`.Valid` of a (freshly cloned) nullable operand. Cloning keeps the
// AST a tree - the operand is referenced from both the guard and the result, so a shared
// node would be annotated/lowered twice.
TExprPtr FieldOf(const TExprPtr& obj, const char* field, TLocation loc) {
    return NQumir::MakeIntrusive<TFieldAccessExpr>(loc, CloneExpr(obj), field);
}
TExprPtr BoolLit(NQumir::NAst::TTypeFactory& types, bool v, TLocation loc) {
    return NQumir::MakeIntrusive<TNumberExpr>(loc, v, types);
}
TExprPtr Bin(const char* op, TExprPtr a, TExprPtr b, TLocation loc) {
    return NQumir::MakeIntrusive<TBinaryExpr>(loc, TOperator(std::string_view(op)),
        std::move(a), std::move(b));
}
TExprPtr Not(TExprPtr a, TLocation loc) {
    return NQumir::MakeIntrusive<TUnaryExpr>(loc, TOperator(std::string_view("!")), std::move(a));
}
TExprPtr MakeNull(TLocation loc) {
    return NQumir::MakeIntrusive<TCallExpr>(loc, Ident("qdb_sql_null", loc), std::vector<TExprPtr>{});
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

// Whether every value of `e` derives from bare NULLs: a CASE/`if`, COALESCE or operator
// over such values only is NULL on every row.
bool IsNullOnly(const TExprPtr& e) {
    if (IsNullLiteral(e)) {
        return true;
    }
    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        return IsNullOnly(ifp.Cast()->Then) && IsNullOnly(ifp.Cast()->Else);
    }
    if (auto un = TMaybeNode<TUnaryExpr>(e)) {
        return IsNullOnly(un.Cast()->Operand);
    }
    if (auto bin = TMaybeNode<TBinaryExpr>(e)) {
        return IsNullOnly(bin.Cast()->Left) && IsNullOnly(bin.Cast()->Right);
    }
    if (auto call = TMaybeNode<TCallExpr>(e)) {
        auto id = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
        return id && id.Cast()->Name == "coalesce" &&
            std::ranges::all_of(call.Cast()->Args, IsNullOnly);
    }
    return false;
}

// An untyped NULL is a NULL-only expression that no operand gives a type (`NULL`,
// `CASE WHEN c THEN NULL END`, `NULL + NULL`). The context that consumes it supplies one.
bool IsUntypedNull(const TExprPtr& e, const TTypePtr& type) {
    return !type && IsNullOnly(e);
}

// A value context with nothing to type an untyped NULL (`SELECT NULL AS x`) gives it
// INTEGER, the type DuckDB resolves such a column to: every value is NULL, and a plain
// fixed-width column composes with the operators and outputs downstream.
TTypePtr UntypedNullValueType(NQumir::NAst::TTypeFactory& types) {
    return types.MakeInteger(TIntegerType::I32);
}

// Calls whose result is NOT null-strict (never wrapped by the pass): they return a
// non-null bool or have bespoke NULL semantics handled in qumirdb.oz / elsewhere.
// SimplifyOuterJoins relies on this list: every call outside it is assumed null-strict.
// A future non-strict function (one that can return non-NULL on a NULL argument, such as
// concat, greatest, least) must be added here, or the pass will drop rows.
bool IsExcludedCall(const std::string& name) {
    return name == "qdb_is_null" || name == "qdb_is_true" ||
           name == "coalesce" || name == "nullif" || name == "nvl" || name == "ifnull";
}

// SQL string functions that produce new bytes: `strcat` (the parser's `a || b`),
// `upper` and `lower` become a runtime call taking the kernel's scratch arena first.
const char* ArenaStringFunction(const std::string& name) {
    if (name == "strcat") {
        return "qdb_string_concat";
    }
    if (name == "upper") {
        return "qdb_string_upper";
    }
    if (name == "lower") {
        return "qdb_string_lower";
    }
    return nullptr;
}

bool IsComparison(TOperator op) {
    return op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=";
}
bool IsLogical(TOperator op) { return op == "&&" || op == "||"; }

TTypePtr ValueType(const TTypePtr& t) { return t ? UnwrapNullableType(t) : t; }
// The type a value cast targets before ExpandDecimal runs. A decimal keeps its precision
// and scale: its erased storage type could not tell the decimal pass which scale to
// convert the operand to.
TTypePtr CastTargetQ(NQumir::NAst::TTypeFactory& types, const TTypePtr& value) {
    return IsDecimalType(value) ? value : ToQumirType(types, value);
}

TTypePtr NullableQ(NQumir::NAst::TTypeFactory& types, const TTypePtr& value) {
    auto nullable = MakeNullableType(types, value);
    return IsDecimalType(value)
        ? nullable
        : ToQumirType(types, nullable);
}

// An expanded untyped NULL becomes the NULL of the value type its context gives it. A
// predicate is a boolean context, so its untyped NULL is NULL[bool] (`WHERE NULL`,
// `CASE WHEN NULL`): the kernel then tests it like any other nullable predicate.
std::pair<TExprPtr, TTypePtr> TypeUntypedNull(
    NQumir::NAst::TTypeFactory& types,
    std::pair<TExprPtr, TTypePtr> expanded,
    const TTypePtr& value)
{
    if (!IsUntypedNull(expanded.first, expanded.second)) {
        return expanded;
    }
    const TLocation loc = expanded.first->Location;
    return {
        NQumir::MakeIntrusive<TCastExpr>(loc, MakeNull(loc), NullableQ(types, value)),
        MakeNullableType(types, value),
    };
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
TTypePtr Propagate(NQumir::NAst::TTypeFactory& types, std::initializer_list<TTypePtr> operands, const TTypePtr& R) {
    if (!R) {
        return R;
    }
    bool nullable = IsNullableType(R);
    for (const auto& t : operands) {
        nullable = nullable || !t || IsNullableType(t);
    }
    TTypePtr base = UnwrapNullableType(R);
    return nullable ? MakeNullableType(types, base) : base;
}

bool IsInt(const TTypePtr& t) {
    return static_cast<bool>(MaybeIntegerType(t));
}
bool IsFloat(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TFloatType>(t)); }

// A number a DATE does not mix with: a plain integer, a float or a decimal.
bool IsNumber(const TTypePtr& type) {
    const TTypePtr value = UnwrapNamedValueType(type);
    return (IsInt(value) && !IsDateType(value)) || IsFloat(value) || DecimalSpecOfValueType(value);
}

bool IsReal(const TTypePtr& t) {
    auto floating = TMaybeType<TFloatType>(t);
    return floating && floating.Cast()->Kind == TFloatType::F32;
}

// One operand REAL and the other another number (value types). PostgreSQL has REAL
// operators only for two REAL operands and widens REAL to DOUBLE PRECISION otherwise.
bool MixesReal(const TTypePtr& a, const TTypePtr& b) {
    auto isNumber = [](const TTypePtr& t) {
        return t && (DecimalSpecOfValueType(t) || IsInt(UnwrapNamedValueType(t))
            || IsFloat(UnwrapNamedValueType(t)));
    };
    return isNumber(a) && isNumber(b) && IsReal(UnwrapNamedValueType(a)) != IsReal(UnwrapNamedValueType(b));
}

// Whether `e` is an integer literal whose value `type` can represent (Qumir's
// IntegerLiteralFits). Such a literal adopts the other operand's integer type; one that
// does not fit keeps i64 and widens the operation.
bool IntLiteralFits(const TExprPtr& e, const TTypePtr& type) {
    auto n = TMaybeNode<TNumberExpr>(e);
    auto integer = MaybeIntegerType(type);
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
    value = UnwrapNamedValueType(value);
    if (IsInt(value) || IsFloat(value)) {
        return value;
    }
    return nullptr;
}

// Mirrors qumir NTypeAnnotation::CommonNumericType + WideningIntOK for the types qdb sees
// (all ints signed): two floats give the wider, a float with an integer the float, two ints
// the wider. TODO(reuse): call qumir's own BinaryNumericResultType once it is exposed (see
// NULL_PROPAGATION_PASS_PLAN.md).
TTypePtr CommonNumeric(const TTypePtr& a, const TTypePtr& b) {
    auto af = TMaybeType<TFloatType>(a);
    auto bf = TMaybeType<TFloatType>(b);
    if (af && bf) {
        return af.Cast()->BitWidth() >= bf.Cast()->BitWidth() ? a : b;
    }
    if (af || bf) {
        return af ? a : b;
    }
    auto ai = MaybeIntegerType(a);
    auto bi = MaybeIntegerType(b);
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

// The digits of a product add, and so do the scales. The precision is capped at the
// 38 digits of i128 (the product can then overflow); a scale above 38 has no type.
TDecimalSpec DecimalProductSpec(TOperator op, const TDecimalSpec& left, const TDecimalSpec& right) {
    const int32_t scale = left.Scale + right.Scale;
    if (scale > MaxDecimalPrecision) {
        throw NQumir::TError("decimal operator '" + op.ToString() + "' needs scale " +
            std::to_string(scale) + ", above the DECIMAL maximum of " +
            std::to_string(MaxDecimalPrecision) +
            "; cast an operand to a smaller scale or to DOUBLE");
    }
    return {
        .Precision = std::min(MaxDecimalPrecision, left.Precision + right.Precision),
        .Scale = scale,
    };
}

// DECIMAL(38, s) operands leave no room for fractional digits in the quotient, and
// the reduced scale would truncate it below the dividend's own scale (150 / 8 at
// DECIMAL(38,30) gave 18). Such a quotient is a DOUBLE, as in DuckDB. A reduced
// scale that still holds the dividend's scale stays a DECIMAL: the division is then
// exact at that scale, and widening it would overflow the 128-bit dividend.
bool IsDecimalQuotientDouble(const TDecimalSpec& left, const TDecimalSpec& right) {
    return DecimalDivisionSpec(left, right).Scale < left.Scale;
}

} // namespace

std::optional<TDecimalLiteral> DecimalLiteralOf(const TExprPtr& e) {
    bool negative = false;
    TExprPtr inner = e;
    if (auto un = TMaybeNode<TUnaryExpr>(e); un && un.Cast()->Operator == "-") {
        negative = true;
        inner = un.Cast()->Operand;
    }
    auto num = TMaybeNode<TNumberExpr>(inner);
    if (!num || !num.Cast()->IsFloat() || IsReal(num.Cast()->Type)
        || !std::isfinite(num.Cast()->FloatValue))
    {
        return std::nullopt;
    }
    char buf[400];
    auto res = std::to_chars(buf, buf + sizeof(buf), std::fabs(num.Cast()->FloatValue),
        std::chars_format::fixed);
    if (res.ec != std::errc()) {
        return std::nullopt;
    }
    std::string_view text(buf, res.ptr - buf);
    const size_t dot = text.find('.');
    const std::string_view intPart = text.substr(0, dot);
    const std::string_view fracPart = dot == std::string_view::npos
        ? std::string_view() : text.substr(dot + 1);
    const int32_t scale = static_cast<int32_t>(fracPart.size());
    const bool intIsZero = intPart == "0";
    const int32_t precision = intIsZero ? std::max(scale, 1) : static_cast<int32_t>(intPart.size()) + scale;
    if (precision > 18) {
        return std::nullopt;
    }
    int64_t unscaled = 0;
    for (std::string_view part : {intPart, fracPart}) {
        for (char c : part) {
            unscaled = unscaled * 10 + (c - '0');
        }
    }
    return TDecimalLiteral{negative ? -unscaled : unscaled, {.Precision = precision, .Scale = scale}};
}

namespace {

// An integer beside a decimal (an operand of + - * %, a CASE branch, a COALESCE argument)
// counts as DECIMAL(w, 0), w being the digits of its SQL integer type (INTEGER is
// DECIMAL(10, 0)). An integer literal is a decimal of its own digits, like a fractional
// literal (TDecimalLiteral): 100 is DECIMAL(3, 0).
TDecimalSpec IntegerDecimalSpec(TExprPtr e, const TTypePtr& type) {
    if (auto un = TMaybeNode<TUnaryExpr>(e); un && un.Cast()->Operator == "-") {
        e = un.Cast()->Operand;
    }
    if (auto n = TMaybeNode<TNumberExpr>(e); n && !n.Cast()->IsFloat()) {
        const int64_t value = n.Cast()->IntValue;
        uint64_t magnitude = value < 0
            ? uint64_t{0} - static_cast<uint64_t>(value)
            : static_cast<uint64_t>(value);
        int32_t digits = 1;
        while (magnitude >= 10) {
            magnitude /= 10;
            ++digits;
        }
        return {.Precision = digits, .Scale = 0};
    }
    switch (MaybeIntegerType(type).Cast()->BitWidth()) {
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

// A DECIMAL beside a non-literal DOUBLE is an approximate-number operation: as in DuckDB, the
// decimal is converted to DOUBLE and the result is DOUBLE, instead of rounding the DOUBLE to
// the decimal's scale. A fractional literal is not such an operand: it is a decimal of its
// own digits (see TDecimalLiteral), so by now it has a decimal spec.
bool IsDecimalBesideFloat(const std::optional<TDecimalSpec>& dl, const std::optional<TDecimalSpec>& dr,
    const TTypePtr& vl, const TTypePtr& vr)
{
    return (dl && !dr && IsFloat(UnwrapNamedValueType(vr))) || (dr && !dl && IsFloat(UnwrapNamedValueType(vl)));
}

// A typed REAL literal (a folded constant) is a value, not a literal the query wrote.
bool IsFloatLiteral(TExprPtr e) {
    if (auto un = TMaybeNode<TUnaryExpr>(e); un && un.Cast()->Operator == "-") {
        e = un.Cast()->Operand;
    }
    auto num = TMaybeNode<TNumberExpr>(e);
    return num && num.Cast()->IsFloat() && !IsReal(num.Cast()->Type);
}

// IsDecimalBesideFloat for a binary operation. A float literal is a constant, not an
// approximate column: one that DecimalLiteralOf could not read as a decimal (more than 18
// digits) keeps meeting the decimal at the decimal's scale.
bool IsDecimalBesideFloat(const std::optional<TDecimalSpec>& dl, const std::optional<TDecimalSpec>& dr,
    const TTypePtr& vl, const TTypePtr& vr, const TExprPtr& le, const TExprPtr& re)
{
    return IsDecimalBesideFloat(dl, dr, vl, vr) && !IsFloatLiteral(dl ? re : le);
}

TTypePtr DecimalBinaryValueType(NQumir::NAst::TTypeFactory& types, TOperator op, const TExprPtr& le, const TExprPtr& re,
    TTypePtr vl, TTypePtr vr)
{
    auto dl = DecimalSpecOfValueType(vl);
    auto dr = DecimalSpecOfValueType(vr);
    if (dl && !dr && IsFloat(UnwrapNamedValueType(vr))) {
        if (auto lit = DecimalLiteralOf(re)) {
            dr = lit->Spec;
            vr = MakeDecimalType(types, lit->Spec);
        }
    } else if (dr && !dl && IsFloat(UnwrapNamedValueType(vl))) {
        if (auto lit = DecimalLiteralOf(le)) {
            dl = lit->Spec;
            vl = MakeDecimalType(types, lit->Spec);
        }
    }
    if (!dl && !dr) {
        return nullptr;
    }
    if (IsComparison(op)) {
        return types.Make<TBoolType>();
    }
    // DOUBLE % is not implemented (see the `%` check below), so it stays an error.
    if (op != "%" && IsDecimalBesideFloat(dl, dr, vl, vr, le, re)) {
        return types.MakeFloat();
    }
    if (op == "/") {
        if (dl && dr) {
            if (IsDecimalQuotientDouble(*dl, *dr)) {
                return types.MakeFloat();
            }
            return MakeDecimalType(types, DecimalDivisionSpec(*dl, *dr));
        }
        if (dl && !dr && IsInt(vr)) {
            return MakeDecimalType(types, *dl);
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
        return MakeDecimalType(types, {
            .Precision = std::min(MaxDecimalPrecision, intDigits + scale),
            .Scale = scale,
        });
    }
    if (op == "*") {
        if ((!dl && !IsInt(vl)) || (!dr && !IsInt(vr))) {
            ThrowDecimalUnsupported(op, "only decimal * decimal and decimal * integer are implemented");
        }
        const TDecimalSpec l = dl ? *dl : IntegerDecimalSpec(le, vl);
        const TDecimalSpec r = dr ? *dr : IntegerDecimalSpec(re, vr);
        return MakeDecimalType(types, DecimalProductSpec(op, l, r));
    }
    if (op == "+" || op == "-") {
        if (!dl && !IsInt(vl) && !IsFloat(vl)) {
            ThrowDecimalUnsupported(op, "left operand cannot be converted to decimal");
        }
        if (!dr && !IsInt(vr) && !IsFloat(vr)) {
            ThrowDecimalUnsupported(op, "right operand cannot be converted to decimal");
        }
        // A float literal that no decimal holds is converted at the decimal's scale and
        // counts as the decimal's own type.
        auto operandSpec = [](const std::optional<TDecimalSpec>& spec, const TExprPtr& e,
            const TTypePtr& v, const TDecimalSpec& other) -> TDecimalSpec
        {
            if (spec) {
                return *spec;
            }
            return IsInt(v)
                ? IntegerDecimalSpec(e, v)
                : other;
        };
        const TDecimalSpec l = operandSpec(dl, le, vl, dr ? *dr : *dl);
        const TDecimalSpec r = operandSpec(dr, re, vr, dl ? *dl : *dr);
        const int32_t scale = std::max(l.Scale, r.Scale);
        const int32_t intDigits = std::max(
            l.Precision - l.Scale,
            r.Precision - r.Scale);
        const int32_t precision = std::min(MaxDecimalPrecision, intDigits + scale + 1);
        return MakeDecimalType(types, {.Precision = precision, .Scale = scale});
    }
    ThrowDecimalUnsupported(op, "operator is not implemented for decimal values");
}

TTypePtr InferType(NQumir::NAst::TTypeFactory& types, const TExprPtr& e, const TStructType& schema);

// Arithmetic of fractional literals, integers and decimals: PostgreSQL types it NUMERIC, qdb
// DOUBLE. A REAL or DOUBLE operand makes it a float in both, so it is not NUMERIC.
bool IsNumericArithmetic(NQumir::NAst::TTypeFactory& types, const TExprPtr& e, const TStructType& schema) {
    if (IsFloatLiteral(e)) {
        return true;
    }
    if (auto un = TMaybeNode<TUnaryExpr>(e)) {
        return un.Cast()->Operator == "-" && IsNumericArithmetic(types, un.Cast()->Operand, schema);
    }
    if (auto bin = TMaybeNode<TBinaryExpr>(e)) {
        const TOperator op = bin.Cast()->Operator;
        return (op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
            && IsNumericArithmetic(types, bin.Cast()->Left, schema)
            && IsNumericArithmetic(types, bin.Cast()->Right, schema);
    }
    const TTypePtr value = ValueType(InferType(types, e, schema));
    return value && (DecimalSpecOfValueType(value) || IsInt(UnwrapNamedValueType(value)));
}

// Whether PostgreSQL converts the value to REAL in a common type (CASE, COALESCE, IN list):
// a NULL, an integer, a decimal, a REAL, a fractional literal or arithmetic of
// them (qdb types it DOUBLE, PostgreSQL NUMERIC), or a CASE of such values. Without the
// schema only literals are told.
bool AbsorbedByReal(NQumir::NAst::TTypeFactory& types, const TExprPtr& e, const TStructType* schema) {
    if (!e || IsNullLiteral(e) || IsFloatLiteral(e)) {
        return true;
    }
    if (!schema) {
        return false;
    }
    if (IsNumericArithmetic(types, e, *schema)) {
        return true;
    }
    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        return AbsorbedByReal(types, ifp.Cast()->Then, schema) && AbsorbedByReal(types, ifp.Cast()->Else, schema);
    }
    const TTypePtr value = ValueType(InferType(types, e, *schema));
    return !value || DecimalSpecOfValueType(value) || IsInt(UnwrapNamedValueType(value))
        || IsReal(UnwrapNamedValueType(value));
}

// PostgreSQL's common type of REAL with values it converts to REAL is REAL.
bool IsRealCommonType(NQumir::NAst::TTypeFactory& types, const std::vector<TTypePtr>& argTypes, const std::vector<TExprPtr>& exprs,
    const TStructType* schema)
{
    bool real = false;
    for (size_t i = 0; i < argTypes.size(); ++i) {
        const TTypePtr value = ValueType(argTypes[i]);
        if (!value) {
            continue;
        }
        const bool isReal = IsReal(UnwrapNamedValueType(value));
        real = real || isReal;
        if (!isReal && !DecimalSpecOfValueType(value) && !IsInt(UnwrapNamedValueType(value))
            && !AbsorbedByReal(types, exprs[i], schema))
        {
            return false;
        }
    }
    return real;
}

// Common value type of two CASE/`if` branches (SQL branch unification): numeric promotion
// (i32 then, i64 else -> i64), otherwise assume the branches already agree. The branch
// expressions, when given, tell an integer literal beside a decimal (IntegerDecimalSpec)
// and a value REAL absorbs (AbsorbedByReal, which sees CASE chains with the schema).
TTypePtr UnifyBranchTypes(NQumir::NAst::TTypeFactory& types, const TTypePtr& a, const TTypePtr& b,
    const TExprPtr& ea = nullptr, const TExprPtr& eb = nullptr,
    const TStructType* schema = nullptr)
{
    if (!a) return b ? UnwrapNullableType(b) : nullptr;
    if (!b) return UnwrapNullableType(a);
    TTypePtr va = UnwrapNullableType(a), vb = UnwrapNullableType(b);
    if ((IsDateType(va) && IsNumber(vb)) || (IsNumber(va) && IsDateType(vb))) {
        throw NQumir::TError("types " + SqlTypeName(va) + " and " + SqlTypeName(vb) +
            " cannot be matched");
    }
    auto da = DecimalSpecOfValueType(va);
    auto db = DecimalSpecOfValueType(vb);
    if (da || db) {
        // The float keeps its width: REAL absorbs NUMERIC, as in PostgreSQL.
        if (IsDecimalBesideFloat(da, db, va, vb)) {
            return UnwrapNamedValueType(da ? vb : va);
        }
        if (!da && IsInt(va)) {
            da = IntegerDecimalSpec(ea, va);
        }
        if (!db && IsInt(vb)) {
            db = IntegerDecimalSpec(eb, vb);
        }
        if (da && db) {
            const int32_t scale = std::max(da->Scale, db->Scale);
            const int32_t intDigits = std::max(
                da->Precision - da->Scale,
                db->Precision - db->Scale);
            return MakeDecimalType(types, {
                .Precision = std::min(MaxDecimalPrecision, intDigits + scale),
                .Scale = scale,
            });
        }
        return MakeDecimalType(types, da ? *da : *db);
    }
    if (IsDateType(va) && IsDateType(vb)) {
        return va;
    }
    if ((IsInt(va) || IsFloat(va)) && (IsInt(vb) || IsFloat(vb))) {
        if (IsReal(va) && AbsorbedByReal(types, eb, schema)) {
            return va;
        }
        if (IsReal(vb) && AbsorbedByReal(types, ea, schema)) {
            return vb;
        }
        return CommonNumeric(va, vb);
    }
    return va;
}

// COALESCE result: value type (unified over non-NULL args) and whether every arg is
// nullable (a bare NULL arg is ignored). Non-nullable arg => the whole COALESCE is
// non-nullable (it is guaranteed to return that arg's value at worst).
std::pair<TTypePtr, bool> CoalesceValueType(NQumir::NAst::TTypeFactory& types, const std::vector<TTypePtr>& argTypes,
    const std::vector<TExprPtr>& args, const TStructType* schema = nullptr)
{
    if (IsRealCommonType(types, argTypes, args, schema)) {
        bool allNullable = true;
        for (const auto& t : argTypes) {
            allNullable = allNullable && (!t || IsNullableType(t));
        }
        return {types.MakeFloat(TFloatType::F32), allNullable};
    }
    TTypePtr v = nullptr;
    TExprPtr first;
    bool allNullable = true;
    for (size_t i = 0; i < argTypes.size(); ++i) {
        const auto& t = argTypes[i];
        if (!t) continue; // bare NULL - never chosen
        if (v) {
            v = UnifyBranchTypes(types, v, t, first, args[i]);
            first = nullptr;
        } else {
            v = UnwrapNullableType(t);
            first = args[i];
        }
        if (!IsNullableType(t)) allNullable = false;
    }
    return {v, allNullable};
}

// PostgreSQL has only date + integer, integer + date, date - integer (a date) and
// date - date (an integer number of days) for a date operand; the width of a min/max
// DATE (an i64) carries over to the result. Deliberate deviation: any integer width is a
// day count, because qdb types integer literals and integer arithmetic as BIGINT.
TTypePtr DateBinaryValueType(NQumir::NAst::TTypeFactory& types, TOperator op, const TExprPtr& le, const TExprPtr& re,
    TTypePtr vl, TTypePtr vr)
{
    // A literal that fits an INTEGER is one, not a BIGINT.
    const TTypePtr integer = types.MakeInteger(TIntegerType::I32);
    if (!IsDateType(vl) && IntLiteralFits(le, integer)) {
        vl = integer;
    }
    if (!IsDateType(vr) && IntLiteralFits(re, integer)) {
        vr = integer;
    }
    auto leftMaybe = MaybeIntegerType(vl);
    auto rightMaybe = MaybeIntegerType(vr);
    if (leftMaybe && rightMaybe) {
        const auto left = leftMaybe.Cast();
        const auto right = rightMaybe.Cast();
        const bool leftDate = IsDateType(vl);
        const bool rightDate = IsDateType(vr);
        const auto wider = left->BitWidth() > right->BitWidth() ? left->Kind : right->Kind;
        if (leftDate && rightDate && op == "-") {
            return types.MakeInteger(wider);
        }
        if (leftDate != rightDate && (op == "+" || (op == "-" && leftDate))) {
            return MakeDateType(types, wider);
        }
    }
    throw NQumir::TError(le->Location, "operator does not exist: " + SqlTypeName(vl) + " " +
        op.ToString() + " " + SqlTypeName(vr));
}

// The plain (non-null-propagated) result value type of a binary op, matching qumir's
// AnnotateBinary: `/` -> a float (the common one, else DOUBLE), `+ - * %`/bitwise ->
// CommonNumeric, comparisons/logical -> bool. An integer-literal operand adopts the other
// operand's int type when its value fits it (qumir's RetypeIntegerLiteralOperands).
TTypePtr BinaryValueType(NQumir::NAst::TTypeFactory& types, TOperator op, const TExprPtr& le, const TExprPtr& re,
    const TTypePtr& lt, const TTypePtr& rt)
{
    if (IsComparison(op) || IsLogical(op)) {
        return types.Make<TBoolType>();
    }
    TTypePtr vl = UnwrapNamedValueType(ValueType(lt));
    TTypePtr vr = UnwrapNamedValueType(ValueType(rt));
    if (IsDateType(vl) || IsDateType(vr)) {
        return DateBinaryValueType(types, op, le, re, vl, vr);
    }
    if (auto decimal = DecimalBinaryValueType(types, op, le, re, ValueType(lt), ValueType(rt))) {
        return decimal;
    }
    if (op == "+" && (TMaybeType<TStringType>(vl) || TMaybeType<TStringType>(vr))) {
        return types.Make<TStringType>(); // string concatenation
    }
    if (IsInt(vl) && IsInt(vr)) {
        if (IntLiteralFits(re, vl)) vr = vl;
        else if (IntLiteralFits(le, vr)) vl = vr;
    }
    // REAL beside another number computes in DOUBLE PRECISION (CoerceRealOperands).
    if (MixesReal(vl, vr)) {
        return types.MakeFloat(TFloatType::F64);
    }
    if (op == "/") {
        auto common = CommonNumeric(vl, vr);
        return IsFloat(common) ? common : types.MakeFloat();
    }
    return CommonNumeric(vl, vr);
}

// BinaryValueType of an operation with a bare-NULL operand (null type). A null-strict
// operator turns it into NULL whatever the NULL stands for, so the operand only has to give
// the operation a result type. It takes the other operand's type, except next to a decimal
// in `*`: a decimal there would double the precision and scale, so such a NULL is an
// integer, as SQL types a bare NULL.
TTypePtr BinaryValueTypeWithNull(NQumir::NAst::TTypeFactory& types, TOperator op, const TExprPtr& le, const TExprPtr& re,
    const TTypePtr& lt, const TTypePtr& rt)
{
    TTypePtr nullType = lt ? lt : rt;
    if (op == "*" && nullType && DecimalSpecOfValueType(ValueType(nullType))) {
        nullType = types.MakeInteger(TIntegerType::I32);
    }
    return BinaryValueType(types, op, le, re, lt ? lt : nullType, rt ? rt : nullType);
}

// SQL `%` on exact numbers and decimal `/` have no value for a zero divisor, and unlike
// DOUBLE (NaN, infinity) their result types cannot stand for one. They yield NULL, as in
// DuckDB. PostgreSQL raises "division by zero"; qdb has no runtime error channel yet.
bool IsNullOnZeroDivisor(TOperator op, const TTypePtr& result) {
    const TTypePtr value = ValueType(result);
    if (DecimalSpecOfValueType(value)) {
        return op == "%" || op == "/";
    }
    return op == "%" && IsInt(UnwrapNamedValueType(value));
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
TTypePtr InferType(NQumir::NAst::TTypeFactory& types, const TExprPtr& e, const TStructType& schema) {
    if (!e || IsNullLiteral(e)) {
        return nullptr; // bare/typed NULL - the enclosing node supplies the value type
    }
    if (TMaybeNode<TStringLiteralExpr>(e)) {
        return types.Make<TStringType>();
    }
    if (auto bitcast = TMaybeNode<TBitcastExpr>(e)) {
        return bitcast.Cast()->Type;
    }
    if (auto num = TMaybeNode<TNumberExpr>(e)) {
        // The ctor sets ->Type (bool / i64 / float); i32_literal overrides to i32.
        return num.Cast()->Type ? num.Cast()->Type
            : types.MakeInteger();
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
        TTypePtr lt = InferType(types, node->Left, schema);
        TTypePtr rt = InferType(types, node->Right, schema);
        // A bare-NULL operand is typed as Expand types it.
        TTypePtr R = BinaryValueTypeWithNull(types, node->Operator, node->Left, node->Right, lt, rt);
        TTypePtr type = Propagate(types, {lt, rt}, R);
        if (type && IsNullOnZeroDivisor(node->Operator, R) && MayBeZero(node->Right)) {
            return MakeNullableType(types, UnwrapNullableType(type));
        }
        return type;
    }
    if (auto un = TMaybeNode<TUnaryExpr>(e)) {
        auto node =un.Cast();
        TTypePtr ot = InferType(types, node->Operand, schema);
        if (IsDateType(ot)) {
            throw NQumir::TError(node->Location,
                "operator does not exist: " + node->Operator.ToString() + " DATE");
        }
        TTypePtr R = node->Operator == "!"
            ? types.Make<TBoolType>()
            : ValueType(ot);
        return Propagate(types, {ot}, R);
    }
    if (auto cst = TMaybeNode<TCastExpr>(e)) {
        auto node =cst.Cast();
        TTypePtr ot = InferType(types, node->Operand, schema);
        // A cast whose target is itself nullable (e.g. the UNION-branch coercion
        // `cast(i32, Nullable[i32])`) is Nullable[base] regardless of the operand.
        if (IsNullableType(node->Type)) {
            return MakeNullableType(types, UnwrapNullableType(node->Type));
        }
        return Propagate(types, {ot}, FromQumirType(types, node->Type));
    }
    if (auto call = TMaybeNode<TCallExpr>(e)) {
        auto node =call.Cast();
        std::string name;
        if (auto cid = TMaybeNode<TIdentExpr>(node->Callee)) {
            name = cid.Cast()->Name;
        }
        std::vector<TTypePtr> at;
        for (const auto& a : node->Args) {
            at.push_back(InferType(types, a, schema));
        }
        if (name == "qdb_is_null" || name == "qdb_is_true") {
            return types.Make<TBoolType>(); // non-null bool regardless of args
        }
        if (name == "coalesce") {
            auto [v, allNullable] = CoalesceValueType(types, at, node->Args, &schema);
            if (!v) return nullptr;
            return allNullable
                ? MakeNullableType(types, v) : v;
        }
        if (name == "qdb_in_list") {
            bool nullable = false;
            for (const auto& t : at) {
                nullable = nullable || !t || IsNullableType(t);
            }
            auto result = types.Make<TBoolType>();
            return nullable
                ? MakeNullableType(types, result)
                : result;
        }
        if (ArenaStringFunction(name)) {
            if (name != "strcat" &&
                (at.size() != 1 || (at[0] && !TMaybeType<TStringType>(ValueType(at[0])))))
            {
                throw NQumir::TError(name + " expects one string argument");
            }
            bool nullable = false;
            for (const auto& t : at) nullable = nullable || !t || IsNullableType(t);
            TTypePtr s = types.Make<TStringType>();
            return nullable
                ? MakeNullableType(types, s) : s;
        }
        if (auto value = NumericPreservingUnaryCall(name, at)) {
            return Propagate(types, {at[0]}, value);
        }
        TTypePtr ret = CallReturnType(*node, name, at);
        bool nullable = false;
        for (const auto& t : at) nullable = nullable || !t || IsNullableType(t);
        TTypePtr base = UnwrapNullableType(ret);
        return (nullable || IsNullableType(ret))
            ? MakeNullableType(types, base) : base;
    }
    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node =ifp.Cast();
        // The condition does not shape the result type, but it is typed too: it may
        // call an unknown function.
        InferType(types, node->Cond, schema);
        TTypePtr tt = InferType(types, node->Then, schema);
        TTypePtr et = node->Else ? InferType(types, node->Else, schema) : nullptr;
        TTypePtr base = UnifyBranchTypes(types, tt, et, node->Then, node->Else, &schema);
        if (!base) {
            return nullptr;
        }
        bool nullable = !tt || !et || IsNullableType(tt) || IsNullableType(et);
        return nullable ? MakeNullableType(types, base) : base;
    }
    if (auto fa = TMaybeNode<TFieldAccessExpr>(e)) {
        auto node =fa.Cast();
        TTypePtr ot = InferType(types, node->Object, schema);
        if (ot && IsNullableType(ot)) {
            if (node->FieldName == "Value") return UnwrapNullableType(ot);
            if (node->FieldName == "Valid") return types.Make<TBoolType>();
        }
        return nullptr;
    }
    if (auto blk = TMaybeNode<TBlockExpr>(e)) {
        const auto& s = blk.Cast()->Stmts;
        return s.empty() ? nullptr : InferType(types, s.back(), schema);
    }
    return nullptr;
}

TTypePtr ResultTypeUnary(NQumir::NAst::TTypeFactory& types, TOperator op, const TTypePtr& ot) {
    return op == "!" ? types.Make<TBoolType>()
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
    auto var = NQumir::MakeIntrusive<TVarStmt>(loc, name, nullptr);
    var->Init = expr;
    stmts.push_back(var);
    return Ident(name, loc);
}

TExprPtr BuildNullStrict(NQumir::NAst::TTypeFactory& types, const std::vector<std::pair<TExprPtr, bool>>& operands,
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
    auto nq = NullableQ(types, R);
    TExprPtr guard = valids.front();
    for (size_t i = 1; i < valids.size(); ++i) {
        guard = Bin("&&", guard, valids[i], loc);
    }
    TExprPtr thenE = NQumir::MakeIntrusive<TCastExpr>(loc, apply(unwrapped), nq);
    TExprPtr elseE = NQumir::MakeIntrusive<TCastExpr>(loc, MakeNull(loc), nq);
    stmts.push_back(NQumir::MakeIntrusive<TIfExpr>(loc, guard, thenE, elseE));
    return NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(stmts));
}

// SQL 3VL for AND/OR: evaluates both operands; a valid dominating value (FALSE for AND,
// TRUE for OR) wins over NULL. Each operand contributes (Valid, Value) plain bools;
// `ref == nullptr` is a bare NULL, non-nullable is always valid.
TExprPtr OpValid(NQumir::NAst::TTypeFactory& types, const TExprPtr& ref, const TTypePtr& t, TLocation loc) {
    if (!t) return BoolLit(types, false, loc);
    if (IsNullableType(t)) return FieldOf(ref, "Valid", loc);
    return BoolLit(types, true, loc);
}
TExprPtr OpValue(NQumir::NAst::TTypeFactory& types, const TExprPtr& ref, const TTypePtr& t, TLocation loc) {
    if (!t) return BoolLit(types, false, loc);
    if (IsNullableType(t)) return FieldOf(ref, "Value", loc);
    return CloneExpr(ref);
}
TExprPtr Build3VL(NQumir::NAst::TTypeFactory& types, bool isAnd, const TExprPtr& a, const TTypePtr& at,
    const TExprPtr& b, const TTypePtr& bt, uint64_t& counter, TLocation loc)
{
    std::vector<TExprPtr> stmts;
    // Bind nullable operands to temps (read .Valid/.Value repeatedly below).
    TExprPtr ra = (at && IsNullableType(at)) ? BindTemp(a, stmts, counter, loc) : a;
    TExprPtr rb = (bt && IsNullableType(bt)) ? BindTemp(b, stmts, counter, loc) : b;
    auto boolN = NullableQ(types, types.Make<TBoolType>());
    auto nTrue = NQumir::MakeIntrusive<TCastExpr>(loc, BoolLit(types, true, loc), boolN);
    auto nFalse = NQumir::MakeIntrusive<TCastExpr>(loc, BoolLit(types, false, loc), boolN);
    auto nNull = NQumir::MakeIntrusive<TCastExpr>(loc, MakeNull(loc), boolN);
    auto bothValid = Bin("&&", OpValid(types, ra, at, loc), OpValid(types, rb, bt, loc), loc);
    TExprPtr result;
    if (isAnd) {
        auto dominant = Bin("||",
            Bin("&&", OpValid(types, ra, at, loc), Not(OpValue(types, ra, at, loc), loc), loc),
            Bin("&&", OpValid(types, rb, bt, loc), Not(OpValue(types, rb, bt, loc), loc), loc), loc);
        result = NQumir::MakeIntrusive<TIfExpr>(loc, dominant, nFalse,
            NQumir::MakeIntrusive<TIfExpr>(loc, bothValid, nTrue, nNull));
    } else {
        auto dominant = Bin("||",
            Bin("&&", OpValid(types, ra, at, loc), OpValue(types, ra, at, loc), loc),
            Bin("&&", OpValid(types, rb, bt, loc), OpValue(types, rb, bt, loc), loc), loc);
        result = NQumir::MakeIntrusive<TIfExpr>(loc, dominant, nTrue,
            NQumir::MakeIntrusive<TIfExpr>(loc, bothValid, nFalse, nNull));
    }
    stmts.push_back(result);
    return NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(stmts));
}

// The conversion of a plain `value` of type `from` to a cast target: a row of the cast
// table when one covers it, else a Qumir `cast`. `castTarget` is the target as the
// query wrote it (it still tells DATE from i32); `valueType` is the planner value type
// the plain cast targets. CastTargetQ keeps a DECIMAL planner-typed so ExpandDecimal can
// still lower it and spells VARCHAR as StringView: the planner's `string` is not the
// type the `cast` operators of qumirdb.oz produce, so none would be a candidate.
TExprPtr ConvertValue(NQumir::NAst::TTypeFactory& types, const TExprPtr& value, const TTypePtr& from, const TTypePtr& castTarget,
    const TTypePtr& valueType, TLocation loc)
{
    if (auto converted = ApplyCastRule(types, ValueType(from), castTarget, value, loc)) {
        return converted;
    }
    return NQumir::MakeIntrusive<TCastExpr>(loc, value, CastTargetQ(types, valueType));
}

// The type of a cast operand as the query wrote it, when that says more than the planner
// type: the target of an inner cast to DATE, which FromQumirType erases to i32 before an
// outer cast could choose the DATE rule (`CAST(CAST(d AS DATE) AS VARCHAR)`). Taken before
// the operand is expanded, since the expansion replaces the inner cast node. A DATE column
// or literal is a plain i32 to the planner (issue #226) and has no such spelling.
TTypePtr NominalCastType(const TExprPtr& operand) {
    auto cast = TMaybeNode<TCastExpr>(operand);
    if (!cast) {
        return nullptr;
    }
    const TTypePtr target = UnwrapNullableType(cast.Cast()->Type);
    if (auto named = TMaybeType<TNamedType>(target); named && named.Cast()->Name == "DATE") {
        return target;
    }
    return nullptr;
}

// NULLIF(divisor, 0) as Nullable[value]: a zero divisor becomes one more NULL operand of
// the division, so the null-strict guard skips computing it.
TExprPtr NullIfZero(NQumir::NAst::TTypeFactory& types, const TExprPtr& divisor, const TTypePtr& type, uint64_t& counter,
    TLocation loc)
{
    std::vector<TExprPtr> stmts;
    TExprPtr ref = BindTemp(divisor, stmts, counter, loc);
    auto nq = NullableQ(types, ValueType(type));
    TExprPtr nonZero = Bin("!=", OpValue(types, ref, type, loc),
        NQumir::MakeIntrusive<TNumberExpr>(loc, int64_t{0}, types), loc);
    TExprPtr kept = CloneExpr(ref);
    if (IsNullableType(type)) {
        nonZero = Bin("&&", FieldOf(ref, "Valid", loc), nonZero, loc);
    } else {
        kept = NQumir::MakeIntrusive<TCastExpr>(loc, kept, nq);
    }
    stmts.push_back(NQumir::MakeIntrusive<TIfExpr>(loc, nonZero, kept,
        NQumir::MakeIntrusive<TCastExpr>(loc, MakeNull(loc), nq)));
    return NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(stmts));
}

// Coerce an `if`/CASE branch to Nullable[value] so all branches share one type: NULL ->
// null[value]; a plain branch is widened if needed, then wrapped via nullable_from_value;
// an already-nullable branch of a narrower type is widened through a null-strict cast.
TExprPtr CoerceBranch(NQumir::NAst::TTypeFactory& types, const TExprPtr& branch, const TTypePtr& branchType, bool isNull,
    const TTypePtr& value, uint64_t& counter, TLocation loc)
{
    auto nq = NullableQ(types, value);
    if (isNull) {
        return NQumir::MakeIntrusive<TCastExpr>(loc, branch ? branch : MakeNull(loc), nq);
    }
    auto tv = CastTargetQ(types, value);
    if (!branchType || !IsNullableType(branchType)) {
        TExprPtr v = branch;
        if (branchType && PlanTypeKey(branchType) != PlanTypeKey(value)) {
            v = NQumir::MakeIntrusive<TCastExpr>(loc, branch, tv); // widen plain value
        }
        return NQumir::MakeIntrusive<TCastExpr>(v->Location, v, nq); // nullable_from_value
    }
    if (PlanTypeKey(UnwrapNullableType(branchType)) == PlanTypeKey(value)) {
        return branch; // already Nullable[value]
    }
    TExprPtr b = branch; // Nullable but narrower: widen the value under the validity guard
    return BuildNullStrict(types, {{b, true}},
        [tv, loc](std::vector<TExprPtr> a) {
            return NQumir::MakeIntrusive<TCastExpr>(loc, a[0], tv);
        }, value, counter, loc);
}

TTypePtr Expand(NQumir::NAst::TTypeFactory& types, TExprPtr& e, const TStructType& inputType, uint64_t& counter);

// An IN list with this many constants is one lookup in a sorted table: LLVM compiles a
// comparison per item in time superlinear in their number, and from 16 constants on the
// lookup also runs no slower than the comparisons (measured in #606).
constexpr size_t InTableMinItems = 16;

using TNumericConstant = std::variant<int64_t, float, double>;

// A number literal in its own type, through a unary minus and the float casts that
// CoerceRealOperands writes.
std::optional<TNumericConstant> NumericConstant(const TExprPtr& e) {
    if (auto num = TMaybeNode<TNumberExpr>(e)) {
        const TTypePtr type = UnwrapNamedValueType(num.Cast()->Type);
        if (IsReal(type)) {
            return static_cast<float>(num.Cast()->FloatValue);
        }
        if (IsFloat(type)) {
            return num.Cast()->FloatValue;
        }
        if (auto integer = MaybeIntegerType(type); integer && integer.Cast()->IsSigned()) {
            return num.Cast()->IntValue;
        }
        return std::nullopt;
    }
    if (auto un = TMaybeNode<TUnaryExpr>(e); un && un.Cast()->Operator == "-"_op) {
        auto operand = NumericConstant(un.Cast()->Operand);
        if (!operand || *operand == TNumericConstant{std::numeric_limits<int64_t>::min()}) {
            return std::nullopt;
        }
        return std::visit([](auto v) { return TNumericConstant{-v}; }, *operand);
    }
    if (auto cast = TMaybeNode<TCastExpr>(e)) {
        auto target = TMaybeType<TFloatType>(UnwrapNamedValueType(cast.Cast()->Type));
        auto operand = NumericConstant(cast.Cast()->Operand);
        if (!target || !operand) {
            return std::nullopt;
        }
        const bool single = target.Cast()->Kind == TFloatType::F32;
        return std::visit([single](auto v) {
            return single ? TNumericConstant{static_cast<float>(v)}
                          : TNumericConstant{static_cast<double>(v)};
        }, *operand);
    }
    return std::nullopt;
}

// A string literal, also under the cast to StringView the parser writes.
const TStringLiteralExpr* StringConstant(NQumir::NAst::TTypeFactory& types, const TExprPtr& e) {
    if (auto cast = TMaybeNode<TCastExpr>(e)) {
        return TMaybeType<TStringType>(FromQumirType(types, cast.Cast()->Type))
            ? StringConstant(types, cast.Cast()->Operand)
            : nullptr;
    }
    auto literal = TMaybeNode<TStringLiteralExpr>(e);
    return literal ? literal.Cast() : nullptr;
}

// Digits at scale `from` as digits at scale `to`; nullopt if they need more fractional
// digits or more than i64, so that no DECIMAL(18) value at `to` equals them.
std::optional<int64_t> RescaleDigits(int64_t digits, int32_t from, int32_t to) {
    for (; from < to; ++from) {
        if (__builtin_mul_overflow(digits, 10, &digits)) {
            return std::nullopt;
        }
    }
    for (; from > to; --from) {
        if (digits % 10 != 0) {
            return std::nullopt;
        }
        digits /= 10;
    }
    return digits;
}

// The constant items of an IN list as the table of qdb_in_sorted_* (qumirdb.oz), written
// into the bytes of a string literal. An item is taken only where `==` with the lhs
// compares values: integers and dates, decimals at the lhs's scale, floats in the type
// `==` converts both sides to, strings by their bytes. NaN and anything else stay
// comparisons.
class TInTable {
public:
    TInTable(NQumir::NAst::TTypeFactory& types, const TTypePtr& lhsValue)
        : Types_(types)
    {
        const TTypePtr type = UnwrapNamedValueType(lhsValue);
        if (auto decimal = DecimalSpecOfValueType(lhsValue)) {
            if (DecimalStorageOf(*decimal) == EDecimalStorage::I64) {
                Kind_ = EKind::Decimal;
                Decimal_ = *decimal;
            }
        } else if (auto integer = MaybeIntegerType(type);
            integer && integer.Cast()->IsSigned() && integer.Cast()->BitWidth() <= 64)
        {
            Kind_ = EKind::Integer;
        } else if (IsFloat(type)) {
            Kind_ = EKind::Float;
            RealLhs_ = IsReal(type);
        } else if (TMaybeType<TStringType>(type)) {
            Kind_ = EKind::String;
        }
    }

    // Takes a constant item. One that no lhs value equals (a decimal with more nonzero
    // fractional digits than the lhs has) is taken without an entry.
    bool Take(const TExprPtr& item) {
        switch (Kind_) {
            case EKind::None:
                return false;
            case EKind::Integer: {
                auto constant = NumericConstant(item);
                if (!constant || !std::holds_alternative<int64_t>(*constant)) {
                    return false;
                }
                Integers_.push_back(std::get<int64_t>(*constant));
                break;
            }
            case EKind::Decimal: {
                // An integer literal is a decimal of scale 0, a fractional one of its digits.
                std::optional<TDecimalLiteral> literal;
                if (auto constant = NumericConstant(item);
                    constant && std::holds_alternative<int64_t>(*constant))
                {
                    literal = TDecimalLiteral{std::get<int64_t>(*constant), {.Scale = 0}};
                } else {
                    literal = DecimalLiteralOf(item);
                }
                if (!literal) {
                    return false;
                }
                auto digits = RescaleDigits(literal->Unscaled, literal->Spec.Scale, Decimal_.Scale);
                if (digits) {
                    Integers_.push_back(*digits);
                }
                break;
            }
            case EKind::Float: {
                auto constant = NumericConstant(item);
                if (!constant) {
                    return false;
                }
                // A REAL lhs meets an integer as REAL, any other pair meets as DOUBLE;
                // widening a REAL to DOUBLE keeps `==`.
                double value = std::visit([this](auto v) {
                    if constexpr (std::is_same_v<decltype(v), int64_t>) {
                        return RealLhs_ ? static_cast<double>(static_cast<float>(v))
                                        : static_cast<double>(v);
                    } else {
                        return static_cast<double>(v);
                    }
                }, *constant);
                if (std::isnan(value)) {
                    return false;
                }
                Floats_.push_back(value);
                break;
            }
            case EKind::String: {
                const TStringLiteralExpr* literal = StringConstant(Types_, item);
                if (!literal) {
                    return false;
                }
                Strings_.push_back(literal->Value);
                break;
            }
        }
        ++Taken_;
        return true;
    }

    size_t Taken() const {
        return Taken_;
    }

    // Whether `lhsValue` is in the table: a bool, the lhs is not NULL.
    TExprPtr Lookup(const TExprPtr& lhsValue, TLocation loc) {
        std::string table;
        auto append = [&table](uint64_t bits) {
            for (int shift = 0; shift < 64; shift += 8) {
                table.push_back(static_cast<char>(bits >> shift));
            }
        };
        const char* lookup = "qdb_in_sorted_i64";
        TExprPtr x;
        size_t count = 0;
        switch (Kind_) {
            case EKind::None:
                throw std::logic_error("TInTable::Lookup: no table for the lhs type");
            case EKind::Integer:
            case EKind::Decimal: {
                std::ranges::sort(Integers_);
                const auto [end, _] = std::ranges::unique(Integers_);
                Integers_.erase(end, Integers_.end());
                for (int64_t v : Integers_) {
                    append(static_cast<uint64_t>(v));
                }
                count = Integers_.size();
                // A DECIMAL is passed as its digits at the scale of the table.
                x = NQumir::MakeIntrusive<TCastExpr>(loc, CloneExpr(lhsValue), Kind_ == EKind::Decimal
                    ? MakeDecimalType(Types_, Decimal_)
                    : Types_.MakeInteger());
                break;
            }
            case EKind::Float: {
                // Without NaN `<` is a strict weak order; -0.0 and 0.0 are one entry.
                std::ranges::sort(Floats_);
                const auto [end, _] = std::ranges::unique(Floats_);
                Floats_.erase(end, Floats_.end());
                for (double v : Floats_) {
                    append(std::bit_cast<uint64_t>(v));
                }
                count = Floats_.size();
                lookup = "qdb_in_sorted_f64";
                x = NQumir::MakeIntrusive<TCastExpr>(loc, CloneExpr(lhsValue),
                    Types_.MakeFloat());
                break;
            }
            case EKind::String: {
                // std::string orders bytes as unsigned, as qdb_filter_string_compare does.
                std::ranges::sort(Strings_);
                const auto [end, _] = std::ranges::unique(Strings_);
                Strings_.erase(end, Strings_.end());
                count = Strings_.size();
                uint64_t offset = (count + 1) * 8;
                append(offset);
                for (const auto& s : Strings_) {
                    offset += s.size();
                    append(offset);
                }
                for (const auto& s : Strings_) {
                    table += s;
                }
                lookup = "qdb_in_sorted_string";
                x = CloneExpr(lhsValue);
                break;
            }
        }
        auto bytes = NQumir::MakeIntrusive<TBitcastExpr>(loc,
            NQumir::MakeIntrusive<TStringLiteralExpr>(loc, std::move(table), Types_),
            Types_.MakePointer(Types_.MakeInteger(TIntegerType::U8)));
        return NQumir::MakeIntrusive<TCallExpr>(loc, Ident(lookup, loc), std::vector<TExprPtr>{
            std::move(x), std::move(bytes),
            NQumir::MakeIntrusive<TNumberExpr>(loc, static_cast<int64_t>(count), Types_),
        });
    }

private:
    enum class EKind {
        None,
        Integer,
        Decimal,
        Float,
        String,
    };

    NQumir::NAst::TTypeFactory& Types_;
    EKind Kind_ = EKind::None;
    TDecimalSpec Decimal_;
    bool RealLhs_ = false;
    std::vector<int64_t> Integers_;
    std::vector<double> Floats_;
    std::vector<std::string> Strings_;
    size_t Taken_ = 0;
};

// qdb_in_list(lhs, item...) preserves the SQL IN boundary until operand types are
// known. Hoist lhs validity once, compare its plain value to every item, and only
// retain per-item 3VL when an item itself can be NULL.
std::pair<TExprPtr, TTypePtr> ExpandInList(NQumir::NAst::TTypeFactory& types, const TCallExpr& node,
    const TStructType& inputType, uint64_t& counter)
{
    const TLocation loc = node.Location;
    auto boolType = types.Make<TBoolType>();
    auto nullableBool = MakeNullableType(types, boolType);
    if (node.Args.size() < 2) {
        throw NQumir::TError("qdb_in_list expects an lhs and at least one item");
    }

    TExprPtr lhs = node.Args.front();
    TTypePtr lhsType = Expand(types, lhs, inputType, counter);
    if (!lhsType) {
        return {
            NQumir::MakeIntrusive<TCastExpr>(loc, MakeNull(loc), NullableQ(types, boolType)),
            nullableBool,
        };
    }

    std::vector<TExprPtr> stmts;
    TExprPtr lhsRef = BindTemp(lhs, stmts, counter, loc);
    TExprPtr lhsValue = IsNullableType(lhsType)
        ? FieldOf(lhsRef, "Value", loc)
        : lhsRef;

    std::vector<std::pair<TExprPtr, TTypePtr>> items;
    items.reserve(node.Args.size() - 1);
    for (size_t i = 1; i < node.Args.size(); ++i) {
        TExprPtr item = node.Args[i];
        TTypePtr itemType = Expand(types, item, inputType, counter);
        items.emplace_back(std::move(item), std::move(itemType));
    }

    // The OR of the comparisons is commutative in 3VL: the constants of a long list go
    // to one lookup, the other items (NULL, columns) stay comparisons beside it.
    std::vector<std::pair<TExprPtr, TTypePtr>> terms;
    if (items.size() >= InTableMinItems) {
        TInTable table(types, ValueType(lhsType));
        std::vector<std::pair<TExprPtr, TTypePtr>> compared;
        for (const auto& item : items) {
            if (!table.Take(item.first)) {
                compared.push_back(item);
            }
        }
        if (table.Taken() >= InTableMinItems) {
            terms.emplace_back(table.Lookup(lhsValue, loc), boolType);
            items = std::move(compared);
        }
    }
    for (auto& [item, itemType] : items) {
        TExprPtr equal;
        TTypePtr equalType = nullptr;
        if (!itemType) {
            equal = NQumir::MakeIntrusive<TCastExpr>(
                loc, MakeNull(loc), NullableQ(types, boolType));
            equalType = nullableBool;
        } else if (IsNullableType(itemType)) {
            equal = BuildNullStrict(types, {{item, true}},
                [lhsValue, loc](std::vector<TExprPtr> args) {
                    return Bin("==", CloneExpr(lhsValue), std::move(args[0]), loc);
                }, boolType, counter, loc);
            equalType = nullableBool;
        } else {
            equal = Bin("==", CloneExpr(lhsValue), std::move(item), loc);
            equalType = boolType;
        }

        terms.emplace_back(std::move(equal), equalType);
    }

    // OR the comparisons as a balanced tree: a left-deep chain is as deep as the
    // list and overflows the stack in the recursive passes. Leaf order is kept.
    while (terms.size() > 1) {
        std::vector<std::pair<TExprPtr, TTypePtr>> next;
        next.reserve((terms.size() + 1) / 2);
        for (size_t i = 0; i + 1 < terms.size(); i += 2) {
            auto& [l, lt] = terms[i];
            auto& [r, rt] = terms[i + 1];
            if (IsNullableType(lt) || IsNullableType(rt)) {
                next.emplace_back(Build3VL(types, /*isAnd=*/false, l, lt, r, rt, counter, loc),
                    nullableBool);
            } else {
                next.emplace_back(Bin("||", std::move(l), std::move(r), loc), boolType);
            }
        }
        if (terms.size() % 2 == 1) {
            next.push_back(std::move(terms.back()));
        }
        terms = std::move(next);
    }
    TExprPtr result = std::move(terms.front().first);
    TTypePtr resultType = terms.front().second;

    if (IsNullableType(lhsType)) {
        result = NQumir::MakeIntrusive<TIfExpr>(
            loc,
            FieldOf(lhsRef, "Valid", loc),
            CoerceBranch(types, result, resultType, /*isNull=*/false, boolType, counter, loc),
            NQumir::MakeIntrusive<TCastExpr>(loc, MakeNull(loc), NullableQ(types, boolType)));
        resultType = nullableBool;
    }

    stmts.push_back(std::move(result));
    return {
        NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(stmts)),
        resultType,
    };
}

size_t TreeDepth(const TExprPtr& e)
{
    size_t depth = 0;
    for (const auto& child : e->Children()) {
        if (child) {
            depth = std::max(depth, TreeDepth(child));
        }
    }
    return depth + 1;
}

// COALESCE(a, b, ...) -> an `if`-chain: return the first arg with `.Valid`; once a
// non-nullable arg is reached it is returned directly (result becomes non-nullable).
// Bare-NULL args are dropped. Args are bound to temps (referenced for both `.Valid` and
// value). Returns {rewritten expr, planner type}.
std::pair<TExprPtr, TTypePtr> ExpandCoalesce(NQumir::NAst::TTypeFactory& types, const TCallExpr& node,
    const TStructType& inputType, uint64_t& counter)
{
    const TLocation loc = node.Location;
    std::vector<std::pair<TExprPtr, TTypePtr>> args; // (expr, type); bare NULLs dropped
    std::vector<TTypePtr> argTypes;
    std::vector<TExprPtr> exprs;
    for (const auto& a : node.Args) {
        TExprPtr arg = a;
        TTypePtr t = Expand(types, arg, inputType, counter);
        if (t) { args.emplace_back(arg, t); argTypes.push_back(t); exprs.push_back(arg); }
    }
    auto [T, allNullable] = CoalesceValueType(types, argTypes, exprs);
    if (args.empty() || !T) {
        return {MakeNull(loc), nullptr}; // degenerate (all NULL) - untyped
    }
    const bool resultNullable = allNullable;
    auto tq = CastTargetQ(types, T);

    // Terminator = first non-nullable arg (guaranteed value), else the last arg.
    size_t m = args.size() - 1;
    for (size_t i = 0; i < args.size(); ++i) {
        if (!IsNullableType(args[i].second)) { m = i; break; }
    }

    // The value contributed by a (temp-bound) arg when it is the chosen one.
    auto pick = [&](const TExprPtr& ref, const TTypePtr& t) -> TExprPtr {
        if (resultNullable) {
            return CoerceBranch(types, ref, t, /*isNull*/false, T, counter, loc); // -> Nullable[T]
        }
        TExprPtr val = IsNullableType(t) ? FieldOf(ref, "Value", loc) : CloneExpr(ref);
        if (PlanTypeKey(UnwrapNullableType(t)) != PlanTypeKey(T)) {
            val = NQumir::MakeIntrusive<TCastExpr>(loc, val, tq); // widen to T
        }
        return val;
    };

    // One `if` per argument is nested, argument i sits i levels down; the parser
    // sees only the flat list. Every argument is shallower than the limit already.
    for (size_t i = 0; i <= m; ++i) {
        if (i + TreeDepth(args[i].first) > MaxNesting) {
            throw NQumir::TError(loc, NestingTooDeepMessage());
        }
    }

    std::vector<TExprPtr> stmts;
    // Terminator is returned unconditionally (no `.Valid` guard).
    TExprPtr acc = pick(args[m].first, args[m].second);
    // Chain args[0..m-1] (all nullable) from the inside out.
    for (int i = static_cast<int>(m) - 1; i >= 0; --i) {
        TExprPtr ref = BindTemp(args[i].first, stmts, counter, loc);
        acc = NQumir::MakeIntrusive<TIfExpr>(loc,
            FieldOf(ref, "Valid", loc), pick(ref, args[i].second), acc);
    }
    if (stmts.empty()) {
        return {acc, resultNullable ? MakeNullableType(types, T) : T};
    }
    stmts.push_back(acc);
    return {NQumir::MakeIntrusive<TBlockExpr>(loc, std::move(stmts)),
        resultNullable ? MakeNullableType(types, T) : T};
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
    return literal ? literal.Cast() : nullptr;
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

// Bottom-up rewrite. Mutates `e` in place, returns its planner type (Nullable-aware),
// or nullptr for a bare NULL leaf (the enclosing `if`/op supplies the type).
TTypePtr Expand(NQumir::NAst::TTypeFactory& types, TExprPtr& e, const TStructType& inputType, uint64_t& counter) {
    if (IsNullLiteral(e)) {
        return nullptr;
    }
    // A string literal is a non-null `string` leaf. It is always the operand of a
    // parser-emitted `cast(lit, StringView)`; annotating the bare literal would wrap it in
    // a var (owned storage) and fail the lifetime validator, so type it directly.
    if (TMaybeNode<TStringLiteralExpr>(e)) {
        return types.Make<TStringType>();
    }

    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node = ifp.Cast();
        Expand(types, node->Cond, inputType, counter);
        if (!node->Then) node->Then = MakeNull(node->Location);
        if (!node->Else) node->Else = MakeNull(node->Location);
        TTypePtr thenT = Expand(types, node->Then, inputType, counter);
        TTypePtr elseT = Expand(types, node->Else, inputType, counter);
        const bool thenNull = !thenT, elseNull = !elseT;
        const bool thenNullable = thenNull || IsNullableType(thenT);
        const bool elseNullable = elseNull || IsNullableType(elseT);
        if (!thenNullable && !elseNullable) {
            // no null - unify (i32/i64 -> i64)
            return UnifyBranchTypes(types, thenT, elseT, node->Then, node->Else);
        }
        TTypePtr value = UnifyBranchTypes(types, thenT, elseT, node->Then, node->Else);
        if (!value) {
            return nullptr; // both branches NULL - untyped, leave for the parent
        }
        node->Then = CoerceBranch(types, node->Then, thenT, thenNull, value, counter, node->Location);
        node->Else = CoerceBranch(types, node->Else, elseT, elseNull, value, counter, node->Location);
        return MakeNullableType(types, value);
    }

    if (auto bp = TMaybeNode<TBinaryExpr>(e)) {
        auto node = bp.Cast();
        TTypePtr lt = Expand(types, node->Left, inputType, counter);
        TTypePtr rt = Expand(types, node->Right, inputType, counter);
        TOperator op = node->Operator;
        const bool lNull = !lt || IsNullableType(lt);
        bool rNull = !rt || IsNullableType(rt);
        if (IsLogical(op)) {
            if (!lNull && !rNull) {
                return types.Make<TBoolType>();
            }
            e = Build3VL(types, op == "&&", node->Left, lt, node->Right, rt, counter, node->Location);
            return MakeNullableType(types, types.Make<TBoolType>());
        }
        if (lt && rt && MayBeZero(node->Right)) {
            TTypePtr R = BinaryValueType(types, op, node->Left, node->Right, lt, rt);
            if (IsNullOnZeroDivisor(op, R)) {
                if (TMaybeNode<TNumberExpr>(node->Right)) {
                    // `x % 0`: NULL on every row.
                    e = NQumir::MakeIntrusive<TCastExpr>(node->Location, MakeNull(node->Location), NullableQ(types, R));
                    return MakeNullableType(types, R);
                }
                node->Right = NullIfZero(types, node->Right, rt, counter, node->Location);
                rt = MakeNullableType(types, ValueType(rt));
                rNull = true;
            }
        }
        if (!lNull && !rNull) {
            return BinaryValueType(types, op, node->Left, node->Right, lt, rt);
        }
        // Bare-NULL operand => the whole op is unconditionally NULL[R].
        if (!lt || !rt) {
            TTypePtr R = BinaryValueTypeWithNull(types, op, node->Left, node->Right, lt, rt);
            if (!R) {
                // No operand has a type (`NULL + NULL`): the result is an untyped NULL.
                e = MakeNull(node->Location);
                return nullptr;
            }
            e = NQumir::MakeIntrusive<TCastExpr>(node->Location, MakeNull(node->Location), NullableQ(types, R));
            return MakeNullableType(types, R);
        }
        TTypePtr R = BinaryValueType(types, op, node->Left, node->Right, lt, rt);
        TExprPtr l = node->Left, r = node->Right;
        e = BuildNullStrict(types, {{l, lNull}, {r, rNull}},
            [op, node](std::vector<TExprPtr> a) {
                return NQumir::MakeIntrusive<TBinaryExpr>(node->Location, op, a[0], a[1]);
            }, R, counter, node->Location);
        return MakeNullableType(types, R);
    }

    if (auto up = TMaybeNode<TUnaryExpr>(e)) {
        auto node = up.Cast();
        TTypePtr ot = Expand(types, node->Operand, inputType, counter);
        TOperator op = node->Operator;
        TTypePtr R = ResultTypeUnary(types, op, ot);
        if (IsUntypedNull(node->Operand, ot)) {
            // `!` fixes the type of an untyped-NULL operand: NOT NULL is NULL[bool], while
            // `-NULL` stays untyped.
            if (!R) {
                e = MakeNull(node->Location);
                return nullptr;
            }
            e = NQumir::MakeIntrusive<TCastExpr>(node->Location, MakeNull(node->Location), NullableQ(types, R));
            return MakeNullableType(types, R);
        }
        if (!ot || !IsNullableType(ot)) {
            return ot ? R : nullptr;
        }
        TExprPtr operand = node->Operand;
        e = BuildNullStrict(types, {{operand, true}},
            [op, node](std::vector<TExprPtr> a) {
                return NQumir::MakeIntrusive<TUnaryExpr>(node->Location, op, a[0]);
            }, R, counter, node->Location);
        return MakeNullableType(types, R);
    }

    if (auto cp = TMaybeNode<TCastExpr>(e)) {
        auto node = cp.Cast();
        const TTypePtr nominal = NominalCastType(node->Operand);
        TTypePtr ot = Expand(types, node->Operand, inputType, counter);
        // `target` is the (non-nullable) value type; a nullable cast target contributes
        // nullability separately (BuildNullStrict / nullable_from_value wrap it once).
        TTypePtr target = IsNullableType(node->Type)
            ? UnwrapNullableType(node->Type)
            : FromQumirType(types, node->Type);
        const TTypePtr castTarget = UnwrapNullableType(node->Type);
        // The operand type the cast table sees; `ot` keeps deciding the nullability.
        const TTypePtr from = nominal ? nominal : ot;
        if (IsUntypedNull(node->Operand, ot)) {
            // CAST(NULL AS T) is the NULL of T.
            e = NQumir::MakeIntrusive<TCastExpr>(node->Location, MakeNull(node->Location), NullableQ(types, target));
            return MakeNullableType(types, target);
        }
        if (!ot || !IsNullableType(ot)) {
            // Non-nullable operand cast to a nullable target (e.g. a UNION-branch
            // coercion `cast(i32, Nullable[i32])`): emit nullable_from_value. The cast
            // target must be the qumir Nullable type - a raw planner Nullable does not
            // compile - and the value is widened to the target base if needed. (Checked
            // on the raw node->Type: FromQumirType strips the planner Nullable.)
            if (ot && IsNullableType(node->Type)) {
                TTypePtr base = UnwrapNullableType(node->Type);
                TExprPtr val = node->Operand;
                if (PlanTypeKey(UnwrapNullableType(ot)) != PlanTypeKey(base)) {
                    // Base conversion targets the planner base type so ExpandDecimal can
                    // still turn a float/int -> decimal cast into qdb_decimal_from_*.
                    val = ConvertValue(types, val, from, base, base, node->Location);
                }
                e = NQumir::MakeIntrusive<TCastExpr>(node->Location, val, ToQumirType(types, node->Type));
                return MakeNullableType(types, base);
            }
            // Without a rule the plain cast stays as written: its target may be a type
            // only qumirdb.oz knows, like `DATE` for a date literal. A cast to VARCHAR
            // is spelled with the StringView target so that a `cast` operator of
            // qumirdb.oz (boolean -> VARCHAR) is a candidate.
            if (auto converted = ot
                ? ApplyCastRule(types, from, castTarget, node->Operand, node->Location)
                : nullptr)
            {
                e = converted;
            } else if (TMaybeType<TStringType>(castTarget)) {
                e = NQumir::MakeIntrusive<TCastExpr>(node->Location, node->Operand,
                    CastTargetQ(types, castTarget));
            }
            return ot ? target : nullptr;
        }
        TExprPtr operand = node->Operand;
        // BuildNullStrict already re-wraps the result in Nullable[target], so the inner
        // conversion targets the plain value type.
        e = BuildNullStrict(types, {{operand, true}},
            [from, castTarget, target, node, &types](std::vector<TExprPtr> a) -> TExprPtr {
                return ConvertValue(types, a[0], from, castTarget, target, node->Location);
            }, target, counter, node->Location);
        return MakeNullableType(types, target);
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
                    node->Callee = NQumir::MakeIntrusive<TIdentExpr>(
                        node->Location, fast->Func);
                    name = fast->Func;
                }
            }
        }
        if (name == "coalesce") {
            auto [expr, type] = ExpandCoalesce(types, *node, inputType, counter);
            e = expr;
            return type;
        }
        if (name == "qdb_in_list") {
            auto [expr, type] = ExpandInList(types, *node, inputType, counter);
            e = expr;
            return type;
        }
        if (const char* runtimeName = ArenaStringFunction(name)) {
            // E.g. `strcat(a, b)` -> the runtime call `qdb_string_concat(__arena__, a, b)`.
            // __arena__ is a placeholder the codegen binds to the kernel's scratch
            // arena. Null-strict: NULL if any operand is NULL (SQL `NULL || x = NULL`).
            std::vector<std::pair<TExprPtr, bool>> ops;
            for (auto& arg : node->Args) {
                auto t = Expand(types, arg, inputType, counter);
                ops.emplace_back(arg, !t || IsNullableType(t));
            }
            TLocation loc = node->Location;
            auto apply = [loc, runtimeName](std::vector<TExprPtr> a) -> TExprPtr {
                std::vector<TExprPtr> callArgs = { NQumir::MakeIntrusive<TIdentExpr>(loc, "__arena__") };
                for (auto& x : a) callArgs.push_back(std::move(x));
                return NQumir::MakeIntrusive<TCallExpr>(loc,
                    NQumir::MakeIntrusive<TIdentExpr>(loc, runtimeName), std::move(callArgs));
            };
            TTypePtr R = types.Make<TStringType>();
            bool anyNull = false;
            for (const auto& [expr, nullable] : ops) anyNull = anyNull || nullable;
            if (!anyNull) {
                e = apply(node->Args);
                return R;
            }
            e = BuildNullStrict(types, ops, apply, R, counter, loc);
            return MakeNullableType(types, R);
        }
        std::vector<TTypePtr> argTypes;
        for (auto& arg : node->Args) {
            argTypes.push_back(Expand(types, arg, inputType, counter));
        }
        if (name == "qdb_is_true" && node->Args.size() == 1) {
            node->Args[0] = TypeUntypedNull(
                types, {node->Args[0], argTypes[0]}, types.Make<TBoolType>()).first;
        }
        if (name == "qdb_is_null" || name == "qdb_is_true") {
            // Non-null bool. The arguments are expanded already: typing them again with
            // InferType would not see the temporaries the expansion introduced.
            return types.Make<TBoolType>();
        }
        if (IsExcludedCall(name)) {
            return InferType(types, e, inputType);
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
            e = BuildNullStrict(types, operands,
                [node](std::vector<TExprPtr> a) {
                    return NQumir::MakeIntrusive<TCallExpr>(
                        node->Location, node->Callee, std::move(a));
                }, value, counter, node->Location);
            return MakeNullableType(types, value);
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
        e = BuildNullStrict(types, operands,
            [node](std::vector<TExprPtr> a) {
                return NQumir::MakeIntrusive<TCallExpr>(node->Location, node->Callee, std::move(a));
            }, R, counter, node->Location);
        return MakeNullableType(types, R);
    }

    if (TMaybeNode<TIdentExpr>(e) || TMaybeNode<TNumberExpr>(e) ||
        TMaybeNode<TStringLiteralExpr>(e))
    {
        return InferType(types, e, inputType); // leaves - no rewrite
    }

    // Anything else (field access already produced by this pass, etc.): recurse into
    // children to rewrite nested nullables, then type by rule.
    for (auto* child : e->MutableChildren()) {
        Expand(types, *child, inputType, counter);
    }
    return InferType(types, e, inputType);
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

TExprPtr Int64(NQumir::NAst::TTypeFactory& types, TLocation loc, int64_t value) {
    auto n = NQumir::MakeIntrusive<TNumberExpr>(loc, value, types);
    n->Type = types.MakeInteger();
    return n;
}

TExprPtr Call(const std::string& name, std::vector<TExprPtr> args, TLocation loc) {
    return NQumir::MakeIntrusive<TCallExpr>(loc, Ident(name, loc), std::move(args));
}

TExprPtr CastToI64(NQumir::NAst::TTypeFactory& types, TExprPtr expr, TLocation loc) {
    return NQumir::MakeIntrusive<TCastExpr>(loc, std::move(expr),
        types.MakeInteger());
}

TExprPtr DecimalScaleLiteral(NQumir::NAst::TTypeFactory& types, TLocation loc, int32_t scale) {
    return Int64(types, loc, static_cast<int64_t>(scale));
}

// The expression as a DECIMAL of `target`: the cast-table row from its value type. The
// target's precision picks the storage the value is converted to.
TExprPtr DecimalValueAs(NQumir::NAst::TTypeFactory& types, TExprPtr expr, const TTypePtr& type, TDecimalSpec target, TLocation loc) {
    if (auto converted = ApplyDecimalCastRule(types, ValueType(type), MakeDecimalType(types, target), expr, loc)) {
        return converted;
    }
    throw NQumir::TError(
        "cannot convert type '" + (type ? TypeDiagnosticName(type) : std::string("unknown")) +
        "' to decimal");
}

// The digits an operand of a decimal operation brings: its own spec, DECIMAL(w, 0) for an
// integer, and the widest precision for a float literal no decimal holds.
TDecimalSpec DecimalOperandSpec(const TExprPtr& e, const TTypePtr& type) {
    const TTypePtr value = ValueType(type);
    if (auto decimal = DecimalSpecOfValueType(value)) {
        return *decimal;
    }
    if (IsInt(value)) {
        return IntegerDecimalSpec(e, value);
    }
    return {.Precision = MaxDecimalPrecision, .Scale = 0};
}

// The spec both operands of a comparison take: the common scale and the wider integer
// part, so the storage holds either side.
TDecimalSpec DecimalComparisonSpec(const TExprPtr& le, const TTypePtr& lt,
    const TExprPtr& re, const TTypePtr& rt)
{
    const TDecimalSpec l = DecimalOperandSpec(le, lt);
    const TDecimalSpec r = DecimalOperandSpec(re, rt);
    const int32_t scale = std::max(l.Scale, r.Scale);
    const int32_t intDigits = std::max(l.Precision - l.Scale, r.Precision - r.Scale);
    return {.Precision = std::min(MaxDecimalPrecision, intDigits + scale), .Scale = scale};
}

bool IsDecimal64(const TDecimalSpec& spec) {
    return DecimalStorageOf(spec) == EDecimalStorage::I64;
}

// A decimal operand widened to i128 at its own scale.
TDecimalSpec Wide(const TDecimalSpec& spec) {
    return {.Precision = MaxDecimalPrecision, .Scale = spec.Scale};
}

// The expression as the float `target`: a decimal is converted like CAST(... AS target),
// anything else already is one or is converted by the caller's cast.
TExprPtr DecimalValueAsFloat(NQumir::NAst::TTypeFactory& types, TExprPtr expr, const TTypePtr& type, const TTypePtr& target,
    TLocation loc)
{
    if (auto converted = ApplyDecimalCastRule(types, ValueType(type), target, expr, loc)) {
        return converted;
    }
    return expr;
}

TExprPtr DecimalValueAsDouble(NQumir::NAst::TTypeFactory& types, TExprPtr expr, const TTypePtr& type, TLocation loc) {
    return DecimalValueAsFloat(types, std::move(expr), type, types.MakeFloat(), loc);
}

TTypePtr ExpandDecimalNode(NQumir::NAst::TTypeFactory& types, TExprPtr& e, const TStructType& inputType, TTypeEnv& env);

TTypePtr ExpandDecimalBlock(NQumir::NAst::TTypeFactory& types, TBlockExpr& node, const TStructType& inputType, TTypeEnv& env) {
    TTypePtr last = nullptr;
    TTypeEnv local = env;
    for (auto& stmt : node.Stmts) {
        if (auto var = TMaybeNode<TVarStmt>(stmt)) {
            TTypePtr initType = nullptr;
            if (var.Cast()->Init) {
                initType = ExpandDecimalNode(types, var.Cast()->Init, inputType, local);
            }
            TTypePtr varType = var.Cast()->Type ? FromQumirType(types, var.Cast()->Type) : initType;
            if (varType) {
                local[var.Cast()->Name] = varType;
            }
            last = varType;
            continue;
        }
        if (auto assign = TMaybeNode<TAssignExpr>(stmt)) {
            last = ExpandDecimalNode(types, assign.Cast()->Value, inputType, local);
            if (last) {
                local[assign.Cast()->Name] = last;
            }
            continue;
        }
        last = ExpandDecimalNode(types, stmt, inputType, local);
    }
    return last;
}

// A literal has at most 18 digits (DecimalLiteralOf), so its storage is a plain i64.
TExprPtr DecimalConstant(NQumir::NAst::TTypeFactory& types, const TDecimalLiteral& lit, TLocation loc) {
    return Int64(types, loc, lit.Unscaled);
}

TTypePtr ExpandDecimalNode(NQumir::NAst::TTypeFactory& types, TExprPtr& e, const TStructType& inputType, TTypeEnv& env) {
    if (!e || IsNullLiteral(e)) {
        return nullptr;
    }
    if (TMaybeNode<TStringLiteralExpr>(e)) {
        return types.Make<TStringType>();
    }
    if (auto num = TMaybeNode<TNumberExpr>(e)) {
        return num.Cast()->Type ? num.Cast()->Type
            : types.MakeInteger();
    }
    if (auto id = TMaybeNode<TIdentExpr>(e)) {
        return LookupIdentType(id.Cast()->Name, inputType, env);
    }
    if (auto fa = TMaybeNode<TFieldAccessExpr>(e)) {
        auto node = fa.Cast();
        TTypePtr objectType = ExpandDecimalNode(types, node->Object, inputType, env);
        if (objectType && IsNullableType(objectType)) {
            if (node->FieldName == "Value") {
                return UnwrapNullableType(objectType);
            }
            if (node->FieldName == "Valid") {
                return types.Make<TBoolType>();
            }
        }
        auto structType = TMaybeType<TStructType>(UnwrapNamedValueType(objectType));
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
        ExpandDecimalNode(types, node->Cond, inputType, env);
        TTypePtr thenT = ExpandDecimalNode(types, node->Then, inputType, env);
        TTypePtr elseT = node->Else ? ExpandDecimalNode(types, node->Else, inputType, env) : nullptr;
        TTypePtr result = UnifyBranchTypes(types, thenT, elseT, node->Then, node->Else);
        if (auto decimal = DecimalSpecOfValueType(result)) {
            node->Then = DecimalValueAs(types, node->Then, thenT, *decimal, node->Location);
            if (node->Else) {
                node->Else = DecimalValueAs(types, node->Else, elseT, *decimal, node->Location);
            }
        } else if (result && IsFloat(result)) {
            node->Then = DecimalValueAsFloat(types, node->Then, thenT, result, node->Location);
            if (node->Else) {
                node->Else = DecimalValueAsFloat(types, node->Else, elseT, result, node->Location);
            }
        }
        bool nullable = !thenT || !elseT || IsNullableType(thenT) || IsNullableType(elseT);
        return result && nullable
            ? MakeNullableType(types, result)
            : result;
    }
    if (auto bp = TMaybeNode<TBinaryExpr>(e)) {
        auto node = bp.Cast();
        TTypePtr lt = ExpandDecimalNode(types, node->Left, inputType, env);
        TTypePtr rt = ExpandDecimalNode(types, node->Right, inputType, env);
        if (DecimalSpecOfValueType(ValueType(lt)) && IsFloat(UnwrapNamedValueType(ValueType(rt)))) {
            if (auto lit = DecimalLiteralOf(node->Right)) {
                node->Right = DecimalConstant(types, *lit, node->Location);
                rt = MakeDecimalType(types, lit->Spec);
            }
        } else if (DecimalSpecOfValueType(ValueType(rt)) && IsFloat(UnwrapNamedValueType(ValueType(lt)))) {
            if (auto lit = DecimalLiteralOf(node->Left)) {
                node->Left = DecimalConstant(types, *lit, node->Location);
                lt = MakeDecimalType(types, lit->Spec);
            }
        }
        auto dl = DecimalSpecOfValueType(ValueType(lt));
        auto dr = DecimalSpecOfValueType(ValueType(rt));
        if (node->Operator != "%"_op &&
            IsDecimalBesideFloat(dl, dr, ValueType(lt), ValueType(rt), node->Left, node->Right)) {
            // The comparison or arithmetic then runs on DOUBLEs, with the decimal converted
            // like CAST(... AS DOUBLE). The converted operand keeps its nullability.
            auto& decimalOperand = dl ? node->Left : node->Right;
            auto& decimalType = dl ? lt : rt;
            decimalOperand = DecimalValueAsDouble(types, std::move(decimalOperand), decimalType, node->Location);
            TTypePtr floatType = types.MakeFloat();
            decimalType = IsNullableType(decimalType)
                ? MakeNullableType(types, floatType)
                : floatType;
            dl = dr = std::nullopt;
        }
        if (node->Operator == "/"_op && dl && dr && IsDecimalQuotientDouble(*dl, *dr)) {
            // Both operands convert like CAST(... AS DOUBLE); nullability is kept.
            TTypePtr floatType = types.MakeFloat();
            for (auto [operand, type] : {std::pair{&node->Left, &lt}, std::pair{&node->Right, &rt}}) {
                *operand = DecimalValueAsDouble(types, std::move(*operand), *type, node->Location);
                *type = IsNullableType(*type)
                    ? MakeNullableType(types, floatType)
                    : floatType;
            }
            dl = dr = std::nullopt;
        }
        if (!dl && !dr) {
            return Propagate(types, {lt, rt},
                BinaryValueType(types, node->Operator, node->Left, node->Right, lt, rt));
        }
        TTypePtr result = BinaryValueType(types, node->Operator, node->Left, node->Right, lt, rt);
        if (IsComparison(node->Operator)) {
            const TDecimalSpec spec = DecimalComparisonSpec(node->Left, lt, node->Right, rt);
            node->Left = DecimalValueAs(types, node->Left, lt, spec, node->Location);
            node->Right = DecimalValueAs(types, node->Right, rt, spec, node->Location);
            return Propagate(types, {lt, rt}, result);
        }
        auto resultDecimal = DecimalSpecOfValueType(result);
        if (!resultDecimal) {
            return Propagate(types, {lt, rt}, result);
        }
        // Operands of + - % take the result's spec: it holds either of them, and an i64
        // result computes with the i64 operators.
        if (node->Operator == "+"_op || node->Operator == "-"_op || node->Operator == "%"_op) {
            node->Left = DecimalValueAs(types, node->Left, lt, *resultDecimal, node->Location);
            node->Right = DecimalValueAs(types, node->Right, rt, *resultDecimal, node->Location);
            return Propagate(types, {lt, rt}, result);
        }
        // The unscaled product is at scale s1 + s2, the result's scale. An i64 result has at
        // most 18 digits, so its i64 operands multiply without overflow; a wider one is
        // computed in i128.
        if (node->Operator == "*") {
            const bool narrow = IsDecimal64(*resultDecimal);
            if (dl && dr) {
                node->Left = DecimalValueAs(types, node->Left, lt, narrow ? *dl : Wide(*dl), node->Location);
                node->Right = DecimalValueAs(types, node->Right, rt, narrow ? *dr : Wide(*dr), node->Location);
                if (!narrow) {
                    e = Call("qdb_decimal_mul", {
                        std::move(node->Left),
                        std::move(node->Right),
                    }, node->Location);
                }
                return Propagate(types, {lt, rt}, result);
            }
            auto& decimalOperand = dl ? node->Left : node->Right;
            auto& integerOperand = dl ? node->Right : node->Left;
            const TDecimalSpec decimal = dl ? *dl : *dr;
            decimalOperand = DecimalValueAs(types, decimalOperand, dl ? lt : rt,
                narrow ? decimal : Wide(decimal), node->Location);
            integerOperand = CastToI64(types, integerOperand, node->Location);
            if (!narrow) {
                e = Call("qdb_decimal_mul_i64", {
                    std::move(decimalOperand),
                    std::move(integerOperand),
                }, node->Location);
            }
            return Propagate(types, {lt, rt}, result);
        }
        if (node->Operator == "/") {
            if (dl && dr) {
                // The dividend is scaled up past both scales, which needs i128 whatever
                // the operands are; an i64 result is narrowed back.
                node->Left = DecimalValueAs(types, node->Left, lt, Wide(*dl), node->Location);
                node->Right = DecimalValueAs(types, node->Right, rt, Wide(*dr), node->Location);
                const int32_t dividendScaleDelta =
                    resultDecimal->Scale + dr->Scale - dl->Scale;
                if (dividendScaleDelta < 0) {
                    throw NQumir::TError(
                        "decimal / decimal produced negative dividend scale delta");
                }
                if (dividendScaleDelta > 0) {
                    node->Left = Call("qdb_decimal_scale_up", {
                        std::move(node->Left),
                        DecimalScaleLiteral(types, node->Location, dividendScaleDelta),
                    }, node->Location);
                }
                e = Call("qdb_decimal_div", {
                    std::move(node->Left),
                    std::move(node->Right),
                }, node->Location);
                if (IsDecimal64(*resultDecimal)) {
                    e = Call("qdb_decimal64_narrow", {std::move(e)}, node->Location);
                }
                return Propagate(types, {lt, rt}, result);
            }
            if (dl && !dr) {
                node->Left = DecimalValueAs(types, node->Left, lt, *dl, node->Location);
                node->Right = CastToI64(types, node->Right, node->Location);
                e = Call(IsDecimal64(*dl) ? "qdb_decimal64_div_i64" : "qdb_decimal_div_i64", {
                    std::move(node->Left),
                    std::move(node->Right),
                }, node->Location);
                return Propagate(types, {lt, rt}, result);
            }
        }
        return Propagate(types, {lt, rt}, result);
    }
    if (auto up = TMaybeNode<TUnaryExpr>(e)) {
        auto node = up.Cast();
        TTypePtr ot = ExpandDecimalNode(types, node->Operand, inputType, env);
        if (auto decimal = DecimalSpecOfValueType(ValueType(ot))) {
            if (node->Operator == "-") {
                node->Operand = DecimalValueAs(types, node->Operand, ot, *decimal, node->Location);
                if (!IsDecimal64(*decimal)) {
                    e = Call("qdb_decimal_neg", {std::move(node->Operand)}, node->Location);
                }
                return Propagate(types, {ot}, MakeDecimalType(types, *decimal));
            }
            ThrowDecimalUnsupported(node->Operator, "unary operator is not implemented");
        }
        return Propagate(types, {ot}, ResultTypeUnary(types, node->Operator, ot));
    }
    if (auto cp = TMaybeNode<TCastExpr>(e)) {
        auto node = cp.Cast();
        TTypePtr ot = ExpandDecimalNode(types, node->Operand, inputType, env);
        TTypePtr target = FromQumirType(types, node->Type);
        if (IsNullableType(target)) {
            if (auto decimal = DecimalSpecOfValueType(UnwrapNullableType(target))) {
                if (ot && !IsBinIntStorageType(ot)) {
                    node->Operand = DecimalValueAs(types, node->Operand, ot, *decimal, node->Location);
                }
                node->Type = ToQumirType(types, target);
                return target;
            }
        }
        if (auto decimal = DecimalSpecOfValueType(target)) {
            e = DecimalValueAs(types, node->Operand, ot, *decimal, node->Location);
            return MakeDecimalType(types, *decimal);
        }
        // DECIMAL -> DOUBLE / INTEGER: the cast-table row from the operand's value type.
        if (auto converted = ot
            ? ApplyDecimalCastRule(types, ValueType(ot), ValueType(target), node->Operand, node->Location)
            : nullptr)
        {
            e = converted;
        }
        return Propagate(types, {ot}, target);
    }
    if (auto cl = TMaybeNode<TCallExpr>(e)) {
        auto node = cl.Cast();
        std::vector<TTypePtr> argTypes;
        argTypes.reserve(node->Args.size());
        for (auto& arg : node->Args) {
            argTypes.push_back(ExpandDecimalNode(types, arg, inputType, env));
        }
        if (auto cid = TMaybeNode<TIdentExpr>(node->Callee)) {
            if (cid.Cast()->Name == "qdb_is_null" || cid.Cast()->Name == "qdb_is_true") {
                return types.Make<TBoolType>();
            }
            // ExpandInList passes a DECIMAL lhs to its table as the digits at the table's scale.
            if (cid.Cast()->Name == "qdb_in_sorted_i64") {
                return types.Make<TBoolType>();
            }
            if (cid.Cast()->Name == "coalesce") {
                auto [value, allNullable] = CoalesceValueType(types, argTypes, node->Args);
                return value && allNullable
                    ? MakeNullableType(types, value)
                    : value;
            }
            if (auto value = NumericPreservingUnaryCall(cid.Cast()->Name, argTypes)) {
                if (auto decimal = DecimalSpecOfValueType(value)) {
                    node->Args[0] = DecimalValueAs(
                        types, std::move(node->Args[0]), argTypes[0], *decimal, node->Location);
                    e = Call(cid.Cast()->Name, {std::move(node->Args[0])}, node->Location);
                }
                return Propagate(types, {argTypes[0]}, value);
            }
            if (cid.Cast()->Name.rfind("qdb_decimal_", 0) == 0) {
                if (cid.Cast()->Name == "qdb_decimal_lt" ||
                    cid.Cast()->Name == "qdb_decimal_le" ||
                    cid.Cast()->Name == "qdb_decimal_gt" ||
                    cid.Cast()->Name == "qdb_decimal_ge" ||
                    cid.Cast()->Name == "qdb_decimal_eq" ||
                    cid.Cast()->Name == "qdb_decimal_ne")
                {
                    return types.Make<TBoolType>();
                }
                return BinIntStorageType(types);
            }
            // To kernels a decimal stored in i64 is a plain i64: a call this pass does not
            // lower would take the unscaled digits for the number.
            for (const auto& argType : argTypes) {
                auto decimal = DecimalSpecOfValueType(ValueType(argType));
                if (decimal && IsDecimal64(*decimal)) {
                    throw NQumir::TError(node->Location,
                        "function '" + cid.Cast()->Name + "' does not take DECIMAL arguments");
                }
            }
        }
        TTypePtr ret = InferType(types, e, inputType);
        return ret;
    }
    if (auto blk = TMaybeNode<TBlockExpr>(e)) {
        return ExpandDecimalBlock(types, *blk.Cast(), inputType, env);
    }
    if (auto var = TMaybeNode<TVarStmt>(e)) {
        TTypePtr initType = nullptr;
        if (var.Cast()->Init) {
            initType = ExpandDecimalNode(types, var.Cast()->Init, inputType, env);
        }
        TTypePtr varType = var.Cast()->Type ? FromQumirType(types, var.Cast()->Type) : initType;
        if (varType) {
            env[var.Cast()->Name] = varType;
        }
        return varType;
    }
    if (auto assign = TMaybeNode<TAssignExpr>(e)) {
        TTypePtr valueType = ExpandDecimalNode(types, assign.Cast()->Value, inputType, env);
        if (valueType) {
            env[assign.Cast()->Name] = valueType;
        }
        return valueType;
    }
    for (auto* child : e->MutableChildren()) {
        ExpandDecimalNode(types, *child, inputType, env);
    }
    return InferType(types, e, inputType);
}

std::pair<TExprPtr, TTypePtr> ExpandDecimal(NQumir::NAst::TTypeFactory& types, const TExprPtr& expr,
    const TStructType& inputType)
{
    TExprPtr e = CloneExpr(expr);
    TTypeEnv env;
    TTypePtr t = ExpandDecimalNode(types, e, inputType, env);
    return {e, t ? t : InferType(types, e, inputType)};
}

} // namespace

TTypePtr AnnotateExprType(
    NQumir::NAst::TTypeFactory& types,
    const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    TAnnotationScope scope(types, context);
    if (!expr) {
        throw NQumir::TError("project type inference: null expression");
    }
    TTypePtr t = InferType(types, expr, inputType);
    if (IsUntypedNull(expr, t)) {
        t = MakeNullableType(types, UntypedNullValueType(types));
    }
    if (!t) {
        throw NQumir::TError("project type inference: could not infer type of '" +
            NQumir::NAst::NCore::PrintAst(expr) + "'");
    }
    return t;
}

bool IsUntypedNullValue(
    NQumir::NAst::TTypeFactory& types,
    const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    TAnnotationScope scope(types, context);
    return expr && IsUntypedNull(expr, InferType(types, expr, inputType));
}

TTypePtr EffectiveBinaryNumericType(
    const TExprPtr& left,
    const TExprPtr& right)
{
    if (!left || !right) {
        return nullptr;
    }
    TTypePtr leftType = UnwrapNamedValueType(ValueType(left->Type));
    TTypePtr rightType = UnwrapNamedValueType(ValueType(right->Type));
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
    NQumir::NAst::TTypeFactory& types,
    const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    TAnnotationScope scope(types, context);
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
            auto leftType = UnwrapNamedValueType(ValueType(binary.Cast()->Left->Type));
            auto rightType = UnwrapNamedValueType(ValueType(binary.Cast()->Right->Type));
            if (IsInt(leftType) && IsInt(rightType)) {
                if (IntLiteralFits(binary.Cast()->Right, leftType)) {
                    binary.Cast()->Right->Type = leftType;
                } else if (IntLiteralFits(binary.Cast()->Left, rightType)) {
                    binary.Cast()->Left->Type = rightType;
                }
            }
        }
        TTypePtr type = nullptr;
        try {
            type = InferType(types, node, inputType);
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
// comparisons, strcat/upper/lower -> their arena calls, qdb_string_view_sql_like -> a
// constant-pattern fast path, a cast to a nullable target -> nullable_from_value (the raw
// planner Nullable won't compile), a cast the cast table rewrites or rejects (integer ->
// VARCHAR, VARCHAR -> DATE), a cast to VARCHAR -> its StringView spelling, and a `%` or
// decimal `/` by what may be zero -> a NULLIF guard. Float division keeps the fast path:
// it has no such rewrite.
bool ContainsMustExpandCall(NQumir::NAst::TTypeFactory& types, const TExprPtr& e, const TStructType& inputType) {
    if (!e) return false;
    if (auto call = TMaybeNode<TCallExpr>(e)) {
        if (auto id = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
            const std::string& name = id.Cast()->Name;
            if (name == "coalesce" || name == "qdb_in_list" || ArenaStringFunction(name) ||
                name == "qdb_string_view_sql_like")
            {
                return true;
            }
        }
    }
    if (auto cast = TMaybeNode<TCastExpr>(e)) {
        if (IsNullableType(cast.Cast()->Type)) return true;
        if (TMaybeType<TStringType>(cast.Cast()->Type)) return true;
        if (HasCastRule(ValueType(InferType(types, cast.Cast()->Operand, inputType)),
                UnwrapNullableType(cast.Cast()->Type)))
        {
            return true;
        }
    }
    if (auto bin = TMaybeNode<TBinaryExpr>(e)) {
        const TOperator op = bin.Cast()->Operator;
        if ((op == "%" || op == "/") && MayBeZero(bin.Cast()->Right)) {
            if (TTypePtr type = InferType(types, e, inputType); type && IsNullOnZeroDivisor(op, type)) {
                return true;
            }
        }
    }
    for (const auto& child : e->Children()) {
        if (ContainsMustExpandCall(types, child, inputType)) return true;
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

// Whether the expression reads a REAL value: a REAL column, a cast to REAL, a REAL literal.
bool InvolvesReal(NQumir::NAst::TTypeFactory& types, const TExprPtr& e, const TStructType& inputType) {
    if (!e) {
        return false;
    }
    if (auto id = TMaybeNode<TIdentExpr>(e)) {
        for (const auto& [name, type] : inputType.Fields) {
            if (name == id.Cast()->Name) {
                return IsReal(UnwrapNamedValueType(ValueType(type)));
            }
        }
        return false;
    }
    if (TMaybeNode<TCastExpr>(e) || TMaybeNode<TNumberExpr>(e)) {
        if (IsReal(UnwrapNamedValueType(ValueType(FromQumirType(types, e->Type))))) {
            return true;
        }
    }
    for (const auto& child : e->Children()) {
        if (InvolvesReal(types, child, inputType)) {
            return true;
        }
    }
    return false;
}

bool ReadsColumn(const TExprPtr& e, const TStructType& inputType) {
    if (auto id = TMaybeNode<TIdentExpr>(e)) {
        for (const auto& [name, _] : inputType.Fields) {
            if (name == id.Cast()->Name) {
                return true;
            }
        }
    }
    for (const auto& child : e->Children()) {
        if (child && ReadsColumn(child, inputType)) {
            return true;
        }
    }
    return false;
}

TExprPtr CastTo(NQumir::NAst::TTypeFactory& types, TExprPtr e, TFloatType::EKind kind) {
    auto loc = e->Location;
    return NQumir::MakeIntrusive<TCastExpr>(loc, std::move(e), types.MakeFloat(kind));
}

// Qumir computes a float beside an integer or a float literal at the float's width; the
// planner types REAL as PostgreSQL does. Writes the casts that make the kernel compute the
// planner's types: a REAL operand beside another number is widened (BinaryValueType), and a
// CASE branch, a COALESCE argument or an IN-list constant that REAL absorbs is narrowed.
void CoerceRealOperands(NQumir::NAst::TTypeFactory& types, TExprPtr& e, const TStructType& inputType) {
    for (auto* child : e->MutableChildren()) {
        if (*child) {
            CoerceRealOperands(types, *child, inputType);
        }
    }
    auto typeOf = [&](const TExprPtr& operand) {
        return operand ? ValueType(InferType(types, operand, inputType)) : nullptr;
    };
    auto narrow = [&](TExprPtr& operand) {
        const auto type = typeOf(operand);
        if (type && !IsReal(UnwrapNamedValueType(type))) {
            operand = CastTo(types, operand, TFloatType::F32);
        }
    };
    if (auto bin = TMaybeNode<TBinaryExpr>(e); bin && !IsLogical(bin.Cast()->Operator)) {
        auto node = bin.Cast();
        const auto lt = typeOf(node->Left);
        if (MixesReal(lt, typeOf(node->Right))) {
            auto& real = IsReal(UnwrapNamedValueType(lt)) ? node->Left : node->Right;
            real = CastTo(types, real, TFloatType::F64);
        }
        return;
    }
    if (auto ifp = TMaybeNode<TIfExpr>(e)) {
        auto node = ifp.Cast();
        const auto value = UnifyBranchTypes(types, typeOf(node->Then), typeOf(node->Else),
            node->Then, node->Else, &inputType);
        if (IsReal(UnwrapNamedValueType(ValueType(value)))) {
            narrow(node->Then);
            if (node->Else) {
                narrow(node->Else);
            }
        }
        return;
    }
    auto call = TMaybeNode<TCallExpr>(e);
    if (!call) {
        return;
    }
    auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee);
    if (!callee) {
        return;
    }
    auto& args = call.Cast()->Args;
    if (callee.Cast()->Name == "coalesce") {
        std::vector<TTypePtr> argTypes;
        for (const auto& arg : args) {
            argTypes.push_back(typeOf(arg));
        }
        if (IsRealCommonType(types, argTypes, args, &inputType)) {
            for (auto& arg : args) {
                narrow(arg);
            }
        }
        return;
    }
    if (callee.Cast()->Name != "qdb_in_list" || args.size() < 2) {
        return;
    }
    // PostgreSQL compares a list of two or more constants in the common type of the list
    // and its left operand; every other item is a separate `=`.
    const auto lhs = typeOf(args[0]);
    std::vector<TExprPtr*> constants;
    std::vector<TTypePtr> argTypes{lhs};
    std::vector<TExprPtr> exprs{args[0]};
    for (size_t i = 1; i < args.size(); ++i) {
        if (!ReadsColumn(args[i], inputType)) {
            constants.push_back(&args[i]);
            argTypes.push_back(typeOf(args[i]));
            exprs.push_back(args[i]);
        }
    }
    if (lhs && IsReal(UnwrapNamedValueType(lhs)) && constants.size() >= 2
        && IsRealCommonType(types, argTypes, exprs, &inputType))
    {
        for (auto* item : constants) {
            narrow(*item);
        }
    }
    // The item of a mixed `=` is cast to DOUBLE, which widens a REAL left operand too.
    for (size_t i = 1; i < args.size(); ++i) {
        if (MixesReal(lhs, typeOf(args[i]))) {
            args[i] = CastTo(types, args[i], TFloatType::F64);
        }
    }
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
    NQumir::NAst::TTypeFactory& types,
    const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    TAnnotationScope scope(types, context);
    // Fast path: with no nullable column and no bare NULL there is nothing to propagate,
    // so leave the expression untouched (non-nullable path stays ordinary).
    bool anyNullable = false;
    for (const auto& [name, type] : inputType.Fields) {
        if (IsNullableType(type)) { anyNullable = true; break; }
    }
    if (!anyNullable && !ContainsNull(expr) && !ContainsMustExpandCall(types, expr, inputType)) {
        return {expr, InferType(types, expr, inputType)};
    }
    // Rewrite a clone - this runs at kernel build on the plan's shared expression, which
    // must stay intact (a node may be lowered into more than one kernel).
    TExprPtr e = CloneExpr(expr);
    uint64_t counter = 0; // per-query temp-name counter (unique within this expansion)
    TTypePtr t = Expand(types, e, inputType, counter);
    return {e, t};
}

bool IsNullStrictCallArgument(const std::string& name, size_t index) {
    if (IsExcludedCall(name)) {
        return false;
    }
    return name != "qdb_in_list" || index == 0;
}

std::pair<TExprPtr, TTypePtr> ExpandKernelExpr(NQumir::NAst::TTypeFactory& types, const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    TAnnotationScope scope(types, context);
    TExprPtr coerced = expr;
    if (InvolvesReal(types, expr, inputType)) {
        coerced = CloneExpr(expr);
        CoerceRealOperands(types, coerced, inputType);
    }
    auto nullable = ExpandNullable(types, coerced, inputType, context);
    auto expanded = !SchemaHasDecimal(inputType) && !ContainsDecimalTypeRef(nullable.first)
        ? std::move(nullable)
        : ExpandDecimal(types, nullable.first, inputType);
    // The plan's own nodes keep their DATE types: a node may be lowered into more than one kernel.
    if (expanded.first == expr && TDateEraser(types).Holds(expr)) {
        expanded.first = CloneExpr(expr);
    }
    EraseDateTypes(types, expanded.first);
    return expanded;
}

std::pair<TExprPtr, TTypePtr> ExpandKernelPredicate(NQumir::NAst::TTypeFactory& types, const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    return TypeUntypedNull(types,
        ExpandKernelExpr(types, expr, inputType, context),
        types.Make<TBoolType>());
}

std::pair<TExprPtr, TTypePtr> ExpandKernelValue(NQumir::NAst::TTypeFactory& types, const TExprPtr& expr,
    const TStructType& inputType,
    const TAnnotationContext* context)
{
    return TypeUntypedNull(types,
        ExpandKernelExpr(types, expr, inputType, context),
        UntypedNullValueType(types));
}

} // namespace NQdb::NKernel
