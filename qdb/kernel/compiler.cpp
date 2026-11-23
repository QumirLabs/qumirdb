#include <qdb/kernel/compiler.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/builder.h>
#include <qdb/kernel/column_value.h>
#include <qdb/kernel/finalize.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/types/date.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/types/type_text.h>
#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/join_gen.h>
#include <qdb/kernel/join_key.h>
#include <qdb/kernel/lib.h>
#include <qdb/kernel/query_modules.h>
#include <qdb/kernel/spec.h>
#include <qdb/exec/runtime_context.h>
#include <qdb/exec/runtime_filter.h>
#include <qdb/exec/instrumentation/operator.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/utils/module_path.h>

#include <qumir/error.h>
#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace NQdb {

static_assert(sizeof(TRowSet) == TKernelCompiler::RowSetSize);

namespace {

using NQumir::NRegistry::QumirDbNamedType;

void PrintKernelAst(
    std::ostream* out,
    const std::string& nodeName,
    const NQumir::NAst::TExprPtr& ast)
{
    if (!out) {
        return;
    }
    *out << "\n========== RUNTIME NODE: " << nodeName << " ==========\n"
         << "----- AST -----\n";
    NQumir::NAst::NCore::PrintAst(*out, ast);
    *out << "\n----- IR / LLVM -----\n";
}

void FinishKernelDiagnostics(std::ostream* out) {
    if (out) {
        *out << "========== END RUNTIME NODE ==========\n";
    }
}

NQumir::NAst::TExprPtr ClonePredicate(
    const NQumir::NAst::TExprPtr& predicate)
{
    return CloneExpr(predicate);
}

// A join residual prepared for the generated jt_residual_filter, which reads
// the inner join row (left fields ++ right fields, split at LeftFieldCount)
// from both row stores. Predicate is null when the join has no residual.
struct TJoinResidual {
    NQumir::NAst::TExprPtr Predicate;
    NQumir::NAst::TTypePtr InnerType = nullptr;
    size_t LeftFieldCount = 0;

    const NQumir::NAst::TStructType* Inner() const {
        return static_cast<const NQumir::NAst::TStructType*>(InnerType);
    }
};

TJoinResidual PrepareJoinResidual(
    NQumir::NAst::TTypeFactory& types,
    const NKernel::TOperatorKernelSpec& spec,
    const NKernel::TAnnotationContext& annotation,
    const std::string& kernel)
{
    using namespace NQumir::NAst;
    if (spec.Expressions.empty()) {
        return {};
    }
    auto innerOutput = ComputeJoinOutputType(
        types, spec.InputSchemas[0], spec.InputSchemas[1], EJoinType::Inner);
    if (!innerOutput) {
        throw NQumir::TError(
            kernel + ": residual filter: " + innerOutput.error().ToString());
    }
    const auto* innerType = static_cast<const TStructType*>(*innerOutput);
    return TJoinResidual{
        .Predicate = NKernel::ExpandKernelPredicate(
            types, ClonePredicate(spec.Expressions[0]), *innerType, annotation).first,
        .InnerType = *innerOutput,
        .LeftFieldCount =
            static_cast<const TStructType*>(spec.InputSchemas[0])->Fields.size(),
    };
}

// Replaces the library's default jt_residual_filter (join_residual_default.oz)
// with the generated one, keeping its position before the helpers that call it.
void InjectJoinResidualFilter(
    NQumir::NAst::TTypeFactory& types,
    std::vector<NQumir::NAst::TExprPtr>& library,
    const NQumir::NAst::TExprPtr& predicate,
    const NQumir::NAst::TStructType& innerType,
    size_t leftFieldCount,
    const NQumir::NAst::TTypePtr& columnType,
    const NQumir::NAst::TTypePtr& rowSetType,
    const NQumir::NAst::TTypePtr& stringViewType)
{
    for (auto& f : library) {
        auto fun = NQumir::NAst::TMaybeNode<NQumir::NAst::TFunDecl>(f);
        if (fun && fun.Cast()->Name == "jt_residual_filter") {
            f = NKernel::GenJoinResidualFilterAst(
                types, predicate, innerType, leftFieldCount,
                columnType, rowSetType, stringViewType);
            return;
        }
    }
}

} // namespace

void EnsureQumirDbUse(const NQumir::NAst::TExprPtr& ast) {
    using namespace NQumir::NAst;
    auto block = TMaybeNode<TBlockExpr>(ast);
    if (!block) {
        return;
    }
    for (const auto& stmt : block.Cast()->Stmts) {
        auto use = TMaybeNode<TUseExpr>(stmt);
        if (use && use.Cast()->ModuleName == "qumirdb") {
            return;
        }
    }
    block.Cast()->Stmts.insert(
        block.Cast()->Stmts.begin(),
        NQumir::MakeIntrusive<TUseExpr>(NQumir::TLocation{}, "qumirdb"));
}

std::expected<NQumir::NFrontend::TComposeResult, NQumir::TError> ComposeKernelProgram(
    NKernel::TQueryModules& modules,
    NQumir::NFrontend::TComposeResult program)
{
    using namespace NQumir::NAst;
    EnsureQumirDbUse(program.Ast);
    NQumir::NFrontend::TSourceModuleLoader loader(modules.Types());
    if (auto loaded = modules.Load(loader, NUtils::ModuleFile("qumirdb.oz")); !loaded) {
        return std::unexpected(loaded.error());
    }
    auto composed = NQumir::NFrontend::LoadAndCompose(
        loader, program.Ast, program.Pragmas, /*cloneSourceModules*/ true);
    if (!composed) {
        return std::unexpected(composed.error());
    }
    std::erase_if(TMaybeNode<TBlockExpr>(composed->Ast).Cast()->Stmts, [](const TExprPtr& stmt) {
        auto use = TMaybeNode<TUseExpr>(stmt);
        return use && use.Cast()->Resolved;
    });
    composed->LlvmBitcode.insert(
        composed->LlvmBitcode.end(), program.LlvmBitcode.begin(), program.LlvmBitcode.end());
    return composed;
}

namespace {

// Composes the program with qumirdb.oz and hands it to `compile`; on a compose error sets
// *error and returns an empty result.
template <class TResult, class TCompile>
TResult CompileComposed(
    NKernel::TQueryModules& modules,
    NQumir::NFrontend::TComposeResult program,
    std::string* error,
    TCompile&& compile)
{
    auto composed = ComposeKernelProgram(modules, std::move(program));
    if (!composed) {
        if (error) {
            *error = composed.error().ToString();
        }
        return {};
    }
    return compile(std::move(*composed));
}

} // namespace

std::unordered_map<std::string, void*> CompileKernelAst(
    NQumir::TLLVMRunner& runner,
    NKernel::TQueryModules& modules,
    NQumir::NFrontend::TComposeResult program,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    return CompileComposed<std::unordered_map<std::string, void*>>(
        modules, std::move(program), error, [&](NQumir::NFrontend::TComposeResult composed) {
            return runner.CompileKernelAst(std::move(composed), entryNames, error);
        });
}

namespace {
constexpr const char* CacheSchemaVersion = "v1";
// Bump when the generated key helpers or the .oz kernel libraries change.
// The row join table adds join_rows.oz and the lookup-key equality overload.
// Join dispatch entrypoints take the residual's string arena.
// qumirdb.oz gains upper and lower, which map strings in blocks of 16 bytes.
// The row join table pads the unmatched probe rows of an outer join.
// Semi and anti joins can build their left side and mark its rows.
// The row table moves to kernel/row_table.oz under row names.
// The aggregate table gains the row layout; HashTable embeds it.
// HashTable counts the rebuilds of the row layout for instrumentation.
// Decimal and i128 keys are aligned to 16 bytes in key layouts.
// qumirdb.oz gains qdb_decimal_mul (decimal * decimal).
// qumir aligns a struct field that holds an i128 to 16 (QumirLabs/qumir#169).
// qumirdb.oz stores DECIMAL(p <= 18) in i64 and checks its range (qdb_decimal64_*).
// Hash and compare f32 keys by their canonical 32-bit bits.
// radix.oz gains qumir_radix_key(f32).
// row_group_interval.oz gains f32 operand intervals.
// qumirdb.oz gains the sorted-table lookups of long IN lists (qdb_in_sorted_*).
// qdb_decimal_from_f32 returns right after an overflow, which returns while row errors are deferred.
constexpr const char* KernelLibVersion = "69";
} // namespace

NQumir::NCodeGen::TLlvmRunner::TLinkedModule CompileKernelAstCached(
    NQumir::TLLVMRunner& runner,
    NKernel::TQueryModules& modules,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    std::string* error)
{
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    return CompileComposed<NQumir::NCodeGen::TLlvmRunner::TLinkedModule>(
        modules, {.Ast = std::move(ast)}, error, [&](NQumir::NFrontend::TComposeResult composed) {
            return runner.CompileFusedKernelsCached(
                std::move(composed.Ast),
                entryNames,
                cacheDir,
                CacheSchemaVersion,
                KernelLibVersion,
                error);
        });
}

std::shared_ptr<TKernelSlot> TKernelCompiler::CompileInstrumentation() {
    auto& types = *Types_;
    return EmitInstrumentation().Slot;
}

TGeneratedKernel TKernelCompiler::EmitInstrumentation() {
    auto library = Annotation_.Modules->KernelLibrary(".", "instrumentation.oz");
    if (!library) {
        throw library.error();
    }
    auto ast = NKernel::NOz::Block(std::move(*library));
    return EmitKernel("instrumentation", {"qdb_instrumentation_rows", "qdb_instrumentation_hash"},
        std::move(ast));
}

TGeneratedKernel TKernelCompiler::EmitKernel(
    std::string name,
    std::vector<std::string> entrypoints,
    NQumir::NAst::TExprPtr ast)
{
    NKernel::EraseDateTypes(*Types_, ast);
    TGeneratedKernel kernel{
        .Name = std::move(name),
        .Stage = Stage_,
        .ExecStageId = ExecStageId_,
        .Entrypoints = std::move(entrypoints),
        .Ast = std::move(ast),
        .RuntimeContext = RuntimeContext_,
        .Slot = std::make_shared<TKernelSlot>(),
    };
    kernel.Slot->Fns.resize(kernel.Entrypoints.size(), nullptr);
    if (Sink_) {
        kernel.Index = static_cast<uint32_t>(Sink_->size());
        Sink_->push_back(kernel);
    }
    if (BindNow_) {
        FinalizeKernels(
            std::span(&kernel, 1), Types_, Annotation_, Backend_, Diagnostics_, JitCacheDir_);
    }
    return kernel;
}

NQumir::TLLVMRunnerOptions KernelRunnerOptions() {
    NQumir::TLLVMRunnerOptions opts;
    opts.CoreInput = true;
    opts.ResolveCoreInput = true;
    opts.AllowOverloads = true;
    // TODO: Fix definite-assignment performance on large fused ASTs and re-enable it.
    opts.RunDefiniteAssignment = false;
    opts.OptLevel = 3;
    opts.ModuleFiles = {NUtils::ModuleFile("qumirdb.oz")};
    return opts;
}

std::optional<std::string> CompileKernelAstToObject(
    NQumir::TLLVMRunner& runner,
    NKernel::TQueryModules& modules,
    NQumir::NFrontend::TComposeResult program,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    return CompileComposed<std::optional<std::string>>(
        modules, std::move(program), error, [&](NQumir::NFrontend::TComposeResult composed) {
            return runner.CompileKernelAstToObject(std::move(composed), entryNames, error);
        });
}

std::optional<NQumir::TLLVMRunner::TCachedObjectModule>
CompileKernelAstToObjectsCached(
    NQumir::TLLVMRunner& runner,
    NKernel::TQueryModules& modules,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    std::string* error)
{
    return CompileComposed<std::optional<NQumir::TLLVMRunner::TCachedObjectModule>>(
        modules, {.Ast = std::move(ast)}, error, [&](NQumir::NFrontend::TComposeResult composed) {
            return runner.CompileFusedKernelsToObjectsCached(
                std::move(composed.Ast),
                entryNames,
                cacheDir,
                CacheSchemaVersion,
                KernelLibVersion,
                error);
        });
}

namespace {


NQumir::NAst::TTypePtr SortCoreType(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto valueType = PhysicalValueType(types, type);
    if (auto integer = MaybeIntegerType(valueType)) {
        return types.MakeInteger(integer.Cast()->Kind);
    }
    if (auto floating = TMaybeType<TFloatType>(valueType)) {
        return types.MakeFloat(floating.Cast()->Kind);
    }
    return nullptr;
}

int64_t SortRadixKeyBits(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto valueType = PhysicalValueType(types, type);
    if (auto integer = MaybeIntegerType(valueType)) {
        return integer.Cast()->BitWidth();
    }
    if (auto floating = TMaybeType<TFloatType>(valueType)) {
        return floating.Cast()->BitWidth();
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return 8;
    }
    return 0;
}

bool SortKeyIsString(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TStringType>(
        UnwrapNamedValueType(UnwrapNullableType(type))));
}

bool SortKeyIsBool(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TBoolType>(
        UnwrapNamedValueType(UnwrapNullableType(type))));
}

bool SortValueIsBinInt(const NQumir::NAst::TTypePtr& type) {
    return HasBinIntLayout(type);
}

// The kernel type a decimal window argument is loaded as: BinInt, or the i64 of a
// decimal stored in i64.
NQumir::NAst::TTypePtr WindowDecimalArgType(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& type) {
    return HasBinIntLayout(type)
        ? BinIntStorageType(types)
        : PhysicalValueType(types, type);
}

bool SortValueIsInteger(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    const auto valueType = PhysicalValueType(types, type);
    return static_cast<bool>(MaybeIntegerType(valueType));
}

NQumir::NAst::TExprPtr Int64Literal(NQumir::NAst::TTypeFactory& types, int64_t value) {
    auto expr = NKernel::NOz::Int(types, value);
    expr->Type = types.MakeInteger();
    return expr;
}

NQumir::NAst::TTypePtr Ptr(NQumir::NAst::TTypeFactory& types, NQumir::NAst::TTypePtr type) {
    return types.MakePointer(std::move(type));
}

NQumir::NAst::TExprPtr Cast(
    NQumir::NAst::TExprPtr expr,
    NQumir::NAst::TTypePtr type)
{
    return NQumir::MakeIntrusive<NQumir::NAst::TCastExpr>(
        NQumir::TLocation{}, std::move(expr), std::move(type));
}

NQumir::NAst::TExprPtr BuildRowIdSortCall(
    NQumir::NAst::TTypeFactory& types,
    const TSortRadixKeyInput& key,
    bool nullable,
    NQumir::NAst::TExprPtr desc,
    NQumir::NAst::TExprPtr nullsFirst,
    std::string storeName)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto columnIndex = Int64Literal(types, key.ColumnIndex);
    auto ptrU64 = Ptr(types, types.MakeInteger(TIntegerType::U64));

    if (SortKeyIsString(key.Type)) {
        std::vector<TExprPtr> args{
            Oz::Ident(storeName),
            std::move(columnIndex),
            Oz::Ident("row_ids"),
            Cast(Oz::Ident("work"), ptrU64),
            Oz::Ident("counts"),
            Oz::Ident("n"),
            std::move(desc),
        };
        if (nullable) {
            args.push_back(std::move(nullsFirst));
        }
        return Oz::Call(
            nullable ? "sort_string_rowids_nullable" : "sort_string_rowids",
            std::move(args));
    }
    if (SortKeyIsBool(key.Type)) {
        std::vector<TExprPtr> args{
            Oz::Ident(storeName),
            std::move(columnIndex),
            Oz::Ident("row_ids"),
            Oz::Ident("work"),
            Oz::Ident("counts"),
            Oz::Ident("n"),
            std::move(desc),
        };
        if (nullable) {
            args.push_back(std::move(nullsFirst));
        }
        return Oz::Call(
            nullable ? "sort_bool_rowids_nullable" : "sort_bool_rowids",
            std::move(args));
    }

    if (SortValueIsBinInt(key.Type)) {
        // 128-bit decimal: two-word radix (Lo then Hi) inside sort_binint_rowids.
        std::vector<TExprPtr> args{
            Oz::Ident(storeName),
            std::move(columnIndex),
            Oz::Ident("row_ids"),
            Oz::Ident("work"),
            Oz::Ident("counts"),
            Oz::Ident("n"),
            std::move(desc),
        };
        if (nullable) {
            args.push_back(std::move(nullsFirst));
        }
        return Oz::Call(
            nullable ? "sort_binint_rowids_nullable" : "sort_binint_rowids",
            std::move(args));
    }

    auto coreType = SortCoreType(types, key.Type);
    const int64_t keyBits = SortRadixKeyBits(types, key.Type);
    if (!coreType || keyBits == 0) {
        throw NQumir::TError(
            "CompileRadixSortComposite: unsupported key type " +
            SqlTypeName(key.Type));
    }
    std::vector<TExprPtr> args{
        Oz::Ident(storeName),
        std::move(columnIndex),
        Oz::Ident("row_ids"),
        Oz::Ident("work"),
        Oz::Ident("counts"),
        Oz::Ident("n"),
        Int64Literal(types, keyBits),
        std::move(desc),
    };
    if (nullable) {
        args.push_back(std::move(nullsFirst));
    }
    args.push_back(Cast(Int64Literal(types, 0), Ptr(types, std::move(coreType))));
    return Oz::Call(
        nullable ? "sort_fixed_rowids_nullable" : "sort_fixed_rowids",
        std::move(args));
}

NQumir::NAst::TExprPtr BuildDynamicRowIdSortCall(
    NQumir::NAst::TTypeFactory& types,
    const TSortRadixKeyInput& key,
    size_t keyIdx,
    bool nullable)
{
    namespace Oz = NKernel::NOz;
    auto desc = Oz::Index("descs", Int64Literal(types, static_cast<int64_t>(keyIdx)));
    auto nullsFirst = Oz::Index(
        "nulls_firsts", Int64Literal(types, static_cast<int64_t>(keyIdx)));
    return BuildRowIdSortCall(
        types, key, nullable, std::move(desc), std::move(nullsFirst), "store");
}

NQumir::NAst::TExprPtr BuildRadixCompositeWrapperAst(
    NQumir::NAst::TTypeFactory& types,
    const std::vector<TSortRadixKeyInput>& keys,
    bool nullable)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    auto u32Type = types.MakeInteger(TIntegerType::U32);
    auto boolType = types.Make<TBoolType>();
    auto rowSetPtrType = Ptr(types, QumirDbNamedType(types, "TRowSet"));
    auto ptrI64Type = Ptr(types, i64Type);
    auto ptrU32Type = Ptr(types, u32Type);
    auto ptrBoolType = Ptr(types, boolType);

    Oz::TFunBuilder builder(
        nullable
            ? "qdb_radix_sort_indices_composite_nullable"
            : "qdb_radix_sort_indices_composite");
    builder
        .Param("store", rowSetPtrType)
        .Param("row_ids", ptrI64Type)
        .Param("work", ptrI64Type)
        .Param("counts", ptrU32Type)
        .Param("n", i64Type)
        .Param("descs", ptrBoolType)
        .Return(types.Make<TVoidType>());
    if (nullable) {
        builder.Param("nulls_firsts", ptrBoolType);
    }

    for (size_t k = keys.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        builder.Stmt(BuildDynamicRowIdSortCall(types, keys[keyIdx], keyIdx, nullable));
    }
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr Field(
    NQumir::NAst::TExprPtr object,
    std::string field)
{
    return NQumir::MakeIntrusive<NQumir::NAst::TFieldAccessExpr>(
        NQumir::TLocation{}, std::move(object), std::move(field));
}

NQumir::NAst::TExprPtr Unary(
    std::string op,
    NQumir::NAst::TExprPtr value)
{
    return NQumir::MakeIntrusive<NQumir::NAst::TUnaryExpr>(
        NQumir::TLocation{}, NQumir::NAst::TOperator(op), std::move(value));
}

NQumir::NAst::TExprPtr ArrayAssign(
    std::string name,
    NQumir::NAst::TExprPtr index,
    NQumir::NAst::TExprPtr value)
{
    std::vector<NQumir::NAst::TExprPtr> indices;
    indices.push_back(std::move(index));
    return NQumir::MakeIntrusive<NQumir::NAst::TArrayAssignExpr>(
        NQumir::TLocation{}, std::move(name), std::move(indices), std::move(value));
}

NQumir::NAst::TExprPtr TypedInt(
    NQumir::NAst::TTypeFactory& types,
    int64_t value,
    NQumir::NAst::TTypePtr type)
{
    auto expr = NQumir::MakeIntrusive<NQumir::NAst::TNumberExpr>(
        NQumir::TLocation{}, value, types);
    expr->Type = std::move(type);
    return expr;
}

NQumir::NAst::TExprPtr BoolConst(NQumir::NAst::TTypeFactory& types, bool value) {
    return NKernel::NOz::Bool(types, value);
}

NQumir::NAst::TExprPtr Deref(NQumir::NAst::TTypeFactory& types, NQumir::NAst::TExprPtr ptr) {
    return NKernel::NOz::Index(std::move(ptr), Int64Literal(types, 0));
}

NQumir::NAst::TExprPtr Low32(NQumir::NAst::TTypeFactory& types, NQumir::NAst::TExprPtr value) {
    return NKernel::NOz::Bin(
        NQumir::NAst::TOperator("&"),
        std::move(value),
        Int64Literal(types, 4294967295LL));
}

void AppendSetup(
    NKernel::NOz::TFunBuilder& builder,
    std::vector<NQumir::NAst::TExprPtr>& setup)
{
    for (auto& stmt : setup) {
        builder.Stmt(std::move(stmt));
    }
}

NQumir::NAst::TExprPtr ComparableValue(
    const NKernel::TColumnValueAst& value,
    const NQumir::NAst::TTypePtr& i64Type)
{
    using namespace NQumir::NAst;
    if (TMaybeType<TBoolType>(UnwrapNamedValueType(value.ValueType))) {
        return Cast(value.Value, i64Type);
    }
    return value.Value;
}

void AppendSortLessBody(
    NQumir::NAst::TTypeFactory& types,
    NKernel::NOz::TFunBuilder& builder,
    const std::vector<TSortRadixKeyInput>& keys,
    const std::string& leftColumns,
    const std::string& leftRow,
    const std::string& rightColumns,
    const std::string& rightRow,
    NQumir::NAst::TExprPtr equalResult)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    auto columnType = QumirDbNamedType(types, "TColumn");
    auto stringViewType = QumirDbNamedType(types, "StringView");

    for (size_t k = 0; k < keys.size(); ++k) {
        const auto& key = keys[k];
        const std::string suffix = std::to_string(k);
        const std::string leftCol = "left_col_" + suffix;
        const std::string rightCol = "right_col_" + suffix;

        builder
            .Var(leftCol, columnType)
            .Assign(leftCol,
                Oz::Index(leftColumns, Int64Literal(types, key.ColumnIndex)))
            .Var(rightCol, columnType)
            .Assign(rightCol,
                Oz::Index(rightColumns, Int64Literal(types, key.ColumnIndex)));

        auto leftValue = NKernel::BuildColumnValueAst(
            types, leftCol, leftRow, "left_key_" + suffix,
            key.Type, stringViewType);
        auto rightValue = NKernel::BuildColumnValueAst(
            types, rightCol, rightRow, "right_key_" + suffix,
            key.Type, stringViewType);
        AppendSetup(builder, leftValue.Setup);
        AppendSetup(builder, rightValue.Setup);

        auto anyInvalid = Oz::Bin(TOperator("||"),
            Unary("!", leftValue.IsValid),
            Unary("!", rightValue.IsValid));
        auto validityDiffers = Oz::Bin(TOperator("!="),
            leftValue.IsValid, rightValue.IsValid);
        auto leftBeforeByNull = key.NullsFirst
            ? Unary("!", leftValue.IsValid)
            : leftValue.IsValid;
        builder.Stmt(Oz::If(
            std::move(anyInvalid),
            Oz::Block({
                Oz::If(std::move(validityDiffers),
                    Oz::Block({Oz::Return(std::move(leftBeforeByNull))}))
            })));

        auto leftComparable = ComparableValue(leftValue, i64Type);
        auto rightComparable = ComparableValue(rightValue, i64Type);
        builder.Stmt(Oz::If(
            Oz::Bin(TOperator("<"), leftComparable, rightComparable),
            Oz::Block({Oz::Return(BoolConst(types, !key.Desc))})));
        builder.Stmt(Oz::If(
            Oz::Bin(TOperator("<"), rightComparable, leftComparable),
            Oz::Block({Oz::Return(BoolConst(types, key.Desc))})));
    }

    builder.Stmt(Oz::Return(std::move(equalResult)));
}

NQumir::NAst::TExprPtr BuildTopSortTempBeforeStateAst(
    NQumir::NAst::TTypeFactory& types,
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto rowSetPtrType = Ptr(types, rowSetType);
    auto columnType = QumirDbNamedType(types, "TColumn");
    auto columnPtrType = Ptr(types, columnType);

    Oz::TFunBuilder builder("qdb_top_sort_temp_before_state");
    builder
        .Param("state", rowSetPtrType)
        .Param("batch", rowSetPtrType)
        .Param("state_row", i64Type)
        .Param("temp_row", i64Type)
        .Return(types.Make<TBoolType>())
        .Var("state_rs", rowSetType)
        .Assign("state_rs", Deref(types, Oz::Ident("state")))
        .Var("batch_rs", rowSetType)
        .Assign("batch_rs", Deref(types, Oz::Ident("batch")))
        .Var("state_cols", columnPtrType)
        .Assign("state_cols", Field(Oz::Ident("state_rs"), "Columns"))
        .Var("batch_cols", columnPtrType)
        .Assign("batch_cols", Field(Oz::Ident("batch_rs"), "Columns"));

    AppendSortLessBody(
        types, builder, keys,
        "batch_cols", "temp_row",
        "state_cols", "state_row",
        BoolConst(types, false));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildTopSortMergePicksAst(NQumir::NAst::TTypeFactory& types)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    auto u8Type = types.MakeInteger(TIntegerType::U8);
    auto u32Type = types.MakeInteger(TIntegerType::U32);
    auto rowSetPtrType = Ptr(types, QumirDbNamedType(types, "TRowSet"));
    auto ptrI64Type = Ptr(types, i64Type);
    auto ptrU8Type = Ptr(types, u8Type);
    auto ptrU32Type = Ptr(types, u32Type);

    auto makePickState = [&]() {
        return Oz::Block({
            ArrayAssign("pick_src", Oz::Ident("out"), TypedInt(types, 0, u8Type)),
            ArrayAssign("pick_idx", Oz::Ident("out"),
                Cast(Oz::Ident("left"), u32Type)),
            Oz::Assign("left", Oz::Add(Oz::Ident("left"), Int64Literal(types, 1))),
        });
    };
    auto makePickTemp = [&]() {
        return Oz::Block({
            Oz::Assign("temp_id", Oz::Index("temp_row_ids", Oz::Ident("right"))),
            Oz::Assign("temp_row", Low32(types, Oz::Ident("temp_id"))),
            ArrayAssign("pick_src", Oz::Ident("out"), TypedInt(types, 1, u8Type)),
            ArrayAssign("pick_idx", Oz::Ident("out"),
                Cast(Oz::Ident("temp_row"), u32Type)),
            Oz::Assign("right", Oz::Add(Oz::Ident("right"), Int64Literal(types, 1))),
        });
    };

    Oz::TFunBuilder builder("qdb_top_sort_merge_picks");
    builder
        .Param("state", rowSetPtrType)
        .Param("batch", rowSetPtrType)
        .Param("temp_row_ids", ptrI64Type)
        .Param("temp_n", i64Type)
        .Param("pick_src", ptrU8Type)
        .Param("pick_idx", ptrU32Type)
        .Param("limit", i64Type)
        .Return(i64Type)
        .Var("state_rs", QumirDbNamedType(types, "TRowSet"))
        .Assign("state_rs", Deref(types, Oz::Ident("state")))
        .Var("state_n", i64Type)
        .Assign("state_n", Field(Oz::Ident("state_rs"), "RowCount"))
        .Var("left", i64Type)
        .Assign("left", Int64Literal(types, 0))
        .Var("right", i64Type)
        .Assign("right", Int64Literal(types, 0))
        .Var("out", i64Type)
        .Assign("out", Int64Literal(types, 0))
        .Var("temp_id", i64Type)
        .Assign("temp_id", Int64Literal(types, 0))
        .Var("temp_row", i64Type)
        .Assign("temp_row", Int64Literal(types, 0));

    auto hasOutputRoom = Oz::Bin(TOperator("<"), Oz::Ident("out"), Oz::Ident("limit"));
    auto hasState = Oz::Bin(TOperator("<"), Oz::Ident("left"), Oz::Ident("state_n"));
    auto hasTemp = Oz::Bin(TOperator("<"), Oz::Ident("right"), Oz::Ident("temp_n"));
    auto loopCond = Oz::Bin(TOperator("&&"), std::move(hasOutputRoom),
        Oz::Bin(TOperator("||"), std::move(hasState), std::move(hasTemp)));
    auto tempBeforeState = Oz::Call("qdb_top_sort_temp_before_state", {
        Oz::Ident("state"),
        Oz::Ident("batch"),
        Oz::Ident("left"),
        Oz::Ident("temp_row"),
    });
    auto chooseByCompare = Oz::Block({
        Oz::Assign("temp_id", Oz::Index("temp_row_ids", Oz::Ident("right"))),
        Oz::Assign("temp_row", Low32(types, Oz::Ident("temp_id"))),
        Oz::If(std::move(tempBeforeState), makePickTemp(), makePickState()),
    });
    auto loopBody = Oz::Block({
        Oz::If(
            Oz::Bin(TOperator("=="), Oz::Ident("right"), Oz::Ident("temp_n")),
            makePickState(),
            Oz::If(
                Oz::Bin(TOperator("=="), Oz::Ident("left"), Oz::Ident("state_n")),
                makePickTemp(),
                std::move(chooseByCompare))),
        Oz::Assign("out", Oz::Add(Oz::Ident("out"), Int64Literal(types, 1))),
    });
    builder.Stmt(Oz::While(std::move(loopCond), std::move(loopBody)));
    builder.Stmt(Oz::Return(Oz::Ident("out")));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildTopSortUpdateAst(
    NQumir::NAst::TTypeFactory& types,
    const std::vector<TSortRadixKeyInput>& keys,
    bool materializeOutput)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    auto i8Type = types.MakeInteger(TIntegerType::I8);
    auto u8Type = types.MakeInteger(TIntegerType::U8);
    auto u32Type = types.MakeInteger(TIntegerType::U32);
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto rowSetPtrType = Ptr(types, rowSetType);
    auto rowSetRefType = types.MakeReference(rowSetType);
    auto ptrI64Type = Ptr(types, i64Type);
    auto ptrU32Type = Ptr(types, u32Type);
    auto ptrU8Type = Ptr(types, u8Type);
    auto boolType = types.Make<TBoolType>();

    bool anyNullableKey = false;
    for (const auto& key : keys) {
        anyNullableKey = anyNullableKey || IsNullableType(key.Type);
    }

    Oz::TFunBuilder builder("qdb_top_sort_update");
    builder
        .Param("state", rowSetPtrType)
        .Param("batch", rowSetPtrType)
        .Param("row_ids", ptrI64Type)
        .Param("work", ptrI64Type)
        .Param("counts", ptrU32Type)
        .Param("n", i64Type)
        .Param("sort_input", boolType)
        .Param("pick_src", ptrU8Type)
        .Param("pick_idx", ptrU32Type)
        .Param("limit", i64Type)
        .Return(i64Type);
    if (materializeOutput) {
        builder.Param("out_rowset", rowSetRefType);
    }

    std::vector<TExprPtr> sortStmts;
    for (size_t k = keys.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        sortStmts.push_back(BuildRowIdSortCall(
            types, keys[keyIdx],
            anyNullableKey,
            BoolConst(types, keys[keyIdx].Desc),
            BoolConst(types, keys[keyIdx].NullsFirst),
            "batch"));
    }
    builder.Stmt(Oz::If(
        Oz::Ident("sort_input"), Oz::Block(std::move(sortStmts))));
    builder
        .Var("out_count", i64Type)
        .Assign("out_count", Oz::Call("qdb_top_sort_merge_picks", {
            Oz::Ident("state"),
            Oz::Ident("batch"),
            Oz::Ident("row_ids"),
            Oz::Ident("n"),
            Oz::Ident("pick_src"),
            Oz::Ident("pick_idx"),
            Oz::Ident("limit"),
        }));
    if (!materializeOutput) {
        builder.Stmt(Oz::Return(Oz::Ident("out_count")));
        return std::move(builder).Build();
    }

    builder.Stmt(Oz::If(
        Oz::Bin(TOperator("<="), Oz::Ident("out_count"), Int64Literal(types, 0)),
        Oz::Block({Oz::Return(Int64Literal(types, 0))})));
    builder
        .Var("materialize_store", rowSetPtrType)
        .Assign("materialize_store", Oz::Cast(
            Oz::Call("qdb_alloc", {Oz::Mul(Int64Literal(types, 2),
                Int64Literal(types, static_cast<int64_t>(TKernelCompiler::RowSetSize)))}),
            rowSetPtrType))
        .Stmt(Oz::ArrayAssign("materialize_store", Int64Literal(types, 0), Deref(types, Oz::Ident("state"))))
        .Stmt(Oz::ArrayAssign("materialize_store", Int64Literal(types, 1), Deref(types, Oz::Ident("batch"))))
        .Var("i", i64Type)
        .Assign("i", Int64Literal(types, 0))
        .Stmt(Oz::While(
            Oz::Bin(TOperator("<"), Oz::Ident("i"), Oz::Ident("out_count")),
            Oz::Block({
                Oz::ArrayAssign("row_ids", Oz::Ident("i"),
                    Oz::Bin(TOperator("|"),
                        Oz::Bin(TOperator("<<"),
                            Oz::Cast(Oz::Index("pick_src", Oz::Ident("i")), i64Type),
                            Int64Literal(types, 32)),
                        Oz::Cast(Oz::Index("pick_idx", Oz::Ident("i")), i64Type))),
                Oz::Assign("i", Oz::Add(Oz::Ident("i"), Int64Literal(types, 1))),
            })))
        .Var("materialized", i64Type)
        .Assign("materialized", Oz::Call("sort_materialize_row_ids", {
            Oz::Ident("materialize_store"),
            Oz::Ident("row_ids"),
            Int64Literal(types, 0),
            Oz::Ident("out_count"),
            Oz::Ident("out_rowset"),
        }))
        .Stmt(Oz::Call("qdb_free", {
            Oz::Cast(Oz::Ident("materialize_store"), Ptr(types, i8Type)),
        }))
        .Stmt(Oz::Return(Oz::Ident("materialized")));
    return std::move(builder).Build();
}

// Only integer and float columns take the fixed-column materialize path.
int64_t MaterializeFixedWidth(NQumir::NAst::TTypeFactory& types, const NQumir::NAst::TTypePtr& type) {
    return SortCoreType(types, type) ? NKernel::FixedValueWidth(type) : 0;
}

// Emits an entry that gathers `inputType`'s columns from a sorted row-id slice.
// `entryName` names it. Callers may append computed columns (e.g. window
// functions): reserve `extraColumnCount` column slots and `extraOwnerCount`
// owner slots, and pass `emitExtraColumns` to fill them - it receives the
// builder, the column-slot accessor, and the next free owner index (updated in
// place). Shared by sort (sort_materialize_row_ids) and window.
using TExtraColumnEmitter = std::function<void(
    NKernel::NOz::TFunBuilder&,
    const std::function<NQumir::NAst::TExprPtr(size_t)>&,
    int64_t&)>;

NQumir::NAst::TExprPtr BuildSortMaterializeWrapperAst(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TStructType& inputType,
    const std::string& entryName = "sort_materialize_row_ids",
    int64_t extraColumnCount = 0,
    int64_t extraOwnerCount = 0,
    const TExtraColumnEmitter& emitExtraColumns = {})
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto columnType = QumirDbNamedType(types, "TColumn");
    auto rowSetPtrType = Ptr(types, rowSetType);
    auto rowSetRefType = types.MakeReference(rowSetType);
    auto columnPtrType = Ptr(types, columnType);
    auto ptrI64Type = Ptr(types, i64Type);

    const int64_t columnCount =
        static_cast<int64_t>(inputType.Fields.size()) + extraColumnCount;
    int64_t ownedPtrCount = 1; // columns array, stored by sort_materialize_begin.
    for (const auto& field : inputType.Fields) {
        ownedPtrCount += SortKeyIsString(field.second) ? 3 : 2;
    }
    ownedPtrCount += extraOwnerCount;

    auto column = [&](size_t idx) -> TExprPtr {
        return Oz::Index("columns", Int64Literal(types, static_cast<int64_t>(idx)));
    };

    Oz::TFunBuilder builder(entryName);
    builder
        .Param("store", rowSetPtrType)
        .Param("row_ids", ptrI64Type)
        .Param("start", i64Type)
        .Param("limit", i64Type)
        .Param("out_rowset", rowSetRefType)
        .Return(i64Type)
        .Var("n", i64Type)
        .Assign("n", Oz::Ident("limit"))
        .Stmt(Oz::If(
            Oz::Bin(TOperator("<="), Oz::Ident("n"), Int64Literal(types, 0)),
            Oz::Block({Oz::Return(Int64Literal(types, 0))})))
        .Var("columns", columnPtrType)
        .Assign("columns", Oz::Call("sort_materialize_begin", {
            Int64Literal(types, columnCount),
            Int64Literal(types, ownedPtrCount),
            Oz::Ident("n"),
            Oz::Ident("out_rowset"),
        }));

    int64_t ownerIdx = 2;
    for (size_t c = 0; c < inputType.Fields.size(); ++c) {
        const auto& type = inputType.Fields[c].second;
        const auto srcCol = Int64Literal(types, static_cast<int64_t>(c));
        if (SortKeyIsString(type)) {
            const int64_t offsetsOwner = ownerIdx++;
            const int64_t dataOwner = ownerIdx++;
            const int64_t maskOwner = ownerIdx++;
            builder.Stmt(Oz::Call("sort_materialize_string_column", {
                Oz::Ident("store"),
                Oz::Ident("row_ids"),
                Oz::Ident("start"),
                Oz::Ident("n"),
                srcCol,
                column(c),
                Oz::Ident("out_rowset"),
                Int64Literal(types, offsetsOwner),
                Int64Literal(types, dataOwner),
                Int64Literal(types, maskOwner),
            }));
            continue;
        }
        const int64_t dataOwner = ownerIdx++;
        const int64_t maskOwner = ownerIdx++;
        if (SortKeyIsBool(type)) {
            builder.Stmt(Oz::Call("sort_materialize_bool_column", {
                Oz::Ident("store"),
                Oz::Ident("row_ids"),
                Oz::Ident("start"),
                Oz::Ident("n"),
                srcCol,
                column(c),
                Oz::Ident("out_rowset"),
                Int64Literal(types, dataOwner),
                Int64Literal(types, maskOwner),
            }));
            continue;
        }
        if (SortValueIsBinInt(type)) {
            builder.Stmt(Oz::Call("sort_materialize_binint_column", {
                Oz::Ident("store"),
                Oz::Ident("row_ids"),
                Oz::Ident("start"),
                Oz::Ident("n"),
                srcCol,
                column(c),
                Oz::Ident("out_rowset"),
                Int64Literal(types, dataOwner),
                Int64Literal(types, maskOwner),
            }));
            continue;
        }
        auto coreType = SortCoreType(types, type);
        const int64_t width = MaterializeFixedWidth(types, type);
        if (!coreType || width == 0) {
            throw NQumir::TError(
                "BuildSortMaterializeWrapperAst: unsupported column type " +
                SqlTypeName(type));
        }
        builder.Stmt(Oz::Call("sort_materialize_fixed_column", {
            Oz::Ident("store"),
            Oz::Ident("row_ids"),
            Oz::Ident("start"),
            Oz::Ident("n"),
            srcCol,
            column(c),
            Oz::Ident("out_rowset"),
            Int64Literal(types, dataOwner),
            Int64Literal(types, maskOwner),
            Int64Literal(types, width),
            Cast(Int64Literal(types, 0), Ptr(types, std::move(coreType))),
        }));
    }
    if (emitExtraColumns) {
        emitExtraColumns(builder, column, ownerIdx);
    }
    builder.Stmt(Oz::Return(Oz::Ident("n")));
    return std::move(builder).Build();
}

// Builds a `runName` stage entry (sort ABI): optionally sorts the row-ids by
// `keys`, then materializes the [start, limit) slice via `materializeEntry`.
// Shared by sort (sort_materialize_row_ids) and window (window_materialize_row_ids).
NQumir::NAst::TExprPtr BuildSortRunWrapperAst(
    NQumir::NAst::TTypeFactory& types,
    const std::string& runName,
    const std::vector<TSortRadixKeyInput>& keys,
    bool nullable,
    const std::string& materializeEntry)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    auto u32Type = types.MakeInteger(TIntegerType::U32);
    auto boolType = types.Make<TBoolType>();
    auto rowSetPtrType = Ptr(types, QumirDbNamedType(types, "TRowSet"));
    auto rowSetRefType = types.MakeReference(QumirDbNamedType(types, "TRowSet"));
    auto ptrI64Type = Ptr(types, i64Type);
    auto ptrU32Type = Ptr(types, u32Type);
    auto ptrBoolType = Ptr(types, boolType);

    Oz::TFunBuilder builder(runName);
    builder
        .Param("store", rowSetPtrType)
        .Param("row_ids", ptrI64Type)
        .Param("work", ptrI64Type)
        .Param("counts", ptrU32Type)
        .Param("n", i64Type)
        .Param("descs", ptrBoolType)
        .Param("nulls_firsts", ptrBoolType)
        .Param("do_sort", boolType)
        .Param("start", i64Type)
        .Param("limit", i64Type)
        .Param("out_rowset", rowSetRefType)
        .Return(i64Type);

    std::vector<TExprPtr> sortStmts;
    for (size_t k = keys.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        auto desc = Oz::Index("descs", Int64Literal(types, static_cast<int64_t>(keyIdx)));
        auto nullsFirst = Oz::Index(
            "nulls_firsts", Int64Literal(types, static_cast<int64_t>(keyIdx)));
        sortStmts.push_back(BuildRowIdSortCall(
            types, keys[keyIdx], nullable, std::move(desc), std::move(nullsFirst), "store"));
    }
    builder.Stmt(Oz::If(Oz::Ident("do_sort"), Oz::Block(std::move(sortStmts))));
    builder.Stmt(Oz::If(
        Oz::Bin(TOperator("<="), Oz::Ident("limit"), Int64Literal(types, 0)),
        Oz::Block({Oz::Return(Int64Literal(types, 0))})));
    builder.Stmt(Oz::Return(Oz::Call(materializeEntry, {
        Oz::Ident("store"),
        Oz::Ident("row_ids"),
        Oz::Ident("start"),
        Oz::Ident("limit"),
        Oz::Ident("out_rowset"),
    })));
    return std::move(builder).Build();
}

void AddSortMaterializeLibrary(
    NKernel::TQueryModules& modules,
    std::vector<NQumir::NAst::TExprPtr>& programStmts,
    const char* errorPrefix)
{
    using namespace NQumir::NAst;
    for (const auto& name : {"materialize.oz"}) {
        auto library = modules.KernelLibrary("sort", name);
        if (!library) {
            throw NQumir::TError(
                std::string(errorPrefix) + ": " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    }
    auto stringOps = modules.KernelLibrary("aggregation", "string_ops.oz");
    if (!stringOps) {
        throw NQumir::TError(
            std::string(errorPrefix) + ": string_ops.oz: " +
            stringOps.error().ToString());
    }
    for (auto& stmt : *stringOps) {
        if (TMaybeNode<TUseExpr>(stmt)) {
            continue;
        }
        programStmts.push_back(std::move(stmt));
    }
}

// Like BuildSortMaterializeWrapperAst, but the output rowset carries the input
// columns (in sorted order) followed by one computed column per window function
// (entry window_materialize_row_ids). Each window column is filled by a scan
// over the sorted row-id slice (window.oz).
NQumir::NAst::TExprPtr BuildWindowMaterializeWrapperAst(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TWindowFuncInput>& funcs)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    const int64_t inputColumnCount = static_cast<int64_t>(inputType.Fields.size());
    int64_t extraOwnerCount = 0;
    for (const auto& func : funcs) {
        // data; plus Mask for aggregates.
        extraOwnerCount += func.Func == "rank" || func.Func == "row_number" ? 1 : 2;
    }

    auto emitWindowColumns = [&](
        Oz::TFunBuilder& builder,
        const std::function<TExprPtr(size_t)>& column,
        int64_t& ownerIdx)
    {
        for (size_t f = 0; f < funcs.size(); ++f) {
            const int64_t dataOwner = ownerIdx++;
            if (funcs[f].Func == "rank" || funcs[f].Func == "row_number") {
                const char* filler = funcs[f].Func == "rank"
                    ? "window_fill_rank"
                    : "window_fill_row_number";
                builder.Stmt(Oz::Call(filler, {
                    Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                    Oz::Ident("n"),
                    column(static_cast<size_t>(inputColumnCount) + f),
                    Oz::Ident("out_rowset"), Int64Literal(types, dataOwner),
                }));
            } else if (funcs[f].Func == "avg_partition") {
                const int64_t maskOwner = ownerIdx++;
                const auto& argType =
                    inputType.Fields[static_cast<size_t>(funcs[f].ArgColumn)].second;
                if (SortValueIsBinInt(argType) || IsDecimalType(argType)) {
                    // Scale the sum up to the result decimal's scale before dividing
                    // by the count, so the quotient keeps fractional precision. The
                    // delta is exactly the scale the result type added over the arg.
                    auto argSpec = DecimalSpecOfValueType(UnwrapNullableType(argType));
                    auto resSpec = DecimalSpecOfValueType(
                        UnwrapNullableType(funcs[f].ResultType));
                    const int64_t scaleDelta =
                        (argSpec && resSpec) ? (resSpec->Scale - argSpec->Scale) : 0;
                    auto outType = resSpec
                        ? DecimalStorageType(types, *resSpec)
                        : BinIntStorageType(types);
                    const int64_t outBytes = resSpec
                        ? DecimalStorageBytes(*resSpec)
                        : 16;
                    builder.Stmt(Oz::Call("window_fill_partition_avg_binint", {
                        Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                        Oz::Ident("n"),
                        Int64Literal(types, static_cast<int64_t>(funcs[f].ArgColumn)),
                        Cast(Int64Literal(types, 0), Ptr(types, WindowDecimalArgType(types, argType))),
                        Cast(Int64Literal(types, 0), Ptr(types, std::move(outType))),
                        Int64Literal(types, outBytes),
                        column(static_cast<size_t>(inputColumnCount) + f),
                        Oz::Ident("out_rowset"), Int64Literal(types, dataOwner),
                        Int64Literal(types, maskOwner),
                        Int64Literal(types, scaleDelta),
                    }));
                    continue;
                }
                auto argCoreType = SortCoreType(types, argType);
                if (!argCoreType) {
                    throw NQumir::TError(
                        "BuildWindowMaterializeWrapperAst: unsupported avg argument type " +
                        SqlTypeName(argType));
                }
                builder.Stmt(Oz::Call("window_fill_partition_avg_f64", {
                    Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                    Oz::Ident("n"),
                    Int64Literal(types, static_cast<int64_t>(funcs[f].ArgColumn)),
                    Cast(Int64Literal(types, 0), Ptr(types, std::move(argCoreType))),
                    column(static_cast<size_t>(inputColumnCount) + f),
                    Oz::Ident("out_rowset"), Int64Literal(types, dataOwner),
                    Int64Literal(types, maskOwner),
                }));
            } else if (funcs[f].Func == "sum_prefix" ||
                       funcs[f].Func == "sum_partition" ||
                       funcs[f].Func == "max_prefix")
            {
                const int64_t maskOwner = ownerIdx++;
                const auto& argType =
                    inputType.Fields[static_cast<size_t>(funcs[f].ArgColumn)].second;
                const bool isSum = funcs[f].Func != "max_prefix";
                // A decimal sum accumulates in i128 whatever the argument's storage.
                const bool binInt = SortValueIsBinInt(argType) ||
                    (isSum && IsDecimalType(argType));
                const bool isPrefix = funcs[f].Func != "sum_partition";
                if (binInt) {
                    const char* filler = funcs[f].Func == "sum_prefix"
                        ? "window_fill_prefix_sum_binint"
                        : funcs[f].Func == "sum_partition"
                            ? "window_fill_partition_sum_binint"
                            : "window_fill_prefix_max_binint";
                    std::vector<TExprPtr> args = {
                        Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                        Oz::Ident("n"),
                        Int64Literal(types, static_cast<int64_t>(funcs[f].ArgColumn)),
                    };
                    if (isSum) {
                        args.push_back(Cast(Int64Literal(types, 0), Ptr(types, WindowDecimalArgType(types, argType))));
                    }
                    args.insert(args.end(), {
                        column(static_cast<size_t>(inputColumnCount) + f),
                        Oz::Ident("out_rowset"), Int64Literal(types, dataOwner),
                        Int64Literal(types, maskOwner),
                    });
                    if (isPrefix) {
                        args.push_back(Oz::Bool(types, funcs[f].PeerInclusive));
                    }
                    builder.Stmt(Oz::Call(filler, std::move(args)));
                    continue;
                }
                auto argCoreType = SortCoreType(types, argType);
                const int64_t width = MaterializeFixedWidth(types, argType);
                if (!argCoreType || width == 0) {
                    throw NQumir::TError(
                        "BuildWindowMaterializeWrapperAst: unsupported window argument type " +
                        SqlTypeName(argType));
                }
                const char* filler = funcs[f].Func == "sum_prefix"
                    ? "window_fill_prefix_sum_fixed"
                    : funcs[f].Func == "sum_partition"
                        ? "window_fill_partition_sum_fixed"
                        : "window_fill_prefix_max_fixed";
                const bool integerSum =
                    (funcs[f].Func == "sum_prefix" || funcs[f].Func == "sum_partition") &&
                    SortValueIsInteger(types, argType);
                std::vector<TExprPtr> args = {
                    Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                    Oz::Ident("n"), Int64Literal(types, static_cast<int64_t>(funcs[f].ArgColumn)),
                    Cast(Int64Literal(types, 0), Ptr(types, std::move(argCoreType))),
                };
                if (funcs[f].Func == "sum_prefix" || funcs[f].Func == "sum_partition") {
                    // Integer sums widen to i64 and float sums to f64, as AggregateResultType.
                    const auto argValueType = PhysicalValueType(types, argType);
                    const bool floatSum = !integerSum && TMaybeType<TFloatType>(argValueType);
                    TTypePtr outCoreType = integerSum
                        ? TTypePtr(types.MakeInteger())
                        : (floatSum ? TTypePtr(types.MakeFloat()) : SortCoreType(types, argType));
                    const int64_t outWidth = (integerSum || floatSum) ? 8 : width;
                    if (!outCoreType || outWidth == 0) {
                        throw NQumir::TError(
                            "BuildWindowMaterializeWrapperAst: unsupported window sum result type " +
                            SqlTypeName(argType));
                    }
                    args.push_back(Cast(Int64Literal(types, 0), Ptr(types, std::move(outCoreType))));
                    args.push_back(Int64Literal(types, outWidth));
                } else {
                    args.push_back(Int64Literal(types, width));
                }
                args.push_back(column(static_cast<size_t>(inputColumnCount) + f));
                args.push_back(Oz::Ident("out_rowset"));
                args.push_back(Int64Literal(types, dataOwner));
                args.push_back(Int64Literal(types, maskOwner));
                if (isPrefix) {
                    args.push_back(Oz::Bool(types, funcs[f].PeerInclusive));
                }
                builder.Stmt(Oz::Call(filler, std::move(args)));
            } else {
                throw NQumir::TError(
                    "BuildWindowMaterializeWrapperAst: unsupported window function " +
                    funcs[f].Func);
            }
        }
    };

    return BuildSortMaterializeWrapperAst(
        types, inputType,
        "window_materialize_row_ids",
        static_cast<int64_t>(funcs.size()),
        extraOwnerCount,
        emitWindowColumns);
}

NQumir::NAst::TExprPtr BuildWindowKeyComparatorAst(
    NQumir::NAst::TTypeFactory& types,
    std::string name,
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    auto boolType = types.Make<TBoolType>();
    auto rowSetPtrType = Ptr(types, QumirDbNamedType(types, "TRowSet"));

    Oz::TFunBuilder builder(std::move(name));
    builder
        .Param("store", rowSetPtrType)
        .Param("left_row_id", i64Type)
        .Param("right_row_id", i64Type)
        .Return(boolType);

    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        const auto columnIdx = Int64Literal(types, static_cast<int64_t>(key.ColumnIndex));
        const std::string leftValid = "left_valid_" + std::to_string(i);
        const std::string rightValid = "right_valid_" + std::to_string(i);

        builder
            .Var(leftValid, boolType)
            .Assign(leftValid, Oz::Call("sr_row_valid", {
                Oz::Ident("store"), Oz::Ident("left_row_id"), columnIdx,
            }))
            .Var(rightValid, boolType)
            .Assign(rightValid, Oz::Call("sr_row_valid", {
                Oz::Ident("store"), Oz::Ident("right_row_id"),
                Int64Literal(types, static_cast<int64_t>(key.ColumnIndex)),
            }))
            .Stmt(Oz::If(
                Oz::Bin(TOperator("!="), Oz::Ident(leftValid), Oz::Ident(rightValid)),
                Oz::Block({Oz::Return(Oz::Bool(types, false))})));

        std::vector<TExprPtr> compareStmts;
        if (SortKeyIsString(key.Type)) {
            compareStmts.push_back(Oz::If(
                Oz::Bin(TOperator("!="),
                    Oz::Call("sr_sort_compare", {
                        Oz::Ident("store"),
                        Int64Literal(types, static_cast<int64_t>(key.ColumnIndex)),
                        Oz::Ident("left_row_id"),
                        Oz::Ident("right_row_id"),
                    }),
                    Int64Literal(types, 0)),
                Oz::Block({Oz::Return(Oz::Bool(types, false))})));
        } else if (SortKeyIsBool(key.Type)) {
            compareStmts.push_back(Oz::If(
                Oz::Bin(TOperator("!="),
                    Oz::Call("sr_load_bool_key", {
                        Oz::Ident("store"),
                        Oz::Ident("left_row_id"),
                        Int64Literal(types, static_cast<int64_t>(key.ColumnIndex)),
                    }),
                    Oz::Call("sr_load_bool_key", {
                        Oz::Ident("store"),
                        Oz::Ident("right_row_id"),
                        Int64Literal(types, static_cast<int64_t>(key.ColumnIndex)),
                    })),
                Oz::Block({Oz::Return(Oz::Bool(types, false))})));
        } else {
            auto coreType = SortValueIsBinInt(key.Type)
                ? BinIntStorageType(types)
                : SortCoreType(types, key.Type);
            if (!coreType) {
                throw NQumir::TError(
                    "BuildWindowKeyComparatorAst: unsupported key type " +
                    SqlTypeName(key.Type));
            }
            auto witness = [&]() {
                return Cast(Int64Literal(types, 0), Ptr(types, coreType));
            };
            compareStmts.push_back(Oz::If(
                Oz::Bin(TOperator("!="),
                    Oz::Call("sr_load_fixed_key", {
                        Oz::Ident("store"),
                        Oz::Ident("left_row_id"),
                        Int64Literal(types, static_cast<int64_t>(key.ColumnIndex)),
                        witness(),
                    }),
                    Oz::Call("sr_load_fixed_key", {
                        Oz::Ident("store"),
                        Oz::Ident("right_row_id"),
                        Int64Literal(types, static_cast<int64_t>(key.ColumnIndex)),
                        witness(),
                    })),
                Oz::Block({Oz::Return(Oz::Bool(types, false))})));
        }
        builder.Stmt(Oz::If(
            Oz::Ident(leftValid),
            Oz::Block(std::move(compareStmts))));
    }

    builder.Stmt(Oz::Return(Oz::Bool(types, true)));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildHeapTopKEntryAst(NQumir::NAst::TTypeFactory& types)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = types.MakeInteger();
    Oz::TFunBuilder builder("qdb_top_k_select");
    builder
        .Param("store", Ptr(types, QumirDbNamedType(types, "TRowSet")))
        .Param("row_ids", Ptr(types, i64Type))
        .Param("size", i64Type)
        .Param("limit", i64Type)
        .Return(i64Type)
        .Stmt(Oz::Return(Oz::Call("heap_top_k", {
            Oz::Ident("row_ids"),
            Oz::Ident("size"),
            Oz::Ident("limit"),
            Oz::Ident("store"),
        })));
    return std::move(builder).Build();
}

// The two struct input schemas of a binary join kernel spec.
std::pair<const NQumir::NAst::TStructType*, const NQumir::NAst::TStructType*>
JoinInputTypes(const NKernel::TOperatorKernelSpec& spec, const std::string& caller) {
    using namespace NQumir::NAst;
    if (spec.Kind != NKernel::EOperatorKernelKind::Binary || spec.OperatorName != "join") {
        throw NQumir::TError(caller + ": expected join kernel spec");
    }
    if (spec.InputSchemas.size() != 2) {
        throw NQumir::TError(caller + ": expected two input schemas");
    }
    auto leftType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    auto rightType = TMaybeType<TStructType>(spec.InputSchemas[1]);
    if (!leftType || !rightType) {
        throw NQumir::TError(caller + ": input schemas must be structs");
    }
    return {
        leftType.Cast(),
        rightType.Cast(),
    };
}

std::vector<TJoinKey> JoinKeysOf(const NKernel::TOperatorKernelSpec& spec) {
    std::vector<TJoinKey> keys;
    keys.reserve(spec.JoinKeys.size());
    for (const auto& key : spec.JoinKeys) {
        keys.push_back({key.Left.Name, key.Right.Name, key.NullsEqual});
    }
    return keys;
}

// The key descriptor of an equi-join, with each key's NULL-safe flag. Throws
// on incompatible types or missing columns.
NKernel::TJoinKeyDescriptor JoinKeyDescriptorOf(
    NQumir::NAst::TTypeFactory& types,
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::vector<TJoinKey>& keys)
{
    std::vector<std::pair<std::string, std::string>> keyNames;
    keyNames.reserve(keys.size());
    for (const auto& key : keys) {
        keyNames.emplace_back(key.Left, key.Right);
    }
    auto keyDesc = NKernel::BuildJoinKeyDescriptor(types, leftType, rightType, keyNames);
    for (size_t i = 0; i < keys.size(); ++i) {
        keyDesc.Fields[i].NullsEqual = keys[i].NullsEqual;
    }
    return keyDesc;
}

// The head of a join program: key type declarations, the string operations
// the key overloads call (when a key or the output has strings), then the
// per-key overloads (rh_hash, rh_key_equal).
void AppendJoinKeyDecls(
    NKernel::TQueryModules& modules,
    std::vector<NQumir::NAst::TExprPtr>& program,
    const NKernel::TJoinKeyDescriptor& keyDesc,
    bool hasString,
    const std::string& caller)
{
    for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) {
        program.push_back(std::move(f));
    }
    if (hasString || keyDesc.HasDistinctLookupType()) {
        auto stringOps = modules.KernelLibrary("aggregation", "string_ops.oz");
        if (!stringOps) {
            throw NQumir::TError(caller + ": string_ops.oz: " + stringOps.error().ToString());
        }
        for (auto& f : *stringOps) {
            program.push_back(std::move(f));
        }
    }
    for (auto& f : NKernel::GenJoinKeyOpsFunDecls(modules.Types(), keyDesc)) {
        program.push_back(std::move(f));
    }
}

} // namespace

NQumir::NAst::TExprPtr BuildSortRowIdLessAst(
    NQumir::NAst::TTypeFactory& types,
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    if (keys.empty()) {
        throw NQumir::TError("BuildSortRowIdLessAst: empty key list");
    }

    auto i64Type = types.MakeInteger();
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto rowSetPtrType = Ptr(types, rowSetType);
    auto columnPtrType = Ptr(types, QumirDbNamedType(types, "TColumn"));

    // Overloads heap_less on the store pointer, so the heap items stay bare
    // packed row ids instead of carrying the store in every element.
    Oz::TFunBuilder builder("heap_less");
    builder
        .Param("store", rowSetPtrType)
        .Param("left_id", i64Type)
        .Param("right_id", i64Type)
        .Return(types.Make<TBoolType>())
        .Var("left_batch", i64Type)
        .Assign("left_batch", Oz::Bin(
            TOperator(">>"), Oz::Ident("left_id"), Int64Literal(types, 32)))
        .Var("right_batch", i64Type)
        .Assign("right_batch", Oz::Bin(
            TOperator(">>"), Oz::Ident("right_id"), Int64Literal(types, 32)))
        .Var("left_row", i64Type)
        .Assign("left_row", Low32(types, Oz::Ident("left_id")))
        .Var("right_row", i64Type)
        .Assign("right_row", Low32(types, Oz::Ident("right_id")))
        .Var("left_row_set", rowSetType)
        .Assign("left_row_set", Oz::Index("store", Oz::Ident("left_batch")))
        .Var("right_row_set", rowSetType)
        .Assign("right_row_set", Oz::Index("store", Oz::Ident("right_batch")))
        .Var("left_cols", columnPtrType)
        .Assign("left_cols", Field(Oz::Ident("left_row_set"), "Columns"))
        .Var("right_cols", columnPtrType)
        .Assign("right_cols", Field(Oz::Ident("right_row_set"), "Columns"));

    AppendSortLessBody(
        types, builder, keys,
        "left_cols", "left_row",
        "right_cols", "right_row",
        Oz::Bin(TOperator("<"), Oz::Ident("left_id"), Oz::Ident("right_id")));

    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildHeapTopKProgramAst(
    NKernel::TQueryModules& modules,
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    auto& types = modules.Types();

    if (keys.empty()) {
        throw NQumir::TError("BuildHeapTopKProgramAst: empty key list");
    }

    auto library = modules.KernelLibrary("sort", "heap.oz");
    if (!library) {
        throw NQumir::TError(
            "BuildHeapTopKProgramAst: " + library.error().ToString());
    }

    std::vector<TExprPtr> programStmts;
    for (auto& stmt : *library) {
        programStmts.push_back(std::move(stmt));
    }
    programStmts.push_back(BuildSortRowIdLessAst(types, keys));
    programStmts.push_back(BuildHeapTopKEntryAst(types));
    return NQumir::MakeIntrusive<TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildWindowProgramAst(
    NKernel::TQueryModules& modules,
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortRadixKeyInput>& partitionKeys,
    const std::vector<TSortRadixKeyInput>& orderKeys,
    const std::vector<TWindowFuncInput>& funcs)
{
    using namespace NQumir::NAst;
    auto& types = modules.Types();
    if (keys.empty()) {
        throw NQumir::TError("BuildWindowProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = modules.KernelLibrary("sort", name);
        if (!library) {
            throw NQumir::TError("BuildWindowProgramAst: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (skipUse && TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    };
    addLibrary("radix.oz", false);
    addLibrary("sort_rowids.oz", true);
    AddSortMaterializeLibrary(modules, programStmts, "BuildWindowProgramAst");
    programStmts.push_back(BuildWindowKeyComparatorAst(
        types, "window_same_partition", partitionKeys));
    programStmts.push_back(BuildWindowKeyComparatorAst(
        types, "window_same_order", orderKeys));
    addLibrary("window.oz", true);
    programStmts.push_back(BuildWindowMaterializeWrapperAst(
        types, inputType, funcs));
    // Nullable keys need the null-aware radix (nulls segregated to FIRST/LAST);
    // the non-nullable variant would bucket NULL rows by raw value bytes and
    // scatter a partition.
    const bool nullable = std::any_of(keys.begin(), keys.end(),
        [](const TSortRadixKeyInput& key) { return IsNullableType(key.Type); });
    programStmts.push_back(BuildSortRunWrapperAst(
        types, "qdb_window_run", keys, nullable, "window_materialize_row_ids"));
    return NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildRadixSortProgramAst(
    NKernel::TQueryModules& modules,
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;
    auto& types = modules.Types();
    if (keys.empty()) {
        throw NQumir::TError("BuildRadixSortProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = modules.KernelLibrary("sort", name);
        if (!library) {
            throw NQumir::TError(
                "BuildRadixSortProgramAst: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (skipUse && TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    };
    addLibrary("radix.oz", false);
    addLibrary("sort_rowids.oz", true);
    programStmts.push_back(BuildRadixCompositeWrapperAst(types, keys, false));
    if (materializeType) {
        AddSortMaterializeLibrary(modules, programStmts, "BuildRadixSortProgramAst");
        programStmts.push_back(BuildSortMaterializeWrapperAst(types, *materializeType));
        programStmts.push_back(BuildSortRunWrapperAst(types, "qdb_sort_run", keys, false, "sort_materialize_row_ids"));
    }
    return NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildRadixSortNullableProgramAst(
    NKernel::TQueryModules& modules,
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;
    auto& types = modules.Types();
    if (keys.empty()) {
        throw NQumir::TError("BuildRadixSortNullableProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = modules.KernelLibrary("sort", name);
        if (!library) {
            throw NQumir::TError(
                "BuildRadixSortNullableProgramAst: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (skipUse && TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    };
    addLibrary("radix.oz", false);
    addLibrary("sort_rowids.oz", true);
    programStmts.push_back(BuildRadixCompositeWrapperAst(types, keys, true));
    if (materializeType) {
        AddSortMaterializeLibrary(modules, programStmts, "BuildRadixSortNullableProgramAst");
        programStmts.push_back(BuildSortMaterializeWrapperAst(types, *materializeType));
        programStmts.push_back(BuildSortRunWrapperAst(types, "qdb_sort_run", keys, true, "sort_materialize_row_ids"));
    }
    return NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildTopSortMergeProgramAst(
    NKernel::TQueryModules& modules,
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;
    auto& types = modules.Types();
    if (keys.empty()) {
        throw NQumir::TError("BuildTopSortMergeProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = modules.KernelLibrary("sort", name);
        if (!library) {
            throw NQumir::TError(
                "BuildTopSortMergeProgramAst: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            if (skipUse && TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            programStmts.push_back(std::move(stmt));
        }
    };
    addLibrary("radix.oz", false);
    addLibrary("sort_rowids.oz", true);
    if (materializeType) {
        AddSortMaterializeLibrary(modules, programStmts, "BuildTopSortMergeProgramAst");
        programStmts.push_back(BuildSortMaterializeWrapperAst(types, *materializeType));
    }
    programStmts.push_back(BuildTopSortTempBeforeStateAst(types, keys));
    programStmts.push_back(BuildTopSortMergePicksAst(types));
    programStmts.push_back(BuildTopSortUpdateAst(types, keys, materializeType != nullptr));
    return NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

TRuntimeFilterKernel TKernelCompiler::CompileRuntimeFilter() {
    using namespace NQumir::NAst;
    // Type declarations go first, as in a source file.
    std::vector<TExprPtr> programStmts;
    std::vector<TExprPtr> functions;
    for (const auto& [kind, name] : {
        std::pair{"sort", "radix.oz"},
        std::pair{"aggregation", "swiss_group.oz"},
        std::pair{"join", "runtime_filter.oz"}})
    {
        auto library = Annotation_.Modules->KernelLibrary(kind, name);
        if (!library) {
            throw NQumir::TError("CompileRuntimeFilter: " + library.error().ToString());
        }
        for (auto& stmt : *library) {
            (TMaybeNode<TTypeDeclStmt>(stmt) ? programStmts : functions)
                .push_back(std::move(stmt));
        }
    }
    programStmts.insert(programStmts.end(),
        std::make_move_iterator(functions.begin()), std::make_move_iterator(functions.end()));
    auto ast = NQumir::MakeIntrusive<TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
    PrintKernelAst(Diagnostics_, "runtime-filter", ast);
    auto kernel = EmitKernel("runtime-filter",
        {RuntimeFilterKernelEntrypoints.begin(), RuntimeFilterKernelEntrypoints.end()},
        std::move(ast));
    FinishKernelDiagnostics(Diagnostics_);

    return {.Slot = kernel.Slot, .Kernel = kernel.Index};
}

TKernelCompiler::TFilterDispatch TKernelCompiler::CompileFilter(
    const NKernel::TOperatorKernelSpec& spec)
{
    auto& types = *Types_;
    using namespace NQumir::NAst;

    if (spec.InputSchemas.empty() || spec.Expressions.empty()) {
        throw std::runtime_error("filter kernel spec is incomplete");
    }
    auto inputType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    if (!inputType) {
        throw std::runtime_error("filter kernel input must have TStructType");
    }
    const auto& fields = inputType.Cast()->Fields;
    const auto& predicate = spec.Expressions[0];

    std::unordered_map<std::string, int32_t> fieldIndices;
    for (int32_t i = 0; i < static_cast<int32_t>(fields.size()); ++i) {
        fieldIndices[fields[i].first] = i;
    }

    auto columnType = QumirDbNamedType(types, "TColumn");
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto stringViewType = QumirDbNamedType(types, "StringView");

    auto kernelAst = NKernel::BuildFilterProgramAst(
        types, ClonePredicate(predicate), *inputType.Cast(), fieldIndices, columnType,
        rowSetType, stringViewType);
    if (!kernelAst) {
        throw NQumir::TError(
            "CompileFilter: " + kernelAst.error().ToString());
    }

    if (Diagnostics_) {
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
    }
    PrintKernelAst(Diagnostics_, "filter", *kernelAst);

    auto kernel = EmitKernel(
        "filter", {"<kernel>"}, std::move(*kernelAst));
    FinishKernelDiagnostics(Diagnostics_);

    return {
        .Slot = kernel.Slot,
        .Runtime = RuntimeContext_,
        .Kernel = kernel.Index,
    };
}

void TKernelCompiler::TFilterDispatch::operator()(
    TRowSet& rowSet, const uint8_t* inputSelection) const
{
    using TFilterFn = void(*)(void*, void*, void**, const uint8_t*);
    TStringArena arena;
    Slot->Call<TFilterFn>(0,
        &rowSet, &arena, Runtime ? Runtime->Regexes.Handles() : nullptr,
        inputSelection);
}

TKernelCompiler::TProjectDispatch TKernelCompiler::CompileProject(
    const NKernel::TOperatorKernelSpec& spec)
{
    auto& types = *Types_;
    using namespace NQumir::NAst;

    if (spec.InputSchemas.empty()) {
        throw std::runtime_error("project kernel spec is missing input schema");
    }
    auto inputType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    if (!inputType) {
        throw std::runtime_error("project kernel input must have TStructType");
    }
    auto outputType = TMaybeType<TStructType>(spec.OutputSchema);
    if (!outputType) {
        throw std::runtime_error("project kernel output must have TStructType");
    }
    const auto& fields = inputType.Cast()->Fields;
    std::vector<NQumir::NAst::TTypePtr> computedTypes;
    computedTypes.reserve(outputType.Cast()->Fields.size());
    for (const auto& field : outputType.Cast()->Fields) {
        computedTypes.push_back(field.second);
    }

    std::unordered_map<std::string, int32_t> fieldIndices;
    for (int32_t i = 0; i < static_cast<int32_t>(fields.size()); ++i) {
        fieldIndices[fields[i].first] = i;
    }

    auto columnType = QumirDbNamedType(types, "TColumn");
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto stringViewType = QumirDbNamedType(types, "StringView");

    std::vector<NQumir::NAst::TExprPtr> cloned;
    cloned.reserve(spec.Expressions.size());
    for (const auto& expr : spec.Expressions) {
        cloned.push_back(ClonePredicate(expr));
    }

    auto kernelAst = NKernel::GenProjectKernelAst(
        types, std::move(cloned), computedTypes, *inputType.Cast(), fieldIndices,
        columnType, rowSetType, stringViewType);

    if (Diagnostics_) {
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
    }
    PrintKernelAst(Diagnostics_, "project", kernelAst);

    auto kernel = EmitKernel(
        "project", {"<project>"}, std::move(kernelAst));
    FinishKernelDiagnostics(Diagnostics_);

    auto dispatch = BindProject(kernel.Slot, RuntimeContext_);
    dispatch.Kernel = kernel.Index;
    return dispatch;
}

TKernelCompiler::TProjectDispatch BindProject(
    std::shared_ptr<TKernelSlot> slot,
    std::shared_ptr<TRuntimeContext> runtime)
{
    return {
        .Slot = std::move(slot),
        .Runtime = std::move(runtime),
    };
}

void TKernelCompiler::TProjectDispatch::operator()(
    TRowSet* in, void** outBuffers, void* arena, const uint8_t* inputSelection) const
{
    using TProjectFn = void(*)(void*, void**, void*, void**, const uint8_t*);
    Slot->Call<TProjectFn>(0,
        in, outBuffers, arena,
        Runtime ? Runtime->Regexes.Handles() : nullptr, inputSelection);
}

TKernelCompiler::TSortRadixCompositeDispatch TKernelCompiler::CompileRadixSortComposite(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;

    if (keys.empty()) {
        throw NQumir::TError("CompileRadixSortComposite: empty key list");
    }
    if (!materializeType) {
        throw NQumir::TError("CompileRadixSortComposite: materialize schema is required");
    }

    auto program = BuildRadixSortProgramAst(*Annotation_.Modules, keys, materializeType);
    PrintKernelAst(Diagnostics_, "sort.run.fused", program);

    auto kernel = EmitKernel(
        "sort.run.fused",
        {"qdb_sort_run"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto dispatch = BindSortRadixComposite(kernel.Slot);
    dispatch.Kernel = kernel.Index;
    return dispatch;
}

TKernelCompiler::TSortRadixCompositeDispatch
TKernelCompiler::CompileWindow(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortRadixKeyInput>& partitionKeys,
    const std::vector<TSortRadixKeyInput>& orderKeys,
    const std::vector<TWindowFuncInput>& funcs)
{
    using namespace NQumir::NAst;
    if (keys.empty()) {
        throw NQumir::TError("CompileWindow: empty key list");
    }

    auto program = BuildWindowProgramAst(
        *Annotation_.Modules, keys, inputType, partitionKeys, orderKeys, funcs);
    PrintKernelAst(Diagnostics_, "window.run.fused", program);

    auto kernel = EmitKernel(
        "window.run.fused",
        {"qdb_window_run"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto dispatch = BindSortRadixComposite(kernel.Slot);
    dispatch.Kernel = kernel.Index;
    return dispatch;
}

TKernelCompiler::TSortRadixCompositeNullableDispatch
TKernelCompiler::CompileRadixSortCompositeNullable(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;
    if (!materializeType) {
        throw NQumir::TError("CompileRadixSortCompositeNullable: materialize schema is required");
    }

    auto program = BuildRadixSortNullableProgramAst(*Annotation_.Modules, keys, materializeType);
    PrintKernelAst(Diagnostics_, "sort.run.nullable.fused", program);

    auto kernel = EmitKernel(
        "sort.run.nullable.fused",
        {"qdb_sort_run"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto dispatch = BindSortRadixComposite(kernel.Slot);
    dispatch.Kernel = kernel.Index;
    return dispatch;
}

TKernelCompiler::TTopSortDispatch TKernelCompiler::CompileTopSort(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;

    if (!materializeType) {
        throw NQumir::TError("CompileTopSort: materialize schema is required");
    }
    auto program = BuildTopSortMergeProgramAst(*Annotation_.Modules, keys, materializeType);
    PrintKernelAst(Diagnostics_, "top-sort.fused", program);

    auto kernel = EmitKernel(
        "top-sort.fused",
        {"qdb_top_sort_update"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto dispatch = BindTopSort(kernel.Slot);
    dispatch.Kernel = kernel.Index;
    return dispatch;
}

TKernelCompiler::THeapTopKDispatch TKernelCompiler::CompileHeapTopK(
    const std::vector<TSortRadixKeyInput>& keys)
{
    auto program = BuildHeapTopKProgramAst(*Annotation_.Modules, keys);
    PrintKernelAst(Diagnostics_, "top-k.heap", program);

    auto kernel = EmitKernel(
        "top-k.heap",
        {"qdb_top_k_select"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto dispatch = BindHeapTopK(kernel.Slot);
    dispatch.Kernel = kernel.Index;
    return dispatch;
}

TKernelCompiler::TSortRadixCompositeDispatch BindSortRadixComposite(
    std::shared_ptr<TKernelSlot> slot)
{
    return {.Slot = std::move(slot)};
}

TKernelCompiler::TTopSortDispatch BindTopSort(std::shared_ptr<TKernelSlot> slot)
{
    return {.Slot = std::move(slot)};
}

TKernelCompiler::THeapTopKDispatch BindHeapTopK(std::shared_ptr<TKernelSlot> slot)
{
    return {.Slot = std::move(slot)};
}

int64_t TKernelCompiler::TSortRadixCompositeDispatch::operator()(
    TRowSet* store, int64_t* rowIds, int64_t* work, uint32_t* counts,
    int64_t n, bool* descs, bool* nullsFirsts, bool doSort,
    int64_t start, int64_t limit, TRowSet* output) const
{
    using TSortFn = int64_t(*)(
        TRowSet*, int64_t*, int64_t*, uint32_t*, int64_t, bool*, bool*,
        bool, int64_t, int64_t, TRowSet*);
    return Slot->Call<TSortFn>(0,
        store, rowIds, work, counts, n, descs, nullsFirsts,
        doSort, start, limit, output);
}

int64_t TKernelCompiler::TTopSortDispatch::operator()(
    TRowSet* state, TRowSet* batch, int64_t* rowIds, int64_t* work,
    uint32_t* counts, int64_t n, bool sortInput, uint8_t* pickSrc,
    uint32_t* pickIdx, int64_t limit, TRowSet* output) const
{
    using TTopSortFn = int64_t(*)(
        TRowSet*, TRowSet*, int64_t*, int64_t*, uint32_t*,
        int64_t, bool, uint8_t*, uint32_t*, int64_t, TRowSet*);
    return Slot->Call<TTopSortFn>(0,
        state, batch, rowIds, work, counts, n, sortInput,
        pickSrc, pickIdx, limit, output);
}

int64_t TKernelCompiler::THeapTopKDispatch::operator()(
    TRowSet* store, int64_t* rowIds, int64_t size, int64_t limit) const
{
    using THeapTopKFn = int64_t(*)(TRowSet*, int64_t*, int64_t, int64_t);
    return Slot->Call<THeapTopKFn>(0, store, rowIds, size, limit);
}

TAggregateKernels TKernelCompiler::CompileAggregate(
    const NKernel::TOperatorKernelSpec& spec)
{
    auto& types = *Types_;
    if (spec.Kind != NKernel::EOperatorKernelKind::UnaryBlocking ||
        spec.OperatorName != "aggregate") {
        throw NQumir::TError("CompileAggregate: expected aggregate kernel spec");
    }
    if (spec.InputSchemas.size() != 1) {
        throw NQumir::TError("CompileAggregate: expected one input schema");
    }

    const auto* inputTypePtr = static_cast<const NQumir::NAst::TStructType*>(
        spec.InputSchemas[0]);
    if (!inputTypePtr) {
        throw NQumir::TError("CompileAggregate: input schema must be a struct");
    }
    const auto& inputType = *inputTypePtr;

    std::vector<std::string> groupKeys;
    if (!spec.Keys.empty()) {
        groupKeys.reserve(spec.Keys[0].Columns.size());
        for (const auto& column : spec.Keys[0].Columns) {
            groupKeys.push_back(column.Name);
        }
    }

    std::vector<TAggregateSpec> aggs;
    aggs.reserve(spec.Aggregates.size());
    for (const auto& agg : spec.Aggregates) {
        aggs.push_back({
            .Name = agg.Name,
            .Func = agg.Func,
            .Arg = agg.ArgExpr,
            .EmptyIsZero = agg.EmptyIsZero,
        });
    }

    if (Diagnostics_) {
        *Diagnostics_ << "\n========== KERNEL SPEC ==========\n";
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
        *Diagnostics_ << "=================================\n";
    }

    using namespace NQumir::NAst;

    auto fieldType = [&](const std::string& name) -> TTypePtr {
        for (const auto& [n, t] : inputType.Fields) {
            if (n == name) {
                return t;
            }
        }
        return nullptr;
    };
    auto requireField = [&](const std::string& name) -> TTypePtr {
        auto type = fieldType(name);
        if (!type) {
            throw NQumir::TError("CompileAggregate: unknown column '" + name + "'");
        }
        return type;
    };
    const auto keyDescriptor = NKernel::BuildAggregateKeyDescriptor(types, inputType, groupKeys);
    for (const auto& field : keyDescriptor.Fields) {
        const auto valueType = UnwrapNullableType(field.Type);
        const auto type = PhysicalValueType(types, valueType);
        if (!IsBinIntStorageType(valueType) &&
            !MaybeIntegerType(type) && !TMaybeType<TFloatType>(type) &&
            !TMaybeType<TStringType>(type) && !TMaybeType<TBoolType>(type)) {
            throw NQumir::TError(
                "CompileAggregate: group key column '" + field.ColumnName +
                "' must be integer, f64, bool, string, or BinInt");
        }
    }
    auto columnIndex = [&](const std::string& name) -> int32_t {
        for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
            if (inputType.Fields[i].first == name) {
                return i;
            }
        }
        return -1;
    };

    std::vector<std::string> funcs;
    std::vector<NKernel::TAggArg> args;
    funcs.reserve(aggs.size());
    args.reserve(aggs.size());
    for (const auto& agg : aggs) {
        if (agg.Func != "count" && agg.Func != "sum" && agg.Func != "min" && agg.Func != "max") {
            throw NQumir::TError("CompileAggregate: unsupported aggregate function '" + agg.Func + "'");
        }
        funcs.push_back(agg.Func);

        if (agg.Func != "count" && !agg.Arg) {
            throw NQumir::TError(
                "CompileAggregate: '" + agg.Func + "' requires an argument column");
        }
        NKernel::TAggArg arg;
        if (agg.Arg) {
            auto ident = TMaybeNode<TIdentExpr>(agg.Arg);
            if (!ident) {
                throw NQumir::TError("CompileAggregate: aggregate argument must be a column reference");
            }
            const std::string& name = ident.Cast()->Name;
            const auto type = requireField(name);
            const auto valueType = UnwrapNullableType(type);
            // SUM over a decimal stored in i64 keeps an i128 state (DECIMAL(38, s)).
            const bool isBinInt = IsBinIntStorageType(valueType) || IsDecimal128Type(valueType) ||
                (IsDecimalType(valueType) && agg.Func == "sum");
            const auto unwrapped = isBinInt ? valueType : PhysicalValueType(types, valueType);
            const bool isFloat = static_cast<bool>(TMaybeType<TFloatType>(unwrapped));
            const bool isInteger = static_cast<bool>(MaybeIntegerType(unwrapped));
            const bool isString = static_cast<bool>(TMaybeType<TStringType>(unwrapped));
            const bool isBool = static_cast<bool>(TMaybeType<TBoolType>(unwrapped));
            if ((isString || isBool) && agg.Func == "sum") {
                throw NQumir::TError(
                    "CompileAggregate: 'sum' does not support " +
                    std::string(isString ? "string" : "boolean") + " arguments");
            }
            if (!isInteger && !isFloat && !isBinInt && !isString && !isBool) {
                throw NQumir::TError(
                    "CompileAggregate: aggregate argument column '" + name +
                    "' must be integer, f64, bool, string, or BinInt");
            }
            arg.ValueKind = isBinInt
                ? NKernel::EAggValueKind::BinInt
                : (isString ? NKernel::EAggValueKind::String
                    : (isBool ? NKernel::EAggValueKind::Bool
                        : (isFloat ? NKernel::EAggValueKind::Float64
                                   : NKernel::EAggValueKind::Int64)));
            arg.IsNullable = IsNullableType(type) ||
                (!spec.EmptyInputGroups.empty() && agg.Func != "count" && !agg.EmptyIsZero);
            arg.ColumnIndex = columnIndex(name);
        }
        args.push_back(arg);
    }
    const auto layout = NKernel::BuildAggReducerLayout(funcs, args);

    auto columnType = QumirDbNamedType(types, "TColumn");
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto hashTableType = QumirDbNamedType(types, "HashTable");

    const auto rows = spec.AggregateRowLayout
        ? NKernel::BuildAggRowLayout(keyDescriptor, layout, !spec.EmptyInputGroups.empty())
        : std::nullopt;
    auto program = NKernel::BuildGenericAggregateFusedProgramAst(
        *Annotation_.Modules, inputType, keyDescriptor, layout,
        columnType, rowSetType, hashTableType, spec.HasPrecomputedHash, spec.EmptyInputGroups,
        rows);
    if (!program) {
        throw NQumir::TError(
            "CompileAggregate: " + program.error().ToString());
    }

    TAggregateKernelMeta kernels;
    kernels.NumAggs = funcs.size();
    kernels.StateWords = layout.NumAggBuffers;
    kernels.RowBytes = rows ? rows->RowBytes : 0;
    for (const auto& reducer : layout.Reducers) {
        if (reducer.IsString()) {
            kernels.StringMetadataBuffers.push_back(reducer.ExtraBufIdx);
        }
    }
    kernels.OutputAggs.reserve(layout.Reducers.size());
    for (const auto& reducer : layout.Reducers) {
        kernels.OutputAggs.push_back({.IsNullable = reducer.IsNullableOutput});
    }
    kernels.OutputKeys.reserve(keyDescriptor.Fields.size());
    for (const auto& field : keyDescriptor.Fields) {
        const auto logicalType = UnwrapNamedValueType(field.Type);
        kernels.OutputKeys.push_back({
            .Kind = TMaybeType<TStringType>(logicalType)
                ? EAggregateOutputKeyKind::String
                : EAggregateOutputKeyKind::Fixed,
            .IsNullable = field.IsNullable,
            .Size = field.Size,
            .Alignment = field.Alignment,
        });
    }

    const size_t sinkBefore = Sink_ ? Sink_->size() : 0;
    PrintKernelAst(Diagnostics_, "aggregate", *program);
    auto kernel = EmitKernel(
        "aggregate",
        {"agg_dispatch", "agg_finish_rowset"},
        std::move(*program));
    FinishKernelDiagnostics(Diagnostics_);

    if (Sink_) {
        // Preserve output-layout metadata alongside the generated kernel.
        std::vector<TGeneratedKernel::TAggKeyMeta> keyMeta;
        keyMeta.reserve(kernels.OutputKeys.size());
        for (const auto& outputKey : kernels.OutputKeys) {
            keyMeta.push_back({
                .IsString = outputKey.Kind == EAggregateOutputKeyKind::String,
                .IsNullable = outputKey.IsNullable,
            });
        }
        std::vector<TGeneratedKernel::TAggValueMeta> valueMeta;
        valueMeta.reserve(kernels.OutputAggs.size());
        for (const auto& outputAgg : kernels.OutputAggs) {
            valueMeta.push_back({.IsNullable = outputAgg.IsNullable});
        }
        for (size_t i = sinkBefore; i < Sink_->size(); ++i) {
            (*Sink_)[i].AggStringMetadataBuffers = kernels.StringMetadataBuffers;
            (*Sink_)[i].AggKeys = keyMeta;
            (*Sink_)[i].AggValues = valueMeta;
        }
    }

    kernels.Kernel = kernel.Index;
    return BindAggregateKernels(kernel.Slot, std::move(kernels));
}

TAggregateKernels BindAggregateKernels(
    std::shared_ptr<TKernelSlot> slot,
    TAggregateKernelMeta meta)
{
    using TDispatchFn = int64_t(*)(void*, TRowSet*, int64_t, int64_t);
    using TFinishRowSetFn = int64_t(*)(void*, TRowSet*);

    TAggregateKernels kernels{std::move(meta)};
    kernels.Dispatch = [slot](void* ht, TRowSet* batch, int64_t arg, int64_t op) {
        return slot->Call<TDispatchFn>(0, ht, batch, arg, op);
    };
    kernels.FinishRowSet = [slot](void* ht, TRowSet* output) {
        return slot->Call<TFinishRowSetFn>(1, ht, output);
    };
    kernels.Slot = std::move(slot);
    return kernels;
}

TJoinKernels TKernelCompiler::CompileJoin(
    const NKernel::TOperatorKernelSpec& spec)
{
    auto& types = *Types_;
    using namespace NQumir::NAst;

    const auto [leftType, rightType] = JoinInputTypes(spec, "CompileJoin");
    const auto keys = JoinKeysOf(spec);

    // A mark join with a residual needs the marker of a correlated IN, which
    // belongs to general unnesting.
    if (!spec.Expressions.empty() && spec.JoinType == EJoinType::LeftMark) {
        throw NQumir::TError(
            "CompileJoin: residual filter is not yet supported for this join type");
    }
    const auto residual = PrepareJoinResidual(types, spec, Annotation_, "CompileJoin");

    if (Diagnostics_) {
        *Diagnostics_ << "\n========== KERNEL SPEC ==========\n";
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
        *Diagnostics_ << "=================================\n";
    }

    return CompileJoin(
        *leftType, *rightType, keys, spec.JoinType,
        residual.Predicate, residual.Inner(), residual.LeftFieldCount,
        spec.HasPrecomputedHash);
}

TCrossJoinKernels TKernelCompiler::CompileCrossJoin(
    const NKernel::TOperatorKernelSpec& spec)
{
    auto& types = *Types_;
    using namespace NQumir::NAst;

    if (spec.Kind != NKernel::EOperatorKernelKind::Binary ||
        spec.OperatorName != "cross-join") {
        throw NQumir::TError("CompileCrossJoin: expected cross-join kernel spec");
    }
    if (spec.InputSchemas.size() != 2) {
        throw NQumir::TError("CompileCrossJoin: expected two input schemas");
    }

    auto leftType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    auto rightType = TMaybeType<TStructType>(spec.InputSchemas[1]);
    if (!leftType || !rightType) {
        throw NQumir::TError("CompileCrossJoin: input schemas must be structs");
    }

    if (Diagnostics_) {
        *Diagnostics_ << "\n========== KERNEL SPEC ==========\n";
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
        *Diagnostics_ << "=================================\n";
    }

    auto columnType = QumirDbNamedType(types, "TColumn");
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto pairBufferType = QumirDbNamedType(types, "PairBuffer");
    auto stringViewType = QumirDbNamedType(types, "StringView");
    const auto residual = PrepareJoinResidual(
        types, spec, Annotation_, "CompileCrossJoin");
    const bool includeRight =
        spec.JoinType != EJoinType::LeftSemi && spec.JoinType != EJoinType::LeftAnti;

    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        auto library = NKernel::BuildCrossJoinKernelLibrary(*Annotation_.Modules);
        if (!library) {
            throw NQumir::TError(
                "CompileCrossJoin: " + library.error().ToString());
        }
        if (residual.Predicate) {
            InjectJoinResidualFilter(
                types, *library, residual.Predicate, *residual.Inner(), residual.LeftFieldCount,
                columnType, rowSetType, stringViewType);
        }

        std::vector<TExprPtr> program;
        for (auto& f : *library) {
            program.push_back(std::move(f));
        }

        const bool hasStringOutput = [&] {
            auto isString = [](const auto& field) {
                return static_cast<bool>(TMaybeType<TStringType>(
                    UnwrapNamedValueType(UnwrapNullableType(field.second))));
            };
            return std::any_of(
                leftType.Cast()->Fields.begin(), leftType.Cast()->Fields.end(), isString) ||
                std::any_of(
                    rightType.Cast()->Fields.begin(), rightType.Cast()->Fields.end(), isString);
        }();
        if (hasStringOutput) {
            auto stringOps = Annotation_.Modules->KernelLibrary("aggregation", "string_ops.oz");
            if (!stringOps) {
                throw NQumir::TError(
                    "CompileCrossJoin: string_ops.oz: " +
                    stringOps.error().ToString());
            }
            for (auto& f : *stringOps) {
                program.push_back(std::move(f));
            }
        }

        program.push_back(NKernel::GenCrossJoinDispatchAst(
            types, spec.JoinType, rowSetType, pairBufferType));
        program.push_back(NKernel::GenJoinMaterializeAst(
            types, *leftType.Cast(), *rightType.Cast(),
            includeRight
                ? NKernel::EJoinOutputColumns::LeftRight
                : NKernel::EJoinOutputColumns::Left,
            columnType, rowSetType, pairBufferType, stringViewType));
        return program;
    };

    std::vector<std::string> entries = {"xj_dispatch", "jt_materialize"};
    auto program = NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "cross-join", program);

    auto kernel = EmitKernel("cross-join", entries, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    TCrossJoinKernels kernels;
    kernels.Kernel = kernel.Index;
    return BindCrossJoinKernels(kernel.Slot, std::move(kernels));
}

TCrossJoinKernels BindCrossJoinKernels(
    std::shared_ptr<TKernelSlot> slot,
    TCrossJoinKernels meta)
{
    using TDispatchFn = bool(*)(
        TRowSet*, int64_t, TRowSet*, int64_t, uint8_t*, void*, void*, int64_t);
    using TMaterializeFn = int64_t(*)(
        void*, TRowSet*, TRowSet*, TRowSet*, TRowSet*, int64_t, int64_t, TRowSet*);

    meta.Dispatch = [slot](
        TRowSet* leftStore,
        int64_t batchIdx,
        TRowSet* rightStore,
        int64_t rightBatchCount,
        uint8_t* rightMatched,
        void* pairs,
        int64_t op) {
        // Scratch of the residual's strings, which do not outlive the call.
        TStringArena arena;
        return slot->Call<TDispatchFn>(0,
            leftStore, batchIdx, rightStore, rightBatchCount, rightMatched, pairs,
            &arena, op);
    };
    meta.Materialize = [slot](
        void* pairs,
        TRowSet* leftStore,
        TRowSet* rightStore,
        TRowSet* streamLeft,
        TRowSet* streamRight,
        int64_t start,
        int64_t limit,
        TRowSet* out) {
        return slot->Call<TMaterializeFn>(1,
            pairs, leftStore, rightStore, streamLeft, streamRight, start, limit, out);
    };
    meta.Slot = std::move(slot);
    return meta;
}

TJoinHashKernels TKernelCompiler::CompileJoinHash(
    const NKernel::TOperatorKernelSpec& spec)
{
    auto& types = *Types_;
    using namespace NQumir::NAst;

    if (spec.Kind != NKernel::EOperatorKernelKind::Binary ||
        spec.OperatorName != "join") {
        throw NQumir::TError("CompileJoinHash: expected join kernel spec");
    }
    if (spec.InputSchemas.size() != 2) {
        throw NQumir::TError("CompileJoinHash: expected two input schemas");
    }

    auto leftType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    auto rightType = TMaybeType<TStructType>(spec.InputSchemas[1]);
    if (!leftType || !rightType) {
        throw NQumir::TError("CompileJoinHash: input schemas must be structs");
    }

    std::vector<std::pair<std::string, std::string>> keys;
    keys.reserve(spec.JoinKeys.size());
    for (const auto& key : spec.JoinKeys) {
        keys.emplace_back(key.Left.Name, key.Right.Name);
    }
    const auto keyDesc = NKernel::BuildJoinKeyDescriptor(
        types, *leftType.Cast(),
        *rightType.Cast(),
        keys);

    auto columnType = QumirDbNamedType(types, "TColumn");
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto stringViewType = QumirDbNamedType(types, "StringView");

    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        std::vector<TExprPtr> program;
        for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        // String keys hash as StringView: the rh_hash overload calls
        // qdb_string_hash from string_ops.oz.
        if (keyDesc.HasDistinctLookupType()) {
            auto stringOps = Annotation_.Modules->KernelLibrary("aggregation", "string_ops.oz");
            if (!stringOps) {
                throw NQumir::TError(
                    "CompileJoinHash: string_ops.oz: " + stringOps.error().ToString());
            }
            for (auto& f : *stringOps) program.push_back(std::move(f));
        }
        for (auto& f : NKernel::GenJoinKeyOpsFunDecls(types, keyDesc)) {
            program.push_back(std::move(f));
        }
        program.push_back(NKernel::GenJoinHashBatchAst(
            types, keyDesc, "jt_hash_batch", columnType, rowSetType, stringViewType));
        program.push_back(NKernel::GenJoinHashEntrypointAst(
            types, keyDesc, "jt_hash_left", rowSetType));
        program.push_back(NKernel::GenJoinHashEntrypointAst(
            types, keyDesc, "jt_hash_right", rowSetType));
        return program;
    };

    auto program = NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "join_hash", program);

    auto kernel = EmitKernel(
        "join_hash", {"jt_hash_left", "jt_hash_right"}, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    TJoinHashKernels kernels;
    kernels.LeftKeyColumns.reserve(keyDesc.Fields.size());
    kernels.RightKeyColumns.reserve(keyDesc.Fields.size());
    for (const auto& field : keyDesc.Fields) {
        kernels.LeftKeyColumns.push_back(field.LeftColumnIndex);
        kernels.RightKeyColumns.push_back(field.RightColumnIndex);
    }
    kernels.Kernel = kernel.Index;
    return BindJoinHashKernels(kernel.Slot, std::move(kernels));
}

TJoinHashKernels BindJoinHashKernels(
    std::shared_ptr<TKernelSlot> slot,
    TJoinHashKernels meta)
{
    meta.Left = BindJoinHash(slot, 0, meta.LeftKeyColumns);
    meta.Right = BindJoinHash(slot, 1, meta.RightKeyColumns);
    meta.Slot = std::move(slot);
    return meta;
}

TJoinHashKernels::TDispatch BindJoinHash(
    std::shared_ptr<TKernelSlot> slot,
    uint32_t entry,
    std::vector<int64_t> keyColumns)
{
    using THashFn = bool(*)(TRowSet*, uint64_t*, const int64_t*);
    return [slot = std::move(slot), entry,
            keyColumns = std::make_shared<const std::vector<int64_t>>(std::move(keyColumns))](
        TRowSet* batch, uint64_t* hashes) {
        return slot->Call<THashFn>(entry, batch, hashes, keyColumns->data());
    };
}

TKernelCompiler::TAggregateHashDispatch TKernelCompiler::CompileAggregateHash(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys)
{
    auto& types = *Types_;
    using namespace NQumir::NAst;

    const auto keyDesc = NKernel::BuildAggregateKeyDescriptor(types, inputType, groupKeys);

    auto columnType = QumirDbNamedType(types, "TColumn");
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto stringViewType = QumirDbNamedType(types, "StringView");

    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        std::vector<TExprPtr> program;
        for (auto& f : NKernel::GenKeyTypeDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        if (keyDesc.HasDistinctLookupType()) {
            auto stringOps = Annotation_.Modules->KernelLibrary("aggregation", "string_ops.oz");
            if (!stringOps) {
                throw NQumir::TError(
                    "CompileAggregateHash: string_ops.oz: " + stringOps.error().ToString());
            }
            for (auto& f : *stringOps) program.push_back(std::move(f));
        }
        for (auto& f : NKernel::GenKeyOperationFunDecls(types, keyDesc)) {
            program.push_back(std::move(f));
        }
        program.push_back(NKernel::GenKeyHashBatchAst(
            types, keyDesc, "agg_hash_batch", columnType, rowSetType, stringViewType));
        program.push_back(NKernel::GenKeyHashEntrypointAst(
            types, keyDesc, "agg_hash", "agg_hash_batch", rowSetType));
        return program;
    };

    auto program = NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "aggregate_hash", program);

    auto kernel = EmitKernel("aggregate_hash", {"agg_hash"}, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    std::vector<int64_t> keyColumns;
    keyColumns.reserve(keyDesc.Fields.size());
    for (const auto& field : keyDesc.Fields) {
        keyColumns.push_back(field.ColumnIndex);
    }

    auto dispatch = BindAggregateHash(kernel.Slot, std::move(keyColumns));
    dispatch.Kernel = kernel.Index;
    return dispatch;
}

TKernelCompiler::TAggregateHashDispatch BindAggregateHash(
    std::shared_ptr<TKernelSlot> slot,
    std::vector<int64_t> keyColumns)
{
    return {
        .Slot = std::move(slot),
        .KeyColumns = std::move(keyColumns),
    };
}

bool TKernelCompiler::TAggregateHashDispatch::operator()(
    TRowSet* batch, uint64_t* hashes) const
{
    using THashFn = bool(*)(TRowSet*, uint64_t*, const int64_t*);
    return Slot->Call<THashFn>(0, batch, hashes, KeyColumns.data());
}

TRowJoinKernels TKernelCompiler::CompileRowJoin(
    const NKernel::TOperatorKernelSpec& spec,
    bool buildIsLeft)
{
    auto& types = *Types_;
    using namespace NQumir::NAst;

    const auto [leftType, rightType] = JoinInputTypes(spec, "CompileRowJoin");
    // An outer join streams its preserved side, so it builds the other one.
    const bool streamsPreserved = buildIsLeft
        ? spec.JoinType == EJoinType::Right
        : spec.JoinType == EJoinType::Left;
    // A SEMI/ANTI join that builds its left side marks the rows it matches.
    const bool marksBuild = buildIsLeft
        && (spec.JoinType == EJoinType::LeftSemi || spec.JoinType == EJoinType::LeftAnti);
    if ((spec.JoinType != EJoinType::Inner && !streamsPreserved && !marksBuild)
        || !spec.Expressions.empty() || spec.JoinKeys.empty()) {
        throw NQumir::TError(
            "CompileRowJoin: only inner joins, outer joins that build their null-supplying "
            "side and semi/anti joins that build their left side are supported, as "
            "equi-joins without a residual");
    }

    const auto keyDesc = JoinKeyDescriptorOf(types, *leftType, *rightType, JoinKeysOf(spec));
    const auto& buildType = buildIsLeft
        ? *leftType
        : *rightType;
    const auto layout = NKernel::BuildRowJoinLayout(types, buildType, keyDesc, marksBuild);
    const bool keepsNullKeyRows = marksBuild && spec.JoinType == EJoinType::LeftAnti;

    TRowJoinKernelMeta kernels;
    for (const auto& field : keyDesc.Fields) {
        kernels.BuildKeyColumns.push_back(
            buildIsLeft ? field.LeftColumnIndex : field.RightColumnIndex);
        kernels.ProbeKeyColumns.push_back(
            buildIsLeft ? field.RightColumnIndex : field.LeftColumnIndex);
    }

    if (Diagnostics_) {
        *Diagnostics_ << "\n========== KERNEL SPEC ==========\n";
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
        *Diagnostics_ << "=================================\n";
    }

    const NKernel::TRowJoinGenTypes joinTypes{
        .Column = QumirDbNamedType(types, "TColumn"),
        .RowSet = QumirDbNamedType(types, "TRowSet"),
        .PairBuffer = QumirDbNamedType(types, "PairBuffer"),
        .StringView = QumirDbNamedType(types, "StringView"),
        .Arena = QumirDbNamedType(types, "RowArena"),
        .Table = QumirDbNamedType(types, "RowTable"),
        .Scratch = QumirDbNamedType(types, "RowScratch"),
    };
    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        auto library = NKernel::BuildRowJoinKernelLibrary(*Annotation_.Modules);
        if (!library) {
            throw NQumir::TError("CompileRowJoin: " + library.error().ToString());
        }
        const bool hasString = [&] {
            auto isString = [](const auto& field) {
                return static_cast<bool>(TMaybeType<TStringType>(
                    UnwrapNamedValueType(UnwrapNullableType(field.second))));
            };
            return std::ranges::any_of(leftType->Fields, isString) ||
                std::ranges::any_of(rightType->Fields, isString);
        }();

        // Same order as CompileJoin: the key declarations, then the library
        // and the generated functions.
        std::vector<TExprPtr> program;
        AppendJoinKeyDecls(*Annotation_.Modules, program, keyDesc, hasString, "CompileRowJoin");
        for (auto& f : *library) {
            program.push_back(std::move(f));
        }
        program.push_back(NKernel::GenRowJoinAppendAst(
            types, keyDesc, layout, "jr_append_batch", joinTypes, spec.HasPrecomputedHash,
            /*nullKeys=*/false));
        if (keepsNullKeyRows) {
            program.push_back(NKernel::GenRowJoinAppendAst(
                types, keyDesc, layout, "jr_append_null_keys", joinTypes, spec.HasPrecomputedHash,
                /*nullKeys=*/true));
        }
        program.push_back(NKernel::GenRowJoinProbeAst(
            types, keyDesc, layout, /*probeIsLeft=*/!buildIsLeft,
            /*emitUnmatched=*/streamsPreserved, "jr_probe_batch", joinTypes,
            spec.HasPrecomputedHash));
        program.push_back(NKernel::GenRowJoinDispatchAst(types, keyDesc, layout, joinTypes, spec.JoinType));
        program.push_back(NKernel::GenRowJoinMaterializeAst(
            types, *leftType, *rightType, buildIsLeft,
            marksBuild ? NKernel::EJoinOutputColumns::Left : NKernel::EJoinOutputColumns::LeftRight,
            layout, joinTypes));
        return program;
    };

    std::vector<std::string> entries = {"jr_dispatch", "jr_materialize"};
    auto program = NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "row-join", program);
    auto kernel = EmitKernel("row-join", entries, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    kernels.RetainsBuildBatches = layout.HasString;
    const auto measure = EmitInstrumentation();
    kernels.ProbeMeasureWidths = InstrumentationColumnWidths(buildIsLeft ? rightType : leftType);
    kernels.EmitsByMark = marksBuild;
    kernels.KeepsNullKeyRows = keepsNullKeyRows;
    kernels.Kernel = kernel.Index;
    kernels.ProbeMeasureKernel = measure.Index;
    return BindRowJoinKernels(kernel.Slot, measure.Slot, std::move(kernels));
}

TRowJoinKernels BindRowJoinKernels(
    std::shared_ptr<TKernelSlot> slot,
    std::shared_ptr<TKernelSlot> measureSlot,
    TRowJoinKernelMeta meta)
{
    using TDispatchFn = bool(*)(
        void*, void*, void*, void*, TRowSet*, const int64_t*, int64_t);
    using TMaterializeFn = int64_t(*)(void*, TRowSet*, int64_t, int64_t, TRowSet*);

    TRowJoinKernels kernels{std::move(meta)};
    kernels.ProbeMeasureSlot = std::move(measureSlot);
    kernels.Dispatch = [slot,
                        buildKeyColumns = std::make_shared<const std::vector<int64_t>>(
                            kernels.BuildKeyColumns),
                        probeKeyColumns = std::make_shared<const std::vector<int64_t>>(
                            kernels.ProbeKeyColumns)](
        void* arena,
        void* table,
        void* scratch,
        void* pairs,
        TRowSet* batch,
        int64_t op) {
        const bool appends = op == static_cast<int64_t>(ERowJoinKernelOp::Append)
            || op == static_cast<int64_t>(ERowJoinKernelOp::AppendNullKeys);
        const auto* keyColumns = appends
            ? buildKeyColumns->data() : probeKeyColumns->data();
        return slot->Call<TDispatchFn>(0,
            arena, table, scratch, pairs, batch, keyColumns, op);
    };
    kernels.Materialize = [slot](
        void* pairs,
        TRowSet* stream,
        int64_t start,
        int64_t limit,
        TRowSet* out) {
        return slot->Call<TMaterializeFn>(1,
            pairs, stream, start, limit, out);
    };
    kernels.Slot = std::move(slot);
    return kernels;
}

TJoinKernels TKernelCompiler::CompileJoin(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    const std::vector<TJoinKey>& keys,
    EJoinType type,
    const NQumir::NAst::TExprPtr& residualPredicate,
    const NQumir::NAst::TStructType* innerType,
    size_t leftFieldCount,
    bool hasPrecomputedHash)
{
    auto& types = *Types_;
    using namespace NQumir::NAst;

    const bool isSemiAnti = (type == EJoinType::LeftSemi || type == EJoinType::LeftAnti);
    const bool isOuter =
        (type == EJoinType::Left || type == EJoinType::Right || type == EJoinType::Full);
    const bool isMark = type == EJoinType::LeftMark;
    if (!isSemiAnti && !isOuter && !isMark && type != EJoinType::Inner) {
        throw NQumir::TError(
            "CompileJoin: only Inner, Left, Right, Full, LeftSemi, LeftAnti, and LeftMark join types are supported");
    }

    // Unified key descriptor (reuses the aggregation key machinery).
    auto keyDesc = JoinKeyDescriptorOf(types, leftType, rightType, keys);
    if (isMark) {
        // IN compares with SQL equality.
        if (std::ranges::any_of(keys, [](const auto& key) { return key.NullsEqual; })) {
            throw NQumir::TError("CompileJoin: left_mark join does not support NULL-safe keys");
        }
        // With several components a NULL makes the row comparison unknown only
        // against right rows that match the other components, which the single
        // NULL group of the right key set cannot tell.
        const bool nullableKey = std::ranges::any_of(
            keyDesc.Fields, [](const auto& field) { return field.IsNullable; });
        if (nullableKey && keyDesc.Fields.size() > 1) {
            throw NQumir::TError(
                "CompileJoin: left_mark join does not support a nullable multi-column key");
        }
    }
    TJoinKernelMeta kernels;
    kernels.LeftKeyColumns.reserve(keyDesc.Fields.size());
    kernels.RightKeyColumns.reserve(keyDesc.Fields.size());
    for (const auto& field : keyDesc.Fields) {
        kernels.LeftKeyColumns.push_back(field.LeftColumnIndex);
        kernels.RightKeyColumns.push_back(field.RightColumnIndex);
    }

    auto columnType = QumirDbNamedType(types, "TColumn");
    auto rowSetType = QumirDbNamedType(types, "TRowSet");
    auto hashTableType = QumirDbNamedType(types, "HashTable");
    auto pairBufferType = QumirDbNamedType(types, "PairBuffer");
    auto stringViewType = QumirDbNamedType(types, "StringView");

    // Fresh program per entry (CompileKernelAst consumes the AST): key type
    // decls + key-ops overloads + shared library + generated process functions.
    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        auto library = NKernel::BuildJoinKernelLibrary(*Annotation_.Modules);
        if (!library) {
            throw NQumir::TError("CompileJoin: " + library.error().ToString());
        }
        if (residualPredicate && innerType) {
            InjectJoinResidualFilter(
                types, *library, residualPredicate, *innerType, leftFieldCount,
                columnType, rowSetType, stringViewType);
        }

        // Program order follows BuildGenericAggregateProgramAst: type decls,
        // string ops (callees of the key-ops overloads), per-key overloads
        // (rh_hash / rh_key_equal / key_owned_bytes / key_clone_owned), the
        // generic dual-key HashTable helpers they instantiate, then the join
        // library and the generated per-query functions.
        const bool includeRight = !isSemiAnti && !isMark;
        const bool hasStringOutput = [&] {
            auto isString = [](const auto& field) {
                return static_cast<bool>(TMaybeType<TStringType>(
                    UnwrapNamedValueType(UnwrapNullableType(field.second))));
            };
            return std::any_of(leftType.Fields.begin(), leftType.Fields.end(), isString) ||
                (includeRight && std::any_of(
                    rightType.Fields.begin(), rightType.Fields.end(), isString));
        }();

        std::vector<TExprPtr> program;
        AppendJoinKeyDecls(*Annotation_.Modules, program, keyDesc, hasStringOutput, "CompileJoin");
        for (auto& f : NKernel::GenJoinKeyOwnershipFunDecls(types, keyDesc)) {
            program.push_back(std::move(f));
        }
        auto dualKeyLibrary = NKernel::BuildJoinDualKeyLibrary(*Annotation_.Modules);
        if (!dualKeyLibrary) {
            throw NQumir::TError("CompileJoin: " + dualKeyLibrary.error().ToString());
        }
        for (auto& f : *dualKeyLibrary) program.push_back(std::move(f));
        for (auto& f : *library) program.push_back(std::move(f));
        // Left and Right run a residual only on the streaming path below;
        // ChooseJoinBuildSide selects it. FULL streams neither side, so its
        // symmetric probes mark the rows a residual accepted on both sides.
        const bool markBoth = type == EJoinType::Full && residualPredicate;
        program.push_back(NKernel::GenJoinProcessAst(types, keyDesc, /*isLeft=*/true,
            "jt_process_left", columnType, rowSetType, hashTableType, pairBufferType,
            stringViewType, hasPrecomputedHash, markBoth));
        program.push_back(NKernel::GenJoinProcessAst(types, keyDesc, /*isLeft=*/false,
            "jt_process_right", columnType, rowSetType, hashTableType, pairBufferType,
            stringViewType, hasPrecomputedHash, markBoth));
        // A streaming outer join builds its preserved side, so the stream of
        // the other side marks the build rows it matches.
        program.push_back(NKernel::GenJoinProbeAst(types, keyDesc, /*isLeft=*/true,
            "jt_probe_left_stream", columnType, rowSetType, hashTableType, pairBufferType,
            stringViewType, hasPrecomputedHash, /*markMatched=*/type == EJoinType::Right));
        program.push_back(NKernel::GenJoinProbeAst(types, keyDesc, /*isLeft=*/false,
            "jt_probe_right_stream", columnType, rowSetType, hashTableType, pairBufferType,
            stringViewType, hasPrecomputedHash, /*markMatched=*/type == EJoinType::Left));
        if (isSemiAnti) {
            program.push_back(NKernel::GenJoinInsertRowsOnlyAst(
                types, keyDesc, /*isLeft=*/true, "jt_insert_left_only",
                columnType, rowSetType, hashTableType, pairBufferType,
                stringViewType, hasPrecomputedHash));
            program.push_back(NKernel::GenJoinProbeMarkAst(
                types, keyDesc, /*keyDecides=*/!residualPredicate, "jt_probe_right_mark",
                columnType, rowSetType, hashTableType, pairBufferType,
                stringViewType, hasPrecomputedHash));
        }
        if (isSemiAnti && !residualPredicate) {
            program.push_back(NKernel::GenJoinInsertKeyOnlyAst(
                types, keyDesc, "jt_insert_key_only",
                columnType, rowSetType, hashTableType, pairBufferType,
                stringViewType));
            program.push_back(NKernel::GenJoinFinalizeSemiAntiAst(
                types, keyDesc, /*isAnti=*/type == EJoinType::LeftAnti,
                "jt_finalize_semi_anti", hashTableType, pairBufferType));
            program.push_back(NKernel::GenJoinProbeSemiAst(
                types, keyDesc, /*isAnti=*/type == EJoinType::LeftAnti,
                "jt_probe_left_semi", columnType, rowSetType, hashTableType,
                pairBufferType, stringViewType, hasPrecomputedHash));
        }
        if (isOuter) {
            // jt_finalize_outer: same logic as ANTI (emit own rows with no opp match)
            program.push_back(NKernel::GenJoinFinalizeSemiAntiAst(
                types, keyDesc, /*isAnti=*/true,
                "jt_finalize_outer", hashTableType, pairBufferType));
        }
        if (isMark) {
            program.push_back(NKernel::GenJoinInsertRowsOnlyAst(
                types, keyDesc, /*isLeft=*/true, "jt_insert_left_only",
                columnType, rowSetType, hashTableType, pairBufferType,
                stringViewType, hasPrecomputedHash));
            program.push_back(NKernel::GenJoinInsertKeyOnlyAst(
                types, keyDesc, "jt_insert_key_only",
                columnType, rowSetType, hashTableType, pairBufferType,
                stringViewType));
            program.push_back(NKernel::GenMarkJoinProbeAst(
                types, keyDesc, "mj_probe_left", columnType, rowSetType, hashTableType,
                pairBufferType, stringViewType, hasPrecomputedHash));
        }
        program.push_back(NKernel::GenJoinDispatchAst(
            types, keyDesc, type, static_cast<bool>(residualPredicate),
            rowSetType, hashTableType, pairBufferType));
        // Output materializer: semi/anti joins emit only the left columns, a
        // mark join the left columns and its marker.
        auto output = NKernel::EJoinOutputColumns::LeftRight;
        if (isSemiAnti) {
            output = NKernel::EJoinOutputColumns::Left;
        } else if (isMark) {
            output = NKernel::EJoinOutputColumns::LeftMarker;
        }
        program.push_back(NKernel::GenJoinMaterializeAst(
            types, leftType, rightType, output,
            columnType, rowSetType, pairBufferType, stringViewType));
        return program;
    };

    std::vector<std::string> entries = {"jt_dispatch", "jt_materialize"};

    auto program = NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "join", program);

    auto kernel = EmitKernel("join", entries, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    kernels.Kernel = kernel.Index;
    return BindJoinKernels(kernel.Slot, std::move(kernels));
}

TJoinKernels BindJoinKernels(
    std::shared_ptr<TKernelSlot> slot,
    TJoinKernelMeta meta)
{
    using TDispatchFn = bool(*)(
        void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*, void*,
        const int64_t*, const int64_t*, int64_t, int64_t);
    using TMaterializeFn = int64_t(*)(
        void*, TRowSet*, TRowSet*, TRowSet*, TRowSet*, int64_t, int64_t, TRowSet*);

    TJoinKernels kernels{std::move(meta)};
    kernels.Dispatch = [slot,
                        leftKeyColumns = std::make_shared<const std::vector<int64_t>>(
                            kernels.LeftKeyColumns),
                        rightKeyColumns = std::make_shared<const std::vector<int64_t>>(
                            kernels.RightKeyColumns)](
        void* left,
        void* right,
        TRowSet* batch,
        int64_t batchIdx,
        void* pairs,
        TRowSet* leftStore,
        TRowSet* rightStore,
        int64_t arg,
        int64_t op) {
        // Scratch of the residual's strings, which do not outlive the call.
        TStringArena arena;
        return slot->Call<TDispatchFn>(0,
            left, right, batch, batchIdx, pairs, leftStore, rightStore, &arena,
            leftKeyColumns->data(), rightKeyColumns->data(), arg, op);
    };
    kernels.Materialize = [slot](
        void* pairs,
        TRowSet* leftStore,
        TRowSet* rightStore,
        TRowSet* streamLeft,
        TRowSet* streamRight,
        int64_t start,
        int64_t limit,
        TRowSet* out) {
        return slot->Call<TMaterializeFn>(1,
            pairs, leftStore, rightStore, streamLeft, streamRight, start, limit, out);
    };
    kernels.Slot = std::move(slot);
    return kernels;
}

} // namespace NQdb
