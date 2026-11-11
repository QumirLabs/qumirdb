#include <coroio/http/httpd.hpp>
#include <coroio/actors/actorsystem.hpp>

#include <qdbd/actors.h>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include "router.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>

using namespace NNet;
using namespace NNet::NActors;

using namespace NQdb::NRemote;

namespace NQdb {
namespace NHttp {

namespace {

constexpr std::string_view PlanWidthHeader = "X-Qdb-Plan-Width";
constexpr size_t MaxPlanWidth = 4096;

// HTTP header names are case-insensitive.
std::optional<std::string_view> FindHeader(const TRequest& request, std::string_view name) {
    for (const auto& [key, value] : request.Headers()) {
        if (key.size() == name.size() && std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<size_t> ParsePlanWidth(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    size_t width = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), width);
    if (error != std::errc() || end != text.data() + text.size() || width == 0 || width > MaxPlanWidth) {
        return std::nullopt;
    }
    return width;
}

struct TRouter final : public IRouter {
    explicit TRouter(TActorSystem* actorSystem, TActorId gatewayId)
        : ActorSystem(actorSystem)
        , GatewayId(gatewayId)
    {
    }

    TFuture<void> HandleRequest(TRequest& request, TResponse& response) override {
        if (request.Method() == "OPTIONS") {
            response.SetStatus(200);
            response.SetHeader("Access-Control-Allow-Origin", "*");
            response.SetHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.SetHeader("Access-Control-Allow-Headers", "Content-Type, X-Qdb-Plan-Width");
            response.SetHeader("Content-Length", "0");
            co_await response.SendHeaders();
            co_return;
        }

        if (request.Method() == "POST") {
            co_return co_await Post(request, response);
        }

        co_await SendText(response, "Not Found", "text/plain; charset=utf-8", 404);
    }

    TFuture<void> Post(TRequest& request, TResponse& response) {
        const auto& path = request.Uri().Path();
        if (path == "/api/run") {
            std::string body = co_await request.ReadBodyFull();
            // The width the client plans for, e.g. nodes x workers of the cluster.
            std::optional<size_t> planWidth;
            if (const auto header = FindHeader(request, PlanWidthHeader)) {
                planWidth = ParsePlanWidth(*header);
                if (!planWidth) {
                    co_return co_await SendText(response,
                        "X-Qdb-Plan-Width must be an integer in [1, " + std::to_string(MaxPlanWidth) + "]",
                        "text/plain; charset=utf-8", 400);
                }
            }

            auto res = co_await ActorSystem->Ask<TExecuteQueryResponse>(GatewayId, TExecuteQuery {
                .Query = std::move(body),
                .PlanWidth = planWidth,
            });

            if (res.Response) {
                if (res.Timing) {
                    response.SetHeader("X-Qdb-Rows", std::to_string(res.Timing->Rows));
                    response.SetHeader("X-Qdb-Planning-Seconds", std::to_string(res.Timing->Planning));
                    response.SetHeader("X-Qdb-KernelBuild-Seconds", std::to_string(res.Timing->KernelBuild));
                    response.SetHeader("X-Qdb-JitLLVM-Seconds", std::to_string(res.Timing->JitLLVM));
                    response.SetHeader("X-Qdb-Execute-Seconds", std::to_string(res.Timing->Execute));
                }
                co_return co_await SendText(response, res.Response.value(), "text/plain; charset=utf-8");
            } else {
                // A failed query is the client's error; only a lack of compute nodes asks to retry.
                co_return co_await SendText(response, res.Response.error().ToString(), "text/plain; charset=utf-8",
                    res.Unavailable ? 503 : 400);
            }
        }

        co_await SendText(response, "Not Found", "text/plain; charset=utf-8", 404);
    }

    TFuture<void> SendText(
        TResponse& response,
        const std::string& body,
        const std::string& contentType,
        int status = 200)
    {
        response.SetStatus(status);
        response.SetHeader("Access-Control-Allow-Origin", "*");
        response.SetHeader("Content-Type", contentType);
        response.SetHeader("Content-Length", std::to_string(body.size()));
        co_await response.SendHeaders();
        co_await response.WriteBodyFull(body);
    }

    TActorSystem* ActorSystem;
    TActorId GatewayId;
};

} // namespace

std::unique_ptr<IRouter> CreateRouter(TActorSystem* actorSystem, TActorId actorId) {
    return std::unique_ptr<IRouter>(new TRouter(actorSystem, actorId));
}

} // namespace NHttp
} // namespace NQdb
