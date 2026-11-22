#pragma once

#include <qdb/plan/ops/operator.h>
#include <qdb/io/table.h>

#include <unordered_set>
#include <string>
#include <utility>

namespace NQdb {

class TSourceOperator : public IOperator {
public:
    static constexpr const char* OpId = "source";

    explicit TSourceOperator(NQumir::NAst::TTypeFactory& types, std::shared_ptr<const ITable> table, std::string path = {});

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override { return {}; }
    const std::shared_ptr<const ITable>& GetTable() const { return Table_; }
    // The table's columns in the query's type factory.
    const TSchema& TableSchema() const {
        return TableSchema_;
    }
    const std::string& SourcePath() const { return SourcePath_; }
    const std::string& GetAlias() const { return Alias_; }
    const NQumir::NAst::TExprPtr& RowGroupPredicate() const {
        return RowGroupPredicate_;
    }
    void SetRowGroupPredicate(NQumir::NAst::TExprPtr predicate) {
        RowGroupPredicate_ = std::move(predicate);
    }
    void RestrictColumns(const std::unordered_set<std::string>& names);
    std::optional<std::vector<std::string>> RestrictedColumns() const;

    // Re-qualifies the output columns as alias.col right away, so the builder binds
    // references to these physical names and two aliases of one table do not
    // collide.
    void SetAlias(NQumir::NAst::TTypeFactory& types, std::string alias);
    void EnableRowId(NQumir::NAst::TTypeFactory& types);
    bool EmitsRowId() const { return EmitRowId_; }
    std::string RowIdColumn() const;

    const std::string ToString() const override;

private:
    std::shared_ptr<const ITable> Table_;
    TSchema TableSchema_;
    std::string SourcePath_;
    std::string Alias_;
    // Optional, conservative scan hint. The physical filter remains in the
    // operator tree and is always responsible for exact SQL semantics.
    NQumir::NAst::TExprPtr RowGroupPredicate_;
    std::optional<std::vector<std::string>> RestrictedColumns_;
    bool EmitRowId_ = false;
};

NQumir::NAst::TTypePtr StructTypeFromSchema(NQumir::NAst::TTypeFactory& types, const TSchema& schema);

} // namespace NQdb
