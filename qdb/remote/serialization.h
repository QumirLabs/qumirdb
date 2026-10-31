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
    std::string buf;
    buf.insert(buf.end(), (char*)&obj.DstTaskId, (char*)&obj.DstTaskId + sizeof(obj.DstTaskId));
    buf.insert(buf.end(), (char*)&obj.SrcLaneId, (char*)&obj.SrcLaneId + sizeof(obj.SrcLaneId));

    auto& rowSet = obj.RowSet;
    buf.insert(buf.end(), (char*)&rowSet.ColumnCount, (char*)&rowSet.ColumnCount + sizeof(rowSet.ColumnCount));
    buf.insert(buf.end(), (char*)&rowSet.RowCount, (char*)&rowSet.RowCount + sizeof(rowSet.RowCount));

    assert(obj.Descriptor);
    assert(obj.Descriptor->EntrySize.size() == rowSet.ColumnCount);

    auto& descs = obj.Descriptor->Descriptors;

    auto rowCount = rowSet.RowCount;
    for (size_t i = 0; i < rowSet.ColumnCount; ++i) {
        auto& desc = descs[i];
        auto& col = rowSet.Columns[i];
        if (desc.IsVariadic) {
            // string
            buf.insert(buf.end(), (char*)&col.OffsetWidth, (char*)&col.OffsetWidth + sizeof(col.OffsetWidth));
            if (rowSet.Selection == nullptr) {
                buf.insert(buf.end(), (char*)col.Offsets, (char*)col.Offsets + col.OffsetWidth * (rowCount + 1));
                uint64_t lastOffset = 0;
                if (col.OffsetWidth == 8) {
                    lastOffset = ((uint8_t*)(col.Offsets))[rowCount];
                }
                buf.insert(buf.end(), (char*)col.Data, (char*)col.Data + lastOffset);
                // mask
                uint64_t maskSize = 0;
                if (col.Mask) {
                    maskSize = (rowCount + 7) / 8;
                }
                buf.insert(buf.end(), (char*)&maskSize, (char*)&maskSize + sizeof(maskSize));
                if (col.Mask) {
                    buf.insert(buf.end(), (char*)col.Mask, (char*)col.Mask + maskSize);
                }
            } else {
                // compaction
            }
        }
    }

    oss << buf;
}

template<>
inline void DeserializeFromStream<TDataChunk>(TDataChunk& obj, std::istringstream& iss) {
}

} // namespace NNet::NActors
