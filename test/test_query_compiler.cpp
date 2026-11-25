#include "mock_source.h"

#include <qdb/compiler/query.h>
#include <qdb/io/parquet/row_group_predicate.h>
#include <qdb/kernel/finalize.h>
#include <qdb/kernel/query_modules.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/plan/ops/source.h>
#include <qdb/scheduler/lowering/plan_lowerer.h>
#include <qdb/utils/module_path.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <sstream>
#include <vector>

using namespace NQdb;
using namespace NQdb::NCompiler;

namespace {

struct TFixture {
    TMockSource Source{{"value"}};
    TExternalModuleCatalog Catalog;
    std::vector<std::string> Tables;
    TTableSourceFactory Sources = [&](NQumir::NAst::TTypeFactory& types, std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        Tables.emplace_back(table);
        return NQumir::MakeIntrusive<TSourceOperator>(types, Borrow(Source), std::string(table));
    };
    // Parses and prepares with one factory, as a query does.
    TQueryCompiler Compiler{TestModuleBase(), Catalog, Sources};

    std::expected<TParsedQuery, NQumir::TError> Parse(
        const std::string& text,
        ESyntax syntax = ESyntax::Sql)
    {
        std::istringstream input(text);
        return Compiler.Parse(input, syntax);
    }
};

using NQumir::NAst::TTypePtr;
using NQumir::NAst::TNamedType;

void CollectNamedTypes(TTypePtr type, std::set<const TNamedType*>& out) {
    using namespace NQumir::NAst;
    if (!type) {
        return;
    }
    if (auto named = TMaybeType<TNamedType>(type)) {
        if (!out.insert(named.Cast()).second) {
            return;
        }
        CollectNamedTypes(named.Cast()->UnderlyingType(), out);
        for (const auto& arg : named.Cast()->TypeArgs) {
            CollectNamedTypes(arg.Type, out);
        }
    } else if (auto pointer = TMaybeType<TPointerType>(type)) {
        CollectNamedTypes(pointer.Cast()->PointeeType, out);
    } else if (auto reference = TMaybeType<TReferenceType>(type)) {
        CollectNamedTypes(reference.Cast()->ReferencedType, out);
    } else if (auto array = TMaybeType<TArrayType>(type)) {
        CollectNamedTypes(array.Cast()->ElementType, out);
    } else if (auto structure = TMaybeType<TStructType>(type)) {
        for (const auto& [name, field] : structure.Cast()->Fields) {
            CollectNamedTypes(field, out);
        }
    } else if (auto function = TMaybeType<TFunctionType>(type)) {
        for (const auto& param : function.Cast()->ParamTypes) {
            CollectNamedTypes(param, out);
        }
        CollectNamedTypes(function.Cast()->ReturnType, out);
    }
}

// A node of an AST with what name resolution would write into it.
struct TNodeState {
    const NQumir::NAst::TExpr* Node = nullptr;
    TTypePtr Type = nullptr;
    TTypePtr RetType = nullptr;
    int32_t Scope = -1;

    bool operator==(const TNodeState&) const = default;
};

std::vector<TNodeState> NodeStates(const NQumir::NAst::TExprPtr& node) {
    std::vector<TNodeState> states;
    std::vector<NQumir::NAst::TExprPtr> stack{node};
    while (!stack.empty()) {
        auto current = std::move(stack.back());
        stack.pop_back();
        if (!current) {
            continue;
        }
        TNodeState state{.Node = current.Get(), .Type = current->Type};
        if (auto fun = NQumir::NAst::TMaybeNode<NQumir::NAst::TFunDecl>(current)) {
            state.RetType = fun.Cast()->RetType;
            state.Scope = fun.Cast()->Scope;
        }
        states.push_back(state);
        for (auto& child : current->MutableChildren()) {
            stack.push_back(child);
        }
    }
    return states;
}

// Every named type that the AST of `node` mentions.
void CollectNamedTypes(const NQumir::NAst::TExprPtr& node, std::set<const TNamedType*>& out) {
    if (!node) {
        return;
    }
    CollectNamedTypes(node->Type, out);
    if (auto fun = NQumir::NAst::TMaybeNode<NQumir::NAst::TFunDecl>(node)) {
        CollectNamedTypes(fun.Cast()->RetType, out);
    }
    for (auto& child : node->MutableChildren()) {
        CollectNamedTypes(child, out);
    }
}

} // namespace

TEST(QueryCompiler, SqlPreparationAndExplain) {
    TFixture fixture;
    auto query = fixture.Parse("  ExPlAiN SELECT value FROM items WHERE value > 1;");
    ASSERT_TRUE(query) << query.error().ToString();
    ASSERT_TRUE(query->Explain);
    ASSERT_EQ(query->Statements.size(), 1);
    EXPECT_TRUE(fixture.Tables.empty());
    auto& compiler = fixture.Compiler;
    auto prepared = compiler.Prepare(query->Statements[0], query->Explain);
    ASSERT_TRUE(prepared) << prepared.error().ToString();
    ASSERT_TRUE(*prepared);
    EXPECT_EQ(fixture.Tables, std::vector<std::string>{"items"});
    EXPECT_NE((**prepared).Annotation.ExternalCatalog, nullptr);
    std::ostringstream text;
    Explain(text, (**prepared).Plan, EExplainMode::Text);
    EXPECT_FALSE(text.str().empty());
    std::ostringstream sexpr;
    Explain(sexpr, (**prepared).Plan, EExplainMode::Sexpr);
    EXPECT_NE(sexpr.str().find("(rel"), std::string::npos);
    std::ostringstream both;
    Explain(both, (**prepared).Plan, EExplainMode::Both);
    EXPECT_EQ(both.str(), sexpr.str() + "\n" + text.str());
}

TEST(QueryCompiler, ExplainRejectsBatchAndDeclarations) {
    TFixture fixture;
    EXPECT_FALSE(fixture.Parse("EXPLAIN SELECT 1; SELECT 2;"));
    auto query = fixture.Parse("EXPLAIN CREATE MODULE math LANGUAGE rust AS $$$$;");
    ASSERT_TRUE(query) << query.error().ToString();
    auto& compiler = fixture.Compiler;
    auto prepared = compiler.Prepare(query->Statements[0], query->Explain);
    ASSERT_FALSE(prepared);
    EXPECT_NE(prepared.error().ToString().find("EXPLAIN requires a SELECT"), std::string::npos);
    EXPECT_TRUE(fixture.Catalog.Snapshot(SharedTestTypes())->ModuleNames().empty());
}

TEST(QueryCompiler, DeclarationsAreAppliedBeforeQueries) {
    TFixture fixture;
    auto query = fixture.Parse(
        "CREATE MODULE math LANGUAGE rust AS $$$$;"
        "CREATE FUNCTION convert(DOUBLE) RETURNS DOUBLE SET MODULE TO math;"
        "SELECT convert(1.0) FROM items;");
    ASSERT_TRUE(query) << query.error().ToString();
    ASSERT_EQ(query->Statements.size(), 3);
    auto& compiler = fixture.Compiler;
    for (size_t i = 0; i < query->Statements.size(); ++i) {
        auto prepared = compiler.Prepare(query->Statements[i]);
        ASSERT_TRUE(prepared) << prepared.error().ToString();
        EXPECT_EQ(prepared->has_value(), i == 2);
        if (*prepared) {
            EXPECT_TRUE((**prepared).Annotation.ExternalCatalog->HasFunction("convert"));
        }
    }
}

// Every query loads qumirdb.oz from the process's frozen module base: queries share its
// AST, which compiling a query leaves as it is (the same nodes, types and scopes), and a
// base type is one object in all.
TEST(QueryCompiler, QueriesShareTheModuleBase) {
    const auto file = NUtils::ModuleFile("qumirdb.oz");
    const auto& base = **TestModuleBase()->Find(file);
    const auto states = NodeStates(base.Ast);
    std::vector<NQumir::NAst::TTypePtr> rowSets;
    for (int i = 0; i < 2; ++i) {
        TFixture fixture;
        auto query = fixture.Parse("SELECT value + 1 FROM items WHERE value > 1 + 1;");
        ASSERT_TRUE(query) << query.error().ToString();
        auto prepared = fixture.Compiler.Prepare(query->Statements[0]);
        ASSERT_TRUE(prepared) << prepared.error().ToString();
        ASSERT_TRUE(*prepared);
        auto& annotation = (**prepared).Annotation;
        NQumir::NFrontend::TSourceModuleLoader loader(annotation.Modules->Types());
        ASSERT_TRUE(annotation.Modules->Load(loader, file));
        EXPECT_EQ(loader.Find("qumirdb")->Ast.Get(), base.Ast.Get());

        auto lowered = NScheduler::LowerPlanToGraph(
            (**prepared).Plan, (**prepared).Types, {}, nullptr, annotation);
        EXPECT_EQ(lowered.Annotation.Modules, annotation.Modules);
        FinalizeKernels(lowered.Kernels, lowered.Types, lowered.Annotation, EKernelBackend::Llvm);
        EXPECT_FALSE(lowered.Kernels.empty());
        rowSets.push_back(NQumir::NRegistry::QumirDbNamedType(*(**prepared).Types, "TRowSet"));
    }
    EXPECT_EQ(rowSets[0], rowSets[1]);
    EXPECT_TRUE(NodeStates(base.Ast) == states);
}

// The base parses each kernel library once and every kernel gets a clone, so compiling the
// kernels leaves the parse unresolved for the next one.
TEST(QueryCompiler, KernelsCompileClonesOfTheLibraries) {
    TFixture fixture;
    auto query = fixture.Parse("SELECT value, count(*) FROM items GROUP BY value;");
    ASSERT_TRUE(query) << query.error().ToString();
    auto prepared = fixture.Compiler.Prepare(query->Statements[0]);
    ASSERT_TRUE(prepared) << prepared.error().ToString();
    ASSERT_TRUE(*prepared);
    const auto& annotation = (**prepared).Annotation;
    auto lowered = NScheduler::LowerPlanToGraph(
        (**prepared).Plan, (**prepared).Types, {}, nullptr, annotation);
    FinalizeKernels(lowered.Kernels, lowered.Types, lowered.Annotation, EKernelBackend::Llvm);

    auto all = annotation.Modules->KernelLibrary("aggregation", "swiss_group.oz");
    auto some = annotation.Modules->KernelLibrary("aggregation", "swiss_group.oz", {"swiss_group_width"});
    ASSERT_TRUE(all) << all.error().ToString();
    ASSERT_TRUE(some) << some.error().ToString();
    ASSERT_EQ(some->size() + 1, all->size());
    EXPECT_NE(all->front().Get(), some->front().Get());
    for (const auto& stmt : *all) {
        for (const auto& state : NodeStates(stmt)) {
            EXPECT_EQ(state.Scope, -1);
        }
    }
}

// Every query clones the base's parse of a kernel library instead of parsing it again.
// runtime_filter.oz declares types of its own, and a parse makes them in the factory it parses
// into: two queries, each with its own factory, get the same type objects from one parse.
TEST(QueryCompiler, QueriesCloneOneParseOfALibrary) {
    TFixture first;
    TFixture second;
    std::vector<std::vector<NQumir::NAst::TExprPtr>> libraries;
    for (auto* fixture : {&first, &second}) {
        auto query = fixture->Parse("SELECT value, count(*) FROM items GROUP BY value;");
        ASSERT_TRUE(query) << query.error().ToString();
        auto prepared = fixture->Compiler.Prepare(query->Statements[0]);
        ASSERT_TRUE(prepared) << prepared.error().ToString();
        ASSERT_TRUE(*prepared);
        auto library = (**prepared).Annotation.Modules->KernelLibrary("join", "runtime_filter.oz");
        ASSERT_TRUE(library) << library.error().ToString();
        libraries.push_back(std::move(*library));
    }
    const auto& a = libraries[0];
    const auto& b = libraries[1];
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_NE(a[i].Get(), b[i].Get()) << i;
        EXPECT_EQ(a[i]->Type, b[i]->Type) << i;
    }
}

TEST(QueryCompiler, ModuleOutsideTheBaseIsAnError) {
    auto found = TestModuleBase()->Find(NUtils::ModuleFile("other.oz"));
    ASSERT_FALSE(found);
    EXPECT_NE(found.error().ToString().find("is not a module of the base"), std::string::npos);
}

// Compiling kernels and row-group pruning resolves the base modules in a query's clone.
// The query must find every named type the modules mention in the base, and never
// make a copy of its own under the same key, which would split the type in two.
TEST(QueryCompiler, QueriesMakeNoCopiesOfBaseTypes) {
    TFixture fixture;
    auto query = fixture.Parse("SELECT value + 1 FROM items WHERE value > 1 + 1;");
    ASSERT_TRUE(query) << query.error().ToString();
    auto prepared = fixture.Compiler.Prepare(query->Statements[0]);
    ASSERT_TRUE(prepared) << prepared.error().ToString();
    ASSERT_TRUE(*prepared);
    auto& types = *(**prepared).Types;
    const auto& annotation = (**prepared).Annotation;
    auto lowered = NScheduler::LowerPlanToGraph(
        (**prepared).Plan, (**prepared).Types, {}, nullptr, annotation);
    FinalizeKernels(lowered.Kernels, lowered.Types, lowered.Annotation, EKernelBackend::Llvm);

    const auto i64 = types.MakeInteger();
    const TSchema schema{{{.Name = "value", .Type = i64}}};
    NQumir::TLocation loc{};
    auto column = NQumir::MakeIntrusive<NQumir::NAst::TIdentExpr>(loc, "value");
    column->Type = i64;
    auto predicate = NQumir::MakeIntrusive<NQumir::NAst::TBinaryExpr>(
        loc, NQumir::NAst::TOperator(">"), column, NQumir::MakeIntrusive<NQumir::NAst::TNumberExpr>(loc, int64_t{10}, types));
    predicate->Type = types.Make<NQumir::NAst::TBoolType>();
    ASSERT_TRUE(TRowGroupPredicateEvaluator::Compile(types, predicate, schema, "", annotation));

    std::set<const TNamedType*> named;
    for (const auto* name : {"qumirdb.oz", "row_group_interval.oz"}) {
        CollectNamedTypes((*TestModuleBase()->Find(NUtils::ModuleFile(name)))->Ast, named);
    }
    ASSERT_FALSE(named.empty());
    for (const auto* type : named) {
        EXPECT_EQ(types.MakeNamed(type->Name, nullptr, auto(type->TypeArgs)), type) << type->ToString();
    }
}

TEST(QueryCompiler, SexprUsesSourceFactory) {
    TFixture fixture;
    auto query = fixture.Parse("explain (rel source \"items.parquet\")", ESyntax::Sexpr);
    ASSERT_TRUE(query) << query.error().ToString();
    ASSERT_TRUE(query->Explain);
    ASSERT_EQ(query->Statements.size(), 1);
    EXPECT_EQ(fixture.Tables, std::vector<std::string>{"items.parquet"});
    auto& compiler = fixture.Compiler;
    auto prepared = compiler.Prepare(query->Statements[0], query->Explain);
    ASSERT_TRUE(prepared) << prepared.error().ToString();
    ASSERT_TRUE(*prepared);
}

TEST(QueryCompiler, SourceErrorsArePreserved) {
    TExternalModuleCatalog catalog;
    TTableSourceFactory sources = [](NQumir::NAst::TTypeFactory&, std::string_view)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::unexpected(NQumir::TError("table unavailable"));
    };
    std::istringstream sexpr("(rel source \"missing\")");
    TQueryCompiler compiler(TestModuleBase(), catalog, sources);
    auto parsedSexpr = compiler.Parse(sexpr, ESyntax::Sexpr);
    ASSERT_FALSE(parsedSexpr);
    EXPECT_NE(parsedSexpr.error().ToString().find("table unavailable"), std::string::npos);
    std::istringstream sql("SELECT * FROM missing;");
    auto parsedSql = compiler.Parse(sql);
    ASSERT_TRUE(parsedSql) << parsedSql.error().ToString();
    auto prepared = compiler.Prepare(parsedSql->Statements[0]);
    ASSERT_FALSE(prepared);
    EXPECT_NE(prepared.error().ToString().find("table unavailable"), std::string::npos);
}

TEST(QueryCompiler, InvalidInputAndExplainKeywordBoundary) {
    TFixture fixture;
    EXPECT_FALSE(fixture.Parse("SELECT FROM;"));
    EXPECT_FALSE(fixture.Parse("123", ESyntax::Sexpr));
    auto query = fixture.Parse("explained SELECT 1;");
    EXPECT_FALSE(query);
    auto empty = fixture.Parse("  ");
    ASSERT_TRUE(empty) << empty.error().ToString();
    EXPECT_FALSE(empty->Explain);
    EXPECT_TRUE(empty->Statements.empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
