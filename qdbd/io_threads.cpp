#include "io_threads.h"

#include <coroio/all.hpp>
#include <coroio/actors/threaded_inbound.hpp>
#include <coroio/actors/threaded_node.hpp>
#include <coroio/loop_thread.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <vector>

namespace NQdb {
namespace NRemote {

namespace {

using namespace NNet;
using namespace NNet::NActors;

// A node whose transport died would keep answering HTTP while its queries hang, so a
// failed I/O loop ends the process and its supervisor restarts the node.
[[noreturn]] void AbortOnFailure(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        std::cerr << "I/O thread failed: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "I/O thread failed with an unknown exception\n";
    }
    std::abort();
}

template <class TPoller>
class TIoThreads final : public IIoThreads {
public:
    using TSocket = typename TPoller::TSocket;

    explicit TIoThreads(uint32_t threads) {
        for (uint32_t i = 0; i < threads; ++i) {
            Threads_.push_back(std::make_unique<TLoopThread<TPoller>>());
        }
        Resolver_ = std::make_unique<TResolver>(Threads_.front()->Poller());
    }

    std::unique_ptr<INode> MakeNode(TMessagesFactory& factory, const THostPort& hostPort) override {
        auto& poller = Threads_.front()->Poller();
        return std::make_unique<TThreadedNode<TPoller>>(poller, factory, *Resolver_,
            [&poller](const TAddress& addr) {
                return TSocket(poller, addr.Domain());
            },
            hostPort);
    }

    void ServeInbound(TActorSystem& sys, TNodeId nodeId, const TAddress& address) override {
        auto& poller = Threads_.back()->Poller();
        TSocket listener(poller, address.Domain());
        listener.Bind(address);
        listener.Listen();
        Delivery_ = std::make_unique<TQueuedDelivery>(sys);
        // V2 leaves payloads in its read chunks, so received TDataChunk row sets point into them.
        Inbound_ = std::make_unique<TInbound>(poller, std::move(listener), nodeId, *Delivery_);
    }

    void Start() override {
        for (auto& thread : Threads_) {
            thread->Start(AbortOnFailure);
        }
    }

    void Stop() override {
        for (auto& thread : Threads_) {
            thread->Stop();
        }
        for (auto& thread : Threads_) {
            thread->Join();
        }
        Inbound_.reset();
        Delivery_.reset();
    }

private:
    using TInbound = TThreadedInbound<TPoller, TQueuedDelivery, TZeroCopyEnvelopeReaderV2>;

    std::vector<std::unique_ptr<TLoopThread<TPoller>>> Threads_;
    std::unique_ptr<TResolver> Resolver_;
    std::unique_ptr<TQueuedDelivery> Delivery_;
    std::unique_ptr<TInbound> Inbound_;
};

} // namespace

template <class TPoller>
std::unique_ptr<IIoThreads> MakeIoThreads(uint32_t threads) {
    return std::make_unique<TIoThreads<TPoller>>(threads);
}

template std::unique_ptr<IIoThreads> MakeIoThreads<NNet::TSelect>(uint32_t);
template std::unique_ptr<IIoThreads> MakeIoThreads<NNet::TPoll>(uint32_t);
#ifdef HAVE_EPOLL
template std::unique_ptr<IIoThreads> MakeIoThreads<NNet::TEPoll>(uint32_t);
#endif
#ifdef HAVE_URING
template std::unique_ptr<IIoThreads> MakeIoThreads<NNet::TUring>(uint32_t);
#endif
#ifdef HAVE_KQUEUE
template std::unique_ptr<IIoThreads> MakeIoThreads<NNet::TKqueue>(uint32_t);
#endif

} // namespace NRemote
} // namespace NQdb
