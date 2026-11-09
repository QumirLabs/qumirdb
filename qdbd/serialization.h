#pragma once

#include <qdbd/messages.h>

namespace NNet {
namespace NActors {

template<>
void SerializeToStream(
    const NQdb::NRemote::TSpawnComputeActorResponse& message,
    std::ostringstream& output);

template<>
void DeserializeFromStream(
    NQdb::NRemote::TSpawnComputeActorResponse& message,
    std::istringstream& input);

template<>
void SerializeToStream(const NQdb::NRemote::TComputeError& message, std::ostringstream& output);

template<>
void DeserializeFromStream(NQdb::NRemote::TComputeError& message, std::istringstream& input);

template<>
void SerializeToStream(const NQdb::NRemote::TTaskGraphMessage& message, std::ostringstream& output);

template<>
void DeserializeFromStream(NQdb::NRemote::TTaskGraphMessage& message, std::istringstream& input);

template<>
void SerializeToStream(const NQdb::NRemote::TDataChunk& message, std::ostringstream& output);

// The decoded row set owns its buffers and descriptor; release it with NQdb::Release.
template<>
void DeserializeFromStream(NQdb::NRemote::TDataChunk& message, std::istringstream& input);

} // namespace NActors
} // namespace NNet
