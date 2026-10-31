#include "messages.h"

namespace NNet::NActors {

using namespace NQdb::NRemote;

template<>
inline void SerializeToStream<TExecuteQuery>(const TExecuteQuery& obj, std::ostringstream& oss) {
    oss << obj.Query;
}

template<>
inline void DeserializeFromStream<TExecuteQuery>(TExecuteQuery& obj, std::istringstream& iss) {
    obj.Query = iss.str();
}

template<>
inline void SerializeToStream<TExecuteQueryResponse>(const TExecuteQueryResponse& obj, std::ostringstream& oss) {
    oss << obj.Response;
}

template<>
inline void DeserializeFromStream<TExecuteQueryResponse>(TExecuteQueryResponse& obj, std::istringstream& iss) {
    obj.Response = iss.str();
}

template<>
inline void SerializeToStream<TSpawnComputeActorResponse>(const TSpawnComputeActorResponse& obj, std::ostringstream& oss) {
    std::string buf;
    buf.resize(sizeof(obj.Count) + obj.Count * obj.ActorIds.size());
    char* ptr = (char*)buf.data();
    memcpy(ptr, &obj.Count, sizeof(obj.Count)); ptr += obj.Count;
    for (const auto& actorId : obj.ActorIds) {
        memcpy(ptr, &actorId, sizeof(actorId)); ptr += sizeof(actorId);
    }
    oss << buf;
}

template<>
inline void DeserializeFromStream<TSpawnComputeActorResponse>(TSpawnComputeActorResponse& obj, std::istringstream& iss) {
    const std::string& buf =  iss.str();
    const char* ptr = buf.data();
    // TODO: bound checks
    memcpy(&obj.Count, ptr, sizeof(obj.Count)); ptr += sizeof(obj.Count);
    obj.ActorIds.resize(obj.Count);
    for (size_t i = 0; i < obj.Count; ++i) {
        memcpy(&obj.ActorIds[i], ptr, sizeof(obj.ActorIds[i])); ptr += sizeof(obj.ActorIds[i]);
    }
}

} // namespace NNet::NActors
