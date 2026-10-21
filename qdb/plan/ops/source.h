#pragma once

#include <qdb/plan/ops/operator.h>

#include <unordered_set>
#include <string>
#include <utility>

namespace NQdb {

class TSourceOperator : public IOperator {
public:
    static constexpr const char* OpId = "source";

    explicit TSourceOperator(ISource& source, std::string path = {});

    std::string_view RelName() const override { return OpId; }
    std::vector<TColumnReference> ColumnReferences() const override { return {}; }
    ISource& GetSource() const { return Source_; }
    const std::string& SourcePath() const { return SourcePath_; }
    const std::string& GetAlias() const { return Alias_; }
    const NQumir::NAst::TExprPtr& RowGroupPredicate() const {
        return RowGroupPredicate_;
    }
    void SetRowGroupPredicate(NQumir::NAst::TExprPtr predicate) {
        RowGroupPredicate_ = std::move(predicate);
    }
    // Re-qualifies the output columns as alias.col right away, so build-time
    // schemas (e.g. a join between two aliases of the same table) don't collide
    // before the QualifyColumns pass runs.
    void SetAlias(std::string alias);
    void EnableRowId();
    bool EmitsRowId() const { return EmitRowId_; }
    std::string RowIdColumn() const;

    const std::string ToString() const override;

private:
    ISource& Source_;
    std::string SourcePath_;
    std::string Alias_;
    // Optional, conservative scan hint. The physical filter remains in the
    // operator tree and is always responsible for exact SQL semantics.
    NQumir::NAst::TExprPtr RowGroupPredicate_;
    bool EmitRowId_ = false;
};

NQumir::NAst::TTypePtr StructTypeFromSchema(const TSchema& schema);

} // namespace NQdb
