#include <gtest/gtest.h>

#include <qdb/sql/lexer.h>
#include <qdb/sql/parser.h>

#include <sstream>
#include <vector>
#include <string>

using namespace NQdb::NSql;
using namespace NQumir::NAst;

namespace {

std::vector<TToken> Tokenize(const std::string& sql) {
    std::istringstream in(sql);
    TTokenStream lexer(in);

    std::vector<TToken> tokens;
    while (true) {
        auto token = lexer.Next();
        // std::cerr << "Token: " << token.RawValue << " (type: " << token.Type << ", location: " << token.Location.ToString() << ")\n";
        if (token.Type == TToken::Operator && token.Value.i64 == (uint64_t)-1) {
            break;
        }
        tokens.push_back(token);
    }
    return tokens;
}

std::vector<std::string> RawTokens(const std::string& sql) {
    std::vector<std::string> result;
    for (const auto& token : Tokenize(sql)) {
        result.push_back(token.RawValue);
    }
    return result;
}

} // namespace

TEST(SqlLexer, SimpleSelect) {
    EXPECT_EQ(
        RawTokens("select a, b from table1 where a >= 10;"),
        (std::vector<std::string>{
            "select", "a", ",", "b", "from", "table1", "where", "a", ">=", "10", ";"
        })
    );
}

TEST(SqlLexer, KeywordsAreUppercased) {
    auto tokens = Tokenize("select from where");
    ASSERT_EQ(tokens.size(), 3u);

    EXPECT_EQ(tokens[0].Type, TToken::Keyword);
    EXPECT_EQ(tokens[0].Name, "SELECT");

    EXPECT_EQ(tokens[1].Type, TToken::Keyword);
    EXPECT_EQ(tokens[1].Name, "FROM");

    EXPECT_EQ(tokens[2].Type, TToken::Keyword);
    EXPECT_EQ(tokens[2].Name, "WHERE");
}

TEST(SqlLexer, UnquotedIdentifiersFoldToLowercase) {
    auto tokens = Tokenize("Foo bar_123");
    ASSERT_EQ(tokens.size(), 2u);

    // SQL folds unquoted identifiers case-insensitively; Name is lowercased, the source
    // spelling is kept in RawValue.
    EXPECT_EQ(tokens[0].Type, TToken::Identifier);
    EXPECT_EQ(tokens[0].Name, "foo");
    EXPECT_EQ(tokens[0].RawValue, "Foo");

    EXPECT_EQ(tokens[1].Type, TToken::Identifier);
    EXPECT_EQ(tokens[1].Name, "bar_123");
}

TEST(SqlLexer, QualifiedName) {
    EXPECT_EQ(
        RawTokens("schema.table.column"),
        (std::vector<std::string>{"schema", ".", "table", ".", "column"})
    );
}

TEST(SqlLexer, Operators) {
    EXPECT_EQ(
        RawTokens("a <= b >= c <> d != e && f || g :: int"),
        (std::vector<std::string>{
            "a", "<=", "b", ">=", "c", "<>", "d", "!=", "e", "&&", "f", "||", "g", "::", "int"
        })
    );
}

TEST(SqlLexer, MinusIsOperatorNotPartOfNumber) {
    EXPECT_EQ(
        RawTokens("a-1 -2"),
        (std::vector<std::string>{"a", "-", "1", "-", "2"})
    );
}

TEST(SqlLexer, Numbers) {
    auto tokens = Tokenize("123 12.34 .5 1e10 1.2e-3");
    ASSERT_EQ(tokens.size(), 5u);

    EXPECT_EQ(tokens[0].Type, TToken::Integer);
    EXPECT_EQ(tokens[0].Value.i64, 123);

    EXPECT_EQ(tokens[1].Type, TToken::Float);
    EXPECT_DOUBLE_EQ(tokens[1].Value.f64, 12.34);

    EXPECT_EQ(tokens[2].Type, TToken::Float);
    EXPECT_DOUBLE_EQ(tokens[2].Value.f64, .5);

    EXPECT_EQ(tokens[3].Type, TToken::Float);
    EXPECT_DOUBLE_EQ(tokens[3].Value.f64, 1e10);

    EXPECT_EQ(tokens[4].Type, TToken::Float);
    EXPECT_DOUBLE_EQ(tokens[4].Value.f64, 1.2e-3);
}

TEST(SqlLexer, SqlString) {
    auto tokens = Tokenize("'hello' 'it''s ok'");
    ASSERT_EQ(tokens.size(), 2u);

    EXPECT_EQ(tokens[0].Type, TToken::String);
    EXPECT_EQ(tokens[0].Name, "hello");

    EXPECT_EQ(tokens[1].Type, TToken::String);
    EXPECT_EQ(tokens[1].Name, "it's ok");
}

TEST(SqlLexer, DollarQuotedString) {
    auto tokens = Tokenize("$$\nfn f() { println!(\"it's $raw\"); }\n$$");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].Type, TToken::String);
    EXPECT_EQ(tokens[0].Name, "\nfn f() { println!(\"it's $raw\"); }\n");
}

TEST(SqlLexer, QuotedIdentifier) {
    auto tokens = Tokenize("\"Column Name\"");
    ASSERT_EQ(tokens.size(), 1u);

    EXPECT_EQ(tokens[0].Type, TToken::Identifier);
    EXPECT_EQ(tokens[0].Name, "Column Name");
}

TEST(SqlLexer, BacktickIdentifier) {
    auto tokens = Tokenize("`Column Name`");
    ASSERT_EQ(tokens.size(), 1u);

    EXPECT_EQ(tokens[0].Type, TToken::Identifier);
    EXPECT_EQ(tokens[0].Name, "Column Name");
}

TEST(SqlLexer, LineComment) {
    EXPECT_EQ(
        RawTokens("select -- comment\n a"),
        (std::vector<std::string>{"select", "a"})
    );
}

TEST(SqlLexer, BlockComment) {
    EXPECT_EQ(
        RawTokens("select /* comment */ a"),
        (std::vector<std::string>{"select", "a"})
    );
}

TEST(SqlLexer, UnterminatedStringThrows) {
    EXPECT_THROW(Tokenize("'abc"), std::runtime_error);
}

TEST(SqlLexer, UnterminatedDollarQuotedStringThrows) {
    EXPECT_THROW(Tokenize("$$abc"), std::runtime_error);
}

TEST(SqlLexer, UnterminatedBlockCommentThrows) {
    EXPECT_THROW(Tokenize("select /* abc"), std::runtime_error);
}

TEST(SqlParser, CreateOrReplaceModule) {
    std::istringstream in(
        "CREATE OR REPLACE MODULE orbital\n"
        "LANGUAGE rust\n"
        "AS $$fn orbit_position() {}$$;");
    TTokenStream tokens(in);
    TParser parser;
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();

    auto module = NQdb::NSql::TMaybeNode<TSqlExternalModule>(*parsed);
    ASSERT_TRUE(module);
    EXPECT_EQ(module.Cast()->Name, "orbital");
    EXPECT_EQ(module.Cast()->Language, "rust");
    EXPECT_EQ(module.Cast()->Code, "fn orbit_position() {}");
    EXPECT_TRUE(module.Cast()->Replace);
}

TEST(SqlParser, CreateOrReplaceExternalFunction) {
    std::istringstream in(
        "CREATE OR REPLACE FUNCTION orbit_position(a DOUBLE, DOUBLE) "
        "RETURNS (DOUBLE, DOUBLE, DOUBLE) "
        "SET SYMBOL = orbit_position "
        "SET MODULE TO orbital;");
    TTokenStream tokens(in);
    TParser parser;
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();

    auto external = NQdb::NSql::TMaybeNode<TSqlExternalFunction>(*parsed);
    ASSERT_TRUE(external);
    EXPECT_TRUE(external.Cast()->Replace);
    EXPECT_EQ(external.Cast()->ModuleName, "orbital");
    ASSERT_TRUE(external.Cast()->Func);
    EXPECT_EQ(external.Cast()->Func->Name, "orbit_position");
    EXPECT_EQ(external.Cast()->Func->MangledName, "orbit_position");
    ASSERT_EQ(external.Cast()->Func->Params.size(), 2u);
    EXPECT_EQ(external.Cast()->Func->Params[0]->Name, "a");
    EXPECT_EQ(external.Cast()->Func->Params[1]->Name, "__external_arg_1");

    auto returnType = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(
        external.Cast()->Func->RetType);
    ASSERT_TRUE(returnType);
    ASSERT_EQ(returnType.Cast()->Fields.size(), 3u);
    EXPECT_EQ(returnType.Cast()->Fields[0].first, "field1");
    EXPECT_EQ(returnType.Cast()->Fields[1].first, "field2");
    EXPECT_EQ(returnType.Cast()->Fields[2].first, "field3");
}

TEST(SqlParser, CreateExternalFunctionWithScalarReturn) {
    std::istringstream in(
        "CREATE FUNCTION orbit_distance(DOUBLE) RETURNS DOUBLE "
        "SET MODULE = orbital;");
    TTokenStream tokens(in);
    TParser parser;
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();

    auto external = NQdb::NSql::TMaybeNode<TSqlExternalFunction>(*parsed);
    ASSERT_TRUE(external);
    EXPECT_FALSE(external.Cast()->Replace);
    EXPECT_EQ(external.Cast()->ModuleName, "orbital");
    ASSERT_TRUE(external.Cast()->Func);
    EXPECT_EQ(external.Cast()->Func->Name, "orbit_distance");
    EXPECT_EQ(external.Cast()->Func->MangledName, "orbit_distance");
    EXPECT_TRUE(NQumir::NAst::TMaybeType<NQumir::NAst::TFloatType>(
        external.Cast()->Func->RetType));
}

TEST(SqlParser, ParseAllExternalModuleFunctionAndQuery) {
    std::istringstream in(
        "CREATE MODULE orbital LANGUAGE rust AS $$\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn distance(x: f64) -> f64 { x + 1.0; x }\n"
        "$$;\n"
        "CREATE FUNCTION distance(DOUBLE) RETURNS DOUBLE "
        "SET MODULE TO orbital SET SYMBOL TO distance;\n"
        "SELECT distance(x) FROM points;");
    TTokenStream tokens(in);
    TParser parser;
    auto parsed = parser.ParseAll(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();
    ASSERT_EQ(parsed->size(), 3u);
    EXPECT_TRUE(NQdb::NSql::TMaybeNode<TSqlExternalModule>((*parsed)[0]));
    EXPECT_TRUE(NQdb::NSql::TMaybeNode<TSqlExternalFunction>((*parsed)[1]));
    EXPECT_TRUE(NQdb::NSql::TMaybeNode<TSqlQuery>((*parsed)[2]));
}

TEST(SqlParser, StructProjectionColumnAliases) {
    std::istringstream in(
        "SELECT orbit_vector(x) AS (position_x, position_y, position_z) "
        "FROM points;");
    TTokenStream tokens(in);
    TParser parser;
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();

    auto query = NQdb::NSql::TMaybeNode<TSqlQuery>(*parsed);
    ASSERT_TRUE(query);
    auto select = NQdb::NSql::TMaybeNode<TSqlSelect>(query.Cast()->Body);
    ASSERT_TRUE(select);
    ASSERT_TRUE(select.Cast()->SelectList);
    ASSERT_EQ(select.Cast()->SelectList->Items.size(), 1u);
    const auto& aliases = select.Cast()->SelectList->Items[0]->ColumnAliases;
    EXPECT_EQ(aliases,
        (std::vector<std::string>{"position_x", "position_y", "position_z"}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
