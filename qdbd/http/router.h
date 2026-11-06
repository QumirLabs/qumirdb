#pragma once

#include <coroio/http/httpd.hpp>
#include <coroio/actors/actorsystem.hpp>

namespace NQdb {
namespace NHttp {

std::unique_ptr<NNet::IRouter> CreateRouter(NNet::NActors::TActorSystem* actorSystem, NNet::NActors::TActorId actorId);

} // namespace NHttp
} // namespace NQdb
