#include "keryx/nexus/grpc_header.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace keryx::nexus::consensus;
static std::uint32_t u32(const std::vector<std::uint8_t> &b, std::size_t &p) {
  if (p + 4 > b.size())
    throw std::runtime_error("truncated header oracle");
  std::uint32_t x = 0;
  for (unsigned i = 0; i < 4; i++)
    x |= std::uint32_t(b[p++]) << (i * 8);
  return x;
}
static std::uint64_t u64(std::span<const std::uint8_t, 8> b) {
  std::uint64_t x = 0;
  for (unsigned i = 0; i < 8; i++)
    x |= std::uint64_t(b[i]) << (i * 8);
  return x;
}
static std::string hex_bytes(const Hash &h) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (auto b : h) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 15]);
  }
  return out;
}
static protowire::RpcBlockHeader make_rpc() {
  protowire::RpcBlockHeader r;
  r.set_version(1);
  auto *l0 = r.add_parents();
  l0->add_parenthashes(std::string(64, '1'));
  l0->add_parenthashes(std::string(64, '2'));
  auto *l1 = r.add_parents();
  l1->add_parenthashes(std::string(64, '3'));
  auto *l2 = r.add_parents();
  l2->add_parenthashes(std::string(64, '3'));
  r.set_hashmerkleroot(std::string(64, '4'));
  r.set_acceptedidmerkleroot(std::string(64, '5'));
  r.set_utxocommitment(std::string(64, '6'));
  r.set_timestamp(1'700'000'123);
  r.set_bits(0x1d00ffff);
  r.set_nonce(0x0102030405060708);
  r.set_daascore(121'211'211);
  r.set_bluework("0102030405060708");
  r.set_bluescore(0x1122334455667788);
  r.set_pruningpoint(std::string(64, '7'));
  r.set_pomfinalstate(0x8877665544332211);
  r.set_servicestatehash(std::string(64, '8'));
  r.set_pomtier(3);
  return r;
}
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error(
          "usage: nexus-header-parity-test OFFICIAL_VECTOR");
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<std::uint8_t> o((std::istreambuf_iterator<char>(f)), {});
    if (o.size() < 48 || std::string(o.begin(), o.begin() + 4) != "NXHD")
      throw std::runtime_error("invalid official header vector");
    std::size_t p = 4;
    Hash expected_override{};
    std::copy_n(o.data() + p, 32, expected_override.data());
    p += 32;
    Hash expected_pph{};
    std::copy_n(o.data() + p, 32, expected_pph.data());
    p += 32;
    const auto h10 = u64(std::span<const std::uint8_t, 8>(o.data() + p, 8));
    p += 8;
    const auto h14 = u64(std::span<const std::uint8_t, 8>(o.data() + p, 8));
    p += 8;
    const auto v4seed = u64(std::span<const std::uint8_t, 8>(o.data() + p, 8));
    p += 8;
    Hash expected_block{};
    std::copy_n(o.data() + p, 32, expected_block.data());
    p += 32;
    Header h;
    h.version = 1;
    h.parents_by_level = {{Hash{}, Hash{}}, {Hash{}}, {Hash{}}};
    h.parents_by_level[0][0].fill(0x11);
    h.parents_by_level[0][1].fill(0x22);
    h.parents_by_level[1][0].fill(0x33);
    h.parents_by_level[2][0].fill(0x33);
    h.hash_merkle_root.fill(0x44);
    h.accepted_id_merkle_root.fill(0x55);
    h.utxo_commitment.fill(0x66);
    h.timestamp = 1'700'000'123;
    h.bits = 0x1d00ffff;
    h.nonce = 0x0102030405060708;
    h.daa_score = 121'211'211;
    h.blue_work_big_endian = {1, 2, 3, 4, 5, 6, 7, 8};
    h.blue_score = 0x1122334455667788;
    h.pruning_point.fill(0x77);
    h.pom_final_state = 0x8877665544332211;
    h.service_state_hash.fill(0x88);
    h.pom_tier = 3;
    const auto override_hash = hash_header_override(
        h, 0x0f1e2d3c4b5a6978, 1'700'000'321, false, false);
    if (override_hash != expected_override)
      throw std::runtime_error("header override hash differs from Rust oracle");
    const auto pre = pre_pow_hash(h);
    if (pre != expected_pph)
      throw std::runtime_error(
          "pre-PoW zero nonce/time hash differs from Rust oracle");
    if (seed_h10(pre, 1'700'000'321, 0x0f1e2d3c4b5a6978) != h10)
      throw std::runtime_error("H10 seed differs from Rust oracle");
    if (seed_h14(pre, 1'700'000'321, 0x0f1e2d3c4b5a6978) != h14)
      throw std::runtime_error("H14 seed differs from Rust oracle");
    if (block_seed(pre, 1'700'000'321, 0x0f1e2d3c4b5a6978, 79'210'000) !=
        v4seed)
      throw std::runtime_error("v4-era seed differs from Rust oracle");
    if (hash_header_override(h, h.nonce, h.timestamp, true, true) !=
        expected_block)
      throw std::runtime_error(
          "fork-active header hash differs from Rust oracle");
    auto rpc = make_rpc();
    const auto parsed = header_from_rpc(rpc);
    if (pre_pow_hash(parsed) != expected_pph)
      throw std::runtime_error(
          "protobuf-to-header conversion differs from official Rust");
    if (block_hash_for_daa(parsed, parsed.nonce, parsed.timestamp) !=
        expected_block)
      throw std::runtime_error(
          "protobuf template full header hash differs from official Rust");
    bool bad_hex = false;
    try {
      auto bad = rpc;
      bad.set_hashmerkleroot("xyz");
      (void)header_from_rpc(bad);
    } catch (const std::invalid_argument &) {
      bad_hex = true;
    }
    if (!bad_hex)
      throw std::runtime_error("invalid protobuf hash accepted");
    bool bad_work = false;
    try {
      auto bad = rpc;
      bad.set_bluework(std::string(50, 'f'));
      (void)header_from_rpc(bad);
    } catch (const std::invalid_argument &) {
      bad_work = true;
    }
    if (!bad_work)
      throw std::runtime_error("blueWork wider than Uint192 accepted");
    bool bad_time = false;
    try {
      auto bad = rpc;
      bad.set_timestamp(-1);
      (void)header_from_rpc(bad);
    } catch (const std::invalid_argument &) {
      bad_time = true;
    }
    if (!bad_time)
      throw std::runtime_error("negative timestamp accepted");
    const auto n = u32(o, p);
    for (std::uint32_t i = 0; i < n; i++) {
      const auto bits = u32(o, p);
      if (p + 32 > o.size())
        throw std::runtime_error("target fixture truncated");
      Hash target{};
      std::copy_n(o.data() + p, 32, target.data());
      p += 32;
      if (decode_compact_target(bits).little_endian != target)
        throw std::runtime_error("compact target mismatch for bits=" +
                                 std::to_string(bits));
    }
    for (const std::uint64_t activation : {76'316'623ULL, 121'985'000ULL}) {
      if (p >= o.size() || o[p++] != 5)
        throw std::runtime_error("official tier count mismatch");
      for (std::uint8_t tier = 0; tier < 5; tier++) {
        if (p + 40 > o.size())
          throw std::runtime_error("official tier anchor vector truncated");
        Hash expected_root{};
        std::copy_n(o.data() + p, 32, expected_root.data());
        p += 32;
        const auto expected_chunks =
            u64(std::span<const std::uint8_t, 8>(o.data() + p, 8));
        p += 8;
        const auto actual = official_tier_anchor(activation, tier);
        if (actual.root != expected_root || actual.chunks != expected_chunks)
          throw std::runtime_error(
              "official tier anchor mismatch at activation=" +
              std::to_string(activation) + " tier=" + std::to_string(tier));
      }
    }
    if (p != o.size())
      throw std::runtime_error("trailing bytes in header vector");
    const auto h6_anchor = official_tier_anchor(121'984'999, 3);
    const auto h14_anchor = official_tier_anchor(121'985'000, 3);
    if (h6_anchor.root == h14_anchor.root || h6_anchor.chunks != 516'762'688 ||
        h14_anchor.chunks != 524'991'232)
      throw std::runtime_error(
          "official H6/H14 tier-3 anchor selection mismatch");
    for (std::uint8_t tier = 0; tier < 5; tier++)
      if (tier != 3 && official_tier_anchor(121'984'999, tier).root !=
                           official_tier_anchor(121'985'000, tier).root)
        throw std::runtime_error("unexpected H14 tier anchor change");
    bool pre_v4_rejected = false;
    try {
      (void)select_seed_era(79'209'999);
    } catch (const std::invalid_argument &) {
      pre_v4_rejected = true;
    }
    if (!pre_v4_rejected || select_seed_era(79'210'000) != SeedEra::v4 ||
        select_seed_era(87'360'000) != SeedEra::h10 ||
        select_seed_era(121'984'999) != SeedEra::h10 ||
        select_seed_era(121'985'000) != SeedEra::h14)
      throw std::runtime_error("DAA fork selection mismatch");
    std::cout << "HEADER_TARGET_SEED_PARITY=PASS official_Rust=1 "
                 "protobuf_conversion=PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "HEADER_PARITY=FAIL " << e.what() << "\n";
    return 1;
  }
}
