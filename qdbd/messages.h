#pragma once
#include "qdb/scheduler/graph.h"
#include <coroio/actors/actor.hpp>
#include <qdb/io/io.h>
#include <qdb/io/rowset_ops.h>

#include <qumir/error.h>

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

// Query -> Scheduler: select nodes; Collector -> LocalScheduler: create actors [private protocol].
struct TSpawnComputeActor {
    static constexpr NNet::NActors::TMessageId MessageId = 101;
    uint32_t Count = 1;
    bool Forwarded = false;
};

// Scheduler -> Query: distinct LocalSchedulerIds; LocalScheduler -> Collector -> Query: compute IDs.
struct TSpawnComputeActorResponse {
    static constexpr NNet::NActors::TMessageId MessageId = 102;
    std::vector<NNet::NActors::TActorId> ActorIds;
    std::string Error;
};

// The receiving RemoteSource is selected by DstTaskId and pushes into its own output lane.
struct TDataChunk {
    static constexpr NNet::NActors::TMessageId MessageId = 200;
    uint64_t DstTaskId = 0;
    // Numbers the chunks of one DstTaskId from 1; TDataConfirm acknowledges them.
    uint64_t Seq = 0;
    TRowSet RowSet{};
    const TValueLayout* Descriptor = nullptr;

    // Force coroio to serialize the payload instead of copying its pointers.
    ~TDataChunk() {}
};

// The receiving RemoteSource is selected by DstTaskId and finishes its own output lane.
struct TFinish {
    static constexpr NNet::NActors::TMessageId MessageId = 201;
    uint64_t DstTaskId = 0;
};

// The consumer's source task has taken every chunk up to LastApplied; SinkId
// selects the producer's sink, which may then send more.
struct TDataConfirm {
    static constexpr NNet::NActors::TMessageId MessageId = 203;
    uint64_t SinkId = 0;
    uint64_t LastApplied = 0;
};

// Compute actor -> query actor, periodically while the graph runs: the actor is alive.
struct TComputeStatus {
    static constexpr NNet::NActors::TMessageId MessageId = 204;
};

// Compute actor -> query actor: a protocol or startup error; the query actor decides what to do.
struct TComputeError {
    static constexpr NNet::NActors::TMessageId MessageId = 205;
    std::string Message;
};

enum class ERemoteEndpointKind {
    Source,
    Sink,
};

// Id is the node's external id in the graph; routing lives here, not in the graph.
struct TRemoteEndpoint {
    uint64_t Id = 0;
    ERemoteEndpointKind Kind = ERemoteEndpointKind::Source;
    // Sink: where the rows go. Source: where the confirms go (the peer sink).
    NNet::NActors::TActorId PeerActor;
    uint64_t PeerTaskId = 0;
};

struct TTaskGraphMessage {
    static constexpr NNet::NActors::TMessageId MessageId = 202;
    using LocalMessage = void; // TODO: must be serializable
    NScheduler::TSealedTaskGraph Graph;
    std::vector<TRemoteEndpoint> Endpoints;
    std::vector<TGeneratedKernel> Kernels;
};

// TODO: Fetch is local, do we need a message for it ?

// client-side
struct TExecuteQuery {
    static constexpr NNet::NActors::TMessageId MessageId = 1000;
    using LocalMessage = void;
    std::string Query;
};

struct TExecuteQueryResponse {
    static constexpr NNet::NActors::TMessageId MessageId = 1001;
    using LocalMessage = void;
    std::expected<std::string, NQumir::TError> Response;
    // The error is a lack of compute nodes, not a bad query: the client may retry.
    bool Unavailable = false;
};

template<typename TMessagesFactory>
inline void RegisterMessages(TMessagesFactory& factory) {
    factory.template RegisterSerializer<TWakeUp>();
    factory.template RegisterSerializer<TPing>();
    factory.template RegisterSerializer<TSpawnComputeActor>();
    factory.template RegisterSerializer<TSpawnComputeActorResponse>();
    factory.template RegisterSerializer<TDataChunk>();
    factory.template RegisterSerializer<TFinish>();
    factory.template RegisterSerializer<TDataConfirm>();
    factory.template RegisterSerializer<TComputeStatus>();
    factory.template RegisterSerializer<TComputeError>();
    factory.template RegisterSerializer<TExecuteQuery>();
    factory.template RegisterSerializer<TExecuteQueryResponse>();
}


} // namespace NRemote
} // namespace NQdb
