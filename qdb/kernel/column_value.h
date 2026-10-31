#pragma once

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <cstdint>
#include <string>
#include <vector>

namespace NQdb::NKernel {

struct TColumnValueAst {
    std::vector<NQumir::NAst::TExprPtr> Setup;
    NQumir::NAst::TExprPtr Value;
    NQumir::NAst::TExprPtr IsValid;
    NQumir::NAst::TTypePtr ValueType;
};

TColumnValueAst BuildColumnValueAst(
    const std::string& columnName,
    const std::string& rowName,
    const std::string& temporaryPrefix,
    const NQumir::NAst::TTypePtr& logicalType,
    const NQumir::NAst::TTypePtr& stringViewType,
    bool needValue = true);

// Byte width of one value in a fixed-width column buffer: integers, floats, decimal
// storage and BinInt/i128, and bool (one byte in kernel buffers). Strings and other
// types give 0, so every host site sizes values through this one function.
int64_t FixedValueWidth(const NQumir::NAst::TTypePtr& type);

} // namespace NQdb::NKernel
