// Offline CPU walk timing: tiles are read from the existing Borsh witness.
// No model load, CUDA, node client, identity or network writes.
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace keryx::nexus;
static std::vector<std::uint8_t> read(const char *name) {
  std::ifstream f(name, std::ios::binary);
  if (!f) throw std::runtime_error("missing offline fixture");
  return {std::istreambuf_iterator<char>(f), {}};
}
static std::uint64_t le(std::span<const std::uint8_t> b, std::size_t p, unsigned n) {
  if (p > b.size() || n > b.size() - p) throw std::runtime_error("truncated fixture");
  std::uint64_t value = 0;
  for (unsigned i = 0; i < n; ++i) value |= std::uint64_t(b[p + i]) << (i * 8);
  return value;
}
int main(int argc, char **argv) {
  try {
    if (argc != 6) throw std::runtime_error("CONTEXT TRACE PROOF ORACLE OUTPUT required");
    const auto context = read(argv[1]), original_trace = read(argv[2]), proof = read(argv[3]);
    if (context.size() != 225 + proof.size() || original_trace.size() != 28 + 257 * 1024 + 256 * 8 ||
        le(proof, 89, 4) != 256 || le(context, 12, 8) % 32)
      throw std::runtime_error("invalid fixture dimensions");
    std::vector<std::uint8_t> tiles;
    tiles.reserve(256 * 1024);
    std::size_t cursor = 93;
    for (unsigned i = 0; i < 256; ++i) {
      if (le(proof, cursor, 4) != 1024 || cursor + 1028 > proof.size())
        throw std::runtime_error("invalid witness tile");
      tiles.insert(tiles.end(), proof.begin() + cursor + 4, proof.begin() + cursor + 1028);
      cursor += 1028;
    }
    const auto seed = le(context, 4, 8), ntiles = le(context, 12, 8) / 32;
    auto check = [&](const consensus::Walk &walk) {
      if (walk.states.size() != 257 || walk.offsets.size() != 256 ||
          std::memcmp(walk.states.data(), original_trace.data() + 28, 257 * 1024))
        throw std::runtime_error("CPU states differ from official fixture");
      for (unsigned i = 0; i < 256; ++i)
        if (walk.offsets[i] != le(original_trace, 28 + 257 * 1024 + i * 8, 8))
          throw std::runtime_error("CPU offset differs from official fixture");
    };
    auto verified = consensus::walk_v4_from_tile_sequence(seed, ntiles, tiles);
    check(verified);
    auto generated_trace = original_trace;
    std::memcpy(generated_trace.data() + 28, verified.states.data(), 257 * 1024);
    for (unsigned i = 0; i < 256; ++i)
      for (unsigned b = 0; b < 8; ++b)
        generated_trace[28 + 257 * 1024 + i * 8 + b] = verified.offsets[i] >> (8 * b);
    RustProofVerifier oracle(argv[4]);
    std::cout << oracle.verify(std::span(context).first(225), generated_trace, proof).summary << '\n';
    std::ofstream output(argv[5]);
    if (!output) throw std::runtime_error("output unavailable");
    using Clock = std::chrono::steady_clock;
    for (unsigned trial = 0; trial < 10; ++trial) {
      double elapsed_ms = 0;
      for (unsigned repeat = 0; repeat < 16; ++repeat) {
        const auto began = Clock::now();
        const auto walk = consensus::walk_v4_from_tile_sequence(seed, ntiles, tiles);
        elapsed_ms += std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        check(walk);
      }
      output << "{\"trial\":" << trial << ",\"walk_ms\":" << elapsed_ms / 16
             << ",\"walks\":16}\n" << std::flush;
    }
    std::cout << "CPU_WALK_REPLAY=PASS states=257 offsets=256 rust_generated_trace=true no_cuda=true\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
