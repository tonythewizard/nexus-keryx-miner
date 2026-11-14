// GPU/host parity of the Vulkan runtime (tools/runtime-vulkan). For synthetic H16-era jobs over
// the official tier-0 model, the winner the runtime's keryx_llama_pom_mine_v5 reports must meet
// the target on Nexus's own host walk, and no earlier nonce of its batch may. Uses the GPU;
// nothing is mined or submitted.
//
//   nexus-vulkan-runtime-parity-test RUNTIME_LIBRARY MODEL.gguf [JOBS]
#include "keryx/nexus/consensus/pom_v5.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/platform/shared_library.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>

using namespace keryx::nexus;
using consensus::Hash;

namespace {
constexpr Hash official_model{
    0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,
    0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
constexpr Hash official_root{
    0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,
    0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};
// The worker's job encoding (src/model/live_vulkan_worker.cpp): POW words carry the H3 salt,
// seed words the H14 tag.
constexpr std::uint64_t h3_salt[4] = {
    0x7C99D381176D4EC4ULL, 0xC2E28E3E28118C36ULL, 0xD496CE1B129B76CAULL, 0x47CF0979FA580BCEULL};
constexpr char h14_tag[] = "KERYX-H14-PRIVATE-INFERENCE-SEED";
// Past the H16 gate: v5 walk, H14 seed and the H16 pad table.
constexpr std::uint64_t daa = 150'000'000;
constexpr std::uint32_t batch = 4096;

using Load = void *(*)(const char *, int, int);
using Free = void (*)(void *);
using LastError = const char *(*)();
using MineV5 = std::int64_t (*)(void *, const std::uint64_t *, const std::uint64_t *, const std::uint64_t *,
                                std::uint64_t, std::uint64_t, std::uint32_t, const std::uint64_t *, std::uint64_t);

std::uint64_t le64(const std::uint8_t *b) {
  std::uint64_t w = 0;
  for (unsigned i = 0; i < 8; ++i) w |= std::uint64_t(b[i]) << (8 * i);
  return w;
}

template <class T> T symbol(void *library, const char *name) {
  auto *found = reinterpret_cast<T>(platform::library_symbol(library, name));
  if (!found) throw std::runtime_error(std::string("runtime lacks ") + name);
  return found;
}
} // namespace

int main(int argc, char **argv) try {
  if (argc < 3) {
    std::cerr << "usage: nexus-vulkan-runtime-parity-test RUNTIME_LIBRARY MODEL.gguf [JOBS]\n";
    return 2;
  }
  const int jobs = argc > 3 ? std::stoi(argv[3]) : 4;
  void *library = platform::open_library(std::filesystem::absolute(argv[1]));
  if (!library) throw std::runtime_error("cannot load the runtime library");
  const auto load = symbol<Load>(library, "keryx_llama_load");
  const auto release = symbol<Free>(library, "keryx_llama_free");
  const auto last_error = symbol<LastError>(library, "keryx_llama_last_error");
  const auto mine = symbol<MineV5>(library, "keryx_llama_pom_mine_v5");

  model::ReadOnlyGguf gguf(argv[2]);
  const std::uint64_t tiles = gguf.chunk_count() / 32;
  const auto pad = consensus::official_v5_pad(0, official_model, official_root, daa);
  const auto pad_words = consensus::v5_pad_seed_words(pad.seed);
  const auto read_tile = [&](std::uint64_t tile) {
    std::array<std::uint8_t, consensus::tile_bytes> bytes;
    gguf.read_tile_into(tile, bytes);
    return bytes;
  };

  void *handle = load(argv[2], 0, 512);
  if (!handle) throw std::runtime_error(std::string("runtime load failed: ") + last_error());

  // About one nonce in 256 meets this target.
  Hash target;
  target.fill(0xff);
  target[31] = 0;
  std::uint64_t t[4];
  for (unsigned i = 0; i < 4; ++i) t[i] = le64(target.data() + 8 * i);

  std::mt19937_64 rng(0x4b455259);
  int failures = 0;
  for (int job = 0; job < jobs; ++job) {
    Hash pph;
    for (auto &b : pph) b = static_cast<std::uint8_t>(rng());
    const std::uint64_t timestamp = 1'790'000'000'000ULL + static_cast<std::uint64_t>(job);
    const std::uint64_t start = rng() >> 1;
    std::uint64_t p[4], s[4];
    for (unsigned i = 0; i < 4; ++i) {
      std::array<std::uint8_t, 8> tagged;
      for (unsigned b = 0; b < 8; ++b)
        tagged[b] = static_cast<std::uint8_t>(pph[8 * i + b] ^ static_cast<std::uint8_t>(h14_tag[8 * i + b]));
      p[i] = le64(pph.data() + 8 * i) ^ h3_salt[i];
      s[i] = le64(tagged.data());
    }
    const auto offset = mine(handle, p, s, t, timestamp, start, batch, pad_words.data(), pad.tiles);
    if (offset < 0 || offset >= batch) {
      std::cerr << "job " << job << ": no winner in the batch (" << offset << ") " << last_error() << "\n";
      ++failures;
      continue;
    }
    // The host walk of every nonce up to the winner: only the winner may meet the target.
    for (std::int64_t n = 0; n <= offset; ++n) {
      const auto seed = consensus::block_seed(pph, timestamp, start + static_cast<std::uint64_t>(n), daa);
      const auto walk = consensus::walk_v5_from_model(seed, tiles, pad, read_tile);
      const bool meets = consensus::little_endian_leq(
          consensus::pom_pow_value_h3(consensus::final_state(walk), pph), target);
      if (meets != (n == offset)) {
        std::cerr << "job " << job << ": host walk " << (meets ? "meets" : "misses") << " the target at offset "
                  << n << ", GPU winner offset " << offset << "\n";
        ++failures;
        break;
      }
    }
    std::cout << "job " << job << ": GPU winner offset " << offset << ", host walk agrees over "
              << offset + 1 << " nonces\n";
  }
  release(handle);
  std::cout << (failures ? "PARITY=FAIL" : "PARITY=PASS") << " jobs=" << jobs << "\n";
  return failures ? 1 : 0;
} catch (const std::exception &e) {
  std::cerr << "PARITY=ERROR " << e.what() << "\n";
  return 1;
}
