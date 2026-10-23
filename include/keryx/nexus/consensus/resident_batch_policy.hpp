#pragma once
#include <cstdint>

namespace keryx::nexus::consensus {
// Selected by controlled SM86 resident-search replay. Keep the same nonce
// space and complete 256-step walk; only the admission/preemption interval
// changes. The resident engine still accepts bounded diagnostic batches.
inline constexpr std::uint64_t live_resident_batch_size = 4608;
static_assert(live_resident_batch_size > 0 && live_resident_batch_size <= 18432);
} // namespace keryx::nexus::consensus
