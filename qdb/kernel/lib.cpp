#include <qdb/kernel/lib.h>

#include <qdb/kernel/gen.h>
#include <qdb/kernel/query_modules.h>
#include <qdb/plan/types/date.h>
#include <qdb/utils/module_path.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace NQdb {
namespace NKernel {

std::string ReadKernel(const std::string& kind, const std::string& name) {
    auto path = NUtils::KernelFile(kind, name);
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open kernel library: " + path);
    }
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

std::string ReadAggregationKernel(const std::string& name) {
    return ReadKernel("aggregation", name);
}

std::string ReadSortKernel(const std::string& name) {
    return ReadKernel("sort", name);
}

std::string ReadRowTableKernel() {
    return ReadKernel(".", "row_table.oz");
}

namespace {

bool HasStringReducer(const TAggReducerLayout& layout) {
    return std::ranges::any_of(
        layout.Reducers, [](const auto& reducer) { return reducer.IsString(); });
}

std::optional<NQumir::TError> AddParsedKernel(
    TQueryModules& modules,
    std::vector<NQumir::NAst::TExprPtr>& library,
    const std::string& kind,
    const std::string& name,
    const std::unordered_set<std::string>& exclude = {})
{
    auto parsed = modules.KernelLibrary(kind, name, exclude);
    if (!parsed) {
        return parsed.error();
    }
    for (auto& fn : *parsed) {
        library.push_back(std::move(fn));
    }
    return std::nullopt;
}

// The row table (kernel/row_table.oz) and the row layout of the aggregate table
// over it, for an aggregate program with a row layout.
std::optional<NQumir::TError> AddRowLayoutLibrary(
    TQueryModules& modules,
    std::vector<NQumir::NAst::TExprPtr>& library)
{
    if (auto error = AddParsedKernel(modules, library, ".", "row_table.oz")) {
        return error;
    }
    return AddParsedKernel(modules, library, "aggregation", "aggregation_rows.oz");
}

std::optional<NQumir::TError> AddStringReducerLibrary(
    TQueryModules& modules,
    std::vector<NQumir::NAst::TExprPtr>& library,
    const TAggReducerLayout& layout)
{
    if (!HasStringReducer(layout)) {
        return std::nullopt;
    }
    return AddParsedKernel(modules, library, "aggregation", "aggregation_string_reducer.oz");
}

// Shared join helpers. Teardown belongs to the allocation domain; only
// working allocation/growth helpers are needed in these programs.
std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
BuildJoinLibraryBase(TQueryModules& modules) {
    std::vector<NQumir::NAst::TExprPtr> library;
    if (auto e = AddParsedKernel(modules, library, "aggregation", "swiss_group.oz")) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, library, "aggregation", "key_ops_i64.oz")) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, library, "aggregation", "robin_hood_rehash_generic.oz")) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, library, "aggregation", "aggregation_hashtable_generic.oz",
            {"aht_update", "aht_destroy", "aht_destroy_slots"})) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, library, "join", "join_table.oz")) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, library, "join", "materialize.oz")) {
        return std::unexpected(*e);
    }
    return library;
}

} // namespace

std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
BuildJoinKernelLibrary(TQueryModules& modules) {
    auto library = BuildJoinLibraryBase(modules);
    if (!library) {
        return library;
    }
    // Default always-true residual filter - must precede join_update.oz and
    // join_semi_anti.oz, whose probe helpers call jt_residual_filter.
    // CompileJoin replaces this with a generated predicate when the query has
    // a residual filter.
    if (auto e = AddParsedKernel(modules, *library, "join", "join_residual_default.oz")) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, *library, "join", "join_update.oz")) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, *library, "join", "join_semi_anti.oz")) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, *library, "join", "join_mark.oz")) {
        return std::unexpected(*e);
    }
    return library;
}

std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
BuildCrossJoinKernelLibrary(TQueryModules& modules) {
    auto library = BuildJoinLibraryBase(modules);
    if (!library) {
        return library;
    }
    // Default always-true residual filter, called by join_cross.oz. Inner
    // keeps it (its residual filters the product downstream); CompileCrossJoin
    // replaces it with the residual that decides matches of the other types.
    if (auto e = AddParsedKernel(modules, *library, "join", "join_residual_default.oz")) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, *library, "join", "join_cross.oz")) {
        return std::unexpected(*e);
    }
    return library;
}

std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
BuildRowJoinKernelLibrary(TQueryModules& modules) {
    auto library = BuildJoinLibraryBase(modules);
    if (!library) {
        return library;
    }
    if (auto e = AddParsedKernel(modules, *library, ".", "row_table.oz", {"ra_destroy", "rt_destroy"})) {
        return std::unexpected(*e);
    }
    if (auto e = AddParsedKernel(modules, *library, "join", "join_rows.oz")) {
        return std::unexpected(*e);
    }
    return library;
}

std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
BuildJoinDualKeyLibrary(TQueryModules& modules) {
    std::vector<NQumir::NAst::TExprPtr> library;
    for (const char* name : {"owned_blocks.oz", "robin_hood_dual_key.oz"}) {
        auto parsed = modules.KernelLibrary("aggregation", name);
        if (!parsed) {
            return std::unexpected(NQumir::TError(
                std::string(name) + ": " + parsed.error().ToString()));
        }
        for (auto& fn : *parsed) {
            library.push_back(std::move(fn));
        }
    }
    return library;
}

std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
ParseFunctionLibrary(
    NQumir::NAst::TTypeFactory& types,
    const std::string& source,
    const std::unordered_set<std::string>& exclude)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    std::istringstream input(source);
    NCore::TTokenStream tokens(input);
    NCore::TParser parser(types);

    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    auto block = TMaybeNode<TBlockExpr>(*parsed);
    if (!block) {
        return std::unexpected(TError("kernel library: expected top-level (block ...)"));
    }

    // Per-query override points that must resolve by bare name in every query
    // (a cached generic calls them by name); never mark these cacheable.
    static const std::unordered_set<std::string> KeepBare = {"jt_residual_filter"};

    std::vector<TExprPtr> result;
    for (auto& stmt : block.Cast()->Stmts) {
        auto fun = TMaybeNode<TFunDecl>(stmt);
        if (fun) {
            if (exclude.contains(fun.Cast()->Name)) {
                continue;
            }
            // Library functions are stable across compilations, so cache them
            // (generics are already content-addressed by their instance name).
            if (fun.Cast()->GenericParams.empty() && !KeepBare.contains(fun.Cast()->Name)) {
                fun.Cast()->Cacheable = true;
            }
        }
        result.push_back(std::move(stmt));
    }
    return result;
}

NQumir::NAst::TExprPtr MergeKernelLibrary(
    std::vector<NQumir::NAst::TExprPtr> library,
    NQumir::NAst::TExprPtr entry)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    library.push_back(std::move(entry));
    return NQumir::MakeIntrusive<TBlockExpr>(TLocation{}, std::move(library));
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildFilterProgramAst(
    NQumir::NAst::TTypeFactory& types,
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType)
{
    using namespace NQumir::NAst;
    auto entry = GenFilterKernelAst(
        types, std::move(predicate), inputType, fieldIndices, columnType, rowSetType,
        stringViewType);
    auto block = TMaybeNode<TBlockExpr>(entry);
    if (!block) {
        return std::unexpected(NQumir::TError(
            "filter kernel generator returned a non-block program"));
    }

    const bool hasString = std::any_of(
        inputType.Fields.begin(), inputType.Fields.end(),
        [](const auto& field) {
            return TMaybeType<TStringType>(UnwrapNamedValueType(field.second));
        });
    if (!hasString) {
        return entry;
    }
    if (!stringViewType) {
        return std::unexpected(NQumir::TError(
            "filter kernel requires the StringView external type"));
    }

    return entry;
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateProgramAst(
    TQueryModules& modules,
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType)
{
    auto& types = modules.Types();
    std::vector<NQumir::NAst::TExprPtr> stmts;
    if (!key.LookupTypeName.empty()) {
        stmts.push_back(NQumir::MakeIntrusive<NQumir::NAst::TTypeDeclStmt>(
            NQumir::TLocation{}, key.LookupType));
    }
    if (!key.StoredTypeName.empty()) {
        stmts.push_back(NQumir::MakeIntrusive<NQumir::NAst::TTypeDeclStmt>(
            NQumir::TLocation{}, key.StoredType));
    } else if (NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.KeyType)) {
        stmts.push_back(NQumir::MakeIntrusive<NQumir::NAst::TTypeDeclStmt>(
            NQumir::TLocation{}, key.KeyType));
    }

    if (key.HasDistinctLookupType()) {
        auto stringOperations = modules.KernelLibrary("aggregation", "string_ops.oz");
        if (!stringOperations) {
            return std::unexpected(NQumir::TError(
                "string_ops.oz: " + stringOperations.error().ToString()));
        }
        stmts.insert(
            stmts.end(), stringOperations->begin(), stringOperations->end());
    }
    if (auto error = AddStringReducerLibrary(modules, stmts, layout)) {
        return std::unexpected(*error);
    }
    auto keyOperations = GenKeyOperationFunDecls(types, key);
    stmts.insert(stmts.end(), keyOperations.begin(), keyOperations.end());
    auto ownership = GenKeyOwnershipFunDecls(types, key);
    stmts.insert(stmts.end(), ownership.begin(), ownership.end());
    auto reducerDecls = GenReducerFunDecls(types, layout);
    stmts.insert(stmts.end(), reducerDecls.begin(), reducerDecls.end());
    stmts.push_back(GenApplyReducersFunDecl(types, layout));

    auto swissGroup = modules.KernelLibrary("aggregation", "swiss_group.oz");
    if (!swissGroup) {
        return std::unexpected(NQumir::TError(
            "swiss_group.oz: " + swissGroup.error().ToString()));
    }
    stmts.insert(stmts.end(), swissGroup->begin(), swissGroup->end());

    auto arenaLifecycle = modules.KernelLibrary("aggregation", "owned_arena_lifecycle.oz");
    if (!arenaLifecycle) {
        return std::unexpected(NQumir::TError(
            "owned_arena_lifecycle.oz: " +
            arenaLifecycle.error().ToString()));
    }
    stmts.insert(stmts.end(), arenaLifecycle->begin(), arenaLifecycle->end());

    auto lifecycle = modules.KernelLibrary(
        "aggregation", "aggregation_hashtable_generic.oz",
        {"rh_lookup_slot", "rh_insert_displace", "aht_rehash", "aht_update"});
    if (!lifecycle) {
        return std::unexpected(NQumir::TError(
            "aggregation_hashtable_generic.oz: " +
            lifecycle.error().ToString()));
    }
    stmts.insert(stmts.end(), lifecycle->begin(), lifecycle->end());

    for (const char* name : {"owned_blocks.oz", "robin_hood_dual_key.oz"}) {
        auto parsed = modules.KernelLibrary("aggregation", name);
        if (!parsed) {
            return std::unexpected(NQumir::TError(
                std::string(name) + ": " + parsed.error().ToString()));
        }
        stmts.insert(stmts.end(), parsed->begin(), parsed->end());
    }

    auto entry = GenGenericAggregateDispatchAst(
        types, inputType, key, layout, std::move(columnType),
        std::move(rowSetType), std::move(hashTableType));
    auto block = NQumir::NAst::TMaybeNode<NQumir::NAst::TBlockExpr>(entry);
    if (!block || block.Cast()->Stmts.size() != 1) {
        return std::unexpected(NQumir::TError(
            "generic aggregate dispatch generator returned an invalid entry block"));
    }
    stmts.push_back(block.Cast()->Stmts.front());
    return NQumir::MakeIntrusive<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(stmts));
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateFinalizeProgramAst(
    TQueryModules& modules,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType)
{
    auto& types = modules.Types();
    auto parsed = modules.KernelLibrary("aggregation", "aggregation_finalize_states.oz");
    if (!parsed) {
        return std::unexpected(NQumir::TError(
            "aggregation_finalize_states.oz: " + parsed.error().ToString()));
    }
    if (key.HasDistinctLookupType()) {
        auto stringOperations = modules.KernelLibrary("aggregation", "string_ops.oz");
        if (!stringOperations) {
            return std::unexpected(NQumir::TError(
                "string_ops.oz: " + stringOperations.error().ToString()));
        }
        parsed->insert(parsed->end(),
            stringOperations->begin(), stringOperations->end());
    }
    if (auto error = AddStringReducerLibrary(modules, *parsed, layout)) {
        return std::unexpected(*error);
    }
    auto entry = GenGenericAggregateFinalizeAst(
        types, key, layout, std::move(hashTableType), std::move(columnType));
    auto block = NQumir::NAst::TMaybeNode<NQumir::NAst::TBlockExpr>(entry);
    if (!block || block.Cast()->Stmts.size() != 1) {
        return std::unexpected(NQumir::TError(
            "generic aggregate finalize generator returned an invalid entry block"));
    }
    if (!key.StoredTypeName.empty() ||
        NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.KeyType)) {
        parsed->insert(parsed->begin(),
            NQumir::MakeIntrusive<NQumir::NAst::TTypeDeclStmt>(
                NQumir::TLocation{}, key.KeyType));
    }
    parsed->push_back(block.Cast()->Stmts.front());
    return NQumir::MakeIntrusive<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*parsed));
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateFusedProgramAst(
    TQueryModules& modules,
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    bool hasPrecomputedHash,
    const std::vector<int64_t>& emptyInputGroups,
    const std::optional<TAggRowLayout>& rows)
{
    using namespace NQumir::NAst;
    auto& types = modules.Types();

    std::vector<TExprPtr> stmts;
    if (!key.LookupTypeName.empty()) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            NQumir::TLocation{}, key.LookupType));
    }
    if (!key.StoredTypeName.empty()) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            NQumir::TLocation{}, key.StoredType));
    } else if (TMaybeType<TNamedType>(key.KeyType)) {
        stmts.push_back(NQumir::MakeIntrusive<TTypeDeclStmt>(
            NQumir::TLocation{}, key.KeyType));
    }

    if (key.HasDistinctLookupType()) {
        auto stringOperations = modules.KernelLibrary("aggregation", "string_ops.oz");
        if (!stringOperations) {
            return std::unexpected(NQumir::TError(
                "string_ops.oz: " + stringOperations.error().ToString()));
        }
        stmts.insert(
            stmts.end(), stringOperations->begin(), stringOperations->end());
    }
    if (auto error = AddStringReducerLibrary(modules, stmts, layout)) {
        return std::unexpected(*error);
    }
    auto keyOperations = GenKeyOperationFunDecls(types, key);
    stmts.insert(stmts.end(), keyOperations.begin(), keyOperations.end());
    auto ownership = GenKeyOwnershipFunDecls(types, key);
    stmts.insert(stmts.end(), ownership.begin(), ownership.end());
    auto reducerDecls = GenReducerFunDecls(types, layout);
    stmts.insert(stmts.end(), reducerDecls.begin(), reducerDecls.end());
    stmts.push_back(GenApplyReducersFunDecl(types, layout));

    auto swissGroup = modules.KernelLibrary("aggregation", "swiss_group.oz");
    if (!swissGroup) {
        return std::unexpected(NQumir::TError(
            "swiss_group.oz: " + swissGroup.error().ToString()));
    }
    stmts.insert(stmts.end(), swissGroup->begin(), swissGroup->end());

    auto arenaLifecycle = modules.KernelLibrary("aggregation", "owned_arena_lifecycle.oz");
    if (!arenaLifecycle) {
        return std::unexpected(NQumir::TError(
            "owned_arena_lifecycle.oz: " +
            arenaLifecycle.error().ToString()));
    }
    stmts.insert(stmts.end(), arenaLifecycle->begin(), arenaLifecycle->end());

    auto lifecycle = modules.KernelLibrary(
        "aggregation", "aggregation_hashtable_generic.oz",
        {"rh_lookup_slot", "rh_insert_displace", "aht_rehash", "aht_update"});
    if (!lifecycle) {
        return std::unexpected(NQumir::TError(
            "aggregation_hashtable_generic.oz: " +
            lifecycle.error().ToString()));
    }
    stmts.insert(stmts.end(), lifecycle->begin(), lifecycle->end());

    for (const char* name :
         {"owned_blocks.oz", "robin_hood_dual_key.oz",
          "aggregation_finalize_states.oz"}) {
        auto parsed = modules.KernelLibrary("aggregation", name);
        if (!parsed) {
            return std::unexpected(NQumir::TError(
                std::string(name) + ": " + parsed.error().ToString()));
        }
        stmts.insert(stmts.end(), parsed->begin(), parsed->end());
    }
    if (rows) {
        if (auto error = AddRowLayoutLibrary(modules, stmts)) {
            return std::unexpected(*error);
        }
    }

    auto appendEntry = [&](TExprPtr entry,
                           const char* what) -> std::optional<NQumir::TError> {
        auto block = TMaybeNode<TBlockExpr>(entry);
        if (!block || block.Cast()->Stmts.size() != 1) {
            return NQumir::TError(
                std::string(what) + " generator returned an invalid entry block");
        }
        stmts.push_back(block.Cast()->Stmts.front());
        return std::nullopt;
    };
    // The row layout's update precedes the dispatch that calls it.
    if (rows) {
        if (auto err = appendEntry(
                GenGenericAggregateRowsUpdateAst(
                    types, inputType, key, layout, *rows, columnType, rowSetType, hashTableType,
                    hasPrecomputedHash),
                "row layout aggregate update")) {
            return std::unexpected(*err);
        }
    }
    if (auto err = appendEntry(
            GenGenericAggregateDispatchAst(
                types, inputType, key, layout, columnType, rowSetType, hashTableType,
                hasPrecomputedHash, emptyInputGroups, rows),
            "generic aggregate dispatch")) {
        return std::unexpected(*err);
    }
    if (auto err = appendEntry(
            GenGenericAggregateMeasureAst(types, key, layout, hashTableType, rows),
            "generic aggregate measure")) {
        return std::unexpected(*err);
    }
    if (auto err = appendEntry(
            GenGenericAggregateFinalizeAst(types, key, layout, hashTableType, columnType, rows),
            "generic aggregate finalize")) {
        return std::unexpected(*err);
    }
    if (auto err = appendEntry(
            GenGenericAggregateFinishRowSetAst(
                types, key, layout, hashTableType, columnType, rowSetType, rows.has_value()),
            "generic aggregate finish rowset")) {
        return std::unexpected(*err);
    }

    return NQumir::MakeIntrusive<TBlockExpr>(NQumir::TLocation{}, std::move(stmts));
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateMeasureProgramAst(
    TQueryModules& modules,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType)
{
    auto& types = modules.Types();
    std::vector<NQumir::NAst::TExprPtr> stmts;
    if (auto error = AddStringReducerLibrary(modules, stmts, layout)) {
        return std::unexpected(*error);
    }
    if (!key.StoredTypeName.empty() ||
        NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.StoredType)) {
        stmts.push_back(NQumir::MakeIntrusive<NQumir::NAst::TTypeDeclStmt>(
            NQumir::TLocation{}, key.StoredType));
    }
    auto entry = GenGenericAggregateMeasureAst(
        types, key, layout, std::move(hashTableType));
    auto block = NQumir::NAst::TMaybeNode<NQumir::NAst::TBlockExpr>(entry);
    if (!block || block.Cast()->Stmts.size() != 1) {
        return std::unexpected(NQumir::TError(
            "generic aggregate measure generator returned an invalid entry block"));
    }
    stmts.push_back(block.Cast()->Stmts.front());
    return NQumir::MakeIntrusive<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(stmts));
}

} // namespace NKernel
} // namespace NQdb
