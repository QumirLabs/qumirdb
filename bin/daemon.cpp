#include <charconv>
#include <limits>
#include <optional>
#include <filesystem>

#include <coroio/all.hpp>
#include <coroio/actors/actorsystem.hpp>
#include <qdbd/http/router.h>
#include <qdbd/actors/gateway.h>
#include <qdbd/actors/scheduler.h>
#include <qdbd/actors/local_scheduler.h>
#include <qdbd/actors/runtime_resources.h>
#include <qdbd/actors.h>
#include <qdbd/io_threads.h>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>
#include <qdbd/time_source.h>

#include <qdb/utils/config.h>
#include <qdb/compiler/query.h>
#include <qdb/io/parquet/table_cache.h>
#include <qdb/kernel/query_modules.h>
#include <qdb/plan/ops/source.h>
#include <qdb/catalog/external_module.h>
#include <qdb/scheduler/settings/engine_settings.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
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

// The backends of this build, by coroio's own macros; poll and select always exist.
constexpr std::string_view AvailablePollers =
    "auto"
#ifdef HAVE_EPOLL
    ", epoll"
#endif
#ifdef HAVE_URING
    ", uring"
#endif
#ifdef HAVE_KQUEUE
    ", kqueue"
#endif
    ", poll, select";

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
        << "                        --net:poller=name network poller, one of: " << AvailablePollers << " (default auto)\n"
        << "                        --net:io_threads=n I/O threads of the sockets, which also serialize and parse the\n"
        << "                          data chunks: 0 on the actor thread, 1 for writes and reads, 2 for writes on one\n"
        << "                          and reads on the other (default 0)\n"
        << "  --help                print this text\n";
}

struct TDaemonOptions {
    TNodeId MyNodeId;
    TNodeId LeaderId;
    int HttpPort;
    std::vector<std::tuple<std::string,int,int>> AllHosts;
    std::string DataDir;
    TEngineSettings Engine;
};

template <typename TPoller>
int RunDaemon(const TDaemonOptions& options, const char* pollerName)
{
    const auto myNodeId = options.MyNodeId;
    const auto leaderId = options.LeaderId;
    const auto httpPort = options.HttpPort;
    const auto& allHosts = options.AllHosts;

    TExternalModuleCatalog catalog;
    std::shared_ptr<const NKernel::TModuleBase> base;
    try {
        base = std::make_shared<const NKernel::TModuleBase>();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    const auto& EngineSettings = options.Engine;
    const auto& dataDir = options.DataDir;
    // Shared by planning and the compute actors' graphs.
    auto tablesCache = std::make_shared<TTableCache>();
    // What the node shares with its compute actors; declared before the actor system, so it outlives them.
    auto resources = std::make_shared<TRuntimeResources>();
    resources->Scheduler = EngineSettings.Scheduler.Scheduler;
    resources->Transport = EngineSettings.Scheduler.Transport;
    resources->Tables = tablesCache;
    resources->SerializeOnIoThreads = EngineSettings.IoThreads > 0;
    resources->PerfJit = EngineSettings.Scheduler.Jit.Perf;
    // Frees the graphs of finished queries off the actor thread; at most two wait for it.
    resources->GraphCleaner = std::make_shared<NScheduler::TGraphCleaner>(2);
    // At exit the actor system stops the compute actors, whose workers are back in the pool then;
    // the cleaner frees the retired graphs, and resources->Workers joins its threads last. A failed
    // I/O loop aborts the process (io_threads.cpp) and joins nothing.
    auto compilerFactory = [&]() {
        auto tableFactory = [&dataDir, tablesCache](NQumir::NAst::TTypeFactory& types, std::string_view table)
            -> std::expected<NQdb::TOperatorPtr, NQumir::TError>
        {
            const auto path = ResolveTablePath(dataDir, table);
            try {
                return NQumir::MakeIntrusive<TSourceOperator>(types, tablesCache->Open(path), path);
            } catch (const std::exception& e) {
                return std::unexpected(NQumir::TError(
                    "cannot open table '" + std::string(table) + "': " + e.what()));
            }
        };
        return std::make_unique<NCompiler::TQueryCompiler>(base, catalog, tableFactory, EngineSettings.Optimizer);
    };

    TAddress httpAddress{"::", httpPort};

    using TSocket = typename TPoller::TSocket;
    TLoop<TPoller> loop;
    TResolver resolver(loop.Poller());

    TMessagesFactory factory;
    RegisterMessages(factory);

    // Declared before the actor system: it destroys its nodes first.
    std::unique_ptr<IIoThreads> io;
    if (EngineSettings.IoThreads > 0) {
        io = MakeIoThreads<TPoller>(EngineSettings.IoThreads);
    }
    auto makeNode = [&](const THostPort& hostPort) -> std::unique_ptr<INode> {
        if (io) {
            return io->MakeNode(factory, hostPort);
        }
        return std::make_unique<TNode<TPoller>>(
            loop.Poller(),
            factory,
            resolver,
            [&](const TAddress& addr) {
                return typename TPoller::TSocket(loop.Poller(), addr.Domain());
            },
            hostPort
        );
    };

    // This node listens on the port of its own --node entry.
    int port = 0;
    std::vector<TNodeId> nodeIds;
    TActorSystem sys(&loop.Poller(), myNodeId);
    for (const auto& [host, nodePort, nodeId] : allHosts) {
        if (nodeId != myNodeId) {
            std::cerr << "Adding node with id: " << nodeId << "\n";
            sys.AddNode(nodeId, makeNode(THostPort(host, nodePort)));
        } else {
            port = nodePort;
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

        if (io) {
            io->ServeInbound(sys, myNodeId, actorSysAddress, factory);
        } else {
            actorSysListenSocket.emplace(loop.Poller(), actorSysAddress.Domain());
            actorSysListenSocket->Bind(actorSysAddress);
            actorSysListenSocket->Listen();
        }
    } catch (const std::exception& e) {
        std::cerr << "Cannot listen on http port " << httpPort << " and actor port " << port << ": " << e.what() << "\n";
        return 1;
    }

    const auto httpListenAddress = httpListenSocket->LocalAddr()->ToString();
    const auto actorSysListenAddress = io
        ? actorSysAddress.ToString()
        : actorSysListenSocket->LocalAddr()->ToString();

    auto logger = [](const std::string& msg) {
        std::cerr << "[QumirDB] " << msg << "\n";
    };

    auto gatewayActorId = sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::Gateway), IActor::TPtr(CreateGatewayActor(compilerFactory, EngineSettings.Scheduler, logger)));
    auto router = NHttp::CreateRouter(&sys, gatewayActorId);

    TWebServer<TSocket> server(std::move(*httpListenSocket), *router, logger);
    server.Start();

    if (io) {
        sys.ServeOutbound();
    } else {
        // V2 leaves payloads in its read chunks, so received TDataChunk row sets point into them.
        sys.Serve<TSocket, TZeroCopyEnvelopeReaderV2>(std::move(*actorSysListenSocket));
    }

    TTimeSource timeSource;
    sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::Scheduler), IActor::TPtr(CreateSchedulerActor(&timeSource, leaderId, logger)));

    uint64_t pingPeriodMs = 1000;
    sys.RegisterWellKnown(TLocalActorId(EWellKnownActors::LocalScheduler), IActor::TPtr(CreateLocalScheduler(leaderId, pingPeriodMs, resources)));

    std::cerr << "QumirDB poller: " << pollerName << "\n";
    std::cerr << "QumirDB I/O threads: " << EngineSettings.IoThreads << "\n";
    std::cerr << "QumirDB listening on " << httpListenAddress << ", " << actorSysListenAddress << "\n";
    if (io) {
        io->Start();
    }
    loop.Loop();
    // The inbound side delivers to the actor system, so it is destroyed before it.
    if (io) {
        io->Stop();
    }

    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    NNet::TInitializer init;
    // Compute actors link kernel images.
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
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

    NUtils::TConfig engineConfig(EngineConfigSchema());
    if (auto loaded = LoadEngineConfig(engineConfig, NQdb::NUtils::DefaultConfigPath(), configPath, overrides); !loaded) {
        std::cerr << loaded.error() << "\n";
        return 1;
    }
    TEngineSettings engineSettings;
    ApplyEngineConfig(engineConfig, engineSettings);
    const TDaemonOptions options{myNodeId, leaderId, httpPort, std::move(allHosts), std::move(dataDir), std::move(engineSettings)};

    switch (options.Engine.Poller) {
    case EPollerBackend::Auto:
        return RunDaemon<TDefaultPoller>(options, "auto");
#ifdef HAVE_EPOLL
    case EPollerBackend::Epoll:
        return RunDaemon<TEPoll>(options, "epoll");
#endif
#ifdef HAVE_URING
    case EPollerBackend::Uring:
        return RunDaemon<TUring>(options, "uring");
#endif
#ifdef HAVE_KQUEUE
    case EPollerBackend::Kqueue:
        return RunDaemon<TKqueue>(options, "kqueue");
#endif
    case EPollerBackend::Poll:
        return RunDaemon<TPoll>(options, "poll");
    case EPollerBackend::Select:
        return RunDaemon<TSelect>(options, "select");
    default:
        std::cerr << "Poller backend '" << engineConfig.GetString("net", "poller")
                  << "' is not available on this platform; available: " << AvailablePollers << "\n";
        return 1;
    }
}
