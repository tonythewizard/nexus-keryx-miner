#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

extern "C" std::uint32_t nexus_protocol_flags(std::uint32_t network, std::uint64_t daa);

namespace keryx::nexus::protocol {
inline constexpr std::uint32_t h15_flag = 1U;
// Source of truth is the pinned Rust network matrix; no version-string inference.
inline std::uint32_t flags(std::string_view network, std::uint64_t daa) {
  const auto id = network == "mainnet" ? 0U : network == "testnet" ? 1U : UINT32_MAX;
  const auto result = nexus_protocol_flags(id, daa);
  if (result == UINT32_MAX) throw std::invalid_argument("unsupported protocol network");
  return result;
}
inline void require_legacy_worker_era(std::string_view network, std::uint64_t daa) {
  if (flags(network, daa) & h15_flag)
    throw std::runtime_error("H15 requires PoM v5 and verifiable inference; this worker is not ready for that era");
}
}
