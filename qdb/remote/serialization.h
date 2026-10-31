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
    uint32_t count = obj.ActorIds.size();
    buf.resize(sizeof(count) + count * obj.ActorIds.size());
    char* ptr = (char*)buf.data();
    memcpy(ptr, &count, sizeof(count)); ptr += count;
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
    uint32_t count = 0;
    memcpy(&count, ptr, sizeof(count)); ptr += sizeof(count);
    obj.ActorIds.resize(count);
    for (size_t i = 0; i < count; ++i) {
        memcpy(&obj.ActorIds[i], ptr, sizeof(obj.ActorIds[i])); ptr += sizeof(obj.ActorIds[i]);
    }
}

template<>
inline void SerializeToStream<TDataChunk>(const TDataChunk& obj, std::ostringstream& oss) {
}

template<>
inline void DeserializeFromStream<TDataChunk>(TDataChunk& obj, std::istringstream& iss) {
}

} // namespace NNet::NActors
