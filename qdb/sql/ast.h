#pragma once

#include <qumir/parser/ast.h>

#include <string_view>

namespace NQdb {
namespace NSql {

struct ISqlVisitor;

struct TSqlNode {
    virtual ~TSqlNode() = default;
    // Identifies the concrete node for TMaybeNode; defaults to "" for nodes
    // that are never downcast to.
    virtual std::string_view NodeName() const { return {}; }
    virtual void Accept(ISqlVisitor& visitor) = 0;
};

using TSqlNodePtr = std::shared_ptr<TSqlNode>;

template <class T>
using TSqlPtr = std::shared_ptr<T>;

// Checked downcast in the style of NAst::TMaybeNode: succeeds only when the
// node's NodeName() matches T::NodeId.
template <class T>
struct TMaybeNode {
    explicit TMaybeNode(const TSqlNodePtr& node)
        : Node(node && std::string_view(T::NodeId) == node->NodeName()
            ? std::static_pointer_cast<T>(node)
            : nullptr)
    {}

    const std::shared_ptr<T>& Cast() const { return Node; }
    operator bool() const { return Node != nullptr; }

    std::shared_ptr<T> Node;
};

enum ESetQuantifier {
    All,
    Distinct,
};

struct TIdentList;
struct TSqlQuery;

struct TSqlCte : TSqlNode {
    std::string Name;
    TSqlPtr<TIdentList> Columns; // optional column name list
    TSqlPtr<TSqlQuery> Query;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlWithClause : TSqlNode {
    bool Recursive = false;
    std::vector<TSqlPtr<TSqlCte>> Ctes;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlOrderItem : TSqlNode {
    NQumir::NAst::TExprPtr Expr;
    bool Desc = false;

    enum class ENullOrder {
        Default,
        First,
        Last
    };
    ENullOrder NullOrder = ENullOrder::Default;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlOrder : TSqlNode {
    std::vector<TSqlPtr<TSqlOrderItem>> Items;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlSelectItem : TSqlNode {
    NQumir::NAst::TExprPtr Expr;
    std::optional<std::string> Alias;
    // PostgreSQL-style aliases for fields flattened from a struct expression:
    // `SELECT expression AS (x, y, z)`.
    std::vector<std::string> ColumnAliases;

    // For "*" and "<qualified_name>.*". When Star is set, Expr and aliases are
    // unused; StarPrefix holds the optional qualifier (empty for a bare "*").
    bool Star = false;
    std::vector<std::string> StarPrefix;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlTableRef : TSqlNode {
    std::optional<std::string> Alias;
};

struct TSqlTableName : TSqlTableRef {
    static constexpr const char* NodeId = "TableName";
    std::string_view NodeName() const override { return NodeId; }

    std::vector<std::string> Name; // schema.table

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlQuery;

struct TSqlSubqueryTable : TSqlTableRef {
    static constexpr const char* NodeId = "SubqueryTable";
    std::string_view NodeName() const override { return NodeId; }

    TSqlPtr<TSqlQuery> Query;
    TSqlPtr<TIdentList> ColumnAliases; // optional column rename list

    void Accept(ISqlVisitor& visitor) override;
};

enum class ESqlJoinType {
    Inner,
    Left,
    Right,
    LeftSemi,
    RightSemi,
    Full,
    Cross,
};

struct TIdentList : TSqlNode {
    std::vector<std::string> Items;

    void Accept(ISqlVisitor& visitor) override;
};

struct TJoinCondition : TSqlNode {
    NQumir::NAst::TExprPtr On;
    TSqlPtr<TIdentList> UsingColumns;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlJoin : TSqlTableRef {
    static constexpr const char* NodeId = "Join";
    std::string_view NodeName() const override { return NodeId; }

    TSqlPtr<TSqlTableRef> Left;
    TSqlPtr<TSqlTableRef> Right;

    ESqlJoinType Type = ESqlJoinType::Inner;

    // NATURAL: the join is USING over the columns both inputs share. The
    // location of the keyword reports a natural join with nothing to join on.
    bool Natural = false;
    NQumir::TLocation NaturalLocation;

    TSqlPtr<TJoinCondition> Condition;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlQuery : TSqlNode {
    static constexpr const char* NodeId = "Query";
    std::string_view NodeName() const override { return NodeId; }

    TSqlPtr<TSqlWithClause> WithClause;

    TSqlNodePtr Body; // TSqlSelect / TSqlSetOp

    TSqlPtr<TSqlOrder> OrderBy;

    NQumir::NAst::TExprPtr Limit;
    NQumir::NAst::TExprPtr Offset;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlSelectList : TSqlNode {
    std::vector<TSqlPtr<TSqlSelectItem>> Items;
    // Leading items a rewrite adds for outputs of its own, such as the correlation
    // keys of a decorrelated subquery. SQL positions do not count them.
    size_t HelperItems = 0;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlFrameBound : TSqlNode {
    static constexpr const char* NodeId = "FrameBound";
    std::string_view NodeName() const override { return NodeId; }

    enum class EType {
        UnboundedPreceding,
        Preceding,
        CurrentRow,
        Following,
        UnboundedFollowing
    };
    EType Type;

    // optional expression for PRECEDING / FOLLOWING
    NQumir::NAst::TExprPtr Expr;

    explicit TSqlFrameBound(EType type, NQumir::NAst::TExprPtr expr)
        : Type(type)
        , Expr(std::move(expr))
    { }

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlWindowFrame : TSqlNode {
    static constexpr const char* NodeId = "WindowFrame";
    std::string_view NodeName() const override { return NodeId; }

    enum class EType {
        Rows,
        Range
    };
    EType Type;

    TSqlPtr<TSqlFrameBound> Start;
    TSqlPtr<TSqlFrameBound> End; // optional; defaults to CURRENT ROW

    explicit TSqlWindowFrame(EType type, TSqlPtr<TSqlFrameBound> start, TSqlPtr<TSqlFrameBound> end)
        : Type(type)
        , Start(std::move(start))
        , End(std::move(end))
    {}

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlWindowSpec : TSqlNode {
    static constexpr const char* NodeId = "WindowSpec";
    std::string_view NodeName() const override { return NodeId; }

    // optional "PARTITION" "BY" <expr_list>
    NQumir::NAst::TExprPtr PartitionBy;
    // optional
    TSqlPtr<TSqlOrder> OrderBy;
    // optionla
    TSqlPtr<TSqlWindowFrame> Frame;

    explicit TSqlWindowSpec(
        NQumir::NAst::TExprPtr partitionBy,
        TSqlPtr<TSqlOrder> orderBy,
        TSqlPtr<TSqlWindowFrame> frame)
        : PartitionBy(std::move(partitionBy))
        , OrderBy(std::move(orderBy))
        , Frame(std::move(frame))
    {}

    void Accept(ISqlVisitor& visitor) override;
};

// One ORDER BY key of a window; the key expression is in TWindowExpr::Operands.
struct TWindowOrder {
    bool Desc = false;
    TSqlOrderItem::ENullOrder NullOrder = TSqlOrderItem::ENullOrder::Default;
};

// `Expr OVER (...)`. The node keeps every expression in Operands, so that
// Children() is one span: Expr, PARTITION BY (optional), the ORDER BY keys,
// then the offsets of the PRECEDING/FOLLOWING frame bounds.
struct TWindowExpr : NQumir::NAst::TExpr {
    static constexpr NQumir::NAst::TExprNodeId NodeId{"WinExpr"};

    std::vector<NQumir::NAst::TExprPtr> Operands;
    bool HasPartitionBy = false;
    std::vector<TWindowOrder> Order;
    TSqlWindowFrame::EType FrameType = TSqlWindowFrame::EType::Range;
    TSqlFrameBound::EType FrameStart = TSqlFrameBound::EType::UnboundedPreceding;
    std::optional<TSqlFrameBound::EType> FrameEnd; // BETWEEN form only

    // A window call is located at its function call.
    TWindowExpr(NQumir::NAst::TExprPtr expr, const TSqlWindowSpec& spec);

    auto& Expr(this auto& self) {
        return self.Operands[0];
    }

    NQumir::NAst::TExprPtr PartitionBy() const {
        return HasPartitionBy ? Operands[1] : nullptr;
    }

    auto OrderKeys(this auto& self) {
        return std::span(self.Operands).subspan(1 + self.HasPartitionBy, self.Order.size());
    }

    // The offset of a PRECEDING/FOLLOWING bound, null for the other bounds.
    NQumir::NAst::TExprPtr FrameStartOffset() const;
    NQumir::NAst::TExprPtr FrameEndOffset() const;

    std::span<const NQumir::NAst::TExprPtr> Children() const override {
        return Operands;
    }

    std::span<NQumir::NAst::TExprPtr> MutableChildren() override {
        return Operands;
    }

    void Accept(NQumir::NAst::IVisitor& visitor) override {
        visitor.VisitOtherwise(*this);
    }
};

struct TSqlRollUp : TSqlNode {
    static constexpr const char* NodeId = "RollUp";
    std::string_view NodeName() const override { return NodeId; }
    explicit TSqlRollUp(NQumir::NAst::TExprPtr exprs) : Exprs(std::move(exprs)) {}
    // BlockExpr
    NQumir::NAst::TExprPtr Exprs;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlCube : TSqlNode {
    static constexpr const char* NodeId = "Cube";
    std::string_view NodeName() const override { return NodeId; }
    explicit TSqlCube(NQumir::NAst::TExprPtr exprs) : Exprs(std::move(exprs)) {}
    // BlockExpr
    NQumir::NAst::TExprPtr Exprs;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlGroupingExprOrList : TSqlNode {
    static constexpr const char* NodeId = "GroupingExprOrList";
    std::string_view NodeName() const override { return NodeId; }
    explicit TSqlGroupingExprOrList(NQumir::NAst::TExprPtr exprs) : Exprs(std::move(exprs)) {}
    // BlockExpr for a list, or a single expr
    NQumir::NAst::TExprPtr Exprs;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlGroupingSet : TSqlNode {
    static constexpr const char* NodeId = "GroupingSet";
    std::string_view NodeName() const override { return NodeId; }
    // TSqlGroupingExprOrList or TSqlRollUp or TSqlCube
    std::vector<TSqlPtr<TSqlNode>> Items;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlGroupBy : TSqlNode {
    // TGroupingExpr or
    // TSqlRollUp or
    // TSqlCube or
    // TSqlGroupingSet
    std::vector<TSqlPtr<TSqlNode>> Items;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlFrom : TSqlNode {
    std::vector<TSqlPtr<TSqlTableRef>> Items;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlSelect : TSqlNode {
    static constexpr const char* NodeId = "Select";
    std::string_view NodeName() const override { return NodeId; }

    ESetQuantifier Quantifier = ESetQuantifier::All;

    TSqlPtr<TSqlSelectList> SelectList;

    TSqlPtr<TSqlFrom> From;

    NQumir::NAst::TExprPtr Where;

    TSqlPtr<TSqlGroupBy> GroupBy;

    NQumir::NAst::TExprPtr Having;

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlSetOp : TSqlNode {
    static constexpr const char* NodeId = "SetOp";
    std::string_view NodeName() const override { return NodeId; }

    TSqlPtr<TSqlNode> Left;
    TSqlPtr<TSqlNode> Right;

    enum class EOp {
        Union,
        Intersect,
        Except
    };
    EOp Op;

    ESetQuantifier Quantifier;

    explicit TSqlSetOp(TSqlPtr<TSqlNode> left, TSqlPtr<TSqlNode> right, EOp op, ESetQuantifier quantifier)
        : Left(std::move(left))
        , Right(std::move(right))
        , Op(op)
        , Quantifier(quantifier)
    { }

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlExternalModule : TSqlNode {
    static constexpr const char* NodeId = "ExternalModule";
    std::string_view NodeName() const override { return NodeId; }

    std::string Name;
    std::string Language;
    std::string Code;
    bool Replace = false;

    explicit TSqlExternalModule(std::string name, std::string language, std::string code, bool replace)
        : Name(std::move(name))
        , Language(std::move(language))
        , Code(std::move(code))
        , Replace(replace)
    { }

    void Accept(ISqlVisitor& visitor) override;
};

struct TSqlExternalFunction : TSqlNode {
    /*
    argnames -> optional
    CREATE [OR REPLACE] FUNCTION orbit_position(
        [a] DOUBLE,
        [e] DOUBLE,
        [i] DOUBLE,
        [w] DOUBLE,
        [node] DOUBLE,
        [m] DOUBLE,
        [epoch] DOUBLE,
        [t] DOUBLE
    )
    RETURNS (DOUBLE, DOUBLE, DOUBLE) // RETURNS DOUBLE also possible for single return value
    // MODULE is required for external functions:
    SET MODULE TO orbital // SET MODULE = orbital also possible
    // SYMBOL is optional and defaults to the SQL function name:
    [ SET SYMBOL TO orbit_position ] // SET SYMBOL = orbit_position also possible
    ;
    */
    static constexpr const char* NodeId = "ExternalFunction";
    std::string_view NodeName() const override { return NodeId; }

    std::string ModuleName;

    // CALLED ON NULL INPUT -> false (true unsupported for now)
    // RETURNS NULL ON NULL INPUT -> true (false unsupported for now)

    // external function declaration
    // Name -> Func->Name
    // Symbol -> Func->MangledName
    // Args -> Func->Params
    // RetType -> Func->RetType
    NQumir::TIntrusivePtr<NQumir::NAst::TFunDecl> Func;
    bool Replace = false;

    explicit TSqlExternalFunction(bool replace)
        : Replace(replace)
    { }

    void Accept(ISqlVisitor& visitor) override;
};

// One Visit per concrete SQL node, so a new node fails to compile until every visitor
// handles it. Expressions go through NQumir::NAst::IVisitor.
struct ISqlVisitor {
    virtual ~ISqlVisitor() = default;
    virtual void Visit(TSqlCte& node) = 0;
    virtual void Visit(TSqlWithClause& node) = 0;
    virtual void Visit(TSqlOrderItem& node) = 0;
    virtual void Visit(TSqlOrder& node) = 0;
    virtual void Visit(TSqlSelectItem& node) = 0;
    virtual void Visit(TSqlTableName& node) = 0;
    virtual void Visit(TSqlSubqueryTable& node) = 0;
    virtual void Visit(TIdentList& node) = 0;
    virtual void Visit(TJoinCondition& node) = 0;
    virtual void Visit(TSqlJoin& node) = 0;
    virtual void Visit(TSqlQuery& node) = 0;
    virtual void Visit(TSqlSelectList& node) = 0;
    virtual void Visit(TSqlFrameBound& node) = 0;
    virtual void Visit(TSqlWindowFrame& node) = 0;
    virtual void Visit(TSqlWindowSpec& node) = 0;
    virtual void Visit(TSqlRollUp& node) = 0;
    virtual void Visit(TSqlCube& node) = 0;
    virtual void Visit(TSqlGroupingExprOrList& node) = 0;
    virtual void Visit(TSqlGroupingSet& node) = 0;
    virtual void Visit(TSqlGroupBy& node) = 0;
    virtual void Visit(TSqlFrom& node) = 0;
    virtual void Visit(TSqlSelect& node) = 0;
    virtual void Visit(TSqlSetOp& node) = 0;
    virtual void Visit(TSqlExternalModule& node) = 0;
    virtual void Visit(TSqlExternalFunction& node) = 0;
};

// A subquery used inside an expression. Bridges a SQL query into the Qumir
// expression tree; visitors handle it through IVisitor::VisitOtherwise.
struct TSubqueryExpr : NQumir::NAst::TExpr {
    static constexpr NQumir::NAst::TExprNodeId NodeId{"Subquery"};

    enum class EKind {
        Scalar, // ( <select> )
        Exists, // EXISTS ( <select> )
        In,     // <operand> IN ( <select> )
    };

    EKind Form = EKind::Scalar;
    TSqlPtr<TSqlQuery> Query;
    NQumir::NAst::TExprPtr Operand; // left-hand side, used by the IN form

    TSubqueryExpr(NQumir::TLocation loc, EKind kind, TSqlPtr<TSqlQuery> query)
        : NQumir::NAst::TExpr(std::move(loc), NodeId)
        , Form(kind)
        , Query(std::move(query))
    { }

    std::span<const NQumir::NAst::TExprPtr> Children() const override {
        return { &Operand, Operand ? 1u : 0u };
    }

    std::span<NQumir::NAst::TExprPtr> MutableChildren() override {
        return { &Operand, Operand ? 1u : 0u };
    }

    void Accept(NQumir::NAst::IVisitor& visitor) override {
        visitor.VisitOtherwise(*this);
    }
};

} // namespace NSql
} // namespace NQdb
