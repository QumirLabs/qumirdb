#pragma once

#include <qdb/scheduler/connection.h>

#include <cstddef>

namespace NQdb {

struct TRowSetInstrumentation;

namespace NScheduler {

struct TInputPort {
    IConnection* Connection = nullptr;
    size_t Lane = 0;
    TRowSetInstrumentation* Instrumentation = nullptr;

    EFetchResult Fetch(TRowSet& rowSet) const;
};

struct TOutputPort {
    IConnection* Connection = nullptr;
    size_t Lane = 0;
    TRowSetInstrumentation* Instrumentation = nullptr;

    bool CanPush() const;
    bool Push(TRowSet&& rowSet) const;
    void Finish() const;
};

enum class EPortKind { Input, Output };

// Moves the task port of Kind bound to {From, FromLane} to {To, ToLane}; an
// unbound port has a null From. The port keeps its instrumentation.
struct TPortRebind {
    EPortKind Kind = EPortKind::Input;
    const IConnection* From = nullptr;
    size_t FromLane = 0;
    IConnection* To = nullptr;
    size_t ToLane = 0;

    // Whether `port` matched and moved.
    bool Apply(TInputPort& port) const;
    bool Apply(TOutputPort& port) const;
};

} // namespace NScheduler
} // namespace NQdb
