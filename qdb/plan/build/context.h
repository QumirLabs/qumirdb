#pragma once

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/build.h>
#include <qdb/plan/build/binder.h>
#include <qdb/plan/build/relation.h>
#include <qdb/plan/sql_lowering.h>
#include <qdb/sql/ast.h>

#include <qumir/error.h>

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_set>

namespace NQdb {
namespace NBuild {

// The state of one canonical translation, shared by its components.
struct TContext {
    const TTableSourceFactory& Sources;
    const NKernel::TAnnotationContext& Annotation;
    // Source aliases taken in the CTE definition or main query being built.
    std::unordered_set<std::string> SourceAliases;
    // Markers and subquery columns, unique in the whole query.
    TFreshNames Names;
    uint32_t NextScope = 1;
    TCteScope* Ctes = nullptr;
};

// Translates `query` into query block `block`. The translator (build.cpp)
// defines it; a subquery re-enters it to build its body as a block of its own.
std::expected<TRelation, NQumir::TError> BuildQuery(const NSql::TSqlQuery& query, TBlock& block, TContext& context);

} // namespace NBuild
} // namespace NQdb
