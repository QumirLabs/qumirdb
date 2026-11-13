#pragma once
#include <coroio/actors/actor.hpp>
#include <qdb/exec/runtime_filter.h>
#include <qdb/io/io.h>
#include <qdb/io/rowset_ops.h>

#include <qumir/error.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

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

// The receiving RemoteSource is selected by DstTaskId and pushes the row sets
// into its own output lane in order; a sink packs consecutive small row sets
// into one chunk. The chunk owns one reference to each row set and releases
// those no receiver takes, so a chunk nobody handles frees its rows too. Within
// a node the receiver gets the sender's chunk; for another node it is
// serialized, then destroyed.
struct TDataChunk {
    static constexpr NNet::NActors::TMessageId MessageId = 200;
    uint64_t DstTaskId = 0;
    // Numbers the chunks of one DstTaskId from 1; TDataConfirm acknowledges them.
    uint64_t Seq = 0;
    // Each held while RefCount > 0; a view with RefCount 0 is never released.
    std::vector<TRowSet> RowSets;
    // The sender's value layouts, one per column; null in a received chunk.
    const TValueLayout* Descriptor = nullptr;

    TDataChunk() = default;

    TDataChunk(uint64_t dstTaskId, uint64_t seq, std::vector<TRowSet> rowSets, const TValueLayout* descriptor = nullptr)
        : DstTaskId(dstTaskId)
        , Seq(seq)
        , RowSets(std::move(rowSets))
        , Descriptor(descriptor)
    {}

    TDataChunk(TDataChunk&& other) noexcept
        : DstTaskId(other.DstTaskId)
        , Seq(other.Seq)
        , RowSets(other.Take())
        , Descriptor(other.Descriptor)
    {}

    TDataChunk& operator=(TDataChunk&& other) noexcept
    {
        if (this != &other) {
            ReleaseRowSets();
            DstTaskId = other.DstTaskId;
            Seq = other.Seq;
            RowSets = other.Take();
            Descriptor = other.Descriptor;
        }
        return *this;
    }

    ~TDataChunk()
    {
        ReleaseRowSets();
    }

    // Hands the references over: the chunk then holds none.
    std::vector<TRowSet> Take()
    {
        return std::exchange(RowSets, {});
    }

private:
    void ReleaseRowSets()
    {
        for (auto& rowSet : RowSets) {
            if (rowSet.RefCount > 0) {
                Release(&rowSet);
            }
        }
        RowSets.clear();
    }
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

// Runtime filter sink -> source: the merged partial of a fragment's build
// lanes. The receiving source is selected by DstTaskId; there is no confirm.
struct TFilterPartial {
    static constexpr NNet::NActors::TMessageId MessageId = 206;
    uint64_t DstTaskId = 0;
    TRuntimeFilterPartial Partial;
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
    FilterSource,
    FilterSink,
    CteSource,
    CteSink,
};

// Id is the node's external id in the graph; routing lives here, not in the graph.
struct TRemoteEndpoint {
    uint64_t Id = 0;
    ERemoteEndpointKind Kind = ERemoteEndpointKind::Source;
    // Sink: where the rows or the partial go. Source: where the confirms go (the
    // peer sink).
    NNet::NActors::TActorId PeerActor;
    uint64_t PeerTaskId = 0;
};

// Query actor -> compute actor: the graph to run, which the compute actor builds and owns.
struct TTaskGraphMessage {
    static constexpr NNet::NActors::TMessageId MessageId = 202;
    // SerializeGraphSpec bytes.
    std::string Spec;
    // SerializeKernelImage bytes of the query's kernels, which the spec's records index.
    std::string KernelImage;
    std::vector<TRemoteEndpoint> Endpoints;
};

// TODO: Fetch is local, do we need a message for it ?

// client-side
struct TExecuteQuery {
    static constexpr NNet::NActors::TMessageId MessageId = 1000;
    using LocalMessage = void;
    std::string Query;
    // The scheduler workers to lower the plan for, e.g. those of the whole
    // cluster; without it the query actor's own settings decide.
    std::optional<size_t> PlanWidth;
};

// What a distributed run spent, like the local run: Planning is prepare after
// parsing, KernelBuild lowers, seals and splits the graph, JitLLVM builds the
// kernel image. Execute runs from sending the fragments to the last chunk, so it
// includes the compute actors' image load. Parsing and the wait for the compute
// actors are in no phase.
struct TQueryTiming {
    uint64_t Rows = 0;
    double Planning = 0;
    double KernelBuild = 0;
    double JitLLVM = 0;
    double Execute = 0;
};

struct TExecuteQueryResponse {
    static constexpr NNet::NActors::TMessageId MessageId = 1001;
    using LocalMessage = void;
    std::expected<std::string, NQumir::TError> Response;
    // The error is a lack of compute nodes, not a bad query: the client may retry.
    bool Unavailable = false;
    // Set for an executed query only.
    std::optional<TQueryTiming> Timing;
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
    factory.template RegisterSerializer<TFilterPartial>();
    factory.template RegisterSerializer<TComputeStatus>();
    factory.template RegisterSerializer<TComputeError>();
    factory.template RegisterSerializer<TTaskGraphMessage>();
    factory.template RegisterSerializer<TExecuteQuery>();
    factory.template RegisterSerializer<TExecuteQueryResponse>();
}


} // namespace NRemote
} // namespace NQdb
