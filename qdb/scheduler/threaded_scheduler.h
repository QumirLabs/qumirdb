#pragma once

namespace NQdb {
namespace NScheduler {

struct TTaskNode;

class IThreadedScheduler {
public:
    virtual ~IThreadedScheduler() = default;

    virtual void Schedule(TTaskNode* task) = 0;
};

} // namespace NScheduler
} // namespace NQdb
