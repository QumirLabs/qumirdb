#pragma once

#include <qdb/catalog/external_module.h>
#include <qdb/plan/build.h>
#include <qdb/plan/pipeline.h>

#include <iosfwd>
#include <optional>
#include <variant>
#include <vector>

namespace NQdb {
namespace NCompiler {

enum class ESyntax { Sexpr, Sql };
enum class EExplainMode { Text, Sexpr, Both };

using TStatement = std::variant<NSql::TSqlNodePtr, TOperatorPtr>;

struct TParsedQuery {
    bool Explain = false;
    std::vector<TStatement> Statements;
};

struct TPreparedQuery {
    TOperatorPtr Plan;
    NKernel::TAnnotationContext Annotation;
    TPlanPassDiagnostics Diagnostics;
};

// Parse -> Prepare each statement in order (nullopt for declarations).
// Then Explain, or LowerPlanToGraph -> JitFinalizeKernels -> RunPlanIntoSink.
// Sources must outlive the prepared plan and its execution.
class TQueryCompiler {
public:
    TQueryCompiler(
        TExternalModuleCatalog& catalog,
        TTableSourceFactory sources,
        TPlanPassOptions options = {});

    std::expected<TParsedQuery, NQumir::TError> Parse(
        std::istream& input,
        ESyntax syntax = ESyntax::Sql) const;

    // Preparation binds the AST in place; prepare each statement once.
    std::expected<std::optional<TPreparedQuery>, NQumir::TError> Prepare(
        const TStatement& statement,
        bool explain = false) const;

private:
    TExternalModuleCatalog& Catalog_;
    TTableSourceFactory Sources_;
    TPlanPassOptions Options_;
};

void Explain(std::ostream& output, const TOperatorPtr& plan, EExplainMode mode);

} // namespace NCompiler
} // namespace NQdb
