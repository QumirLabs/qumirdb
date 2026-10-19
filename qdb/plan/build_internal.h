#pragma once

#include <qdb/plan/build.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NQdb {
namespace NBuild {

// Shared SQL-builder operations. These are internal to plan construction.
struct TAggCall {
    std::string Func;
    NQumir::NAst::TExprPtr Arg; // null for count(*)
    bool Distinct = false;
    bool Star = false;
};

std::string ToLower(std::string value);
bool IsAggFunc(const std::string& name);
std::optional<TAggCall> AsAggCall(const NQumir::NAst::TExprPtr& expr);
NQumir::NAst::TExprPtr Ident(NQumir::TLocation location, std::string name);
NQumir::NAst::TExprPtr I64Literal(NQumir::TLocation location, int64_t value);
NQumir::NAst::TExprPtr Call(
    NQumir::TLocation location,
    std::string name,
    std::vector<NQumir::NAst::TExprPtr> args);
bool HasSubquery(const NQumir::NAst::TExprPtr& expr);
bool HasWindowExpr(const NSql::TSqlQuery& query);
std::string ItemName(const NSql::TSqlSelectItem& item, size_t index);
std::expected<void, NQumir::TError> ValidateOutputColumnName(
    std::string_view name,
    const NQumir::TLocation& location = {});
TOperatorPtr ApplyDistinct(TOperatorPtr projected, const std::vector<std::string>& columns);
std::expected<std::vector<std::string>, NQumir::TError> OutputColumnNames(
    const TOperatorPtr& op,
    const std::string& context);
void CollectLocalColumns(
    const NSql::TSqlPtr<NSql::TSqlTableRef>& ref,
    const TTableSourceFactory& sources,
    std::unordered_set<std::string>& out);
void RenameIdents(
    const NQumir::NAst::TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& rename);
std::expected<TOperatorPtr, NQumir::TError> BuildFrom(
    const NSql::TSqlFrom& from,
    const TTableSourceFactory& sources);
std::expected<TOperatorPtr, NQumir::TError> BuildSelect(
    const NSql::TSqlSelect& select,
    const TTableSourceFactory& sources,
    TOperatorPtr input = nullptr);

} // namespace NBuild
} // namespace NQdb
