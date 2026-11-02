#include "mock_source.h"

#include <qdb/compiler/query.h>
#include <qdb/plan/ops/source.h>

#include <gtest/gtest.h>

#include <sstream>

using namespace NQdb;
using namespace NQdb::NCompiler;

namespace {

struct TFixture {
    TMockSource Source{{"value"}};
    TExternalModuleCatalog Catalog;
    std::vector<std::string> Tables;
    TTableSourceFactory Sources = [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        Tables.emplace_back(table);
        return std::make_shared<TSourceOperator>(Source, std::string(table));
    };

    std::expected<TParsedQuery, NQumir::TError> Parse(
        const std::string& text,
        ESyntax syntax = ESyntax::Sql)
    {
        std::istringstream input(text);
        TQueryCompiler compiler(Catalog, Sources);
        return compiler.Parse(input, syntax);
    }
};

} // namespace

TEST(QueryCompiler, SqlPreparationAndExplain) {
    TFixture fixture;
    auto query = fixture.Parse("  ExPlAiN SELECT value FROM items WHERE value > 1;");
    ASSERT_TRUE(query) << query.error().ToString();
    ASSERT_TRUE(query->Explain);
    ASSERT_EQ(query->Statements.size(), 1);
    EXPECT_TRUE(fixture.Tables.empty());
    TQueryCompiler compiler(fixture.Catalog, fixture.Sources);
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
    TQueryCompiler compiler(fixture.Catalog, fixture.Sources);
    auto prepared = compiler.Prepare(query->Statements[0], query->Explain);
    ASSERT_FALSE(prepared);
    EXPECT_NE(prepared.error().ToString().find("EXPLAIN requires a SELECT"), std::string::npos);
    EXPECT_TRUE(fixture.Catalog.Snapshot()->ModuleNames().empty());
}

TEST(QueryCompiler, DeclarationsAreAppliedBeforeQueries) {
    TFixture fixture;
    auto query = fixture.Parse(
        "CREATE MODULE math LANGUAGE rust AS $$$$;"
        "CREATE FUNCTION convert(DOUBLE) RETURNS DOUBLE SET MODULE TO math;"
        "SELECT convert(1.0) FROM items;");
    ASSERT_TRUE(query) << query.error().ToString();
    ASSERT_EQ(query->Statements.size(), 3);
    TQueryCompiler compiler(fixture.Catalog, fixture.Sources);
    for (size_t i = 0; i < query->Statements.size(); ++i) {
        auto prepared = compiler.Prepare(query->Statements[i]);
        ASSERT_TRUE(prepared) << prepared.error().ToString();
        EXPECT_EQ(prepared->has_value(), i == 2);
        if (*prepared) {
            EXPECT_TRUE((**prepared).Annotation.ExternalCatalog->HasFunction("convert"));
        }
    }
}

TEST(QueryCompiler, SexprUsesSourceFactory) {
    TFixture fixture;
    auto query = fixture.Parse("explain (rel source \"items.parquet\")", ESyntax::Sexpr);
    ASSERT_TRUE(query) << query.error().ToString();
    ASSERT_TRUE(query->Explain);
    ASSERT_EQ(query->Statements.size(), 1);
    EXPECT_EQ(fixture.Tables, std::vector<std::string>{"items.parquet"});
    TQueryCompiler compiler(fixture.Catalog, fixture.Sources);
    auto prepared = compiler.Prepare(query->Statements[0], query->Explain);
    ASSERT_TRUE(prepared) << prepared.error().ToString();
    ASSERT_TRUE(*prepared);
}

TEST(QueryCompiler, SourceErrorsArePreserved) {
    TExternalModuleCatalog catalog;
    TTableSourceFactory sources = [](std::string_view)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::unexpected(NQumir::TError("table unavailable"));
    };
    std::istringstream sexpr("(rel source \"missing\")");
    TQueryCompiler compiler(catalog, sources);
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
