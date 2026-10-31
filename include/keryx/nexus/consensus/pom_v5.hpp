#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"

namespace keryx::nexus::consensus {
inline constexpr std::uint64_t pom_v5_activation_daa = 140'115'000;
inline constexpr std::uint32_t v5_pad_rounds = 1920;
inline constexpr std::array<std::uint64_t, 5> v5_pad_tiles_by_tier{
    638976, 3213312, 4804608, 6471680, 2450432};
struct V5PadSpec {
  Hash seed{};
  std::uint64_t tiles{};
  bool operator==(const V5PadSpec &) const = default;
};
struct V5Proof {
  std::uint8_t tier{};
  // Pad positions have empty tiles AND paths, as required by the official v5 wire format.
  std::vector<std::vector<std::uint8_t>> tiles;
  std::vector<std::vector<Hash>> paths;
};
struct V5ModelProofBuild { Walk walk; V5Proof proof; };
V5PadSpec official_v5_pad(std::uint8_t tier, const Hash &model_id, const Hash &root);
std::array<std::uint64_t, 4> v5_pad_seed_words(const Hash &seed);
Hash v5_pad_entry(const Hash &seed, std::uint64_t index);
std::array<std::uint8_t, tile_bytes> v5_pad_tile(const Hash &seed, std::uint64_t tile);
std::uint32_t v5_pad_steps(std::uint64_t weight_tiles, std::uint64_t pad_tiles);
std::uint64_t v5_state_fold(const std::array<std::uint8_t, tile_bytes> &state);
Walk walk_v5_from_model(std::uint64_t seed, std::uint64_t weight_tiles,
                       const V5PadSpec &pad, const ModelTileReader &read_weight);
V5ModelProofBuild prove_v5_from_model(std::uint64_t seed, std::uint8_t tier,
    std::uint64_t weight_tiles, const V5PadSpec &pad,
    const ModelTileReader &read_weight, const ModelPathReader &read_path);
V5Proof prove_v5_from_walk_parallel(const Walk &walk, std::uint8_t tier,
    std::uint64_t weight_tiles,
    const ModelTileReader &read_weight, const ModelPathReader &read_path,
    unsigned num_threads = 8);
std::vector<std::uint8_t> borsh_v5_container(const V5Proof &proof,
                                           std::uint64_t state, const Hash &pow);
}
