#include <qdb/kernel/compiler.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/builder.h>
#include <qdb/kernel/column_value.h>
#include <qdb/kernel/finalize.h>
#include <qdb/plan/clone_expr.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/join_gen.h>
#include <qdb/kernel/join_key.h>
#include <qdb/kernel/lib.h>
#include <qdb/kernel/spec.h>
#include <qdb/exec/runtime_context.h>
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
        std::make_shared<TUseExpr>(NQumir::TLocation{}, "qumirdb"));
}

} // namespace

std::unordered_map<std::string, void*> CompileKernelAst(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    EnsureQumirDbUse(ast);
    return runner.CompileKernelAst(std::move(ast), entryNames, error);
}

std::unordered_map<std::string, void*> CompileKernelAst(
    NQumir::TLLVMRunner& runner,
    NQumir::NFrontend::TComposeResult composed,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    EnsureQumirDbUse(composed.Ast);
    return runner.CompileKernelAst(std::move(composed), entryNames, error);
}

namespace {
constexpr const char* CacheSchemaVersion = "v1";
// Bump when the generated key helpers or the .oz kernel libraries change.
constexpr const char* KernelLibVersion = "26";
} // namespace

NQumir::NCodeGen::TLlvmRunner::TLinkedModule CompileKernelAstCached(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    std::string* error)
{
    NQumir::NRegistry::EnsureQumirDbRuntimeSymbolsLinked();
    EnsureQumirDbUse(ast);
    return runner.CompileFusedKernelsCached(
        std::move(ast), entryNames, cacheDir, CacheSchemaVersion, KernelLibVersion, error);
}

TGeneratedKernel TKernelCompiler::EmitKernel(
    std::string name,
    std::vector<std::string> entrypoints,
    NQumir::NAst::TExprPtr ast,
    std::shared_ptr<void> storage)
{
    TGeneratedKernel kernel{
        .Name = std::move(name),
        .Stage = Stage_,
        .ExecStageId = ExecStageId_,
        .Entrypoints = std::move(entrypoints),
        .Ast = std::move(ast),
        .Storage = std::move(storage),
        .RuntimeContext = RuntimeContext_,
        .Slot = std::make_shared<TKernelSlot>(),
    };
    kernel.Slot->Fns.resize(kernel.Entrypoints.size(), nullptr);
    if (Sink_) {
        Sink_->push_back(kernel);
    }
    if (BindNow_) {
        JitFinalizeKernels(
            std::span(&kernel, 1), Diagnostics_, ExternalCatalog_);
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
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    EnsureQumirDbUse(ast);
    return runner.CompileKernelAstToObject(std::move(ast), entryNames, error);
}

std::optional<std::string> CompileKernelAstToObject(
    NQumir::TLLVMRunner& runner,
    NQumir::NFrontend::TComposeResult composed,
    const std::vector<std::string>& entryNames,
    std::string* error)
{
    EnsureQumirDbUse(composed.Ast);
    return runner.CompileKernelAstToObject(
        std::move(composed), entryNames, error);
}

std::optional<NQumir::TLLVMRunner::TCachedObjectModule>
CompileKernelAstToObjectsCached(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    const std::string& cacheDir,
    std::string* error)
{
    EnsureQumirDbUse(ast);
    return runner.CompileFusedKernelsToObjectsCached(
        std::move(ast),
        entryNames,
        cacheDir,
        CacheSchemaVersion,
        KernelLibVersion,
        error);
}

namespace {

NQumir::NAst::TTypePtr QumirDbNamedType(const std::string& name) {
    return std::make_shared<NQumir::NAst::TNamedType>(name, nullptr);
}

NQumir::NAst::TTypePtr SortCoreType(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return std::make_shared<TIntegerType>(integer.Cast()->Kind);
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return std::make_shared<TFloatType>();
    }
    return nullptr;
}

int64_t SortRadixKeyBits(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return integer.Cast()->BitWidth();
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return 64;
    }
    if (TMaybeType<TBoolType>(valueType)) {
        return 8;
    }
    return 0;
}

bool SortKeyIsString(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TStringType>(
        UnwrapNamedType(UnwrapNullableType(type))));
}

bool SortKeyIsBool(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TBoolType>(
        UnwrapNamedType(UnwrapNullableType(type))));
}

bool SortValueIsBinInt(const NQumir::NAst::TTypePtr& type) {
    auto valueType = UnwrapNullableType(type);
    return IsDecimalType(valueType) || IsBinIntStorageType(valueType);
}

bool SortValueIsInteger(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    return static_cast<bool>(TMaybeType<TIntegerType>(
        UnwrapNamedType(UnwrapNullableType(type))));
}

NQumir::NAst::TExprPtr Int64Literal(int64_t value) {
    auto expr = NKernel::NOz::Int(value);
    expr->Type = std::make_shared<NQumir::NAst::TIntegerType>();
    return expr;
}

NQumir::NAst::TTypePtr Ptr(NQumir::NAst::TTypePtr type) {
    return std::make_shared<NQumir::NAst::TPointerType>(std::move(type));
}

NQumir::NAst::TExprPtr Cast(
    NQumir::NAst::TExprPtr expr,
    NQumir::NAst::TTypePtr type)
{
    return std::make_shared<NQumir::NAst::TCastExpr>(
        NQumir::TLocation{}, std::move(expr), std::move(type));
}

NQumir::NAst::TExprPtr BuildRowIdSortCall(
    const TSortRadixKeyInput& key,
    bool nullable,
    NQumir::NAst::TExprPtr desc,
    NQumir::NAst::TExprPtr nullsFirst,
    std::string storeName)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto columnIndex = Int64Literal(key.ColumnIndex);
    auto ptrU64 = Ptr(std::make_shared<TIntegerType>(TIntegerType::U64));

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

    auto coreType = SortCoreType(key.Type);
    const int64_t keyBits = SortRadixKeyBits(key.Type);
    if (!coreType || keyBits == 0) {
        throw NQumir::TError(
            "CompileRadixSortComposite: unsupported key type " +
            (key.Type ? key.Type->ToString() : std::string("<null>")));
    }
    std::vector<TExprPtr> args{
        Oz::Ident(storeName),
        std::move(columnIndex),
        Oz::Ident("row_ids"),
        Oz::Ident("work"),
        Oz::Ident("counts"),
        Oz::Ident("n"),
        Int64Literal(keyBits),
        std::move(desc),
    };
    if (nullable) {
        args.push_back(std::move(nullsFirst));
    }
    args.push_back(Cast(Int64Literal(0), Ptr(std::move(coreType))));
    return Oz::Call(
        nullable ? "sort_fixed_rowids_nullable" : "sort_fixed_rowids",
        std::move(args));
}

NQumir::NAst::TExprPtr BuildDynamicRowIdSortCall(
    const TSortRadixKeyInput& key,
    size_t keyIdx,
    bool nullable)
{
    namespace Oz = NKernel::NOz;
    auto desc = Oz::Index("descs", Int64Literal(static_cast<int64_t>(keyIdx)));
    auto nullsFirst = Oz::Index(
        "nulls_firsts", Int64Literal(static_cast<int64_t>(keyIdx)));
    return BuildRowIdSortCall(
        key, nullable, std::move(desc), std::move(nullsFirst), "store");
}

NQumir::NAst::TExprPtr BuildRadixCompositeWrapperAst(
    const std::vector<TSortRadixKeyInput>& keys,
    bool nullable)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto u32Type = std::make_shared<TIntegerType>(TIntegerType::U32);
    auto boolType = std::make_shared<TBoolType>();
    auto rowSetPtrType = Ptr(QumirDbNamedType("TRowSet"));
    auto ptrI64Type = Ptr(i64Type);
    auto ptrU32Type = Ptr(u32Type);
    auto ptrBoolType = Ptr(boolType);

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
        .Return(std::make_shared<TVoidType>());
    if (nullable) {
        builder.Param("nulls_firsts", ptrBoolType);
    }

    for (size_t k = keys.size(); k > 0; --k) {
        const size_t keyIdx = k - 1;
        builder.Stmt(BuildDynamicRowIdSortCall(keys[keyIdx], keyIdx, nullable));
    }
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr Field(
    NQumir::NAst::TExprPtr object,
    std::string field)
{
    return std::make_shared<NQumir::NAst::TFieldAccessExpr>(
        NQumir::TLocation{}, std::move(object), std::move(field));
}

NQumir::NAst::TExprPtr Unary(
    std::string op,
    NQumir::NAst::TExprPtr value)
{
    return std::make_shared<NQumir::NAst::TUnaryExpr>(
        NQumir::TLocation{}, NQumir::NAst::TOperator(op), std::move(value));
}

NQumir::NAst::TExprPtr ArrayAssign(
    std::string name,
    NQumir::NAst::TExprPtr index,
    NQumir::NAst::TExprPtr value)
{
    std::vector<NQumir::NAst::TExprPtr> indices;
    indices.push_back(std::move(index));
    return std::make_shared<NQumir::NAst::TArrayAssignExpr>(
        NQumir::TLocation{}, std::move(name), std::move(indices), std::move(value));
}

NQumir::NAst::TExprPtr TypedInt(
    int64_t value,
    NQumir::NAst::TTypePtr type)
{
    auto expr = std::make_shared<NQumir::NAst::TNumberExpr>(
        NQumir::TLocation{}, value);
    expr->Type = std::move(type);
    return expr;
}

NQumir::NAst::TExprPtr BoolConst(bool value) {
    return NKernel::NOz::Bool(value);
}

NQumir::NAst::TExprPtr Deref(NQumir::NAst::TExprPtr ptr) {
    return NKernel::NOz::Index(std::move(ptr), Int64Literal(0));
}

NQumir::NAst::TExprPtr Low32(NQumir::NAst::TExprPtr value) {
    return NKernel::NOz::Bin(
        NQumir::NAst::TOperator("&"),
        std::move(value),
        Int64Literal(4294967295LL));
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
    if (TMaybeType<TBoolType>(UnwrapNamedType(value.ValueType))) {
        return Cast(value.Value, i64Type);
    }
    return value.Value;
}

void AppendSortLessBody(
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

    auto i64Type = std::make_shared<TIntegerType>();
    auto columnType = QumirDbNamedType("TColumn");
    auto stringViewType = QumirDbNamedType("StringView");

    for (size_t k = 0; k < keys.size(); ++k) {
        const auto& key = keys[k];
        const std::string suffix = std::to_string(k);
        const std::string leftCol = "left_col_" + suffix;
        const std::string rightCol = "right_col_" + suffix;

        builder
            .Var(leftCol, columnType)
            .Assign(leftCol,
                Oz::Index(leftColumns, Int64Literal(key.ColumnIndex)))
            .Var(rightCol, columnType)
            .Assign(rightCol,
                Oz::Index(rightColumns, Int64Literal(key.ColumnIndex)));

        auto leftValue = NKernel::BuildColumnValueAst(
            leftCol, leftRow, "left_key_" + suffix,
            key.Type, stringViewType);
        auto rightValue = NKernel::BuildColumnValueAst(
            rightCol, rightRow, "right_key_" + suffix,
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
            Oz::Block({Oz::Return(BoolConst(!key.Desc))})));
        builder.Stmt(Oz::If(
            Oz::Bin(TOperator("<"), rightComparable, leftComparable),
            Oz::Block({Oz::Return(BoolConst(key.Desc))})));
    }

    builder.Stmt(Oz::Return(std::move(equalResult)));
}

NQumir::NAst::TExprPtr BuildTopSortTempBeforeStateAst(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto rowSetPtrType = Ptr(rowSetType);
    auto columnType = QumirDbNamedType("TColumn");
    auto columnPtrType = Ptr(columnType);

    Oz::TFunBuilder builder("qdb_top_sort_temp_before_state");
    builder
        .Param("state", rowSetPtrType)
        .Param("batch", rowSetPtrType)
        .Param("state_row", i64Type)
        .Param("temp_row", i64Type)
        .Return(std::make_shared<TBoolType>())
        .Var("state_rs", rowSetType)
        .Assign("state_rs", Deref(Oz::Ident("state")))
        .Var("batch_rs", rowSetType)
        .Assign("batch_rs", Deref(Oz::Ident("batch")))
        .Var("state_cols", columnPtrType)
        .Assign("state_cols", Field(Oz::Ident("state_rs"), "Columns"))
        .Var("batch_cols", columnPtrType)
        .Assign("batch_cols", Field(Oz::Ident("batch_rs"), "Columns"));

    AppendSortLessBody(
        builder, keys,
        "batch_cols", "temp_row",
        "state_cols", "state_row",
        BoolConst(false));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildTopSortMergePicksAst()
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u32Type = std::make_shared<TIntegerType>(TIntegerType::U32);
    auto rowSetPtrType = Ptr(QumirDbNamedType("TRowSet"));
    auto ptrI64Type = Ptr(i64Type);
    auto ptrU8Type = Ptr(u8Type);
    auto ptrU32Type = Ptr(u32Type);

    auto makePickState = [&]() {
        return Oz::Block({
            ArrayAssign("pick_src", Oz::Ident("out"), TypedInt(0, u8Type)),
            ArrayAssign("pick_idx", Oz::Ident("out"),
                Cast(Oz::Ident("left"), u32Type)),
            Oz::Assign("left", Oz::Add(Oz::Ident("left"), Int64Literal(1))),
        });
    };
    auto makePickTemp = [&]() {
        return Oz::Block({
            Oz::Assign("temp_id", Oz::Index("temp_row_ids", Oz::Ident("right"))),
            Oz::Assign("temp_row", Low32(Oz::Ident("temp_id"))),
            ArrayAssign("pick_src", Oz::Ident("out"), TypedInt(1, u8Type)),
            ArrayAssign("pick_idx", Oz::Ident("out"),
                Cast(Oz::Ident("temp_row"), u32Type)),
            Oz::Assign("right", Oz::Add(Oz::Ident("right"), Int64Literal(1))),
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
        .Var("state_rs", QumirDbNamedType("TRowSet"))
        .Assign("state_rs", Deref(Oz::Ident("state")))
        .Var("state_n", i64Type)
        .Assign("state_n", Field(Oz::Ident("state_rs"), "RowCount"))
        .Var("left", i64Type)
        .Assign("left", Int64Literal(0))
        .Var("right", i64Type)
        .Assign("right", Int64Literal(0))
        .Var("out", i64Type)
        .Assign("out", Int64Literal(0))
        .Var("temp_id", i64Type)
        .Assign("temp_id", Int64Literal(0))
        .Var("temp_row", i64Type)
        .Assign("temp_row", Int64Literal(0));

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
        Oz::Assign("temp_row", Low32(Oz::Ident("temp_id"))),
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
        Oz::Assign("out", Oz::Add(Oz::Ident("out"), Int64Literal(1))),
    });
    builder.Stmt(Oz::While(std::move(loopCond), std::move(loopBody)));
    builder.Stmt(Oz::Return(Oz::Ident("out")));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildTopSortUpdateAst(
    const std::vector<TSortRadixKeyInput>& keys,
    bool materializeOutput)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto i8Type = std::make_shared<TIntegerType>(TIntegerType::I8);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto u32Type = std::make_shared<TIntegerType>(TIntegerType::U32);
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto rowSetPtrType = Ptr(rowSetType);
    auto rowSetRefType = std::make_shared<TReferenceType>(rowSetType);
    auto ptrI64Type = Ptr(i64Type);
    auto ptrU32Type = Ptr(u32Type);
    auto ptrU8Type = Ptr(u8Type);
    auto boolType = std::make_shared<TBoolType>();

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
            keys[keyIdx],
            anyNullableKey,
            BoolConst(keys[keyIdx].Desc),
            BoolConst(keys[keyIdx].NullsFirst),
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
        Oz::Bin(TOperator("<="), Oz::Ident("out_count"), Int64Literal(0)),
        Oz::Block({Oz::Return(Int64Literal(0))})));
    builder
        .Var("materialize_store", rowSetPtrType)
        .Assign("materialize_store", Oz::Cast(
            Oz::Call("qdb_alloc", {Oz::Mul(Int64Literal(2),
                Int64Literal(static_cast<int64_t>(TKernelCompiler::RowSetSize)))}),
            rowSetPtrType))
        .Stmt(Oz::ArrayAssign("materialize_store", Int64Literal(0), Deref(Oz::Ident("state"))))
        .Stmt(Oz::ArrayAssign("materialize_store", Int64Literal(1), Deref(Oz::Ident("batch"))))
        .Var("i", i64Type)
        .Assign("i", Int64Literal(0))
        .Stmt(Oz::While(
            Oz::Bin(TOperator("<"), Oz::Ident("i"), Oz::Ident("out_count")),
            Oz::Block({
                Oz::ArrayAssign("row_ids", Oz::Ident("i"),
                    Oz::Bin(TOperator("|"),
                        Oz::Bin(TOperator("<<"),
                            Oz::Cast(Oz::Index("pick_src", Oz::Ident("i")), i64Type),
                            Int64Literal(32)),
                        Oz::Cast(Oz::Index("pick_idx", Oz::Ident("i")), i64Type))),
                Oz::Assign("i", Oz::Add(Oz::Ident("i"), Int64Literal(1))),
            })))
        .Var("materialized", i64Type)
        .Assign("materialized", Oz::Call("sort_materialize_row_ids", {
            Oz::Ident("materialize_store"),
            Oz::Ident("row_ids"),
            Int64Literal(0),
            Oz::Ident("out_count"),
            Oz::Ident("out_rowset"),
        }))
        .Stmt(Oz::Call("qdb_free", {
            Oz::Cast(Oz::Ident("materialize_store"), Ptr(i8Type)),
        }))
        .Stmt(Oz::Return(Oz::Ident("materialized")));
    return std::move(builder).Build();
}

int64_t MaterializeFixedWidth(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto valueType = UnwrapNamedType(UnwrapNullableType(type));
    if (auto integer = TMaybeType<TIntegerType>(valueType)) {
        return integer.Cast()->BitWidth() / 8;
    }
    if (TMaybeType<TFloatType>(valueType)) {
        return 8;
    }
    return 0;
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
    const NQumir::NAst::TStructType& inputType,
    const std::string& entryName = "sort_materialize_row_ids",
    int64_t extraColumnCount = 0,
    int64_t extraOwnerCount = 0,
    const TExtraColumnEmitter& emitExtraColumns = {})
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetPtrType = Ptr(rowSetType);
    auto rowSetRefType = std::make_shared<TReferenceType>(rowSetType);
    auto columnPtrType = Ptr(columnType);
    auto ptrI64Type = Ptr(i64Type);

    const int64_t columnCount =
        static_cast<int64_t>(inputType.Fields.size()) + extraColumnCount;
    int64_t ownedPtrCount = 1; // columns array, stored by sort_materialize_begin.
    for (const auto& field : inputType.Fields) {
        ownedPtrCount += SortKeyIsString(field.second) ? 3 : 2;
    }
    ownedPtrCount += extraOwnerCount;

    auto column = [&](size_t idx) -> TExprPtr {
        return Oz::Index("columns", Int64Literal(static_cast<int64_t>(idx)));
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
            Oz::Bin(TOperator("<="), Oz::Ident("n"), Int64Literal(0)),
            Oz::Block({Oz::Return(Int64Literal(0))})))
        .Var("columns", columnPtrType)
        .Assign("columns", Oz::Call("sort_materialize_begin", {
            Int64Literal(columnCount),
            Int64Literal(ownedPtrCount),
            Oz::Ident("n"),
            Oz::Ident("out_rowset"),
        }));

    int64_t ownerIdx = 2;
    for (size_t c = 0; c < inputType.Fields.size(); ++c) {
        const auto& type = inputType.Fields[c].second;
        const auto srcCol = Int64Literal(static_cast<int64_t>(c));
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
                Int64Literal(offsetsOwner),
                Int64Literal(dataOwner),
                Int64Literal(maskOwner),
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
                Int64Literal(dataOwner),
                Int64Literal(maskOwner),
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
                Int64Literal(dataOwner),
                Int64Literal(maskOwner),
            }));
            continue;
        }
        auto coreType = SortCoreType(type);
        const int64_t width = MaterializeFixedWidth(type);
        if (!coreType || width == 0) {
            throw NQumir::TError(
                "BuildSortMaterializeWrapperAst: unsupported column type " +
                (type ? type->ToString() : std::string("<null>")));
        }
        builder.Stmt(Oz::Call("sort_materialize_fixed_column", {
            Oz::Ident("store"),
            Oz::Ident("row_ids"),
            Oz::Ident("start"),
            Oz::Ident("n"),
            srcCol,
            column(c),
            Oz::Ident("out_rowset"),
            Int64Literal(dataOwner),
            Int64Literal(maskOwner),
            Int64Literal(width),
            Cast(Int64Literal(0), Ptr(std::move(coreType))),
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
    const std::string& runName,
    const std::vector<TSortRadixKeyInput>& keys,
    bool nullable,
    const std::string& materializeEntry)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto u32Type = std::make_shared<TIntegerType>(TIntegerType::U32);
    auto boolType = std::make_shared<TBoolType>();
    auto rowSetPtrType = Ptr(QumirDbNamedType("TRowSet"));
    auto rowSetRefType = std::make_shared<TReferenceType>(QumirDbNamedType("TRowSet"));
    auto ptrI64Type = Ptr(i64Type);
    auto ptrU32Type = Ptr(u32Type);
    auto ptrBoolType = Ptr(boolType);

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
        auto desc = Oz::Index("descs", Int64Literal(static_cast<int64_t>(keyIdx)));
        auto nullsFirst = Oz::Index(
            "nulls_firsts", Int64Literal(static_cast<int64_t>(keyIdx)));
        sortStmts.push_back(BuildRowIdSortCall(
            keys[keyIdx], nullable, std::move(desc), std::move(nullsFirst), "store"));
    }
    builder.Stmt(Oz::If(Oz::Ident("do_sort"), Oz::Block(std::move(sortStmts))));
    builder.Stmt(Oz::If(
        Oz::Bin(TOperator("<="), Oz::Ident("limit"), Int64Literal(0)),
        Oz::Block({Oz::Return(Int64Literal(0))})));
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
    std::vector<NQumir::NAst::TExprPtr>& programStmts,
    const char* errorPrefix)
{
    using namespace NQumir::NAst;
    for (const auto& name : {"materialize.oz"}) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
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
    auto stringOps = NKernel::ParseFunctionLibrary(
        NKernel::ReadAggregationKernel("string_ops.oz"));
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
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TWindowFuncInput>& funcs)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    const int64_t inputColumnCount = static_cast<int64_t>(inputType.Fields.size());
    int64_t extraOwnerCount = 0;
    for (const auto& func : funcs) {
        extraOwnerCount += func.Func == "rank" ? 1 : 2; // data; plus Mask for aggregates.
    }

    auto emitWindowColumns = [&](
        Oz::TFunBuilder& builder,
        const std::function<TExprPtr(size_t)>& column,
        int64_t& ownerIdx)
    {
        for (size_t f = 0; f < funcs.size(); ++f) {
            const int64_t dataOwner = ownerIdx++;
            if (funcs[f].Func == "rank") {
                builder.Stmt(Oz::Call("window_fill_rank", {
                    Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                    Oz::Ident("n"),
                    column(static_cast<size_t>(inputColumnCount) + f),
                    Oz::Ident("out_rowset"), Int64Literal(dataOwner),
                }));
            } else if (funcs[f].Func == "avg_partition") {
                const int64_t maskOwner = ownerIdx++;
                const auto& argType =
                    inputType.Fields[static_cast<size_t>(funcs[f].ArgColumn)].second;
                if (SortValueIsBinInt(argType)) {
                    // Scale the sum up to the result decimal's scale before dividing
                    // by the count, so the quotient keeps fractional precision. The
                    // delta is exactly the scale the result type added over the arg.
                    auto argSpec = DecimalSpecOfValueType(UnwrapNullableType(argType));
                    auto resSpec = DecimalSpecOfValueType(
                        UnwrapNullableType(funcs[f].ResultType));
                    const int64_t scaleDelta =
                        (argSpec && resSpec) ? (resSpec->Scale - argSpec->Scale) : 0;
                    builder.Stmt(Oz::Call("window_fill_partition_avg_binint", {
                        Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                        Oz::Ident("n"),
                        Int64Literal(static_cast<int64_t>(funcs[f].ArgColumn)),
                        column(static_cast<size_t>(inputColumnCount) + f),
                        Oz::Ident("out_rowset"), Int64Literal(dataOwner),
                        Int64Literal(maskOwner),
                        Int64Literal(scaleDelta),
                    }));
                    continue;
                }
                auto argCoreType = SortCoreType(argType);
                if (!argCoreType) {
                    throw NQumir::TError(
                        "BuildWindowMaterializeWrapperAst: unsupported avg argument type " +
                        (argType ? argType->ToString() : std::string("<null>")));
                }
                builder.Stmt(Oz::Call("window_fill_partition_avg_f64", {
                    Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                    Oz::Ident("n"),
                    Int64Literal(static_cast<int64_t>(funcs[f].ArgColumn)),
                    Cast(Int64Literal(0), Ptr(std::move(argCoreType))),
                    column(static_cast<size_t>(inputColumnCount) + f),
                    Oz::Ident("out_rowset"), Int64Literal(dataOwner),
                    Int64Literal(maskOwner),
                }));
            } else if (funcs[f].Func == "sum_prefix" ||
                       funcs[f].Func == "sum_partition" ||
                       funcs[f].Func == "max_prefix")
            {
                const int64_t maskOwner = ownerIdx++;
                const auto& argType =
                    inputType.Fields[static_cast<size_t>(funcs[f].ArgColumn)].second;
                const bool binInt = SortValueIsBinInt(argType);
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
                        Int64Literal(static_cast<int64_t>(funcs[f].ArgColumn)),
                        column(static_cast<size_t>(inputColumnCount) + f),
                        Oz::Ident("out_rowset"), Int64Literal(dataOwner),
                        Int64Literal(maskOwner),
                    };
                    if (isPrefix) {
                        args.push_back(Oz::Bool(funcs[f].PeerInclusive));
                    }
                    builder.Stmt(Oz::Call(filler, std::move(args)));
                    continue;
                }
                auto argCoreType = SortCoreType(argType);
                const int64_t width = MaterializeFixedWidth(argType);
                if (!argCoreType || width == 0) {
                    throw NQumir::TError(
                        "BuildWindowMaterializeWrapperAst: unsupported window argument type " +
                        (argType ? argType->ToString() : std::string("<null>")));
                }
                const char* filler = funcs[f].Func == "sum_prefix"
                    ? "window_fill_prefix_sum_fixed"
                    : funcs[f].Func == "sum_partition"
                        ? "window_fill_partition_sum_fixed"
                        : "window_fill_prefix_max_fixed";
                const bool integerSum =
                    (funcs[f].Func == "sum_prefix" || funcs[f].Func == "sum_partition") &&
                    SortValueIsInteger(argType);
                std::vector<TExprPtr> args = {
                    Oz::Ident("store"), Oz::Ident("row_ids"), Oz::Ident("start"),
                    Oz::Ident("n"), Int64Literal(static_cast<int64_t>(funcs[f].ArgColumn)),
                    Cast(Int64Literal(0), Ptr(std::move(argCoreType))),
                };
                if (funcs[f].Func == "sum_prefix" || funcs[f].Func == "sum_partition") {
                    auto outCoreType = integerSum
                        ? std::make_shared<TIntegerType>()
                        : SortCoreType(argType);
                    const int64_t outWidth = integerSum ? 8 : width;
                    if (!outCoreType || outWidth == 0) {
                        throw NQumir::TError(
                            "BuildWindowMaterializeWrapperAst: unsupported window sum result type " +
                            (argType ? argType->ToString() : std::string("<null>")));
                    }
                    args.push_back(Cast(Int64Literal(0), Ptr(std::move(outCoreType))));
                    args.push_back(Int64Literal(outWidth));
                } else {
                    args.push_back(Int64Literal(width));
                }
                args.push_back(column(static_cast<size_t>(inputColumnCount) + f));
                args.push_back(Oz::Ident("out_rowset"));
                args.push_back(Int64Literal(dataOwner));
                args.push_back(Int64Literal(maskOwner));
                if (isPrefix) {
                    args.push_back(Oz::Bool(funcs[f].PeerInclusive));
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
        inputType,
        "window_materialize_row_ids",
        static_cast<int64_t>(funcs.size()),
        extraOwnerCount,
        emitWindowColumns);
}

NQumir::NAst::TExprPtr BuildWindowKeyComparatorAst(
    std::string name,
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
    auto rowSetPtrType = Ptr(QumirDbNamedType("TRowSet"));

    Oz::TFunBuilder builder(std::move(name));
    builder
        .Param("store", rowSetPtrType)
        .Param("left_row_id", i64Type)
        .Param("right_row_id", i64Type)
        .Return(boolType);

    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        const auto columnIdx = Int64Literal(static_cast<int64_t>(key.ColumnIndex));
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
                Int64Literal(static_cast<int64_t>(key.ColumnIndex)),
            }))
            .Stmt(Oz::If(
                Oz::Bin(TOperator("!="), Oz::Ident(leftValid), Oz::Ident(rightValid)),
                Oz::Block({Oz::Return(Oz::Bool(false))})));

        std::vector<TExprPtr> compareStmts;
        if (SortKeyIsString(key.Type)) {
            compareStmts.push_back(Oz::If(
                Oz::Bin(TOperator("!="),
                    Oz::Call("sr_sort_compare", {
                        Oz::Ident("store"),
                        Int64Literal(static_cast<int64_t>(key.ColumnIndex)),
                        Oz::Ident("left_row_id"),
                        Oz::Ident("right_row_id"),
                    }),
                    Int64Literal(0)),
                Oz::Block({Oz::Return(Oz::Bool(false))})));
        } else if (SortKeyIsBool(key.Type)) {
            compareStmts.push_back(Oz::If(
                Oz::Bin(TOperator("!="),
                    Oz::Call("sr_load_bool_key", {
                        Oz::Ident("store"),
                        Oz::Ident("left_row_id"),
                        Int64Literal(static_cast<int64_t>(key.ColumnIndex)),
                    }),
                    Oz::Call("sr_load_bool_key", {
                        Oz::Ident("store"),
                        Oz::Ident("right_row_id"),
                        Int64Literal(static_cast<int64_t>(key.ColumnIndex)),
                    })),
                Oz::Block({Oz::Return(Oz::Bool(false))})));
        } else {
            auto coreType = SortValueIsBinInt(key.Type)
                ? BinIntStorageType()
                : SortCoreType(key.Type);
            if (!coreType) {
                throw NQumir::TError(
                    "BuildWindowKeyComparatorAst: unsupported key type " +
                    (key.Type ? key.Type->ToString() : std::string("<null>")));
            }
            auto witness = [&]() {
                return Cast(Int64Literal(0), Ptr(coreType));
            };
            compareStmts.push_back(Oz::If(
                Oz::Bin(TOperator("!="),
                    Oz::Call("sr_load_fixed_key", {
                        Oz::Ident("store"),
                        Oz::Ident("left_row_id"),
                        Int64Literal(static_cast<int64_t>(key.ColumnIndex)),
                        witness(),
                    }),
                    Oz::Call("sr_load_fixed_key", {
                        Oz::Ident("store"),
                        Oz::Ident("right_row_id"),
                        Int64Literal(static_cast<int64_t>(key.ColumnIndex)),
                        witness(),
                    })),
                Oz::Block({Oz::Return(Oz::Bool(false))})));
        }
        builder.Stmt(Oz::If(
            Oz::Ident(leftValid),
            Oz::Block(std::move(compareStmts))));
    }

    builder.Stmt(Oz::Return(Oz::Bool(true)));
    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildHeapTopKEntryAst()
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>();
    Oz::TFunBuilder builder("qdb_top_k_select");
    builder
        .Param("store", Ptr(QumirDbNamedType("TRowSet")))
        .Param("row_ids", Ptr(i64Type))
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

} // namespace

NQumir::NAst::TExprPtr BuildSortRowIdLessAst(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;
    namespace Oz = NKernel::NOz;

    if (keys.empty()) {
        throw NQumir::TError("BuildSortRowIdLessAst: empty key list");
    }

    auto i64Type = std::make_shared<TIntegerType>();
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto rowSetPtrType = Ptr(rowSetType);
    auto columnPtrType = Ptr(QumirDbNamedType("TColumn"));

    // Overloads heap_less on the store pointer, so the heap items stay bare
    // packed row ids instead of carrying the store in every element.
    Oz::TFunBuilder builder("heap_less");
    builder
        .Param("store", rowSetPtrType)
        .Param("left_id", i64Type)
        .Param("right_id", i64Type)
        .Return(std::make_shared<TBoolType>())
        .Var("left_batch", i64Type)
        .Assign("left_batch", Oz::Bin(
            TOperator(">>"), Oz::Ident("left_id"), Int64Literal(32)))
        .Var("right_batch", i64Type)
        .Assign("right_batch", Oz::Bin(
            TOperator(">>"), Oz::Ident("right_id"), Int64Literal(32)))
        .Var("left_row", i64Type)
        .Assign("left_row", Low32(Oz::Ident("left_id")))
        .Var("right_row", i64Type)
        .Assign("right_row", Low32(Oz::Ident("right_id")))
        .Var("left_row_set", rowSetType)
        .Assign("left_row_set", Oz::Index("store", Oz::Ident("left_batch")))
        .Var("right_row_set", rowSetType)
        .Assign("right_row_set", Oz::Index("store", Oz::Ident("right_batch")))
        .Var("left_cols", columnPtrType)
        .Assign("left_cols", Field(Oz::Ident("left_row_set"), "Columns"))
        .Var("right_cols", columnPtrType)
        .Assign("right_cols", Field(Oz::Ident("right_row_set"), "Columns"));

    AppendSortLessBody(
        builder, keys,
        "left_cols", "left_row",
        "right_cols", "right_row",
        Oz::Bin(TOperator("<"), Oz::Ident("left_id"), Oz::Ident("right_id")));

    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr BuildHeapTopKProgramAst(
    const std::vector<TSortRadixKeyInput>& keys)
{
    using namespace NQumir::NAst;

    if (keys.empty()) {
        throw NQumir::TError("BuildHeapTopKProgramAst: empty key list");
    }

    auto library = NKernel::ParseFunctionLibrary(
        NKernel::ReadSortKernel("heap.oz"));
    if (!library) {
        throw NQumir::TError(
            "BuildHeapTopKProgramAst: " + library.error().ToString());
    }

    std::vector<TExprPtr> programStmts;
    for (auto& stmt : *library) {
        programStmts.push_back(std::move(stmt));
    }
    programStmts.push_back(BuildSortRowIdLessAst(keys));
    programStmts.push_back(BuildHeapTopKEntryAst());
    return std::make_shared<TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildWindowProgramAst(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortRadixKeyInput>& partitionKeys,
    const std::vector<TSortRadixKeyInput>& orderKeys,
    const std::vector<TWindowFuncInput>& funcs)
{
    using namespace NQumir::NAst;
    if (keys.empty()) {
        throw NQumir::TError("BuildWindowProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
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
    AddSortMaterializeLibrary(programStmts, "BuildWindowProgramAst");
    programStmts.push_back(BuildWindowKeyComparatorAst(
        "window_same_partition", partitionKeys));
    programStmts.push_back(BuildWindowKeyComparatorAst(
        "window_same_order", orderKeys));
    addLibrary("window.oz", true);
    programStmts.push_back(BuildWindowMaterializeWrapperAst(
        inputType, funcs));
    // Nullable keys need the null-aware radix (nulls segregated to FIRST/LAST);
    // the non-nullable variant would bucket NULL rows by raw value bytes and
    // scatter a partition.
    const bool nullable = std::any_of(keys.begin(), keys.end(),
        [](const TSortRadixKeyInput& key) { return IsNullableType(key.Type); });
    programStmts.push_back(BuildSortRunWrapperAst(
        "qdb_window_run", keys, nullable, "window_materialize_row_ids"));
    return std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildRadixSortProgramAst(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;
    if (keys.empty()) {
        throw NQumir::TError("BuildRadixSortProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
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
    programStmts.push_back(BuildRadixCompositeWrapperAst(keys, false));
    if (materializeType) {
        AddSortMaterializeLibrary(programStmts, "BuildRadixSortProgramAst");
        programStmts.push_back(BuildSortMaterializeWrapperAst(*materializeType));
        programStmts.push_back(BuildSortRunWrapperAst("qdb_sort_run", keys, false, "sort_materialize_row_ids"));
    }
    return std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildRadixSortNullableProgramAst(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;
    if (keys.empty()) {
        throw NQumir::TError("BuildRadixSortNullableProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
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
    programStmts.push_back(BuildRadixCompositeWrapperAst(keys, true));
    if (materializeType) {
        AddSortMaterializeLibrary(programStmts, "BuildRadixSortNullableProgramAst");
        programStmts.push_back(BuildSortMaterializeWrapperAst(*materializeType));
        programStmts.push_back(BuildSortRunWrapperAst("qdb_sort_run", keys, true, "sort_materialize_row_ids"));
    }
    return std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

NQumir::NAst::TExprPtr BuildTopSortMergeProgramAst(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;
    if (keys.empty()) {
        throw NQumir::TError("BuildTopSortMergeProgramAst: empty key list");
    }
    std::vector<TExprPtr> programStmts;
    auto addLibrary = [&](const std::string& name, bool skipUse) {
        auto library = NKernel::ParseFunctionLibrary(NKernel::ReadSortKernel(name));
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
        AddSortMaterializeLibrary(programStmts, "BuildTopSortMergeProgramAst");
        programStmts.push_back(BuildSortMaterializeWrapperAst(*materializeType));
    }
    programStmts.push_back(BuildTopSortTempBeforeStateAst(keys));
    programStmts.push_back(BuildTopSortMergePicksAst());
    programStmts.push_back(BuildTopSortUpdateAst(keys, materializeType != nullptr));
    return std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(programStmts));
}

TKernelCompiler::TFilterDispatch TKernelCompiler::CompileFilter(
    const NKernel::TOperatorKernelSpec& spec)
{
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

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto stringViewType = QumirDbNamedType("StringView");

    auto literalStorage =
        std::make_shared<std::vector<std::shared_ptr<std::string>>>();
    auto kernelAst = NKernel::BuildFilterProgramAst(
        ClonePredicate(predicate), *inputType.Cast(), fieldIndices, columnType,
        rowSetType, stringViewType, *literalStorage);
    if (!kernelAst) {
        throw NQumir::TError(
            "CompileFilter: " + kernelAst.error().ToString());
    }

    if (Diagnostics_) {
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
    }
    PrintKernelAst(Diagnostics_, "filter", *kernelAst);

    auto kernel = EmitKernel(
        "filter", {"<kernel>"}, std::move(*kernelAst), literalStorage);
    FinishKernelDiagnostics(Diagnostics_);

    using TFilterFn = void(*)(void*, void*, void**);
    return [slot = kernel.Slot, storage = kernel.Storage,
            runtime = RuntimeContext_](TRowSet& rowSet) {
        TStringArena arena;
        reinterpret_cast<TFilterFn>(slot->Fns[0])(
            &rowSet, &arena, runtime ? runtime->Regexes.Handles() : nullptr);
    };
}

TKernelCompiler::TProjectDispatch TKernelCompiler::CompileProject(
    const NKernel::TOperatorKernelSpec& spec)
{
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

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto stringViewType = QumirDbNamedType("StringView");

    auto literalStorage =
        std::make_shared<std::vector<std::shared_ptr<std::string>>>();
    std::vector<NQumir::NAst::TExprPtr> cloned;
    cloned.reserve(spec.Expressions.size());
    for (const auto& expr : spec.Expressions) {
        cloned.push_back(ClonePredicate(expr));
    }

    auto kernelAst = NKernel::GenProjectKernelAst(
        std::move(cloned), computedTypes, *inputType.Cast(), fieldIndices,
        columnType, rowSetType, stringViewType, *literalStorage);

    if (Diagnostics_) {
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
    }
    PrintKernelAst(Diagnostics_, "project", kernelAst);

    auto kernel = EmitKernel(
        "project", {"<project>"}, std::move(kernelAst), literalStorage);
    FinishKernelDiagnostics(Diagnostics_);

    using TProjectFn = void(*)(void*, void**, void*, void**);
    return [slot = kernel.Slot, storage = kernel.Storage,
            runtime = RuntimeContext_](
               TRowSet* in, void** outBuffers, void* arena) {
        reinterpret_cast<TProjectFn>(slot->Fns[0])(
            in, outBuffers, arena,
            runtime ? runtime->Regexes.Handles() : nullptr);
    };
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

    auto program = BuildRadixSortProgramAst(keys, materializeType);
    PrintKernelAst(Diagnostics_, "sort.run.fused", program);

    auto kernel = EmitKernel(
        "sort.run.fused",
        {"qdb_sort_run"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using TSortFn = int64_t(*)(
        TRowSet*, int64_t*, int64_t*, uint32_t*, int64_t, bool*, bool*,
        bool, int64_t, int64_t, TRowSet*);
    return [slot = kernel.Slot](TRowSet* store, int64_t* rowIds, int64_t* work,
        uint32_t* counts, int64_t n, bool* descs, bool* nullsFirsts,
        bool doSort, int64_t start, int64_t limit, TRowSet* output) {
        return reinterpret_cast<TSortFn>(slot->Fns[0])(
            store, rowIds, work, counts, n, descs, nullsFirsts,
            doSort, start, limit, output);
    };
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
        keys, inputType, partitionKeys, orderKeys, funcs);
    PrintKernelAst(Diagnostics_, "window.run.fused", program);

    auto kernel = EmitKernel(
        "window.run.fused",
        {"qdb_window_run"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using TWindowFn = int64_t(*)(
        TRowSet*, int64_t*, int64_t*, uint32_t*, int64_t, bool*, bool*,
        bool, int64_t, int64_t, TRowSet*);
    return [slot = kernel.Slot](TRowSet* store, int64_t* rowIds, int64_t* work,
        uint32_t* counts, int64_t n, bool* descs, bool* nullsFirsts,
        bool doSort, int64_t start, int64_t limit, TRowSet* output) {
        return reinterpret_cast<TWindowFn>(slot->Fns[0])(
            store, rowIds, work, counts, n, descs, nullsFirsts,
            doSort, start, limit, output);
    };
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

    auto program = BuildRadixSortNullableProgramAst(keys, materializeType);
    PrintKernelAst(Diagnostics_, "sort.run.nullable.fused", program);

    auto kernel = EmitKernel(
        "sort.run.nullable.fused",
        {"qdb_sort_run"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using TSortFn = int64_t(*)(
        TRowSet*, int64_t*, int64_t*, uint32_t*, int64_t, bool*, bool*,
        bool, int64_t, int64_t, TRowSet*);
    return [slot = kernel.Slot](TRowSet* store, int64_t* rowIds,
        int64_t* work, uint32_t* counts, int64_t n, bool* descs, bool* nullsFirsts,
        bool doSort, int64_t start, int64_t limit, TRowSet* output) {
        return reinterpret_cast<TSortFn>(slot->Fns[0])(
            store, rowIds, work, counts, n, descs, nullsFirsts,
            doSort, start, limit, output);
    };
}

TKernelCompiler::TTopSortDispatch TKernelCompiler::CompileTopSort(
    const std::vector<TSortRadixKeyInput>& keys,
    const NQumir::NAst::TStructType* materializeType)
{
    using namespace NQumir::NAst;

    if (!materializeType) {
        throw NQumir::TError("CompileTopSort: materialize schema is required");
    }
    auto program = BuildTopSortMergeProgramAst(keys, materializeType);
    PrintKernelAst(Diagnostics_, "top-sort.fused", program);

    auto kernel = EmitKernel(
        "top-sort.fused",
        {"qdb_top_sort_update"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using TTopSortFn = int64_t(*)(
        TRowSet*, TRowSet*, int64_t*, int64_t*, uint32_t*,
        int64_t, bool, uint8_t*, uint32_t*, int64_t, TRowSet*);
    return [slot = kernel.Slot](
        TRowSet* state,
        TRowSet* batch,
        int64_t* rowIds,
        int64_t* work,
        uint32_t* counts,
        int64_t n,
        bool sortInput,
        uint8_t* pickSrc,
        uint32_t* pickIdx,
        int64_t limit,
        TRowSet* output) {
        return reinterpret_cast<TTopSortFn>(slot->Fns[0])(
            state, batch, rowIds, work, counts, n, sortInput,
            pickSrc, pickIdx, limit, output);
    };
}

TKernelCompiler::THeapTopKDispatch TKernelCompiler::CompileHeapTopK(
    const std::vector<TSortRadixKeyInput>& keys)
{
    auto program = BuildHeapTopKProgramAst(keys);
    PrintKernelAst(Diagnostics_, "top-k.heap", program);

    auto kernel = EmitKernel(
        "top-k.heap",
        {"qdb_top_k_select"},
        std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    using THeapTopKFn = int64_t(*)(
        TRowSet*, int64_t*, int64_t, int64_t);
    return [slot = kernel.Slot](
        TRowSet* store,
        int64_t* rowIds,
        int64_t size,
        int64_t limit) {
        return reinterpret_cast<THeapTopKFn>(slot->Fns[0])(
            store, rowIds, size, limit);
    };
}

TAggregateKernels TKernelCompiler::CompileAggregate(
    const NKernel::TOperatorKernelSpec& spec)
{
    if (spec.Kind != NKernel::EOperatorKernelKind::UnaryBlocking ||
        spec.OperatorName != "aggregate") {
        throw NQumir::TError("CompileAggregate: expected aggregate kernel spec");
    }
    if (spec.InputSchemas.size() != 1) {
        throw NQumir::TError("CompileAggregate: expected one input schema");
    }

    const auto* inputTypePtr = static_cast<NQumir::NAst::TStructType*>(
        spec.InputSchemas[0].get());
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
    const auto keyDescriptor = NKernel::BuildAggregateKeyDescriptor(inputType, groupKeys);
    for (const auto& field : keyDescriptor.Fields) {
        const auto valueType = UnwrapNullableType(field.Type);
        const auto type = UnwrapNamedType(valueType);
        if (!IsBinIntStorageType(valueType) &&
            !TMaybeType<TIntegerType>(type) && !TMaybeType<TFloatType>(type) &&
            !TMaybeType<TStringType>(type)) {
            throw NQumir::TError(
                "CompileAggregate: group key column '" + field.ColumnName +
                "' must be integer, f64, string, or BinInt");
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
            const bool isBinInt =
                IsBinIntStorageType(valueType) || IsDecimalType(valueType);
            const auto unwrapped = isBinInt ? valueType : UnwrapNamedType(valueType);
            const bool isFloat = static_cast<bool>(TMaybeType<TFloatType>(unwrapped));
            const bool isInteger = static_cast<bool>(TMaybeType<TIntegerType>(unwrapped));
            const bool isString = static_cast<bool>(TMaybeType<TStringType>(unwrapped));
            if (isString && agg.Func != "count" &&
                agg.Func != "min" && agg.Func != "max") {
                throw NQumir::TError(
                    "CompileAggregate: '" + agg.Func +
                    "' does not support string arguments");
            }
            if (!isInteger && !isFloat && !isBinInt && !isString) {
                throw NQumir::TError(
                    "CompileAggregate: aggregate argument column '" + name +
                    "' must be integer, f64, string, or BinInt");
            }
            arg.ValueKind = isBinInt
                ? NKernel::EAggValueKind::BinInt
                : (isString ? NKernel::EAggValueKind::String
                    : (isFloat ? NKernel::EAggValueKind::Float64
                               : NKernel::EAggValueKind::Int64));
            arg.IsNullable = IsNullableType(type);
            arg.ColumnIndex = columnIndex(name);
        }
        args.push_back(arg);
    }
    const auto layout = NKernel::BuildAggReducerLayout(funcs, args);

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto hashTableType = QumirDbNamedType("HashTable");

    auto program = NKernel::BuildGenericAggregateFusedProgramAst(
        inputType, keyDescriptor, layout,
        columnType, rowSetType, hashTableType, spec.HasPrecomputedHash);
    if (!program) {
        throw NQumir::TError(
            "CompileAggregate: " + program.error().ToString());
    }

    TAggregateKernels kernels;
    kernels.NumAggs = funcs.size();
    kernels.OutputAggs.reserve(layout.Reducers.size());
    for (const auto& reducer : layout.Reducers) {
        kernels.OutputAggs.push_back({.IsNullable = reducer.IsNullableOutput});
    }
    kernels.OutputKeys.reserve(keyDescriptor.Fields.size());
    for (const auto& field : keyDescriptor.Fields) {
        const auto logicalType = UnwrapNamedType(field.Type);
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
            (*Sink_)[i].AggKeys = keyMeta;
            (*Sink_)[i].AggValues = valueMeta;
        }
    }

    using TDispatchFn = int64_t(*)(void*, TRowSet*, int64_t, int64_t);
    using TFinishRowSetFn = int64_t(*)(void*, TRowSet*);

    auto slot = kernel.Slot;
    kernels.Dispatch = [slot](void* ht, TRowSet* batch, int64_t arg, int64_t op) {
        return reinterpret_cast<TDispatchFn>(slot->Fns[0])(ht, batch, arg, op);
    };
    kernels.FinishRowSet = [slot](void* ht, TRowSet* output) {
        return reinterpret_cast<TFinishRowSetFn>(slot->Fns[1])(ht, output);
    };
    return kernels;
}

TJoinKernels TKernelCompiler::CompileJoin(
    const NKernel::TOperatorKernelSpec& spec)
{
    using namespace NQumir::NAst;

    if (spec.Kind != NKernel::EOperatorKernelKind::Binary ||
        spec.OperatorName != "join") {
        throw NQumir::TError("CompileJoin: expected join kernel spec");
    }
    if (spec.InputSchemas.size() != 2) {
        throw NQumir::TError("CompileJoin: expected two input schemas");
    }

    auto leftType = TMaybeType<TStructType>(spec.InputSchemas[0]);
    auto rightType = TMaybeType<TStructType>(spec.InputSchemas[1]);
    if (!leftType || !rightType) {
        throw NQumir::TError("CompileJoin: input schemas must be structs");
    }

    std::vector<TJoinKey> keys;
    keys.reserve(spec.JoinKeys.size());
    for (const auto& key : spec.JoinKeys) {
        keys.push_back({key.Left.Name, key.Right.Name, key.NullsEqual});
    }

    TExprPtr residualPredicate =
        spec.Expressions.empty() ? nullptr : ClonePredicate(spec.Expressions[0]);
    if (residualPredicate &&
        spec.JoinType != EJoinType::Inner &&
        spec.JoinType != EJoinType::LeftSemi &&
        spec.JoinType != EJoinType::LeftAnti) {
        throw NQumir::TError(
            "CompileJoin: residual filter is not yet supported for this join type");
    }

    TTypePtr innerOutputType;
    TStructType* innerType = nullptr;
    size_t leftFieldCount = 0;
    if (residualPredicate) {
        auto innerOutput = ComputeJoinOutputType(
            spec.InputSchemas[0], spec.InputSchemas[1], EJoinType::Inner);
        if (!innerOutput) {
            throw NQumir::TError(
                "CompileJoin: residual filter: " + innerOutput.error().ToString());
        }
        innerOutputType = *innerOutput;
        innerType = static_cast<TStructType*>(innerOutputType.get());
        leftFieldCount = leftType.Cast()->Fields.size();
        residualPredicate = NKernel::ExpandKernelExpr(
            residualPredicate, *innerType, ExternalCatalog_.get()).first;
    }

    if (Diagnostics_) {
        *Diagnostics_ << "\n========== KERNEL SPEC ==========\n";
        NKernel::PrintKernelSpec(*Diagnostics_, spec);
        *Diagnostics_ << "=================================\n";
    }

    return CompileJoin(
        *leftType.Cast(), *rightType.Cast(), keys, spec.JoinType,
        residualPredicate, innerType, leftFieldCount, spec.HasPrecomputedHash);
}

TCrossJoinKernels TKernelCompiler::CompileCrossJoin(
    const NKernel::TOperatorKernelSpec& spec)
{
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

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto pairBufferType = QumirDbNamedType("PairBuffer");
    auto stringViewType = QumirDbNamedType("StringView");

    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        auto library = NKernel::BuildCrossJoinKernelLibrary();
        if (!library) {
            throw NQumir::TError(
                "CompileCrossJoin: " + library.error().ToString());
        }

        std::vector<TExprPtr> program;
        for (auto& f : *library) {
            program.push_back(std::move(f));
        }

        const bool hasStringOutput = [&] {
            auto isString = [](const auto& field) {
                return static_cast<bool>(TMaybeType<TStringType>(
                    UnwrapNamedType(UnwrapNullableType(field.second))));
            };
            return std::any_of(
                leftType.Cast()->Fields.begin(), leftType.Cast()->Fields.end(), isString) ||
                std::any_of(
                    rightType.Cast()->Fields.begin(), rightType.Cast()->Fields.end(), isString);
        }();
        if (hasStringOutput) {
            auto stringOps = NKernel::ParseFunctionLibrary(
                NKernel::ReadAggregationKernel("string_ops.oz"));
            if (!stringOps) {
                throw NQumir::TError(
                    "CompileCrossJoin: string_ops.oz: " +
                    stringOps.error().ToString());
            }
            for (auto& f : *stringOps) {
                program.push_back(std::move(f));
            }
        }

        program.push_back(NKernel::GenJoinMaterializeAst(
            *leftType.Cast(), *rightType.Cast(), /*includeRight=*/true,
            columnType, rowSetType, pairBufferType, stringViewType));
        return program;
    };

    std::vector<std::string> entries = {"xj_dispatch", "jt_materialize"};
    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "cross-join", program);

    auto kernel = EmitKernel("cross-join", entries, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto slot = kernel.Slot;
    using TDispatchFn = bool(*)(TRowSet*, int64_t, TRowSet*, int64_t, void*, int64_t);
    using TMaterializeFn = int64_t(*)(
        void*, TRowSet*, TRowSet*, TRowSet*, TRowSet*, int64_t, int64_t, TRowSet*);

    TCrossJoinKernels kernels;
    kernels.Dispatch = [slot](
        TRowSet* batch,
        int64_t batchIdx,
        TRowSet* rightStore,
        int64_t rightBatchCount,
        void* pairs,
        int64_t op) {
        return reinterpret_cast<TDispatchFn>(slot->Fns[0])(
            batch, batchIdx, rightStore, rightBatchCount, pairs, op);
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
        return reinterpret_cast<TMaterializeFn>(slot->Fns[1])(
            pairs, leftStore, rightStore, streamLeft, streamRight, start, limit, out);
    };
    return kernels;
}

TJoinHashKernels TKernelCompiler::CompileJoinHash(
    const NKernel::TOperatorKernelSpec& spec)
{
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
        *leftType.Cast(),
        *rightType.Cast(),
        keys);

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto stringViewType = QumirDbNamedType("StringView");

    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        std::vector<TExprPtr> program;
        for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        // String keys hash as StringView: the rh_hash overload calls
        // qdb_string_hash from string_ops.oz.
        if (keyDesc.HasDistinctLookupType()) {
            auto stringOps = NKernel::ParseFunctionLibrary(
                NKernel::ReadAggregationKernel("string_ops.oz"));
            if (!stringOps) {
                throw NQumir::TError(
                    "CompileJoinHash: string_ops.oz: " + stringOps.error().ToString());
            }
            for (auto& f : *stringOps) program.push_back(std::move(f));
        }
        for (auto& f : NKernel::GenJoinKeyOpsFunDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        program.push_back(NKernel::GenJoinHashBatchAst(
            keyDesc, "jt_hash_batch", columnType, rowSetType, stringViewType));
        program.push_back(NKernel::GenJoinHashEntrypointAst(
            keyDesc, "jt_hash_left", rowSetType));
        program.push_back(NKernel::GenJoinHashEntrypointAst(
            keyDesc, "jt_hash_right", rowSetType));
        return program;
    };

    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "join_hash", program);

    auto kernel = EmitKernel(
        "join_hash", {"jt_hash_left", "jt_hash_right"}, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto leftKeyColumns = std::make_shared<std::vector<int64_t>>();
    auto rightKeyColumns = std::make_shared<std::vector<int64_t>>();
    leftKeyColumns->reserve(keyDesc.Fields.size());
    rightKeyColumns->reserve(keyDesc.Fields.size());
    for (const auto& field : keyDesc.Fields) {
        leftKeyColumns->push_back(field.LeftColumnIndex);
        rightKeyColumns->push_back(field.RightColumnIndex);
    }

    using THashFn = bool(*)(TRowSet*, uint64_t*, const int64_t*);
    return {
        .Left = [slot = kernel.Slot, keyColumns = std::move(leftKeyColumns)](
                    TRowSet* batch, uint64_t* hashes) {
            return reinterpret_cast<THashFn>(slot->Fns[0])(
                batch, hashes, keyColumns->data());
        },
        .Right = [slot = kernel.Slot, keyColumns = std::move(rightKeyColumns)](
                     TRowSet* batch, uint64_t* hashes) {
            return reinterpret_cast<THashFn>(slot->Fns[1])(
                batch, hashes, keyColumns->data());
        },
    };
}

std::function<bool(TRowSet*, uint64_t*)> TKernelCompiler::CompileAggregateHash(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys)
{
    using namespace NQumir::NAst;

    const auto keyDesc = NKernel::BuildAggregateKeyDescriptor(inputType, groupKeys);

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto stringViewType = QumirDbNamedType("StringView");

    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        std::vector<TExprPtr> program;
        for (auto& f : NKernel::GenKeyTypeDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        if (keyDesc.HasDistinctLookupType()) {
            auto stringOps = NKernel::ParseFunctionLibrary(
                NKernel::ReadAggregationKernel("string_ops.oz"));
            if (!stringOps) {
                throw NQumir::TError(
                    "CompileAggregateHash: string_ops.oz: " + stringOps.error().ToString());
            }
            for (auto& f : *stringOps) program.push_back(std::move(f));
        }
        for (auto& f : NKernel::GenKeyOperationFunDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        program.push_back(NKernel::GenKeyHashBatchAst(
            keyDesc, "agg_hash_batch", columnType, rowSetType, stringViewType));
        program.push_back(NKernel::GenKeyHashEntrypointAst(
            keyDesc, "agg_hash", "agg_hash_batch", rowSetType));
        return program;
    };

    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "aggregate_hash", program);

    auto kernel = EmitKernel("aggregate_hash", {"agg_hash"}, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto keyColumns = std::make_shared<std::vector<int64_t>>();
    keyColumns->reserve(keyDesc.Fields.size());
    for (const auto& field : keyDesc.Fields) {
        keyColumns->push_back(field.ColumnIndex);
    }

    using THashFn = bool(*)(TRowSet*, uint64_t*, const int64_t*);
    return [slot = kernel.Slot, keyColumns = std::move(keyColumns)](
               TRowSet* batch, uint64_t* hashes) {
        return reinterpret_cast<THashFn>(slot->Fns[0])(
            batch, hashes, keyColumns->data());
    };
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
    using namespace NQumir::NAst;

    const bool isSemiAnti = (type == EJoinType::LeftSemi || type == EJoinType::LeftAnti);
    const bool isResidualSemiAnti = isSemiAnti && static_cast<bool>(residualPredicate);
    const bool isOuter =
        (type == EJoinType::Left || type == EJoinType::Right || type == EJoinType::Full);
    if (!isSemiAnti && !isOuter && type != EJoinType::Inner) {
        throw NQumir::TError(
            "CompileJoin: only Inner, Left, Right, Full, LeftSemi, and LeftAnti join types are supported");
    }

    // Unified key descriptor (reuses the aggregation key machinery). Throws on
    // incompatible types / missing columns.
    std::vector<std::pair<std::string, std::string>> keyNames;
    keyNames.reserve(keys.size());
    for (const auto& key : keys) {
        keyNames.emplace_back(key.Left, key.Right);
    }
    auto keyDesc = NKernel::BuildJoinKeyDescriptor(leftType, rightType, keyNames);
    for (size_t i = 0; i < keys.size(); ++i) {
        keyDesc.Fields[i].NullsEqual = keys[i].NullsEqual;
    }
    const int64_t keySize = static_cast<int64_t>(keyDesc.Size);
    auto leftKeyColumns = std::make_shared<std::vector<int64_t>>();
    auto rightKeyColumns = std::make_shared<std::vector<int64_t>>();
    leftKeyColumns->reserve(keyDesc.Fields.size());
    rightKeyColumns->reserve(keyDesc.Fields.size());
    for (const auto& field : keyDesc.Fields) {
        leftKeyColumns->push_back(field.LeftColumnIndex);
        rightKeyColumns->push_back(field.RightColumnIndex);
    }

    auto columnType = QumirDbNamedType("TColumn");
    auto rowSetType = QumirDbNamedType("TRowSet");
    auto hashTableType = QumirDbNamedType("HashTable");
    auto pairBufferType = QumirDbNamedType("PairBuffer");
    auto stringViewType = QumirDbNamedType("StringView");

    // Fresh program per entry (CompileKernelAst consumes the AST): key type
    // decls + key-ops overloads + shared library + generated process functions.
    auto buildProgram = [&]() -> std::vector<TExprPtr> {
        auto library = NKernel::BuildJoinKernelLibrary();
        if (!library) {
            throw NQumir::TError("CompileJoin: " + library.error().ToString());
        }
        // When a residual predicate is given, replace the library's default
        // jt_residual_filter (join_residual_default.oz) with the generated one,
        // keeping its position (before join_update.oz, which calls it).
        if (residualPredicate && innerType) {
            for (auto& f : *library) {
                auto fun = NQumir::NAst::TMaybeNode<NQumir::NAst::TFunDecl>(f);
                if (fun && fun.Cast()->Name == "jt_residual_filter") {
                    f = NKernel::GenJoinResidualFilterAst(
                        residualPredicate, *innerType, leftFieldCount,
                        columnType, rowSetType, stringViewType);
                    break;
                }
            }
        }

        // Program order follows BuildGenericAggregateProgramAst: type decls,
        // string ops (callees of the key-ops overloads), per-key overloads
        // (rh_hash / rh_key_equal / key_owned_bytes / key_clone_owned), the
        // generic dual-key HashTable helpers they instantiate, then the join
        // library and the generated per-query functions.
        const bool includeRight = !isSemiAnti;
        const bool hasStringOutput = [&] {
            auto isString = [](const auto& field) {
                return static_cast<bool>(TMaybeType<TStringType>(
                    UnwrapNamedType(UnwrapNullableType(field.second))));
            };
            return std::any_of(leftType.Fields.begin(), leftType.Fields.end(), isString) ||
                (includeRight && std::any_of(
                    rightType.Fields.begin(), rightType.Fields.end(), isString));
        }();

        std::vector<TExprPtr> program;
        for (auto& f : NKernel::GenJoinKeyTypeDecls(keyDesc)) program.push_back(std::move(f));
        if (hasStringOutput || keyDesc.HasDistinctLookupType()) {
            auto stringOps = NKernel::ParseFunctionLibrary(
                NKernel::ReadAggregationKernel("string_ops.oz"));
            if (!stringOps) {
                throw NQumir::TError(
                    "CompileJoin: string_ops.oz: " + stringOps.error().ToString());
            }
            for (auto& f : *stringOps) program.push_back(std::move(f));
        }
        for (auto& f : NKernel::GenJoinKeyOpsFunDecls(keyDesc)) program.push_back(std::move(f));
        for (auto& f : NKernel::GenJoinKeyOwnershipFunDecls(keyDesc)) {
            program.push_back(std::move(f));
        }
        auto dualKeyLibrary = NKernel::BuildJoinDualKeyLibrary();
        if (!dualKeyLibrary) {
            throw NQumir::TError("CompileJoin: " + dualKeyLibrary.error().ToString());
        }
        for (auto& f : *dualKeyLibrary) program.push_back(std::move(f));
        for (auto& f : *library) program.push_back(std::move(f));
        program.push_back(NKernel::GenJoinProcessAst(keyDesc, /*isLeft=*/true,
            "jt_process_left", columnType, rowSetType, hashTableType, pairBufferType,
            stringViewType, hasPrecomputedHash));
        program.push_back(NKernel::GenJoinProcessAst(keyDesc, /*isLeft=*/false,
            "jt_process_right", columnType, rowSetType, hashTableType, pairBufferType,
            stringViewType, hasPrecomputedHash));
        program.push_back(NKernel::GenJoinProbeAst(keyDesc, /*isLeft=*/true,
            "jt_probe_left_stream", columnType, rowSetType, hashTableType, pairBufferType,
            stringViewType, hasPrecomputedHash));
        program.push_back(NKernel::GenJoinProbeAst(keyDesc, /*isLeft=*/false,
            "jt_probe_right_stream", columnType, rowSetType, hashTableType, pairBufferType,
            stringViewType, hasPrecomputedHash));
        if (isResidualSemiAnti) {
            program.push_back(NKernel::GenJoinInsertRowsOnlyAst(
                keyDesc, /*isLeft=*/true, "jt_insert_left_only",
                columnType, rowSetType, hashTableType, pairBufferType,
                stringViewType, hasPrecomputedHash));
            program.push_back(NKernel::GenJoinProbeMarkAst(
                keyDesc, /*isLeft=*/false, "jt_probe_right_mark",
                columnType, rowSetType, hashTableType, pairBufferType,
                stringViewType, hasPrecomputedHash));
        }
        if (isSemiAnti && !isResidualSemiAnti) {
            program.push_back(NKernel::GenJoinInsertKeyOnlyAst(
                keyDesc, "jt_insert_key_only",
                columnType, rowSetType, hashTableType, pairBufferType,
                stringViewType));
            program.push_back(NKernel::GenJoinFinalizeSemiAntiAst(
                keyDesc, /*isAnti=*/type == EJoinType::LeftAnti,
                "jt_finalize_semi_anti", hashTableType, pairBufferType));
            program.push_back(NKernel::GenJoinProbeSemiAst(
                keyDesc, /*isAnti=*/type == EJoinType::LeftAnti,
                "jt_probe_left_semi", columnType, rowSetType, hashTableType,
                pairBufferType, stringViewType, hasPrecomputedHash));
        }
        if (isOuter) {
            // jt_finalize_outer: same logic as ANTI (emit own rows with no opp match)
            program.push_back(NKernel::GenJoinFinalizeSemiAntiAst(
                keyDesc, /*isAnti=*/true,
                "jt_finalize_outer", hashTableType, pairBufferType));
        }
        program.push_back(NKernel::GenJoinDispatchAst(
            keySize, type, isResidualSemiAnti,
            rowSetType, hashTableType, pairBufferType));
        // Output materializer: semi/anti joins emit only the left columns.
        program.push_back(NKernel::GenJoinMaterializeAst(
            leftType, rightType, includeRight,
            columnType, rowSetType, pairBufferType, stringViewType));
        return program;
    };

    std::vector<std::string> entries = {"jt_dispatch", "jt_materialize"};

    auto program = std::make_shared<TBlockExpr>(NQumir::TLocation{}, buildProgram());
    PrintKernelAst(Diagnostics_, "join", program);

    auto kernel = EmitKernel("join", entries, std::move(program));
    FinishKernelDiagnostics(Diagnostics_);

    auto slot = kernel.Slot;
    using TDispatchFn = bool(*)(
        void*, void*, TRowSet*, int64_t, void*, TRowSet*, TRowSet*,
        const int64_t*, const int64_t*, int64_t, int64_t);
    using TMaterializeFn = int64_t(*)(
        void*, TRowSet*, TRowSet*, TRowSet*, TRowSet*, int64_t, int64_t, TRowSet*);

    TJoinKernels kernels;
    kernels.Dispatch = [slot,
                        leftKeyColumns = std::move(leftKeyColumns),
                        rightKeyColumns = std::move(rightKeyColumns)](
        void* left,
        void* right,
        TRowSet* batch,
        int64_t batchIdx,
        void* pairs,
        TRowSet* leftStore,
        TRowSet* rightStore,
        int64_t arg,
        int64_t op) {
        return reinterpret_cast<TDispatchFn>(slot->Fns[0])(
            left, right, batch, batchIdx, pairs, leftStore, rightStore,
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
        return reinterpret_cast<TMaterializeFn>(slot->Fns[1])(
            pairs, leftStore, rightStore, streamLeft, streamRight, start, limit, out);
    };
    return kernels;
}

} // namespace NQdb
