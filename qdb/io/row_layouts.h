#pragma once

#include <qdb/io/rowset_ops.h>
#include <qdb/io/value_type.h>

#include <qumir/parser/type.h>

#include <optional>
#include <vector>

namespace NQdb {

// Row descriptor of a struct row type: strings {0, false}, bool {0, true}
// (bit-packed), other types {FixedValueWidth}.
std::vector<TValueLayout> RowLayouts(const NQumir::NAst::TTypePtr& type);

// Nullopt for a column type that host code cannot read.
std::optional<TValueType> ValueTypeOf(const NQumir::NAst::TTypePtr& type);

} // namespace NQdb
