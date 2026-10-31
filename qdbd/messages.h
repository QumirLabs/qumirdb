#pragma once
#include <coroio/actors/actor.hpp>
#include <qdb/io/io.h>
#include <qdb/io/rowset_ops.h>

namespace NQdb {
namespace NRemote {

struct TWakeUp {
    static constexpr NNet::NActors::TMessageId MessageId = 10;
};

struct TPing {
    static constexpr NNet::NActors::TMessageId MessageId = 100;
    uint64_t TotalSlots = 0;
    uint64_t UsedSlots = 0;
    uint64_t CpuTime = 0;
    bool Forwarded = false;
};

struct TSpawnComputeActor {
    static constexpr NNet::NActors::TMessageId MessageId = 101;
    uint32_t Count = 1;
    bool Forwarded = false;
};

struct TSpawnComputeActorResponse {
    static constexpr NNet::NActors::TMessageId MessageId = 102;
    std::vector<NNet::NActors::TActorId> ActorIds;
};

// compute-actor

// used for serialization

// Push(srcId, TRowSet&&)
struct TDataChunk {
    static constexpr NNet::NActors::TMessageId MessageId = 200;
    uint32_t DstTaskId;
    uint32_t SrcLaneId;
    TRowSet RowSet;
    const TValueLayout* Descriptor;
};

// Finish(srcId)
struct TFinish {
    static constexpr NNet::NActors::TMessageId MessageId = 201;
    uint32_t DstTaskId;
    uint32_t SrcLaneId;
};

// TODO: Fetch is local, do we need a message for it ?

// client-side
struct TExecuteQuery {
    static constexpr NNet::NActors::TMessageId MessageId = 1000;
    std::string Query;
};

struct TExecuteQueryResponse {
    static constexpr NNet::NActors::TMessageId MessageId = 1001;
    std::string Response;
};

} // namespace NRemote
} // namespace NQdb
