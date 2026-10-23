#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include <algorithm>
#include <span>
#include <stdexcept>

namespace keryx::nexus::consensus {
// Borrow the CPU witness only after checking every CUDA offset and state.
// Both inputs must outlive the view. No third 257-state witness is allocated.
inline const Walk &verified_walk_view(const Walk &cpu,
                                     std::span<const std::uint64_t> offsets,
                                     std::span<const std::uint8_t> states) {
  if (cpu.states.size()!=walk_steps+1 || cpu.offsets.size()!=walk_steps ||
      offsets.size()!=walk_steps || states.size()!=(walk_steps+1)*tile_bytes)
    throw std::runtime_error("CPU/CUDA witness dimensions mismatch");
  if (!std::equal(cpu.offsets.begin(),cpu.offsets.end(),offsets.begin()))
    throw std::runtime_error("CPU/CUDA witness offsets mismatch");
  for (std::size_t step=0;step<=walk_steps;++step)
    if (!std::equal(cpu.states[step].begin(),cpu.states[step].end(),
                    states.begin()+step*tile_bytes))
      throw std::runtime_error("CPU/CUDA witness state mismatch");
  return cpu;
}
} // namespace keryx::nexus::consensus
