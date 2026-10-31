#include "messages.h"

#include <qdb/io/rowset_ops.h>

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
    assert(rowSet.Selection == nullptr); // must be compacted

    auto& descs = obj.Descriptor;

    auto rowCount = rowSet.RowCount;
    auto serializeMask = [&](const void* mask) {
        if (mask) {
            uint64_t maskSize = (rowCount + 7) / 8;
            bool hasMask = true;
            buf.insert(buf.end(), (char*)&hasMask, (char*)&hasMask + sizeof(hasMask));
            buf.insert(buf.end(), (char*)mask, (char*)mask + maskSize);
        }
    };

    // TODO: avoid descriptor copying
    buf.insert(buf.end(), (char*)descs, (char*)descs + rowSet.ColumnCount * sizeof(::NQdb::TValueLayout));

    for (size_t i = 0; i < rowSet.ColumnCount; ++i) {
        auto& desc = descs[i];
        auto& col = rowSet.Columns[i];
        if (desc.Bitmap) {
            // boolean
            serializeMask(col.Data);
            serializeMask(col.Mask);
        } else if (desc.Width == 0) {
            // string
            buf.insert(buf.end(), (char*)&col.OffsetWidth, (char*)&col.OffsetWidth + sizeof(col.OffsetWidth));
            buf.insert(buf.end(), (char*)col.Offsets, (char*)col.Offsets + col.OffsetWidth * (rowCount + 1));
            uint64_t lastOffset = 0;
            if (col.OffsetWidth == 8) {
                lastOffset = ((uint64_t*)(col.Offsets))[rowCount];
            } else {
                lastOffset = ((uint32_t*)(col.Offsets))[rowCount];
            }
            buf.insert(buf.end(), (char*)col.Data, (char*)col.Data + lastOffset);
            // mask
            serializeMask(col.Mask);
        } else {
            // primitive
            buf.insert(buf.end(), (char*)col.Data, (char*)col.Data + desc.Width * rowCount);
            serializeMask(col.Mask);
        }
    }

    oss << buf;
}

template<>
inline void DeserializeFromStream<TDataChunk>(TDataChunk& obj, std::istringstream& iss) {
    auto destroy = +[](::NQdb::TRowSet* rowSet) {
        if (rowSet) {
            for (size_t i = 0; i < rowSet->ColumnCount; ++i) {
                auto& col = rowSet->Columns[i];
                free(col.Data);
                free(col.Offsets);
                free(const_cast<uint8_t*>(col.Mask)); // TODO: Mask should not be const!
            }
            free(rowSet->Columns);
        }
    };

    const std::string& buf =  iss.str();
    const char* ptr = buf.data();
    // TODO: bound checks
    memcpy(&obj.DstTaskId, ptr, sizeof(obj.DstTaskId)); ptr += sizeof(obj.DstTaskId);
    memcpy(&obj.SrcLaneId, ptr, sizeof(obj.SrcLaneId)); ptr += sizeof(obj.SrcLaneId);

    auto& rowSet = obj.RowSet;
    memcpy(&rowSet.ColumnCount, ptr, sizeof(rowSet.ColumnCount)); ptr += sizeof(rowSet.ColumnCount);
    memcpy(&rowSet.RowCount, ptr, sizeof(rowSet.RowCount)); ptr += sizeof(rowSet.RowCount);
    rowSet.Destroy = destroy;
    auto rowCount = rowSet.RowCount;

    // TODO: alignment
    const ::NQdb::TValueLayout* descs = reinterpret_cast<const ::NQdb::TValueLayout*>(ptr); ptr += rowSet.ColumnCount * sizeof(::NQdb::TValueLayout);

    if (rowSet.ColumnCount) {
        rowSet.Columns = (::NQdb::TColumn*)malloc(sizeof(::NQdb::TColumn) * rowSet.ColumnCount);
    } else {
        rowSet.Columns = nullptr;
    }

    auto deserializeMask = [&]() {
        bool hasMask;
        void* ret = nullptr;
        memcpy(&hasMask, ptr, sizeof(hasMask)); ptr += sizeof(hasMask);
        if (hasMask) {
            uint64_t maskSize = (rowCount + 7) / 8;
            ret = malloc(maskSize);
            memcpy(ret, ptr, maskSize); ptr += maskSize;
        }
        return ret;
    };

    for (size_t i = 0; i < rowSet.ColumnCount; ++i) {
        auto& desc = descs[i];
        auto& col = rowSet.Columns[i];
        if (desc.Bitmap) {
            // boolean
            col.Data = (char*)deserializeMask();
            col.Mask = (uint8_t*)deserializeMask();
        } else if (desc.Width == 0) {
            // string
            memcpy(&col.OffsetWidth, ptr, sizeof(col.OffsetWidth)); ptr += col.OffsetWidth;
            auto offsetsSize = col.OffsetWidth * (rowCount + 1);
            col.Offsets = malloc(offsetsSize);
            memcpy(col.Offsets, ptr, offsetsSize); ptr += offsetsSize;
            uint64_t lastOffset = 0;
            if (col.OffsetWidth == 8) {
                lastOffset = ((uint64_t*)(col.Offsets))[rowCount];
            } else {
                lastOffset = ((uint32_t*)(col.Offsets))[rowCount];
            }
            col.Data = (char*)malloc(lastOffset);
            memcpy(col.Data, ptr, lastOffset); ptr += lastOffset;
            col.Mask = (uint8_t*)deserializeMask();
        } else {
            // primitive
            auto dataSize = desc.Width * rowCount;
            col.Data = (char*)malloc(dataSize);
            memcpy(col.Data, ptr, dataSize); ptr += dataSize;
            col.Mask = (uint8_t*)deserializeMask();
        }
    }
}

} // namespace NNet::NActors
