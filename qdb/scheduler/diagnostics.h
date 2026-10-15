#pragma once

#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_adapter.h>

#include <iosfwd>
#include <unordered_map>

namespace NQdb {
namespace NScheduler {

struct TLoweredPlan;

class TTaskDiagnosticSetup {
public:
    TTaskDiagnosticSetup(
        TTaskNode& node,
        const std::unordered_map<const IConnection*, NQumir::NAst::TTypePtr>& types);

    void Input(TInputPort& port);
    void Output(TOutputPort& port);
    TRowSetDiagnostics& Output(IConnection& connection);
    TOperatorDiagnostics& Counters();

private:
    TTaskNode& Node_;
    const std::unordered_map<const IConnection*, NQumir::NAst::TTypePtr>& Types_;
};

// Preparation mutates only this graph, before a scheduler starts. The common
// Oz measurement kernel is emitted only when diagnostics are requested.
void EnableOperatorDiagnostics(TLoweredPlan& lowered);
void PrintOperatorDiagnostics(std::ostream& out, const TTaskGraph& graph);
ETaskResult ExecuteTask(TTaskNode& node);

} // namespace NScheduler
} // namespace NQdb
