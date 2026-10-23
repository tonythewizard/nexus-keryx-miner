#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/amd_memory_handoff.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

using namespace keryx::nexus;
using namespace keryx::nexus::consensus;

int main(int argc, char **argv) {
  try {
    if (argc < 6) {
      std::cerr << "Usage: vulkan_saturation_bench MODEL OLD_SPV OLD_SHA NEW_SPV NEW_SHA [DEVICE_ID] [MAX_TILES]\n";
      return 1;
    }
    model::ReadOnlyGguf model(argv[1]);
    std::uint64_t logical = model.chunk_count() / 32;
    const std::string dev = (argc >= 7) ? argv[6] : "pci:0000:00:00.0";
    if (argc >= 8) {
      const std::uint64_t cap_tiles = std::stoull(argv[7]);
      if (cap_tiles < logical) logical = cap_tiles;
    }
    AmdMemoryHandoff handoff;

    constexpr std::size_t TOTAL = 2048;
    std::vector<std::uint64_t> seeds(TOTAL);
    Hash pph{};
    pph.fill(0x5a);
    for (std::size_t i = 0; i < TOTAL; ++i)
      seeds[i] = block_seed(pph, 1729450000000ULL, 100000ULL + i, 150000000ULL);

    VulkanWalkBatch old_diag, new_diag;
    std::vector<std::uint8_t> old_finals;
    std::vector<std::uint64_t> old_offsets;
    old_finals.reserve(TOTAL * 1024);
    old_offsets.reserve(TOTAL * 256);
    double old_ms = 0.0, new_256_ms = 0.0, new_1024_ms = 0.0, new_2048_ms = 0.0;

    {
      if (dev == "pci:0000:00:00.0") handoff.wait_released();
      VulkanWalkEngine eng(dev, argv[2], argv[3], 256, logical);
      if (!eng.load_resident([&](auto first, auto bytes) { model.read_tiles_streaming(first, bytes); }))
        throw std::runtime_error("old upload failed");
      std::array<std::uint64_t, 1> s1{seeds[0]};
      eng.run(logical, s1, {}, {}, true, old_diag);
      VulkanWalkBatch tmp;
      eng.run(logical, std::span(seeds).subspan(0, 256), {}, {}, false, tmp);
      auto t0 = std::chrono::steady_clock::now();
      for (std::size_t off = 0; off < TOTAL; off += 256) {
        VulkanWalkBatch out;
        eng.run(logical, std::span(seeds).subspan(off, 256), {}, {}, false, out);
        old_finals.insert(old_finals.end(), out.final_states.begin(), out.final_states.end());
        old_offsets.insert(old_offsets.end(), out.offsets.begin(), out.offsets.end());
      }
      auto t1 = std::chrono::steady_clock::now();
      old_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      eng.release_resident();
    }

    {
      if (dev == "pci:0000:00:00.0") handoff.wait_released();
      VulkanWalkEngine eng(dev, argv[4], argv[5], 2048, logical);
      if (!eng.load_resident([&](auto first, auto bytes) { model.read_tiles_streaming(first, bytes); }))
        throw std::runtime_error("new upload failed");
      std::array<std::uint64_t, 1> s1{seeds[0]};
      eng.run(logical, s1, {}, {}, true, new_diag);
      if (old_diag.offsets != new_diag.offsets ||
          old_diag.final_states != new_diag.final_states ||
          old_diag.diagnostic_states != new_diag.diagnostic_states)
        throw std::runtime_error("DIAGNOSTIC PARITY MISMATCH!");

      VulkanWalkBatch out;
      std::vector<std::uint8_t> new256_finals, new1024_finals;
      std::vector<std::uint64_t> new256_offsets, new1024_offsets;
      new256_finals.reserve(TOTAL * 1024);
      new256_offsets.reserve(TOTAL * 256);
      new1024_finals.reserve(TOTAL * 1024);
      new1024_offsets.reserve(TOTAL * 256);

      auto t0 = std::chrono::steady_clock::now();
      for (std::size_t off = 0; off < TOTAL; off += 256) {
        eng.run(logical, std::span(seeds).subspan(off, 256), {}, {}, false, out);
        new256_finals.insert(new256_finals.end(), out.final_states.begin(), out.final_states.end());
        new256_offsets.insert(new256_offsets.end(), out.offsets.begin(), out.offsets.end());
      }
      auto t1 = std::chrono::steady_clock::now();
      new_256_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      if (new256_finals != old_finals || new256_offsets != old_offsets)
        throw std::runtime_error("BATCH 8x256 FULL 2048 PARITY MISMATCH!");

      auto t2 = std::chrono::steady_clock::now();
      for (std::size_t off = 0; off < TOTAL; off += 1024) {
        eng.run(logical, std::span(seeds).subspan(off, 1024), {}, {}, false, out);
        new1024_finals.insert(new1024_finals.end(), out.final_states.begin(), out.final_states.end());
        new1024_offsets.insert(new1024_offsets.end(), out.offsets.begin(), out.offsets.end());
      }
      auto t3 = std::chrono::steady_clock::now();
      new_1024_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
      if (new1024_finals != old_finals || new1024_offsets != old_offsets)
        throw std::runtime_error("BATCH 2x1024 FULL 2048 PARITY MISMATCH!");

      auto t4 = std::chrono::steady_clock::now();
      eng.run(logical, std::span(seeds).subspan(0, 2048), {}, {}, false, out);
      auto t5 = std::chrono::steady_clock::now();
      new_2048_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

      if (out.final_states != old_finals || out.offsets != old_offsets)
        throw std::runtime_error("BATCH 1x2048 FULL 2048 PARITY MISMATCH!");
      eng.release_resident();
    }

    std::cout << "FULL_2048_PARITY=PASS dev=" << dev << " tiles=" << logical << " nonces=" << TOTAL << "\n"
              << "OLD_256_MS=" << old_ms << " (" << (TOTAL * 1000.0 / old_ms) << " nonces/s)\n"
              << "NEW_256_MS=" << new_256_ms << " (" << (TOTAL * 1000.0 / new_256_ms) << " nonces/s)\n"
              << "NEW_1024_MS=" << new_1024_ms << " (" << (TOTAL * 1000.0 / new_1024_ms) << " nonces/s)\n"
              << "NEW_2048_MS=" << new_2048_ms << " (" << (TOTAL * 1000.0 / new_2048_ms) << " nonces/s)\n"
              << "SPEEDUP_256=" << (old_ms / new_256_ms)
              << "x SPEEDUP_1024=" << (old_ms / new_1024_ms)
              << "x SPEEDUP_2048=" << (old_ms / new_2048_ms) << "x\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "BENCH_ERROR: " << e.what() << "\n";
    return 1;
  }
}
