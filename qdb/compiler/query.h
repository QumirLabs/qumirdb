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
    // The query's type factory, declared first: the plan and the annotation context
    // hold its types.
    std::shared_ptr<NQumir::NAst::TTypeFactory> Types;
    TOperatorPtr Plan;
    NKernel::TAnnotationContext Annotation;
    TPlanPassDiagnostics Diagnostics;
};

// Parse -> Prepare each statement in order (nullopt for declarations).
// Then Explain, or LowerPlanToGraph -> JitFinalizeKernels -> RunPlanIntoSink.
// Sources must outlive the prepared plan and its execution. A compiler compiles one
// query: it owns the query's type factory, a child of the process's module `base`,
// which its statements share and which it hands to each prepared statement.
class TQueryCompiler {
public:
    TQueryCompiler(
        std::shared_ptr<const NKernel::TModuleBase> base,
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
    std::shared_ptr<const NKernel::TModuleBase> Base_;
    std::shared_ptr<NQumir::NAst::TTypeFactory> Types_;
    TExternalModuleCatalog& Catalog_;
    TTableSourceFactory Sources_;
    TPlanPassOptions Options_;
};

void Explain(std::ostream& output, const TOperatorPtr& plan, EExplainMode mode);

} // namespace NCompiler
} // namespace NQdb
