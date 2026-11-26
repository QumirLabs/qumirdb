#include "io_thread_chunk.h"

namespace NQdb {
namespace NScheduler {

namespace {

// The I/O thread writes and destroys it, maybe after the task is gone: it keeps the layouts,
// and its row sets are its own copies that own their data (see AdoptKernelOwnedRowSet).
struct TChunkWithLayouts : NRemote::TDataChunk {
    TChunkWithLayouts(NRemote::TDataChunk&& chunk, std::shared_ptr<const std::vector<TValueLayout>> layouts)
        : NRemote::TDataChunk(std::move(chunk))
        , Layouts(std::move(layouts))
    {
        Descriptor = Layouts->data();
    }

    std::shared_ptr<const std::vector<TValueLayout>> Layouts;
};

} // namespace

void SendToIoThread(NNet::NActors::TActorContext& ctx, NNet::NActors::TActorId to, NRemote::TDataChunk&& chunk,
    std::shared_ptr<const std::vector<TValueLayout>> layouts)
{
    using NNet::NActors::TBlob;
    auto owned = std::make_unique<TChunkWithLayouts>(std::move(chunk), std::move(layouts));
    // The blob holds the TDataChunk part, as a Near blob of a TDataChunk does.
    NRemote::TDataChunk* message = owned.release();
    TBlob blob{
        TBlob::TRawPtr(message, NNet::NActors::TBlobDeleter{[](void* ptr) {
            delete static_cast<TChunkWithLayouts*>(static_cast<NRemote::TDataChunk*>(ptr));
        }}),
        sizeof(NRemote::TDataChunk),
        TBlob::PointerType::Near,
    };
    ctx.Send(to, NRemote::TDataChunk::MessageId, std::move(blob));
}

} // namespace NScheduler
} // namespace NQdb
