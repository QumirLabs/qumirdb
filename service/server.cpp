#include <coroio/all.hpp>
#include <coroio/http/httpd.hpp>
#include <coroio/pipe/pipe.hpp>

#include <qdb/io/parquet/source.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/plan/stats_codec.h>
#include <qdb/utils/sha1.h>

#include <qumir/parser/core/printer.h>

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

using namespace NNet;

namespace {

// Dataset descriptor read from every --data-dir.
constexpr std::string_view DatasetMapName = "datasets.map";

struct TOptions {
    std::function<TPipe(const std::string&, const std::vector<std::string>&, bool)> PipeFactory;
    NNet::TPollerBase* Poller = nullptr;
    std::string StaticDir = QDB_SERVICE_STATIC_DIR;
    std::string BinaryDir = QDB_BUILD_BIN_DIR;
    std::string CacheDir;
    std::string SharedLinksDir = "shared";
    std::vector<std::string> DataDirs;
    std::vector<std::string> LocalDataDirs;
    // Directories holding a dataset map; see DatasetMapName.
    std::vector<std::string> DataRoots;
};

enum class EDatasetKind {
    Server,
    Local,
};

struct TServerDataset {
    std::string Id;
    std::string Name;
    std::filesystem::path Path;
    EDatasetKind Kind = EDatasetKind::Server;
    llvm::json::Object Json;
};

struct TRunStats {
    bool Found = false;
    int64_t Rows = 0;
    double Seconds = 0.0;
};

TRunStats ParseRunStats(std::string_view stderrText) {
    static constexpr std::string_view prefix = "Returned ";
    auto pos = stderrText.rfind(prefix);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += prefix.size();

    auto rowsEnd = stderrText.find(" rows in ", pos);
    if (rowsEnd == std::string_view::npos) {
        return {};
    }
    auto secondsBegin = rowsEnd + std::string_view(" rows in ").size();
    auto secondsEnd = stderrText.find(" seconds", secondsBegin);
    if (secondsEnd == std::string_view::npos) {
        return {};
    }

    TRunStats stats;
    std::istringstream rowsIn(std::string(stderrText.substr(pos, rowsEnd - pos)));
    std::istringstream secondsIn(
        std::string(stderrText.substr(secondsBegin, secondsEnd - secondsBegin)));
    if ((rowsIn >> stats.Rows) && (secondsIn >> stats.Seconds)) {
        stats.Found = true;
    }
    return stats;
}

std::string JsonEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 16);
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[ch >> 4];
                    out += hex[ch & 0x0f];
                } else {
                    out += static_cast<char>(ch);
                }
                break;
        }
    }
    return out;
}

std::string ToJsonString(llvm::json::Value value) {
    std::string out;
    llvm::raw_string_ostream os(out);
    os << value;
    return out;
}

llvm::json::Object ErrorJson(std::string stage, std::string message) {
    return llvm::json::Object{
        {"ok", false},
        {"error", llvm::json::Object{
            {"stage", std::move(stage)},
            {"message", std::move(message)},
        }},
    };
}

std::string BaseName(const std::filesystem::path& path) {
    auto name = path.filename().string();
    return name.empty() ? path.string() : name;
}

std::string Trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}

// The engine is packaged separately: keep the first line of its --version output
// and drop control characters.
std::string SanitizeVersion(std::string_view raw) {
    std::string out;
    for (char c : raw) {
        if (c == '\n' || c == '\r') {
            break;
        }
        if (static_cast<unsigned char>(c) >= 0x20) {
            out += c;
        }
    }
    return Trim(out);
}

std::filesystem::path ExpandUserPath(std::string_view value) {
    std::string text = Trim(value);
    if (text == "~" || text.starts_with("~/")) {
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) /
                (text.size() == 1 ? std::filesystem::path{} : std::filesystem::path(text.substr(2)));
        }
    }
    return std::filesystem::path(std::move(text));
}

bool IsDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_directory(path, ec);
}

struct TDataDirArg {
    std::filesystem::path Path;
    std::optional<std::string> Alias;
};

// Paths from a dataset map are relative to the directory holding it.
std::filesystem::path ResolveDataPath(
    std::string_view value, const std::filesystem::path& base)
{
    auto path = ExpandUserPath(value);
    if (base.empty() || path.is_absolute()) {
        return path;
    }
    return base / path;
}

TDataDirArg ParseDataDirArg(
    std::string_view value, const std::filesystem::path& base = {})
{
    const std::string text = Trim(value);
    auto directPath = ResolveDataPath(text, base);
    if (IsDirectory(directPath)) {
        return {.Path = std::move(directPath)};
    }

    const auto end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) {
        return {.Path = std::move(directPath)};
    }
    const auto aliasBegin = text.find_last_of(" \t\r\n", end);
    if (aliasBegin == std::string::npos) {
        return {.Path = std::move(directPath)};
    }

    auto path = ResolveDataPath(text.substr(0, aliasBegin), base);
    auto alias = Trim(std::string_view(text).substr(aliasBegin, end - aliasBegin + 1));
    if (alias.empty() || !IsDirectory(path)) {
        return {.Path = std::move(directPath)};
    }
    return {.Path = std::move(path), .Alias = std::move(alias)};
}

struct TDataDirEntry {
    EDatasetKind Kind = EDatasetKind::Server;
    TDataDirArg Dir;
};

// `<server|local> <path> [alias]` per line, '#' starts a comment.
std::vector<TDataDirEntry> ReadDatasetMap(const std::filesystem::path& root) {
    std::vector<TDataDirEntry> entries;
    const auto mapPath = root / DatasetMapName;
    std::ifstream input(mapPath);
    if (!input) {
        std::cerr << "[QumirDB Web] no dataset map: " << mapPath << "\n";
        return entries;
    }

    for (std::string line; std::getline(input, line); ) {
        const std::string text = Trim(line);
        if (text.empty() || text.front() == '#') {
            continue;
        }
        const auto kindEnd = text.find_first_of(" \t");
        const std::string kindText = text.substr(0, kindEnd);
        const std::string rest =
            kindEnd == std::string::npos ? "" : Trim(std::string_view(text).substr(kindEnd));
        if (rest.empty()) {
            std::cerr << "[QumirDB Web] " << mapPath << ": missing path in: " << text << "\n";
            continue;
        }

        EDatasetKind kind;
        if (kindText == "server") {
            kind = EDatasetKind::Server;
        } else if (kindText == "local") {
            kind = EDatasetKind::Local;
        } else {
            std::cerr << "[QumirDB Web] " << mapPath << ": unknown dataset kind '"
                      << kindText << "'\n";
            continue;
        }
        entries.push_back({.Kind = kind, .Dir = ParseDataDirArg(rest, root)});
    }
    return entries;
}

std::string SchemaTypeName(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;

    if (!type) {
        return "unknown";
    }
    if (auto nullable = TMaybeType<NQdb::TNullable>(type)) {
        return "<named Nullable [" +
            SchemaTypeName(nullable.Cast()->UnderlyingType) + "]>";
    }
    if (auto decimal = NQdb::DecimalSpecOfValueType(type)) {
        return "<named Decimal [" + std::to_string(decimal->Precision) + " " +
            std::to_string(decimal->Scale) + "]>";
    }
    return NQumir::NAst::NCore::PrintType(type);
}

std::string TableNameFromPath(const std::filesystem::path& path) {
    auto name = path.filename().string();
    const std::string suffix = ".parquet";
    if (name.ends_with(suffix)) {
        name.resize(name.size() - suffix.size());
    }
    return name;
}

std::optional<llvm::json::Value> DecodeJsonStatsScalar(
    uint64_t value,
    const NQumir::NAst::TTypePtr& type)
{
    switch (NQdb::StatsScalarKind(type)) {
        case NQdb::EStatsScalarKind::None:
            return std::nullopt;
        case NQdb::EStatsScalarKind::Float:
            return llvm::json::Value(std::bit_cast<double>(value));
        case NQdb::EStatsScalarKind::Int:
            return llvm::json::Value(std::bit_cast<int64_t>(value));
    }
    return std::nullopt;
}

llvm::json::Array ColumnHistogramJson(
    const std::vector<uint64_t>& histogram,
    const NQumir::NAst::TTypePtr& type)
{
    llvm::json::Array out;
    out.reserve(histogram.size());
    for (uint64_t bucket : histogram) {
        if (auto decoded = DecodeJsonStatsScalar(bucket, type)) {
            out.push_back(std::move(*decoded));
        }
    }
    return out;
}

llvm::json::Array ColumnStatsJson(const NQdb::TParquetFile& source) {
    llvm::json::Array columns;
    auto stats = source.Stats();
    if (!stats) {
        return columns;
    }

    for (const auto& column : source.Schema().Columns) {
        const auto name = std::string(column.Name);
        auto it = stats->ColumnStats.find(name);
        if (it == stats->ColumnStats.end() || !it->second) {
            continue;
        }
        const auto& columnStats = *it->second;
        llvm::json::Object out{
            {"name", name},
        };
        if (columnStats.Ndv) {
            out["ndv"] = static_cast<int64_t>(*columnStats.Ndv);
            out["ndv_exact"] = columnStats.NdvIsExact;
        }
        if (columnStats.NullCount) {
            out["null_count"] = static_cast<int64_t>(*columnStats.NullCount);
        }
        if (columnStats.MinValue) {
            if (auto decoded = DecodeJsonStatsScalar(*columnStats.MinValue, column.Type)) {
                out["min"] = std::move(*decoded);
            }
        }
        if (columnStats.MaxValue) {
            if (auto decoded = DecodeJsonStatsScalar(*columnStats.MaxValue, column.Type)) {
                out["max"] = std::move(*decoded);
            }
        }
        if (!columnStats.Histogram.empty()) {
            auto histogram = ColumnHistogramJson(columnStats.Histogram, column.Type);
            if (!histogram.empty()) {
                out["histogram"] = std::move(histogram);
            }
        }
        if (out.size() > 1) {
            columns.push_back(std::move(out));
        }
    }
    return columns;
}

std::string UrlEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 0x0f];
        }
    }
    return out;
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = HexValue(value[i + 1]);
            const int lo = HexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += value[i] == '+' ? ' ' : value[i];
    }
    return out;
}

constexpr size_t MaxSharePayloadSize = 1024 * 1024;
constexpr size_t SharedIdLength = 12;
constexpr size_t LegacySharedIdLength = 24;

TFuture<std::optional<std::string>> ReadBodyLimited(
    TRequest& request,
    size_t limit)
{
    std::string body;
    char buffer[4096];
    for (;;) {
        const ssize_t read = co_await request.ReadBodySome(buffer, sizeof(buffer));
        if (read < 0) {
            continue;
        }
        if (read == 0) {
            co_return body;
        }
        const size_t size = static_cast<size_t>(read);
        if (size > limit - body.size()) {
            co_return std::nullopt;
        }
        body.append(buffer, size);
    }
}

std::string Utf8Prefix(llvm::StringRef value, size_t maxBytes) {
    if (value.size() <= maxBytes) {
        return std::string(value);
    }
    size_t size = maxBytes;
    while (size > 0 &&
           (static_cast<unsigned char>(value[size]) & 0xc0) == 0x80)
    {
        --size;
    }
    return std::string(value.take_front(size));
}

std::string SharedId(std::string_view data) {
    const std::string hex = NQdb::NUtils::Sha1Hex(data);
    unsigned char digest[9];
    for (size_t i = 0; i < std::size(digest); ++i) {
        digest[i] = static_cast<unsigned char>(
            (HexValue(hex[i * 2]) << 4) | HexValue(hex[i * 2 + 1]));
    }

    static constexpr char Base64Url[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string id;
    id.reserve(SharedIdLength);
    for (size_t i = 0; i < std::size(digest); i += 3) {
        id += Base64Url[digest[i] >> 2];
        id += Base64Url[((digest[i] & 0x03) << 4) | (digest[i + 1] >> 4)];
        id += Base64Url[((digest[i + 1] & 0x0f) << 2) | (digest[i + 2] >> 6)];
        id += Base64Url[digest[i + 2] & 0x3f];
    }
    return id;
}

bool IsValidSharedId(std::string_view id) {
    if (id.size() == SharedIdLength) {
        return std::ranges::all_of(id, [](char ch) {
            return (ch >= '0' && ch <= '9') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_';
        });
    }
    return id.size() == LegacySharedIdLength &&
        std::ranges::all_of(id, [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
}

} // namespace

class TRouter final : public IRouter {
public:
    explicit TRouter(TOptions options)
        : Options_(std::move(options))
    {
        std::error_code ec;
        StaticBase_ = std::filesystem::weakly_canonical(Options_.StaticDir, ec);
        if (ec || StaticBase_.empty()) {
            StaticBase_ = std::filesystem::path(Options_.StaticDir).lexically_normal();
        }
        BinaryBase_ = std::filesystem::weakly_canonical(Options_.BinaryDir, ec);
        if (ec || BinaryBase_.empty()) {
            BinaryBase_ = std::filesystem::path(Options_.BinaryDir).lexically_normal();
        }
        SharedLinksBase_ = std::filesystem::absolute(Options_.SharedLinksDir, ec);
        if (ec || SharedLinksBase_.empty()) {
            SharedLinksBase_ =
                std::filesystem::path(Options_.SharedLinksDir).lexically_normal();
        }
        BuildDatasets();
    }

    TFuture<void> HandleRequest(TRequest& request, TResponse& response) override {
        if (request.Method() == "OPTIONS") {
            response.SetStatus(200);
            response.SetHeader("Access-Control-Allow-Origin", "*");
            response.SetHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.SetHeader("Access-Control-Allow-Headers", "Content-Type");
            response.SetHeader("Content-Length", "0");
            co_await response.SendHeaders();
            co_return;
        }

        if (request.Method() == "GET") {
            co_await Get(request, response);
            co_return;
        }

        if (request.Method() == "POST") {
            co_await Post(request, response);
            co_return;
        }

        response.SetStatus(405);
        co_await SendText(response, "Method Not Allowed", "text/plain; charset=utf-8");
    }

private:
    TFuture<void> Get(const TRequest& request, TResponse& response) {
        const auto& path = request.Uri().Path();
        if (path == "/api/version") {
            std::string engine = co_await EngineVersion();
            co_await SendJson(response, ToJsonString(llvm::json::Object{
                {"name", "QumirDB Workbench"},
                {"version", 1},
                {"service", QDB_VERSION_STRING},
                {"engine", engine},
            }));
            co_return;
        }
        if (path == "/api/datasets") {
            co_await SendJson(response, ServerDatasetsJson());
            co_return;
        }
        if (path == "/api/share") {
            co_await ServeShare(request, response);
            co_return;
        }
        if (path.starts_with("/api/local-data/")) {
            co_await ServeLocalData(response, path);
            co_return;
        }
        co_await ServeStatic(response, path);
    }

    TFuture<void> Post(TRequest& request, TResponse& response) {
        const auto& path = request.Uri().Path();
        if (path == "/api/explain") {
            co_await Explain(request, response);
            co_return;
        }
        if (path == "/api/run") {
            co_await Run(request, response);
            co_return;
        }
        if (path == "/api/cancel") {
            co_await Cancel(request, response);
            co_return;
        }
        if (path == "/api/share") {
            co_await CreateShare(request, response);
            co_return;
        }
        response.SetStatus(404);
        co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
    }

    static std::string QueryParam(const TRequest& request, const std::string& name) {
        const auto& params = request.Uri().QueryParameters();
        auto it = params.find(name);
        return it == params.end() ? std::string() : it->second;
    }

    std::filesystem::path SharedPath(std::string_view id) const {
        return SharedLinksBase_ / (std::string(id) + ".json");
    }

    TFuture<void> ServeShare(const TRequest& request, TResponse& response) {
        const auto id = QueryParam(request, "id");
        if (!IsValidSharedId(id)) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("share", "invalid or missing share id")), 400);
            co_return;
        }
        const auto path = SharedPath(id);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("share", "shared query not found")), 404);
            co_return;
        }
        std::ifstream input(path, std::ios::binary);
        std::string json(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof()) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("share", "failed to read shared query")), 500);
            co_return;
        }
        co_await SendJson(response, json);
    }

    TFuture<void> CreateShare(TRequest& request, TResponse& response) {
        auto limitedBody = co_await ReadBodyLimited(request, MaxSharePayloadSize);
        if (!limitedBody) {
            response.SetHeader("Connection", "close");
            co_await SendJson(response,
                ToJsonString(ErrorJson("share", "share payload exceeds 1 MiB")), 413);
            co_return;
        }
        std::string& body = *limitedBody;
        auto parsed = llvm::json::parse(body);
        if (!parsed) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("share", llvm::toString(parsed.takeError()))), 400);
            co_return;
        }
        const auto* root = parsed->getAsObject();
        auto sql = root ? root->getString("sql") : std::optional<llvm::StringRef>{};
        if (!root || !sql || sql->trim().empty()) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("share", "share requires non-empty sql")), 400);
            co_return;
        }

        llvm::json::Object stored{
            {"version", 1},
            {"sql", std::string(*sql)},
        };
        if (auto name = root->getString("name"); name && !name->trim().empty()) {
            stored["name"] = Utf8Prefix(*name, 256);
        }
        if (const auto* dataset = root->getObject("dataset")) {
            llvm::json::Object hint;
            if (auto id = dataset->getString("id")) {
                hint["id"] = Utf8Prefix(*id, 256);
            }
            if (auto name = dataset->getString("name")) {
                hint["name"] = Utf8Prefix(*name, 256);
            }
            if (!hint.empty()) {
                stored["dataset"] = std::move(hint);
            }
        }

        const std::string storedJson = ToJsonString(std::move(stored));
        const std::string id = SharedId(storedJson);
        std::error_code ec;
        std::filesystem::create_directories(SharedLinksBase_, ec);
        if (ec) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("share", "failed to create shared-links directory")),
                500);
            co_return;
        }
        const auto path = SharedPath(id);
        if (!std::filesystem::exists(path, ec)) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << storedJson;
            output.close();
            if (!output) {
                co_await SendJson(response,
                    ToJsonString(ErrorJson("share", "failed to store shared query")), 500);
                co_return;
            }
        }

        co_await SendJson(response, ToJsonString(llvm::json::Object{
            {"ok", true},
            {"id", id},
            {"url", "/?s=" + id},
        }));
    }

    // Kills the child process of an in-flight run/explain identified by runId.
    // qdb_web tracks each spawned process by the runId the client passes on the
    // request; SIGKILL makes qdb exit, its pipes close, and the run coroutine
    // unwinds normally (its registration is erased via RAII).
    TFuture<void> Cancel(TRequest& request, TResponse& response) {
        const auto runId = QueryParam(request, "runId");
        bool killed = false;
        if (!runId.empty()) {
            auto it = ActiveRuns_.find(runId);
            if (it != ActiveRuns_.end()) {
                ::kill(it->second, SIGKILL);
                killed = true;
            }
        }
        co_await SendJson(response,
            killed ? "{\"ok\":true,\"killed\":true}"
                   : "{\"ok\":true,\"killed\":false}");
    }

    // The engine ships in its own package and may be upgraded without restarting
    // us, so its version is asked for and cached briefly.
    TFuture<std::string> EngineVersion() {
        auto now = std::chrono::steady_clock::now();
        if (!EngineVersion_.empty() &&
            (now - EngineVersionTime_) < VersionCacheDuration)
        {
            co_return EngineVersion_;
        }

        auto engine = (BinaryBase_ / "qdb").generic_string();
        auto pipe = Options_.PipeFactory(
            engine,
            std::vector<std::string>{"--version"},
            /*stderrToStdout=*/false);
        pipe.CloseWrite();

        std::string output;
        char buf[256];
        auto reader = TByteReader(pipe);
        for (;;) {
            ssize_t n = co_await reader.ReadSome(buf, sizeof(buf));
            if (n < 0) {
                continue;
            }
            if (n == 0) {
                break;
            }
            output.append(buf, static_cast<size_t>(n));
        }

        const int exitCode = co_await WaitForPipe(pipe);
        auto version = exitCode == 0 ? SanitizeVersion(output) : std::string{};
        EngineVersion_ = version.empty() ? "unknown" : std::move(version);
        EngineVersionTime_ = now;
        co_return EngineVersion_;
    }

    TFuture<void> Explain(TRequest& request, TResponse& response) {
        std::string body = co_await request.ReadBodyFull();
        auto exporter = (BinaryBase_ / "qdb").generic_string();
        std::vector<std::string> exporterArgs{
            "--plan-export",
            "--stdin-json",
            "--stdout-json",
        };
        if (!Options_.CacheDir.empty()) {
            exporterArgs.push_back("--cache");
            exporterArgs.push_back(Options_.CacheDir);
        }
        auto pipe = Options_.PipeFactory(
            exporter,
            exporterArgs,
            /*stderrToStdout=*/true);
        TRunRegistration runReg(this, QueryParam(request, "runId"), pipe.Pid());

        co_await TByteWriter(pipe).Write(body.data(), body.size());
        pipe.CloseWrite();

        std::string output;
        char buf[4096];
        auto reader = TByteReader(pipe);
        for (;;) {
            ssize_t n = co_await reader.ReadSome(buf, sizeof(buf));
            if (n < 0) {
                continue;
            }
            if (n == 0) {
                break;
            }
            output.append(buf, static_cast<size_t>(n));
        }

        const int exitCode = co_await WaitForPipe(pipe);
        if (exitCode != 0) {
            std::string json =
                "{\"ok\":false,\"error\":{\"stage\":\"export\","
                "\"message\":\"qdb --plan-export failed with code " +
                std::to_string(exitCode) + "\",\"output\":\"" +
                JsonEscape(output) + "\"}}";
            co_await SendJson(response, json, 500);
            co_return;
        }

        co_await SendJson(response, output);
    }

    TFuture<void> Run(TRequest& request, TResponse& response) {
        std::string body = co_await request.ReadBodyFull();
        auto parsed = llvm::json::parse(body);
        if (!parsed) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("request", llvm::toString(parsed.takeError()))),
                400);
            co_return;
        }

        const auto* root = parsed->getAsObject();
        auto sql = root ? root->getString("sql") : std::optional<llvm::StringRef>{};
        const auto* datasetValue = root ? root->get("dataset") : nullptr;
        const auto* dataset = datasetValue ? datasetValue->getAsObject() : nullptr;
        auto datasetId = dataset ? dataset->getString("id") : std::optional<llvm::StringRef>{};
        if (!root || !sql || sql->trim().empty() || !datasetId) {
            co_await SendJson(response,
                ToJsonString(ErrorJson("request", "run requires sql and dataset.id")),
                400);
            co_return;
        }

        auto* serverDataset = FindServerDataset(*datasetId);
        if (!serverDataset) {
            co_await SendJson(response,
                ToJsonString(ErrorJson(
                    "run",
                    "selected dataset is not a server-side dataset")),
                400);
            co_return;
        }

        auto queryFile = TempSqlPath();
        {
            std::ofstream out(queryFile);
            out << std::string(*sql);
            if (!sql->ends_with("\n")) {
                out << "\n";
            }
        }

        auto qdb = (BinaryBase_ / "qdb").generic_string();
        const auto started = std::chrono::steady_clock::now();
        auto pipe = Options_.PipeFactory(
            qdb,
            {
                "--sql",
                "--data", serverDataset->Path.string(),
                "--format", "<escape>csv",
                "-i", queryFile.string(),
            },
            /*stderrToStdout=*/false);
        TRunRegistration runReg(this, QueryParam(request, "runId"), pipe.Pid());
        pipe.CloseWrite();

        std::string output;
        char buf[8192];
        for (;;) {
            ssize_t n = co_await pipe.ReadSome(buf, sizeof(buf));
            if (n < 0) {
                continue;
            }
            if (n == 0) {
                break;
            }
            output.append(buf, static_cast<size_t>(n));
        }

        std::string errorOutput;
        for (;;) {
            ssize_t n = co_await pipe.ReadSomeErr(buf, sizeof(buf));
            if (n < 0) {
                continue;
            }
            if (n == 0) {
                break;
            }
            errorOutput.append(buf, static_cast<size_t>(n));
        }

        const int exitCode = co_await WaitForPipe(pipe);
        std::error_code ec;
        std::filesystem::remove(queryFile, ec);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (exitCode != 0) {
            co_await SendJson(response,
                ToJsonString(llvm::json::Object{
                    {"ok", false},
                    {"error", llvm::json::Object{
                        {"stage", "run"},
                        {"message", "QumirDB failed with code " + std::to_string(exitCode)},
                        {"output", output},
                        {"stderr", errorOutput},
                    }},
                }),
                500);
            co_return;
        }

        auto runStats = ParseRunStats(errorOutput);
        llvm::json::Object result{
            {"ok", true},
            {"format", "csv"},
            {"csv", std::move(output)},
            {"stderr", std::move(errorOutput)},
            {"elapsedMs", elapsed},
        };
        if (runStats.Found) {
            result["processingMs"] = static_cast<int64_t>(runStats.Seconds * 1000.0);
            result["processingSeconds"] = runStats.Seconds;
            result["rows"] = runStats.Rows;
        }
        co_await SendJson(response, ToJsonString(std::move(result)));
    }

    void BuildDatasets() {
        namespace fs = std::filesystem;
        ServerDatasets_.clear();
        size_t serverIndex = 0;
        size_t localIndex = 0;
        auto addDir = [&](const TDataDirArg& arg, EDatasetKind kind, size_t index) {
            std::error_code ec;
            fs::path dir = fs::weakly_canonical(arg.Path, ec);
            if (ec || dir.empty()) {
                dir = arg.Path.lexically_normal();
            }
            if (!fs::exists(dir) || !fs::is_directory(dir)) {
                std::cerr << "[QumirDB Web] skip data dir: " << dir << "\n";
                return;
            }

            TServerDataset dataset;
            dataset.Id = (kind == EDatasetKind::Server ? "server:" : "local:") +
                std::to_string(index);
            dataset.Name = arg.Alias ? *arg.Alias : BaseName(dir);
            dataset.Path = dir;
            dataset.Kind = kind;
            dataset.Json = BuildDatasetJson(dataset);
            ServerDatasets_.push_back(std::move(dataset));
        };

        for (const auto& root : Options_.DataRoots) {
            for (const auto& entry : ReadDatasetMap(ExpandUserPath(root))) {
                addDir(
                    entry.Dir,
                    entry.Kind,
                    entry.Kind == EDatasetKind::Server ? serverIndex++ : localIndex++);
            }
        }
        for (const auto& dirValue : Options_.DataDirs) {
            addDir(ParseDataDirArg(dirValue), EDatasetKind::Server, serverIndex++);
        }
        for (const auto& dirValue : Options_.LocalDataDirs) {
            addDir(ParseDataDirArg(dirValue), EDatasetKind::Local, localIndex++);
        }
    }

    llvm::json::Object BuildDatasetJson(const TServerDataset& dataset) const {
        namespace fs = std::filesystem;
        llvm::json::Array tables;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(dataset.Path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".parquet") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        for (const auto& file : files) {
            const auto tableName = TableNameFromPath(file);
            const auto sourceFile = file.filename().string();
            try {
                NQdb::TParquetFile source(file.string());
                llvm::json::Array columns;
                for (const auto& column : source.Schema().Columns) {
                    columns.push_back(llvm::json::Object{
                        {"name", std::string(column.Name)},
                        {"type", SchemaTypeName(column.Type)},
                    });
                }

                int64_t rows = 0;
                int64_t bytes = 0;
                auto rowGroups = source.ScanRowGroups();
                for (const auto& rowGroup : rowGroups) {
                    rows += rowGroup.RowCount;
                    bytes += rowGroup.ByteSize;
                }
                llvm::json::Object stats{
                    {"rows", rows},
                    {"bytes", bytes},
                    {"rowGroups", static_cast<int64_t>(rowGroups.size())},
                };
                auto columnStats = ColumnStatsJson(source);
                if (!columnStats.empty()) {
                    stats["columns"] = std::move(columnStats);
                }

                tables.push_back(llvm::json::Object{
                    {"name", tableName},
                    {"sourceFile", sourceFile},
                    {"columns", std::move(columns)},
                    {"stats", std::move(stats)},
                });
            } catch (const std::exception& e) {
                std::cerr << "[QumirDB Web] failed to read schema for "
                          << file << ": " << e.what() << "\n";
            }
        }

        llvm::json::Array sourceFiles;
        if (dataset.Kind == EDatasetKind::Local) {
            for (const auto& file : files) {
                std::error_code ec;
                const auto size = fs::file_size(file, ec);
                sourceFiles.push_back(llvm::json::Object{
                    {"name", file.filename().string()},
                    {"size", ec ? 0 : static_cast<int64_t>(size)},
                    {"url", "/api/local-data/" + UrlEncode(dataset.Id) + "/" +
                        UrlEncode(file.filename().string())},
                });
            }
        }

        llvm::json::Object source{
            {"kind", dataset.Kind == EDatasetKind::Server ? "server" : "local"},
            {"path", dataset.Path.string()},
        };
        if (dataset.Kind == EDatasetKind::Local) {
            source["files"] = std::move(sourceFiles);
        }

        return llvm::json::Object{
            {"id", dataset.Id},
            {"name", dataset.Name},
            {"source", std::move(source)},
            {"tables", std::move(tables)},
        };
    }

    std::string ServerDatasetsJson() const {
        llvm::json::Array datasets;
        for (const auto& dataset : ServerDatasets_) {
            datasets.push_back(llvm::json::Object(dataset.Json));
        }
        return ToJsonString(llvm::json::Object{
            {"ok", true},
            {"datasets", std::move(datasets)},
        });
    }

    TServerDataset* FindServerDataset(llvm::StringRef id) {
        for (auto& dataset : ServerDatasets_) {
            if (dataset.Kind == EDatasetKind::Server && dataset.Id == id) {
                return &dataset;
            }
        }
        return nullptr;
    }

    TServerDataset* FindLocalDataset(llvm::StringRef id) {
        for (auto& dataset : ServerDatasets_) {
            if (dataset.Kind == EDatasetKind::Local && dataset.Id == id) {
                return &dataset;
            }
        }
        return nullptr;
    }

    std::filesystem::path TempSqlPath() const {
        static std::atomic<uint64_t> nextId{0};
        auto name = "qumirdb-web-" + std::to_string(::getpid()) + "-" +
            std::to_string(nextId.fetch_add(1, std::memory_order_relaxed)) + ".sql";
        return std::filesystem::temp_directory_path() / name;
    }

    // Streams a file to the response with a known Content-Length, reading it in
    // fixed-size chunks via coroio's TFileHandle instead of slurping the whole
    // file into memory. Takes ownership of `fd` (TFileHandle closes it).
    TFuture<void> SendFile(
        TResponse& response,
        int fd,
        uint64_t size,
        const std::string& contentType)
    {
        response.SetHeader("Content-Type", contentType);
        response.SetHeader("Content-Length", std::to_string(size));
        co_await response.SendHeaders();

        TFileHandle file(fd, *Options_.Poller);
        std::vector<char> buf(1 << 18);
        for (;;) {
            ssize_t n = co_await file.ReadSome(buf.data(), buf.size());
            if (n < 0) {
                continue;
            }
            if (n == 0) {
                break;
            }
            co_await response.WriteBodyChunk(buf.data(), static_cast<size_t>(n));
        }
    }

    TFuture<void> ServeLocalData(TResponse& response, const std::string& uriPath) {
        namespace fs = std::filesystem;
        constexpr std::string_view prefix = "/api/local-data/";
        const std::string rest = uriPath.substr(prefix.size());
        const auto slash = rest.find('/');
        if (slash == std::string::npos) {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        const std::string datasetId = UrlDecode(std::string_view(rest).substr(0, slash));
        const std::string fileName = UrlDecode(std::string_view(rest).substr(slash + 1));
        auto* dataset = FindLocalDataset(datasetId);
        const fs::path rel(fileName);
        if (!dataset ||
            fileName.empty() ||
            rel.has_parent_path() ||
            rel.filename().string() != fileName ||
            rel.extension() != ".parquet")
        {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        std::error_code ec;
        const fs::path base = fs::canonical(dataset->Path, ec);
        if (ec) {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }
        fs::path target = fs::canonical(base / rel, ec);
        if (ec || !fs::exists(target) || !fs::is_regular_file(target) ||
            target.parent_path() != base)
        {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        const int fd = ::open(target.c_str(), O_RDONLY);
        if (fd < 0) {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        response.SetHeader(
            "Content-Disposition",
            "attachment; filename=\"" + JsonEscape(fileName) + "\"");
        const auto size = fs::file_size(target, ec);
        co_await SendFile(response, fd, ec ? 0 : static_cast<uint64_t>(size),
            ContentType(target.extension().string()));
    }

    TFuture<void> ServeStatic(TResponse& response, std::string uriPath) {
        namespace fs = std::filesystem;
        if (uriPath.empty() || uriPath == "/") {
            uriPath = "/index.html";
        }
        if (!uriPath.empty() && uriPath.front() == '/') {
            uriPath.erase(0, 1);
        }

        fs::path rel = fs::path(uriPath).lexically_normal();
        const auto relStr = rel.generic_string();
        if (relStr.find("..") != std::string::npos) {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        fs::path target = StaticBase_ / rel;
        std::error_code ec;
        target = fs::canonical(target, ec);
        if (ec || !fs::exists(target) || !fs::is_regular_file(target)) {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        const int fd = ::open(target.c_str(), O_RDONLY);
        if (fd < 0) {
            response.SetStatus(404);
            co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
            co_return;
        }

        const auto size = fs::file_size(target, ec);
        co_await SendFile(response, fd, ec ? 0 : static_cast<uint64_t>(size),
            ContentType(target.extension().string()));
    }

    std::string ContentType(const std::string& ext) const {
        static const std::unordered_map<std::string, std::string> types = {
            {".html", "text/html; charset=utf-8"},
            {".css", "text/css; charset=utf-8"},
            {".js", "application/javascript; charset=utf-8"},
            {".json", "application/json; charset=utf-8"},
            {".svg", "image/svg+xml"},
            {".txt", "text/plain; charset=utf-8"},
            {".parquet", "application/vnd.apache.parquet"},
        };
        auto it = types.find(ext);
        return it == types.end() ? "application/octet-stream" : it->second;
    }

    TFuture<void> SendJson(
        TResponse& response,
        const std::string& json,
        int status = 200)
    {
        response.SetStatus(status);
        co_await SendText(response, json, "application/json; charset=utf-8");
    }

    TFuture<void> SendText(
        TResponse& response,
        const std::string& body,
        const std::string& contentType)
    {
        response.SetHeader("Content-Type", contentType);
        response.SetHeader("Content-Length", std::to_string(body.size()));
        co_await response.SendHeaders();
        co_await response.WriteBodyFull(body);
    }

private:
    TFuture<int> WaitForPipe(TPipe& pipe) {
        if (!Options_.Poller) {
            co_return pipe.Wait();
        }
        auto exitCode = std::async(std::launch::async, [&pipe]() {
            return pipe.Wait();
        });
        while (exitCode.wait_for(std::chrono::milliseconds(0)) !=
               std::future_status::ready)
        {
            co_await Options_.Poller->Sleep(std::chrono::milliseconds(10));
        }
        co_return exitCode.get();
    }

    // RAII entry in the active-run registry: records runId -> child pid on
    // construction and removes it when the run coroutine unwinds (any exit path).
    class TRunRegistration {
    public:
        TRunRegistration(TRouter* router, std::string runId, int pid)
            : Router_(router)
            , RunId_(std::move(runId))
        {
            if (Router_ && !RunId_.empty()) {
                Router_->ActiveRuns_[RunId_] = pid;
            }
        }
        ~TRunRegistration() {
            if (Router_ && !RunId_.empty()) {
                Router_->ActiveRuns_.erase(RunId_);
            }
        }
        TRunRegistration(const TRunRegistration&) = delete;
        TRunRegistration& operator=(const TRunRegistration&) = delete;

    private:
        TRouter* Router_;
        std::string RunId_;
    };

    TOptions Options_;
    std::filesystem::path StaticBase_;
    std::filesystem::path BinaryBase_;
    std::filesystem::path SharedLinksBase_;
    std::vector<TServerDataset> ServerDatasets_;
    std::unordered_map<std::string, int> ActiveRuns_;
    static constexpr std::chrono::minutes VersionCacheDuration{5};
    std::string EngineVersion_;
    std::chrono::steady_clock::time_point EngineVersionTime_;
};

int main(int argc, char** argv) {
    NNet::TInitializer init;

    int port = 8081;
    TOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--static-dir") && i + 1 < argc) {
            options.StaticDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--binary-dir") && i + 1 < argc) {
            options.BinaryDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--cache") && i + 1 < argc) {
            options.CacheDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--shared-links-dir") && i + 1 < argc) {
            options.SharedLinksDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) {
            options.DataDirs.push_back(argv[++i]);
        } else if (!std::strcmp(argv[i], "--local-data") && i + 1 < argc) {
            options.LocalDataDirs.push_back(argv[++i]);
        } else if (!std::strcmp(argv[i], "--data-dir") && i + 1 < argc) {
            options.DataRoots.push_back(argv[++i]);
        } else if (!std::strcmp(argv[i], "--help")) {
            std::cout << "Usage: " << argv[0]
                      << " [--port n] [--static-dir dir] [--binary-dir dir]"
                      << " [--cache dir]"
                      << " [--shared-links-dir dir] [--data-dir dir ...]"
                      << " [--data 'dir [alias]' ...] [--local-data 'dir [alias]' ...]\n"
                      << "\n--data-dir reads " << DatasetMapName
                      << " from the given directory; each line is\n"
                         "  <server|local> <path> [alias]\n"
                         "with '#' comments and paths taken relative to that directory.\n";
            return 0;
        }
    }

    TAddress address{"::", port};
    using TPoller = TDefaultPoller;
    using TSocket = typename TPoller::TSocket;

    TLoop<TPoller> loop;
    TSocket listenSocket(loop.Poller(), address.Domain());
    listenSocket.Bind(address);
    listenSocket.Listen();
    const auto listenAddress = listenSocket.LocalAddr()->ToString();

    options.PipeFactory = [&](const std::string& cmd,
                              const std::vector<std::string>& args,
                              bool stderrToStdout)
    {
        return TPipe(loop.Poller(), cmd, args, stderrToStdout);
    };
    options.Poller = &loop.Poller();

    TRouter router(std::move(options));
    auto logger = [](const std::string& msg) {
        std::cerr << "[QumirDB Web] " << msg << "\n";
    };
    TWebServer<TSocket> server(std::move(listenSocket), router, logger);
    server.Start();

    std::cerr << "QumirDB web listening on " << listenAddress << "\n";
    loop.Loop();
    return 0;
}
