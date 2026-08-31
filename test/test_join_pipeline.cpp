#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst;
using namespace NQumir::NAst::NCore;

namespace {


std::unordered_set<std::string> ColNames(const TTypePtr& type) {
    std::unordered_set<std::string> names;
    if (auto* st = static_cast<TStructType*>(type.get())) {
        for (auto& [name, _] : st->Fields) {
            names.insert(name);
        }
    }
    return names;
}

// Parses `sexp` with two-sided source factory: path "L" -> left, else right.
// Captures the constructed source operators in leftOp/rightOp for inspection.
TOperatorPtr ParseJoinPlan(const std::string& sexp, ISource& left, ISource& right,
    std::shared_ptr<TSourceOperator>& leftOp, std::shared_ptr<TSourceOperator>& rightOp)
{
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "L") {
            leftOp = std::make_shared<TSourceOperator>(left, std::string(path));
            return leftOp;
        }
        rightOp = std::make_shared<TSourceOperator>(right, std::string(path));
        return rightOp;
    };
    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }
    std::istringstream in(sexp);
    TTokenStream ts(in);
    auto result = parser.Parse(ts);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().ToString());
    return std::static_pointer_cast<IOperator>(result.value_or(nullptr));
}

} // namespace

TEST(JoinPipeline, TypingSetsTwoParamTypesAndConcatenatedOutput) {
    NQdb::TMockSource left({"a", "b", "x"});
    NQdb::TMockSource right({"c", "d", "y"});
    std::shared_ptr<TSourceOperator> leftOp, rightOp;

    auto root = ParseJoinPlan(
        "(rel join (rel source \"L\") (rel source \"R\") ((a c)) (inner) (residual (< b d)))",
        left, right, leftOp, rightOp);
    ASSERT_NE(root, nullptr);

    AnnotateTypes(root);

    auto* fun = static_cast<TFunctionType*>(root->Type.get());
    ASSERT_EQ(fun->ParamTypes.size(), 2u);
    EXPECT_EQ(ColNames(fun->ParamTypes[0]).size(), 3u); // left schema
    EXPECT_EQ(ColNames(fun->ParamTypes[1]).size(), 3u); // right schema
    EXPECT_EQ(ColNames(root->OutputColumns()).size(), 6u); // a,b,x,c,d,y
}

TEST(JoinPipeline, PruningNarrowsEachSideIndependently) {
    NQdb::TMockSource left({"a", "b", "x"});
    NQdb::TMockSource right({"c", "d", "y"});
    std::shared_ptr<TSourceOperator> leftOp, rightOp;

    // Downstream project selects only a and d; join key (a,c); filter uses (b,d).
    auto root = ParseJoinPlan(
        "(rel project (rel join (rel source \"L\") (rel source \"R\") "
        "((a c)) (inner) (residual (< b d))) (oa a) (od d))",
        left, right, leftOp, rightOp);
    ASSERT_NE(root, nullptr);

    AnnotateTypes(root);
    ApplyColumnPruning(root);

    ASSERT_TRUE(leftOp);
    ASSERT_TRUE(rightOp);
    // left: key 'a' + filter 'b' + downstream 'a'; 'x' dropped.
    EXPECT_EQ(ColNames(leftOp->OutputColumns()),
              (std::unordered_set<std::string>{"a", "b"}));
    // right: key 'c' + filter 'd' + downstream 'd'; 'y' dropped.
    EXPECT_EQ(ColNames(rightOp->OutputColumns()),
              (std::unordered_set<std::string>{"c", "d"}));
}

TEST(JoinPipeline, PruningKeepsKeysEvenWhenNotSelectedDownstream) {
    NQdb::TMockSource left({"a", "b"});
    NQdb::TMockSource right({"c", "d"});
    std::shared_ptr<TSourceOperator> leftOp, rightOp;

    // Downstream selects only b and d; keys a,c are not selected but still needed.
    auto root = ParseJoinPlan(
        "(rel project (rel join (rel source \"L\") (rel source \"R\") "
        "((a c)) (inner)) (ob b) (od d))",
        left, right, leftOp, rightOp);
    ASSERT_NE(root, nullptr);

    AnnotateTypes(root);
    ApplyColumnPruning(root);

    EXPECT_EQ(ColNames(leftOp->OutputColumns()),
              (std::unordered_set<std::string>{"a", "b"}));
    EXPECT_EQ(ColNames(rightOp->OutputColumns()),
              (std::unordered_set<std::string>{"c", "d"}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
