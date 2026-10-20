#pragma once

#include <qumir/parser/ast.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NQdb {

struct IOperator;

// SQL references survive construction only until ResolveSqlBindings. These
// expressions describe one SELECT scope; physical operators never consume them.
struct TSelectItemBinding {
    std::string Name;
    NQumir::NAst::TExprPtr Expression;
    std::vector<std::string> ColumnAliases;
    bool ImplicitName = false;
    bool Opaque = false;
    bool Hidden = false;
    std::optional<std::string> StarPrefix;
};

struct TSelectBinding {
    std::shared_ptr<IOperator> Input;
    std::vector<TSelectItemBinding> Items;
    NQumir::NAst::TExprPtr Having;
};

struct TGroupingKeyBinding {
    std::string Name;
    NQumir::NAst::TExprPtr Expression;
    bool Computed = false;
};

struct TAggregateBinding {
    std::shared_ptr<const TSelectBinding> Select;
    std::vector<TGroupingKeyBinding> Keys;
    std::vector<std::vector<size_t>> Sets;
    bool HasGroupingSyntax = false;
};

// INTERSECT and EXCEPT match whole rows, NULLs included. The branch outputs
// are known only after binding, which pairs their columns by position.
struct TSetOperationBinding {};

struct TOutputAliasesBinding {
    std::optional<std::vector<std::string>> Columns;
    std::string Qualifier;
};

} // namespace NQdb
