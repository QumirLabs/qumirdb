#pragma once

#include <qdb/io/io.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <span>
#include <vector>
#include <cstdint>

#include "stats.h"

namespace NQdb {

struct IOperator;
using TOperatorPtr = std::shared_ptr<IOperator>;

// A column read by an operator's own keys or expressions. Ident is the
// expression node that names the column; a key is a plain name and has none.
struct TColumnReference {
    std::string Name;
    std::shared_ptr<NQumir::NAst::TIdentExpr> Ident;
};

struct TRuntimeFilterUseSpec {
    uint32_t Id = 0;
    std::vector<std::string> ProbeKeys;
};

struct IOperator : NQumir::NAst::TExpr {
    // Type of Source: TFunctionType : () -> TStructType
    // Type: TFunctionType : (TStructType) -> TStructType
    static constexpr const char* NodeId = "Rel";
    TStatsPtr Stats_;
    // The relation yields at most one row: SQL gives a scalar subquery a value
    // only then. Row estimates take it as a bound.
    bool AtMostOneRow = false;
    // Query-local, approximate consumer bindings placed after optimization.
    // They do not change SQL semantics or row-count estimates.
    std::vector<TRuntimeFilterUseSpec> RuntimeFilterUses;

    const std::string_view NodeName() const override { return NodeId; }
    virtual std::string_view RelName() const = 0;
    void Accept(NQumir::NAst::IVisitor& visitor) override { visitor.VisitOtherwise(*this); }

    // Columns referenced in this node's own keys and expressions, in the order
    // they appear. Excludes what input operators produce - only own references.
    virtual std::vector<TColumnReference> ColumnReferences() const = 0;

    // The names of ColumnReferences().
    std::unordered_set<std::string> ComputeReferencedColumns() const {
        std::unordered_set<std::string> names;
        for (const auto& reference : ColumnReferences()) {
            names.insert(reference.Name);
        }
        return names;
    }

    // Columns the child at `childIdx` must produce, given what this node's
    // consumer needs (`needed`). Default = pass-through (needed + own refs),
    // correct for schema-preserving single-input operators (filter, source).
    // Schema-defining operators (project, aggregate) override to return only
    // their own refs; multi-input operators (join) override to split per side.
    virtual std::unordered_set<std::string> RequiredColumnsForChild(
        size_t /*childIdx*/, const std::unordered_set<std::string>& needed) const {
        std::unordered_set<std::string> required = needed;
        auto own = ComputeReferencedColumns();
        required.insert(own.begin(), own.end());
        return required;
    }

    // Output schema: TFunctionType::ReturnType.
    NQumir::NAst::TConcreteTypePtr<NQumir::NAst::TStructType> OutputColumns() const {
        auto* functionType = static_cast<NQumir::NAst::TFunctionType*>(Type.get());
        return NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(functionType->ReturnType).Cast();
    }

    // Required input schema: TFunctionType::ParamTypes[0].
    // Null for source (no upstream operator) and before typing pass.
    NQumir::NAst::TConcreteTypePtr<NQumir::NAst::TStructType> RequiredColumns() const {
        auto* fun = static_cast<NQumir::NAst::TFunctionType*>(Type.get());
        return fun->ParamTypes.empty() ? nullptr : NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(fun->ParamTypes[0]).Cast();
    }

    // Relational inputs in RequiredColumnsForChild order. The mutable view can
    // replace inputs but cannot change their count.
    virtual std::span<const TOperatorPtr> Inputs() const {
        return {};
    }

    virtual std::span<TOperatorPtr> MutableInputs() {
        return {};
    }
};

inline NQumir::NAst::TTypePtr FieldType(
    const NQumir::NAst::TStructType* structType, const std::string& name) {
    if (!structType) {
        return nullptr;
    }
    for (const auto& [fieldName, type] : structType->Fields) {
        if (fieldName == name) {
            return type;
        }
    }
    return nullptr;
}

// Plan columns may be qualified (`t.x`); an unqualified reference reaches a
// column by this part.
inline std::string BareColumnName(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos
        ? name
        : name.substr(dot + 1);
}

// Analogous to TMaybeNode<T> in qumir.
// Checks NodeName() == "Rel" AND RelName() == T::OpId.
template<typename T>
struct TMaybeOp {
    explicit TMaybeOp(TOperatorPtr node)
        : Op(node
            && std::string_view(IOperator::NodeId) == node->NodeName()
            && std::string_view(T::OpId) == node->RelName()
            ? std::static_pointer_cast<T>(std::move(node))
            : nullptr)
    {}

    std::shared_ptr<T> Cast() const {
        return std::static_pointer_cast<T>(Op);
    }

    TOperatorPtr Op;
    operator bool() const { return Op != nullptr; }
};

} // namespace NQdb
