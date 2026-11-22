#pragma once

#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

#include <set>
#include <string>
#include <string_view>

namespace NQdb {

// The qumirdb.oz types plans use. QumirDB builds them complete, and plan text spells
// them by name, without their struct.
const std::set<std::string>& PlanNamedTypes();

// The fields a value of `type` spreads over: those of a struct or of a named struct, but
// not of a qumirdb.oz value type (PlanNamedTypes); else null.
const NQumir::NAst::TStructType* RecordFields(const NQumir::NAst::TTypePtr& type);

// Prints a type as qumir type text; qdb types print as "<named ...>".
std::string PrintTypeText(const NQumir::NAst::TTypePtr& type);

// The SQL spelling of a type in text users see: DATE, DECIMAL(15,2), BIGINT. A
// Nullable prints as its value type.
std::string SqlTypeName(const NQumir::NAst::TTypePtr& type);

// Parses qumir type text into qdb types (Nullable, DATE, DECIMAL) of `types`.
// Throws std::runtime_error on a syntax error or trailing input.
NQumir::NAst::TTypePtr ParseTypeText(NQumir::NAst::TTypeFactory& types, std::string_view text);

} // namespace NQdb
