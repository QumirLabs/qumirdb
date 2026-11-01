#include <coroio/http/httpd.hpp>
#include <coroio/actors/actorsystem.hpp>

#include <qdbd/actors.h>
#include <qdbd/messages.h>
#include <qdbd/serialization.h>

#include "router.h"

using namespace NNet;
using namespace NNet::NActors;

using namespace NQdb::NRemote;

namespace NQdb {
namespace NHttp {

namespace {

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
            response.SetHeader("Access-Control-Allow-Headers", "Content-Type");
            response.SetHeader("Content-Length", "0");
            co_await response.SendHeaders();
            co_return;
        }

        if (request.Method() == "POST") {
            co_return co_await Post(request, response);
        }

        co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
    }

    TFuture<void> Post(TRequest& request, TResponse& response) {
        const auto& path = request.Uri().Path();
        if (path == "/api/run") {
            std::string body = co_await request.ReadBodyFull();

            auto res = co_await ActorSystem->Ask<TExecuteQueryResponse>(GatewayId, TExecuteQuery {
                .Query = std::move(body)
            });

            co_return co_await SendText(response, res.Response, "text/plain; charset=utf-8");
        }

        co_await SendText(response, "Not Found", "text/plain; charset=utf-8");
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

    TActorSystem* ActorSystem;
    TActorId GatewayId;
};

} // namespace

std::unique_ptr<IRouter> CreateRouter(TActorSystem* actorSystem, TActorId actorId) {
    return std::unique_ptr<IRouter>(new TRouter(actorSystem, actorId));
}

} // namespace NHttp
} // namespace NQdb
