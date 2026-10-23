#pragma once
#include <cstdint>
namespace keryx::nexus::ai {
inline constexpr std::uint64_t escrow_claim_check_seconds = 1800;
inline bool escrow_claim_check_due(std::uint64_t monotonic_now, std::uint64_t monotonic_next,
                                   std::uint64_t unix_now, std::uint64_t unix_last) noexcept {
  return monotonic_now >= monotonic_next &&
      (unix_last == 0 || (unix_now >= unix_last && unix_now - unix_last >= escrow_claim_check_seconds));
}
}
