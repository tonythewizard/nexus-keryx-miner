#include "keryx/nexus/grpc_header.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>
namespace keryx::nexus::consensus {
namespace {
std::uint8_t nibble(char c) {
  if (c >= '0' && c <= '9')
    return static_cast<std::uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<std::uint8_t>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F')
    return static_cast<std::uint8_t>(c - 'A' + 10);
  throw std::invalid_argument("protobuf header contains non-hex character");
}
Hash parse_hash(std::string_view s, bool allow_empty = false) {
  if (allow_empty && s.empty())
    return {};
  if (s.size() != 64)
    throw std::invalid_argument(
        "protobuf consensus hash must be exactly 32-byte hex");
  Hash h{};
  for (std::size_t i = 0; i < h.size(); i++)
    h[i] = static_cast<std::uint8_t>((nibble(s[2 * i]) << 4) |
                                     nibble(s[2 * i + 1]));
  return h;
}
std::vector<std::uint8_t> parse_blue_work(std::string_view s) {
  if (s.empty())
    return {};
  if (s.size() > 48)
    throw std::invalid_argument("protobuf blueWork exceeds Uint192 width");
  std::string padded;
  if ((s.size() & 1U) != 0) {
    padded.push_back('0');
    padded.append(s);
    s = padded;
  }
  std::vector<std::uint8_t> out(s.size() / 2);
  for (std::size_t i = 0; i < out.size(); i++)
    out[i] = static_cast<std::uint8_t>((nibble(s[2 * i]) << 4) |
                                       nibble(s[2 * i + 1]));
  auto first =
      std::find_if(out.begin(), out.end(), [](auto b) { return b != 0; });
  out.erase(out.begin(), first);
  return out;
}
} // namespace
Header header_from_rpc(const protowire::RpcBlockHeader &r) {
  if (r.version() > std::numeric_limits<std::uint16_t>::max())
    throw std::invalid_argument("header version exceeds u16");
  if (r.timestamp() < 0)
    throw std::invalid_argument("negative header timestamp");
  if (r.pomtier() > std::numeric_limits<std::uint8_t>::max())
    throw std::invalid_argument("PoM tier exceeds u8");
  Header h;
  h.version = static_cast<std::uint16_t>(r.version());
  h.hash_merkle_root = parse_hash(r.hashmerkleroot());
  h.accepted_id_merkle_root = parse_hash(r.acceptedidmerkleroot());
  h.utxo_commitment = parse_hash(r.utxocommitment());
  h.timestamp = static_cast<std::uint64_t>(r.timestamp());
  h.bits = r.bits();
  h.nonce = r.nonce();
  h.daa_score = r.daascore();
  h.blue_work_big_endian = parse_blue_work(r.bluework());
  h.blue_score = r.bluescore();
  h.pruning_point = parse_hash(r.pruningpoint());
  h.pom_final_state = r.pomfinalstate();
  h.service_state_hash = parse_hash(r.servicestatehash(), true);
  h.pom_tier = static_cast<std::uint8_t>(r.pomtier());
  h.task_root = parse_hash(r.taskroot(), true);
  if (r.parents_size() > 255)
    throw std::invalid_argument(
        "header parent-level count exceeds u8 consensus limit");
  h.parents_by_level.reserve(static_cast<std::size_t>(r.parents_size()));
  for (const auto &level : r.parents()) {
    std::vector<Hash> parents;
    parents.reserve(static_cast<std::size_t>(level.parenthashes_size()));
    for (const auto &parent : level.parenthashes())
      parents.push_back(parse_hash(parent));
    h.parents_by_level.push_back(std::move(parents));
  }
  return h;
}
Hash block_hash_for_daa(const Header &h, std::uint64_t nonce,
                        std::uint64_t timestamp) {
  return hash_header_override(h, nonce, timestamp, h.daa_score >= 43'450'000,
                              h.daa_score >= h6_activation_daa,
                              h.daa_score >= block_task_activation_daa);
}
} // namespace keryx::nexus::consensus
