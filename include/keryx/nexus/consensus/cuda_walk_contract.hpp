#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include <span>

namespace keryx::nexus::consensus {
// Host/device boundary for a future SM 8.6 walk implementation. This is a data
// contract only; no CUDA kernel is declared or linked in M2.1.
struct CudaWalkRequest {
  std::uint64_t job_generation{};
  std::uint64_t nonce{};
  std::uint64_t timestamp{};
  std::uint64_t seed{};
  std::uint64_t chunk_count{};
  std::uint8_t tier{};
  Hash merkle_root{};
  std::span<const std::uint8_t> canonical_blob;
  bool diagnostic_states{};
};
struct CudaWalkResult {
  std::uint64_t job_generation{};
  std::uint64_t nonce{};
  std::array<std::uint64_t, walk_steps> offsets{};
  std::uint64_t final_state{};
  std::vector<std::array<std::uint8_t, tile_bytes>> diagnostic_states;
};
} // namespace keryx::nexus::consensus
