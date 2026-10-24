#pragma once

#include <qumir/parser/ast.h>

#include <string_view>

namespace NQdb {
namespace NSql {

struct TSqlNode {
    virtual ~TSqlNode() = default;
    // Identifies the concrete node for TMaybeNode; defaults to "" for nodes
    // that are never downcast to.
    virtual std::string_view NodeName() const { return {}; }
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
};

struct TSqlWithClause : TSqlNode {
    bool Recursive = false;
    std::vector<TSqlPtr<TSqlCte>> Ctes;
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
};

struct TSqlOrder : TSqlNode {
    std::vector<TSqlPtr<TSqlOrderItem>> Items;
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
};

struct TSqlTableRef : TSqlNode {
    std::optional<std::string> Alias;
};

struct TSqlTableName : TSqlTableRef {
    static constexpr const char* NodeId = "TableName";
    std::string_view NodeName() const override { return NodeId; }

    std::vector<std::string> Name; // schema.table
};

struct TSqlQuery;

struct TSqlSubqueryTable : TSqlTableRef {
    static constexpr const char* NodeId = "SubqueryTable";
    std::string_view NodeName() const override { return NodeId; }

    TSqlPtr<TSqlQuery> Query;
    TSqlPtr<TIdentList> ColumnAliases; // optional column rename list
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
};

struct TJoinCondition : TSqlNode {
    NQumir::NAst::TExprPtr On;
    TSqlPtr<TIdentList> UsingColumns;
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
};

struct TSqlQuery : TSqlNode {
    static constexpr const char* NodeId = "Query";
    std::string_view NodeName() const override { return NodeId; }

    TSqlPtr<TSqlWithClause> WithClause;

    TSqlNodePtr Body; // TSqlSelect / TSqlSetOp

    TSqlPtr<TSqlOrder> OrderBy;

    NQumir::NAst::TExprPtr Limit;
    NQumir::NAst::TExprPtr Offset;
};

struct TSqlSelectList : TSqlNode {
    std::vector<TSqlPtr<TSqlSelectItem>> Items;
    // Leading items a rewrite adds for outputs of its own, such as the correlation
    // keys of a decorrelated subquery. SQL positions do not count them.
    size_t HelperItems = 0;
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
};

struct TWindowExpr : NQumir::NAst::TExpr {
    static constexpr const char* NodeId = "WindowExpr";

    NQumir::NAst::TExprPtr Expr;
    TSqlPtr<TSqlWindowSpec> WindowSpec;

    // A window call is located at its function call.
    explicit TWindowExpr(NQumir::NAst::TExprPtr expr, TSqlPtr<TSqlWindowSpec> spec)
        : NQumir::NAst::TExpr(expr ? expr->Location : NQumir::TLocation{})
        , Expr(std::move(expr))
        , WindowSpec(std::move(spec))
    { }

    std::vector<NQumir::NAst::TExprPtr> Children() const override {
        std::vector<NQumir::NAst::TExprPtr> children;
        if (Expr) {
            children.push_back(Expr);
        }
        if (WindowSpec) {
            if (WindowSpec->PartitionBy) {
                children.push_back(WindowSpec->PartitionBy);
            }
            if (WindowSpec->OrderBy) {
                for (const auto& item : WindowSpec->OrderBy->Items) {
                    if (item && item->Expr) {
                        children.push_back(item->Expr);
                    }
                }
            }
            if (WindowSpec->Frame) {
                if (WindowSpec->Frame->Start && WindowSpec->Frame->Start->Expr) {
                    children.push_back(WindowSpec->Frame->Start->Expr);
                }
                if (WindowSpec->Frame->End && WindowSpec->Frame->End->Expr) {
                    children.push_back(WindowSpec->Frame->End->Expr);
                }
            }
        }
        return children;
    }

    std::vector<NQumir::NAst::TExprPtr*> MutableChildren() override {
        std::vector<NQumir::NAst::TExprPtr*> children;
        if (Expr) {
            children.push_back(&Expr);
        }
        if (WindowSpec) {
            if (WindowSpec->PartitionBy) {
                children.push_back(&WindowSpec->PartitionBy);
            }
            if (WindowSpec->OrderBy) {
                for (auto& item : WindowSpec->OrderBy->Items) {
                    if (item && item->Expr) {
                        children.push_back(&item->Expr);
                    }
                }
            }
            if (WindowSpec->Frame) {
                if (WindowSpec->Frame->Start && WindowSpec->Frame->Start->Expr) {
                    children.push_back(&WindowSpec->Frame->Start->Expr);
                }
                if (WindowSpec->Frame->End && WindowSpec->Frame->End->Expr) {
                    children.push_back(&WindowSpec->Frame->End->Expr);
                }
            }
        }
        return children;
    }

    const std::string_view NodeName() const override {
        return NodeId;
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
};

struct TSqlCube : TSqlNode {
    static constexpr const char* NodeId = "Cube";
    std::string_view NodeName() const override { return NodeId; }
    explicit TSqlCube(NQumir::NAst::TExprPtr exprs) : Exprs(std::move(exprs)) {}
    // BlockExpr
    NQumir::NAst::TExprPtr Exprs;
};

struct TSqlGroupingExprOrList : TSqlNode {
    static constexpr const char* NodeId = "GroupingExprOrList";
    std::string_view NodeName() const override { return NodeId; }
    explicit TSqlGroupingExprOrList(NQumir::NAst::TExprPtr exprs) : Exprs(std::move(exprs)) {}
    // BlockExpr for a list, or a single expr
    NQumir::NAst::TExprPtr Exprs;
};

struct TSqlGroupingSet : TSqlNode {
    static constexpr const char* NodeId = "GroupingSet";
    std::string_view NodeName() const override { return NodeId; }
    // TSqlGroupingExprOrList or TSqlRollUp or TSqlCube
    std::vector<TSqlPtr<TSqlNode>> Items;
};

struct TSqlGroupBy : TSqlNode {
    // TGroupingExpr or
    // TSqlRollUp or
    // TSqlCube or
    // TSqlGroupingSet
    std::vector<TSqlPtr<TSqlNode>> Items;
};

struct TSqlFrom : TSqlNode {
    std::vector<TSqlPtr<TSqlTableRef>> Items;
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
    std::shared_ptr<NQumir::NAst::TFunDecl> Func;
    bool Replace = false;

    explicit TSqlExternalFunction(bool replace)
        : Replace(replace)
    { }
};

// A subquery used inside an expression. Bridges a SQL query into the Qumir
// expression tree; visitors handle it through IVisitor::VisitOtherwise.
struct TSubqueryExpr : NQumir::NAst::TExpr {
    static constexpr const char* NodeId = "SqlSubquery";

    enum class EKind {
        Scalar, // ( <select> )
        Exists, // EXISTS ( <select> )
        In,     // <operand> IN ( <select> )
    };

    EKind Kind = EKind::Scalar;
    TSqlPtr<TSqlQuery> Query;
    NQumir::NAst::TExprPtr Operand; // left-hand side, used by the IN form

    TSubqueryExpr(NQumir::TLocation loc, EKind kind, TSqlPtr<TSqlQuery> query)
        : NQumir::NAst::TExpr(std::move(loc))
        , Kind(kind)
        , Query(std::move(query))
    { }

    std::vector<NQumir::NAst::TExprPtr> Children() const override {
        if (Operand) {
            return { Operand };
        }
        return {};
    }

    std::vector<NQumir::NAst::TExprPtr*> MutableChildren() override {
        if (Operand) {
            return { &Operand };
        }
        return {};
    }

    const std::string_view NodeName() const override {
        return NodeId;
    }

    void Accept(NQumir::NAst::IVisitor& visitor) override {
        visitor.VisitOtherwise(*this);
    }
};

} // namespace NSql
} // namespace NQdb
