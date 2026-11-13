#pragma once

#include <qdbd/messages.h>

namespace NNet {
namespace NActors {

template<>
size_t SerializedSize(const NQdb::NRemote::TSpawnComputeActorResponse& message);

template<>
void SerializeTo(const NQdb::NRemote::TSpawnComputeActorResponse& message, std::span<char> out);

template<>
void DeserializeFrom(NQdb::NRemote::TSpawnComputeActorResponse& message, TBlob& blob);

template<>
size_t SerializedSize(const NQdb::NRemote::TComputeError& message);

template<>
void SerializeTo(const NQdb::NRemote::TComputeError& message, std::span<char> out);

template<>
void DeserializeFrom(NQdb::NRemote::TComputeError& message, TBlob& blob);

template<>
size_t SerializedSize(const NQdb::NRemote::TTaskGraphMessage& message);

template<>
void SerializeTo(const NQdb::NRemote::TTaskGraphMessage& message, std::span<char> out);

template<>
void DeserializeFrom(NQdb::NRemote::TTaskGraphMessage& message, TBlob& blob);

template<>
size_t SerializedSize(const NQdb::NRemote::TFilterPartial& message);

template<>
void SerializeTo(const NQdb::NRemote::TFilterPartial& message, std::span<char> out);

template<>
void DeserializeFrom(NQdb::NRemote::TFilterPartial& message, TBlob& blob);

template<>
size_t SerializedSize(const NQdb::NRemote::TDataChunk& message);

template<>
void SerializeTo(const NQdb::NRemote::TDataChunk& message, std::span<char> out);

// The decoded row sets point into the blob, which they share; release each with NQdb::Release.
template<>
void DeserializeFrom(NQdb::NRemote::TDataChunk& message, TBlob& blob);

} // namespace NActors
} // namespace NNet
