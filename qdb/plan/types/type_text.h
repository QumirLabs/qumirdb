#pragma once

#include <qumir/parser/type.h>

#include <string>
#include <string_view>

namespace NQdb {

// Prints a type as qumir type text; qdb types print as "<named ...>".
std::string PrintTypeText(const NQumir::NAst::TTypePtr& type);

// Parses qumir type text into qdb types (TNullable, TDecimal).
// Throws std::runtime_error on a syntax error or trailing input.
NQumir::NAst::TTypePtr ParseTypeText(std::string_view text);

} // namespace NQdb
