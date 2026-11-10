#include <qdb/scheduler/port.h>

#include <qdb/exec/instrumentation/operator.h>

#include <utility>

namespace NQdb {
namespace NScheduler {

EFetchResult TInputPort::Fetch(TRowSet& rowSet) const {
    const auto result = Connection
        ? Connection->Fetch(Lane, rowSet)
        : EFetchResult::FINISHED;
    if (result == EFetchResult::OK && Instrumentation) {
        Instrumentation->Add(rowSet);
    }
    return result;
}

bool TOutputPort::CanPush() const {
    return !Connection || Connection->CanPush(Lane);
}

bool TOutputPort::Push(TRowSet&& rowSet) const {
    const auto rows = Instrumentation ? Instrumentation->Rows : 0;
    const auto bytes = Instrumentation ? Instrumentation->Bytes : 0;
    if (Instrumentation) {
        Instrumentation->Add(rowSet);
    }
    const bool pushed = !Connection || Connection->Push(Lane, std::move(rowSet));
    if (!pushed && Instrumentation) {
        Instrumentation->Rows = rows;
        Instrumentation->Bytes = bytes;
    }
    return pushed;
}

void TOutputPort::Finish() const {
    if (Connection) {
        Connection->Finish(Lane);
    }
}

bool TPortRebind::Apply(TInputPort& port) const {
    if (Kind != EPortKind::Input || port.Connection != From || port.Lane != FromLane) {
        return false;
    }
    port.Connection = To;
    port.Lane = ToLane;
    return true;
}

bool TPortRebind::Apply(TOutputPort& port) const {
    if (Kind != EPortKind::Output || port.Connection != From || port.Lane != FromLane) {
        return false;
    }
    port.Connection = To;
    port.Lane = ToLane;
    return true;
}

} // namespace NScheduler
} // namespace NQdb
