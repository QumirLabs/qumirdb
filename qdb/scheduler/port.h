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

} // namespace NScheduler
} // namespace NQdb
