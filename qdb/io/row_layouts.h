#pragma once

#include <qdb/io/rowset_ops.h>
#include <qdb/io/value_type.h>

#include <qumir/parser/type.h>

#include <optional>
#include <vector>

namespace NQdb {

// The layout of the ValueTypeOf of each field of a struct row type.
std::vector<TValueLayout> RowLayouts(const NQumir::NAst::TTypePtr& type);

// Nullopt for a column type that host code cannot read.
std::optional<TValueType> ValueTypeOf(const NQumir::NAst::TTypePtr& type);

} // namespace NQdb
