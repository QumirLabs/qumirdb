#pragma once

#include <string_view>
#include <vector>

#include <qumir/parser/type.h>
#include <qumir/parser/type_factory.h>

namespace NQdb {

// Names view storage of the table or reader that made the schema.
struct TColumnSchema {
    std::string_view Name;
    NQumir::NAst::TTypePtr Type = nullptr;
};

// A schema is built on request in the caller's type factory: tables and readers
// outlive queries, so they hold no types.
struct TSchema {
    std::vector<TColumnSchema> Columns;
};

} // namespace NQdb
