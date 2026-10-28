#pragma once
#include "keryx/nexus/pipeline.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace keryx::nexus::consensus {
using Hash = std::array<std::uint8_t, 32>;
inline constexpr std::size_t dimension = 32;
inline constexpr std::size_t tile_bytes = 1024;
inline constexpr std::size_t chunks_per_tile = 32;
inline constexpr std::size_t walk_steps = 256;
inline constexpr std::uint64_t h10_activation_daa = 87'360'000;
inline constexpr std::uint64_t h14_activation_daa = 121'985'000;
// Mirrors official block_task_activation_daa(): disabled on both networks.
inline constexpr std::uint64_t block_task_activation_daa = UINT64_MAX;
inline constexpr std::uint64_t pom_v4_activation_daa = 79'210'000;
inline constexpr std::uint64_t h6_activation_daa = 76'316'623;

enum class SeedEra { v4, h10, h14 };
struct Walk {
  std::vector<std::array<std::uint8_t, tile_bytes>> states;
  std::vector<std::uint64_t> offsets;
};
struct V4Proof {
  std::uint8_t tier{};
  std::vector<std::array<std::uint8_t, tile_bytes>> tiles;
  std::vector<std::vector<Hash>> paths;
};
struct ModelProofBuild { Walk walk; V4Proof proof; };
using ModelTileReader = std::function<std::array<std::uint8_t, tile_bytes>(std::uint64_t)>;
using ModelPathReader = std::function<std::vector<Hash>(std::uint64_t)>;
struct ModelWalkInput {
  std::vector<std::uint64_t> offsets;
  std::vector<std::uint8_t> tiles;
};
struct CompactTarget {
  Hash little_endian{};
  bool zero{};
};
struct TierAnchor {
  Hash root{};
  std::uint64_t chunks{};
};
struct CandidateBinding {
  std::uint64_t generation{};
  std::uint64_t nonce{};
  std::uint64_t timestamp{};
  Hash pre_pow_hash{};
  Hash job_fingerprint{};
  JobLease lease;
};
enum class CandidateBindingStatus { Valid, StaleLease, IntegrityMismatch };

std::uint64_t mix64(std::uint64_t x) noexcept;
SeedEra select_seed_era(std::uint64_t daa_score);
TierAnchor official_tier_anchor(std::uint64_t daa_score, std::uint8_t tier);
CandidateBinding bind_candidate(const TemplateJob &job, std::uint64_t nonce,
                                std::uint64_t timestamp,
                                const Hash &pre_pow_hash);
bool candidate_matches_job(const CandidateBinding &candidate,
                           const TemplateJob &job) noexcept;
CandidateBindingStatus candidate_binding_status(const CandidateBinding &candidate,
                                                const TemplateJob &job) noexcept;
std::uint64_t block_seed(const Hash &pre_pow_hash, std::uint64_t timestamp,
                         std::uint64_t nonce, std::uint64_t daa_score);
std::array<std::uint64_t,25> seed_sponge_state(const Hash &pph, std::uint64_t timestamp, std::uint64_t daa);
std::uint64_t seed_h10(const Hash &pre_pow_hash, std::uint64_t timestamp,
                       std::uint64_t nonce);
std::uint64_t seed_h14(const Hash &pre_pow_hash, std::uint64_t timestamp,
                       std::uint64_t nonce);
CompactTarget decode_compact_target(std::uint32_t bits);
bool little_endian_leq(const Hash &left, const Hash &right) noexcept;
Hash blake3_hash(std::span<const std::uint8_t> bytes);
Hash hash_pair(const Hash &left, const Hash &right);
Hash merkle_root(std::span<const std::uint8_t> blob);
std::vector<std::uint8_t> make_reference_test_blob();
Walk walk_v4(std::uint64_t seed, std::span<const std::uint8_t> blob);
std::array<std::uint8_t, tile_bytes> v4_initial_state_bytes(std::uint64_t seed);
void v4_transition_into(std::array<std::uint8_t, tile_bytes> &out,
                        const std::array<std::uint8_t, tile_bytes> &state,
                        const std::array<std::uint8_t, tile_bytes> &tile, std::uint32_t step);
Hash v4_initial_state_hash(std::uint64_t seed);
std::uint64_t final_state(const Walk &walk);
std::uint64_t final_state_from_bytes(std::span<const std::uint8_t, 1024> bytes);
V4Proof prove_v4(std::uint64_t seed, std::uint8_t tier,
                 std::span<const std::uint8_t> blob);
ModelProofBuild prove_v4_from_model(std::uint64_t seed, std::uint8_t tier,
                                    std::uint64_t n_tiles,
                                    const ModelTileReader &read_tile,
                                    const ModelPathReader &read_path);
ModelWalkInput gather_v4_model_tiles(std::uint64_t seed, std::uint64_t n_tiles,
                                     const ModelTileReader &read_tile);
Walk walk_v4_from_tile_sequence(std::uint64_t seed, std::uint64_t n_tiles,
                                std::span<const std::uint8_t> tiles);
V4Proof prove_v4_from_walk(const Walk &walk, std::uint8_t tier,
                           std::span<const std::uint8_t> tiles,
                           const ModelPathReader &read_path);
Hash pom_pow_value_h3(std::uint64_t state, const Hash &pre_pow_hash);
std::vector<std::uint8_t> borsh_v4_container(const V4Proof &proof,
                                             std::uint64_t state,
                                             const Hash &pow_value);

struct Header {
  std::uint16_t version{};
  std::vector<std::vector<Hash>> parents_by_level;
  Hash hash_merkle_root{};
  Hash accepted_id_merkle_root{};
  Hash utxo_commitment{};
  std::uint64_t timestamp{};
  std::uint32_t bits{};
  std::uint64_t nonce{};
  std::uint64_t daa_score{};
  std::vector<std::uint8_t> blue_work_big_endian;
  std::uint64_t blue_score{};
  Hash pruning_point{};
  std::uint64_t pom_final_state{};
  Hash service_state_hash{};
  std::uint8_t pom_tier{};
  Hash task_root{};
};
Hash hash_header_override(const Header &header, std::uint64_t nonce,
                          std::uint64_t timestamp, bool include_pom_final_state,
                          bool include_h6_fields, bool include_task_root = false);
Hash pre_pow_hash(const Header &header);
} // namespace keryx::nexus::consensus
