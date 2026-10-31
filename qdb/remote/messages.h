#pragma once
#include <coroio/actors/actor.hpp>
#include <qdb/io/io.h>

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
};

struct TSpawnComputeActor {
    static constexpr NNet::NActors::TMessageId MessageId = 101;
    uint32_t Count = 1;
};

struct TSpawnComputeActorResponse {
    static constexpr NNet::NActors::TMessageId MessageId = 102;
    uint32_t Count = 1;
    std::vector<NNet::NActors::TActorId> ActorIds;
};

// compute-actor
struct TDataChunk {
    static constexpr NNet::NActors::TMessageId MessageId = 200;
    uint32_t TaskId;
    uint32_t ChannelId;
    TRowSet RowSet;
};

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
