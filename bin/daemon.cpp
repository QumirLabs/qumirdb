#include <charconv>
#include <limits>
#include <optional>

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

#include <qdb/utils/config.h>
#include <qdb/compiler/query.h>
#include <qdb/plan/ops/source.h>
#include <qdb/catalog/external_module.h>
#include <qdb/scheduler/settings/engine_settings.h>

#include <qumir/error.h>

using namespace NNet;
using namespace NNet::NActors;

using namespace NQdb;
using namespace NQdb::NRemote;

namespace {

std::string ResolveTablePath(const std::string& dataDir, std::string_view table)
{
    if (table.ends_with(".parquet")) {
        return std::string(table);
    }
    if (dataDir.empty()) {
        return std::string(table) + ".parquet";
    }
    return dataDir + "/" + std::string(table) + ".parquet";
}

std::optional<int> ParseNumber(std::string_view flag, const std::string& text, int min, int max)
{
    int value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (text.empty() || ec != std::errc() || ptr != end || value < min || value > max) {
        std::cerr << "Invalid value '" << text << "' for " << flag
                  << ": expected an integer in [" << min << ", " << max << "]\n";
        return std::nullopt;
    }
    return value;
}

void PrintUsage(std::ostream& out, const char* program)
{
    out << "Usage: " << program << " [options]\n"
        << "  --http-port n         HTTP port (default 8080)\n"
        << "  --node-id n           id of this node (default 1)\n"
        << "  --leader-id n         id of the leader node (default 1)\n"
        << "  --node host:port:id   cluster node, repeat for every node; required, one entry must have this node's id\n"
        << "  --data dir            directory with <table>.parquet files\n"
        << "  --config path         engine config file\n"
        << "  --section:key=value   engine config override\n"
        << "  --help                print this text\n";
}

} // namespace

int main(int argc, char** argv)
{
    NNet::TInitializer init;
    TNodeId myNodeId = 1;
    TNodeId leaderId = 1;
    int httpPort = 8080;
    std::vector<std::tuple<std::string,int,int>> allHosts;
    std::optional<std::string> configPath;
    std::string dataDir;
    std::vector<std::string> overrides;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") {
            PrintUsage(std::cout, argv[0]);
            return 0;
        }
        if (NQdb::NUtils::IsConfigOverride(arg)) {
            overrides.emplace_back(argv[i] + 2);
            continue;
        }
        const bool known = arg == "--http-port" || arg == "--node-id"
            || arg == "--leader-id" || arg == "--node" || arg == "--data" || arg == "--config";
        if (!known) {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage(std::cerr, argv[0]);
            return 1;
        }
        if (i + 1 >= argc) {
            std::cerr << arg << " requires a value\n";
            return 1;
        }
        const std::string value = argv[++i];
        if (arg == "--http-port") {
            const auto parsed = ParseNumber(arg, value, 0, 65535);
            if (!parsed) {
                return 1;
            }
            httpPort = *parsed;
        } else if (arg == "--node-id" || arg == "--leader-id") {
            const auto parsed = ParseNumber(arg, value, 0, std::numeric_limits<int>::max());
            if (!parsed) {
                return 1;
            }
            (arg == "--node-id" ? myNodeId : leaderId) = *parsed;
        } else if (arg == "--node") {
            // host:port:nodeId
            const auto colon2 = value.rfind(':');
            const auto colon1 = colon2 == std::string::npos || colon2 == 0 ? std::string::npos : value.rfind(':', colon2 - 1);
            if (colon1 == std::string::npos) {
                std::cerr << "Invalid node format. Use host:port:nodeId\n";
                return 1;
            }
            const auto nodePort = ParseNumber(arg, value.substr(colon1 + 1, colon2 - colon1 - 1), 1, 65535);
            const auto nodeId = ParseNumber(arg, value.substr(colon2 + 1), 0, std::numeric_limits<int>::max());
            if (!nodePort || !nodeId) {
                return 1;
            }
            allHosts.emplace_back(value.substr(0, colon1), *nodePort, *nodeId);
            std::cerr << "Node: " << value.substr(0, colon1) << ":" << *nodePort << " with id: " << *nodeId << "\n";
        } else if (arg == "--data") {
            dataDir = value;
        } else {
            configPath = value;
        }
    }

    std::unordered_map<std::string, std::unique_ptr<TParquetSource>> sources;
    TExternalModuleCatalog catalog;
    auto tableFactory = [&](std::string_view table)
        -> std::expected<NQdb::TOperatorPtr, NQumir::TError>
    {
        const auto path = ResolveTablePath(dataDir, table);
        try {
            auto& source = sources[path];
            if (!source) {
                source = std::make_unique<TParquetSource>(path);
            }
            auto op = std::make_shared<TSourceOperator>(*source, path);
            return op;
        } catch (const std::exception& e) {
            return std::unexpected(NQumir::TError(
                "cannot open table '" + std::string(table) + "': " + e.what()));
        }
    };

    NUtils::TConfig engineConfig(EngineConfigSchema());
    if (auto loaded = LoadEngineConfig(engineConfig, NQdb::NUtils::DefaultConfigPath(), configPath, overrides); !loaded) {
        std::cerr << loaded.error() << "\n";
        return 1;
    }
    TEngineSettings EngineSettings;
    ApplyEngineConfig(engineConfig, EngineSettings);
    NCompiler::TQueryCompiler compiler(catalog, tableFactory, EngineSettings.Optimizer);

    TAddress httpAddress{"::", httpPort};

    using TPoller = TDefaultPoller;
    using TSocket = typename TPoller::TSocket;
    std::vector<std::tuple<int, std::unique_ptr<TNode<TPoller>>>> nodes;
    TLoop<TPoller> loop;
    TResolver resolver(loop.Poller());

    TMessagesFactory factory;
    RegisterMessages(factory);

    for (auto& [host, nodePort, nodeId] : allHosts) {
        nodes.emplace_back(nodeId, std::make_unique<TNode<TPoller>>(
            loop.Poller(),
            factory,
            resolver,
            [&](const TAddress& addr) {
                return TPoller::TSocket(loop.Poller(), addr.Domain());
            },
            THostPort(host, nodePort)
        ));
    }

    // This node listens on the port of its own --node entry.
    int port = 0;
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
        std::cerr << "Node with id: " << myNodeId << " is not registered. Pass --node host:port:id for it.\n";
        return 1;
    }

    TAddress actorSysAddress{"::", port};
    std::optional<TSocket> httpListenSocket;
    std::optional<TSocket> actorSysListenSocket;
    try {
        httpListenSocket.emplace(loop.Poller(), httpAddress.Domain());
        httpListenSocket->Bind(httpAddress);
        httpListenSocket->Listen();

        actorSysListenSocket.emplace(loop.Poller(), actorSysAddress.Domain());
        actorSysListenSocket->Bind(actorSysAddress);
        actorSysListenSocket->Listen();
    } catch (const std::exception& e) {
        std::cerr << "Cannot listen on http port " << httpPort << " and actor port " << port << ": " << e.what() << "\n";
        return 1;
    }

    const auto httpListenAddress = httpListenSocket->LocalAddr()->ToString();
    const auto actorSysListenAddress = actorSysListenSocket->LocalAddr()->ToString();

    auto logger = [](const std::string& msg) {
        std::cerr << "[QumirDB] " << msg << "\n";
    };

    auto gatewayActorId = sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::Gateway), IActor::TPtr(CreateGatewayActor(compiler, logger)));
    auto router = NHttp::CreateRouter(&sys, gatewayActorId);

    TWebServer<TSocket> server(std::move(*httpListenSocket), *router, logger);
    server.Start();

    sys.Serve(std::move(*actorSysListenSocket));

    TTimeSource timeSource;
    sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::Scheduler), IActor::TPtr(CreateSchedulerActor(&timeSource, leaderId, logger)));

    uint64_t pingPeriodMs = 1000;
    sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::LocalScheduler), IActor::TPtr(CreateLocalScheduler(leaderId, pingPeriodMs)));

    std::cerr << "QumirDB listening on " << httpListenAddress << ", " << actorSysListenAddress << "\n";
    loop.Loop();

    return 0;
}
