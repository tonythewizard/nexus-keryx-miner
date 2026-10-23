#include "keryx/nexus/consensus/pom_v4.hpp"
#include <algorithm>
#include <iostream>
using namespace keryx::nexus::consensus;
int main() {
  try {
    unsigned cases = 0;
    for (unsigned pattern = 0; pattern < 6; ++pattern) {
      const std::uint64_t ntiles = pattern % 2 ? 7 : 3;
      std::vector<std::uint8_t> blob(ntiles * tile_bytes);
      for (std::size_t i = 0; i < blob.size(); ++i) {
        switch (pattern) {
          case 0: blob[i] = 0; break;
          case 1: blob[i] = 127; break;
          case 2: blob[i] = 128; break;
          case 3: blob[i] = 255; break;
          case 4: blob[i] = i % 2 ? 127 : 128; break;
          default: blob[i] = mix64(i); break;
        }
      }
      for (auto seed : {std::uint64_t(0), std::uint64_t(1), UINT64_MAX, std::uint64_t(0x123456789abcdef0ULL)}) {
        const auto scalar = walk_v4(seed, blob);
        const auto input = gather_v4_model_tiles(seed, ntiles, [&](auto off) {
          std::array<std::uint8_t, tile_bytes> tile{};
          std::copy_n(blob.data() + off * tile_bytes, tile_bytes, tile.data());
          return tile;
        });
        const auto actual = walk_v4_from_tile_sequence(seed, ntiles, input.tiles);
        if (actual.states != scalar.states || actual.offsets != scalar.offsets || actual.offsets != input.offsets)
          throw std::runtime_error("CPU dispatch differs from independent scalar walk");
        ++cases;
      }
    }
    bool rejected = false;
    try { (void)walk_v4_from_tile_sequence(0, 0, {}); }
    catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("zero tiles accepted");
    rejected = false;
    try { (void)walk_v4_from_tile_sequence(0, 1, {}); }
    catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("truncated tile sequence accepted");
    std::cout << "CPU_WALK_DISPATCH=PASS full_walks=" << cases
              << " signed_edges=true states=257 offsets=256 invalid_inputs=2\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
