#include <coroio/all.hpp>
#include <coroio/actors/actorsystem.hpp>
#include <qdbd/http/router.h>
#include <qdbd/actors/gateway.h>
#include <qdbd/actors/scheduler.h>
#include <qdbd/actors/local_scheduler.h>
#include <qdbd/actors.h>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>
#include <qdbd/time_source.h>

using namespace NNet;
using namespace NNet::NActors;

using namespace NQdb;
using namespace NQdb::NRemote;

int main(int argc, char** argv) {
    NNet::TInitializer init;
    TNodeId myNodeId = 1;
    TNodeId leaderId = 1;
    int port = 31337;
    int httpPort = 8080;
    std::vector<std::tuple<std::string,int,int>> allHosts;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--http-port") && i + 1 < argc) {
            httpPort = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--node-id") && i + 1 < argc) {
            myNodeId = std::stoi(argv[++i]);
        } else if (!strcmp(argv[i], "--leader-id") && i + 1 < argc) {
            leaderId = std::stoi(argv[++i]);
        } else if (!strcmp(argv[i], "--node") && i + 1 < argc) {
            // Parse host:port:nodeId
            std::string hostPortNode(argv[++i]);
            size_t colon1 = hostPortNode.find(':');
            size_t colon2 = hostPortNode.find(':', colon1 + 1);
            if (colon1 != std::string::npos && colon2 != std::string::npos) {
                std::string host = hostPortNode.substr(0, colon1);
                int port = std::stoi(hostPortNode.substr(colon1 + 1, colon2 - colon1 - 1));
                int nodeId = std::stoi(hostPortNode.substr(colon2 + 1));
                allHosts.emplace_back(host, port, nodeId);
                std::cerr << "Node: " << host << ":" << port << " with id: " << nodeId << "\n";
            } else {
                std::cerr << "Invalid node format. Use host:port:nodeId\n";
                return -1;
            }
        } else if (!std::strcmp(argv[i], "--help")) {
            std::cout
                << "Usage: " << argv[0]
                << " --http-port n \n"
                << " --port n \n"
                << " --node-id n \n"
                << " --leader-id n \n"
                << " --node host:port:id \n"
                ;
            return 0;
        }
    }

    TAddress httpAddress{"::", httpPort};
    TAddress actorSysAddress{"::", port};

    using TPoller = TDefaultPoller;
    using TSocket = typename TPoller::TSocket;
    std::vector<std::tuple<int, std::unique_ptr<TNode<TPoller>>>> nodes;
    TLoop<TPoller> loop;
    TResolver resolver(loop.Poller());

    TMessagesFactory factory;
    RegisterMessages(factory);

    for (auto& [host, port, nodeId] : allHosts) {
        nodes.emplace_back(nodeId, std::make_unique<TNode<TPoller>>(
            loop.Poller(),
            factory,
            resolver,
            [&](const TAddress& addr) {
                return TPoller::TSocket(loop.Poller(), addr.Domain());
            },
            THostPort(host, port)
        ));
    }

    std::vector<TNodeId> nodeIds;
    TActorSystem sys(&loop.Poller(), myNodeId);
    for (auto&& [nodeId, node] : nodes) {
        if (nodeId != myNodeId) {
            std::cerr << "Adding node with id: " << nodeId << "\n";
            sys.AddNode(nodeId, std::move(node));
        } else {
            port = node->GetHostPort().GetPort();
        }
        nodeIds.push_back(nodeId);
    }
    std::sort(nodeIds.begin(), nodeIds.end());
    auto it = std::find(nodeIds.begin(), nodeIds.end(), myNodeId);
    if (it == nodeIds.end()) {
        std::cerr << "Node with id: " << myNodeId << " is not registered.\n";
        return -1;
    }

    TSocket httpListenSocket(loop.Poller(), httpAddress.Domain());
    httpListenSocket.Bind(httpAddress);
    httpListenSocket.Listen();

    TSocket actorSysListenSocket(loop.Poller(), actorSysAddress.Domain());
    actorSysListenSocket.Bind(actorSysAddress);
    actorSysListenSocket.Listen();

    const auto httpListenAddress = httpListenSocket.LocalAddr()->ToString();
    const auto actorSysListenAddress = actorSysListenSocket.LocalAddr()->ToString();

    auto logger = [](const std::string& msg) {
        std::cerr << "[QumirDB] " << msg << "\n";
    };

    auto gatewayActorId = sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::Gateway), IActor::TPtr(CreateGatewayActor(logger)));
    auto router = NHttp::CreateRouter(&sys, gatewayActorId);

    TWebServer<TSocket> server(std::move(httpListenSocket), *router, logger);
    server.Start();

    sys.Serve(std::move(actorSysListenSocket));

    TTimeSource timeSource;
    sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::Scheduler), IActor::TPtr(CreateSchedulerActor(&timeSource, leaderId)));

    uint64_t pingPeriodMs = 1000;
    sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::LocalScheduler), IActor::TPtr(CreateLocalScheduler(&timeSource, leaderId, pingPeriodMs)));

    std::cerr << "QumirDB listening on " << httpListenAddress << ", " << actorSysListenAddress << "\n";
    loop.Loop();

    return 0;
}
