#pragma once

#include <qdbd/messages.h>

#include <coroio/actors/actor.hpp>

#include <memory>
#include <vector>

namespace NQdb {
namespace NScheduler {

// Sends `chunk` to `to` as an object, which a node on an I/O thread serializes there
// (net:io_threads > 0). The chunk keeps `layouts`, its Descriptor, until it is written.
// In a unit without TActorSystem::Send<T>, so the code of net:io_threads=0 stays as it is.
void SendToIoThread(NNet::NActors::TActorContext& ctx, NNet::NActors::TActorId to, NRemote::TDataChunk&& chunk,
    std::shared_ptr<const std::vector<TValueLayout>> layouts);

} // namespace NScheduler
} // namespace NQdb
