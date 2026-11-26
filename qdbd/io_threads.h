#pragma once

#include <coroio/actors/actorsystem.hpp>

#include <cstdint>
#include <memory>

namespace NQdb {
namespace NRemote {

// The sockets of qdbd on I/O threads (net:io_threads): the nodes to the peers write on
// the first thread, the inbound connections are read on the last one; the actors stay
// on the thread of the actor system.
// In a unit of its own: with a second INode in the unit, gcc devirtualizes the INode
// calls of TActorSystem::Send<T> differently, so net:io_threads=0 would change too.
class IIoThreads {
public:
    virtual ~IIoThreads() = default;

    // A node to a peer, for TActorSystem::AddNode.
    virtual std::unique_ptr<NNet::NActors::INode> MakeNode(
        NNet::NActors::TMessagesFactory& factory, const NNet::THostPort& hostPort) = 0;
    // Listens on `address` and delivers to `sys`, which then serves with ServeOutbound();
    // throws if it cannot listen.
    virtual void ServeInbound(
        NNet::NActors::TActorSystem& sys, NNet::NActors::TNodeId nodeId, const NNet::TAddress& address) = 0;
    // A loop that fails aborts the process, see io_threads.cpp.
    virtual void Start() = 0;
    // Stops and joins the threads, then destroys the inbound side while `sys` is alive; the
    // nodes go with `sys`. Safe without Start().
    virtual void Stop() = 0;
};

// Declare the result before the actor system: its nodes live on these threads.
template <class TPoller>
std::unique_ptr<IIoThreads> MakeIoThreads(uint32_t threads);

} // namespace NRemote
} // namespace NQdb
