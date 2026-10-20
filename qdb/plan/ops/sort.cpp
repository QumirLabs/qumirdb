#include <qdb/plan/ops/sort.h>

#include <qumir/parser/type.h>

#include <memory>
#include <utility>
#include <vector>

namespace NQdb {

using namespace NQumir::NAst;

std::string_view SortDirectionName(ESortDirection direction) {
    switch (direction) {
        case ESortDirection::Asc:
            return "asc";
        case ESortDirection::Desc:
            return "desc";
    }
    return "asc";
}

std::string_view SortNullsName(ESortNulls nulls) {
    switch (nulls) {
        case ESortNulls::Default:
            return "nulls-default";
        case ESortNulls::First:
            return "nulls-first";
        case ESortNulls::Last:
            return "nulls-last";
    }
    return "nulls-default";
}

namespace {

std::vector<TColumnReference> SortKeyReferences(const std::vector<TSortKey>& keys) {
    std::vector<TColumnReference> references;
    for (const auto& key : keys) {
        references.push_back({.Name = key.Column});
    }
    return references;
}

} // namespace

TSortOperator::TSortOperator(TOperatorPtr input, std::vector<TSortKey> keys)
    : Input_(std::move(input))
    , Keys_(std::move(keys))
{
    auto inputSchema = Input_->OutputColumns();
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{inputSchema},
        inputSchema);
}

std::vector<TColumnReference> TSortOperator::ColumnReferences() const {
    return SortKeyReferences(Keys_);
}

const std::string TSortOperator::ToString() const {
    std::string s = "(rel sort " + Input_->ToString();
    for (const auto& key : Keys_) {
        s += " (" + key.Column + " ";
        s += SortDirectionName(key.Direction);
        s += " ";
        s += SortNullsName(key.Nulls);
        s += ")";
    }
    return s + ")";
}

TTopSortOperator::TTopSortOperator(TOperatorPtr input, std::vector<TSortKey> keys, int64_t limit)
    : Input_(std::move(input))
    , Keys_(std::move(keys))
    , Limit_(limit)
{
    auto inputSchema = Input_->OutputColumns();
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{inputSchema},
        inputSchema);
}

std::vector<TColumnReference> TTopSortOperator::ColumnReferences() const {
    return SortKeyReferences(Keys_);
}

const std::string TTopSortOperator::ToString() const {
    std::string s = "(rel top-sort " + Input_->ToString();
    for (const auto& key : Keys_) {
        s += " (" + key.Column + " ";
        s += SortDirectionName(key.Direction);
        s += " ";
        s += SortNullsName(key.Nulls);
        s += ")";
    }
    s += " (limit " + std::to_string(Limit_) + ")";
    return s + ")";
}

} // namespace NQdb
