#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/build/relation.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/source.h>
#include <qdb/sql/ast.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace NQdb {
namespace NBuild {

// Name binding of the canonical translation: the scopes SQL names resolve in and
// the columns each FROM item exposes to them.

// A query block (one SELECT) while it is translated. A name resolves against the
// tree of its own block first and then against those of the enclosing blocks,
// so an inner column shadows an outer one.
struct TBlock {
    uint32_t Id = 0;
    TBlock* Parent = nullptr;
    // The tree the next clause applies to. While FROM is translated, it is the
    // part a FROM item may read: the items to its left.
    TRelation Tree;
    // The enclosing blocks whose columns this block or its subqueries read, with
    // the first such reference.
    std::map<uint32_t, NQumir::TLocation> OuterScopes;
    // Above an aggregate: the columns below it, which only an aggregate reads.
    std::optional<TRelation> Ungrouped;
};

// The output column of `relation` an SQL name refers to: a label equal to the
// name or, for a bare name, a qualified label ending in it. A qualified name
// whose qualifier is a FROM item of the relation must be found there.
std::expected<std::optional<size_t>, NQumir::TError> FindColumn(
    const TRelation& relation,
    const std::string& name,
    const NQumir::TLocation& location);

// Resolves an identifier from `block` outwards. A column of the block's own tree
// renames the identifier to its physical column; a column of an enclosing block
// becomes an outer reference, which every block in between records.
std::expected<NQumir::NAst::TExprPtr, NQumir::TError> Resolve(
    TBlock& block,
    const std::shared_ptr<NQumir::NAst::TIdentExpr>& ident,
    bool inAggregate);

// Unaliased sources are named by their table, numbered when the name repeats
// within one CTE definition or the main query.
std::string SourceAlias(const NSql::TSqlTableName& table, std::unordered_set<std::string>& taken);

// The columns of a table, qualified by the name of its FROM item.
TRelation TableRelation(const std::shared_ptr<TSourceOperator>& source, const std::string& qualifier);

// Gives a relation new labels. Physical names follow them unless labels repeat.
std::expected<TRelation, NQumir::TError> Relabel(
    TRelation relation,
    std::vector<std::string> labels,
    const NKernel::TAnnotationContext& annotation);

// Qualifies the columns of a derived table or CTE reference by its alias.
std::expected<TRelation, NQumir::TError> AliasRelation(
    TRelation relation,
    const std::string& alias,
    const NKernel::TAnnotationContext& annotation);

struct TBuiltCte {
    TCteDefinitionPtr Definition; // null while the definition is being built
    std::vector<std::string> Labels;
};

// The CTEs of one WITH clause, by lowercase name. A definition is built at its
// first reference, as a block nested in the block that encloses the WITH query.
struct TCteScope {
    TCteScope* Parent = nullptr;
    TBlock* Enclosing = nullptr;
    std::map<std::string, NSql::TSqlPtr<NSql::TSqlCte>> Definitions;
    std::map<std::string, TBuiltCte> Built;
};

// The WITH clause that defines the CTE named `key` (lowercase): the nearest of
// `scope` and the scopes that enclose it, or null.
TCteScope* FindCteScope(TCteScope* scope, const std::string& key);

} // namespace NBuild
} // namespace NQdb
