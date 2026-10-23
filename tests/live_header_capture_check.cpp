#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "messages.pb.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace keryx::nexus::consensus;
static std::string hex(const Hash &h) {
  static constexpr char d[] = "0123456789abcdef";
  std::string o;
  o.reserve(64);
  for (auto b : h) {
    o.push_back(d[b >> 4]);
    o.push_back(d[b & 15]);
  }
  return o;
}
static void u8(std::vector<std::uint8_t> &o, std::uint8_t x) { o.push_back(x); }
static void u16(std::vector<std::uint8_t> &o, std::uint16_t x) {
  for (unsigned i = 0; i < 2; i++)
    o.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
static void u32(std::vector<std::uint8_t> &o, std::uint32_t x) {
  for (unsigned i = 0; i < 4; i++)
    o.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
static void u64(std::vector<std::uint8_t> &o, std::uint64_t x) {
  for (unsigned i = 0; i < 8; i++)
    o.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
static void bytes(std::vector<std::uint8_t> &o, const Hash &h) {
  o.insert(o.end(), h.begin(), h.end());
}
static void append_header(std::vector<std::uint8_t> &o, const Header &h) {
  u16(o, h.version);
  u32(o, static_cast<std::uint32_t>(h.parents_by_level.size()));
  for (const auto &l : h.parents_by_level) {
    u32(o, static_cast<std::uint32_t>(l.size()));
    for (const auto &p : l)
      bytes(o, p);
  }
  bytes(o, h.hash_merkle_root);
  bytes(o, h.accepted_id_merkle_root);
  bytes(o, h.utxo_commitment);
  u64(o, h.timestamp);
  u32(o, h.bits);
  u64(o, h.nonce);
  u64(o, h.daa_score);
  u32(o, static_cast<std::uint32_t>(h.blue_work_big_endian.size()));
  o.insert(o.end(), h.blue_work_big_endian.begin(),
           h.blue_work_big_endian.end());
  u64(o, h.blue_score);
  bytes(o, h.pruning_point);
  u64(o, h.pom_final_state);
  bytes(o, h.service_state_hash);
  u8(o, h.pom_tier);
}
int main(int argc, char **argv) {
  try {
    if (argc < 3)
      throw std::runtime_error(
          "usage: capture-check VECTOR_OUT CAPTURE [CAPTURE...]");
    std::size_t total = 0;
    std::vector<std::vector<std::uint8_t>> headers;
    for (int a = 2; a < argc; a++) {
      const auto records = keryx::nexus::read_capture(argv[a]);
      for (const auto &r : records) {
        protowire::KaspadResponse response;
        if (!response.ParseFromArray(
                r.response_wire.data(),
                static_cast<int>(r.response_wire.size())) ||
            !response.has_getblocktemplateresponse())
          throw std::runtime_error("capture is not a template response");
        const auto &t = response.getblocktemplateresponse();
        if (!t.has_block() || !t.block().has_header() || !t.issynced())
          throw std::runtime_error(
              "capture has missing header or unsynced node status");
        const auto &rpc = t.block().header();
        const auto h = header_from_rpc(rpc);
        const auto computed = block_hash_for_daa(h, h.nonce, h.timestamp);
        if (!rpc.hash().empty() && hex(computed) != rpc.hash())
          throw std::runtime_error("present RPC header hash mismatches "
                                   "official serialization at DAA=" +
                                   std::to_string(h.daa_score));
        const auto target = decode_compact_target(h.bits);
        if (target.zero)
          throw std::runtime_error("template has zero/invalid compact target");
        (void)official_tier_anchor(h.daa_score, h.pom_tier);
        const auto pph = pre_pow_hash(h);
        const auto seed = block_seed(pph, h.timestamp, h.nonce, h.daa_score);
        std::vector<std::uint8_t> row;
        append_header(row, h);
        bytes(row, computed);
        u64(row, seed);
        bytes(row, target.little_endian);
        headers.push_back(std::move(row));
        std::cout << "LIVE_CAPTURE_FIELDS=PASS network=" << r.network
                  << " daa=" << h.daa_score << " tier=" << unsigned(h.pom_tier)
                  << " seed_era="
                  << (select_seed_era(h.daa_score) == SeedEra::h14 ? "H14"
                                                                   : "H10")
                  << " rpc_hash="
                  << (rpc.hash().empty() ? "omitted" : "present")
                  << " computed=" << hex(computed) << "\n";
        ++total;
      }
    }
    std::vector<std::uint8_t> out{'N', 'X', 'R', 'H'};
    u32(out, static_cast<std::uint32_t>(headers.size()));
    for (const auto &h : headers) {
      u32(out, static_cast<std::uint32_t>(h.size()));
      out.insert(out.end(), h.begin(), h.end());
    }
    std::ofstream f(argv[1], std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(out.data()),
            static_cast<std::streamsize>(out.size()));
    if (!f)
      throw std::runtime_error("cannot write live header vectors");
    std::cout << "LIVE_CAPTURE_SUMMARY records=" << total
              << " submit=DISABLED proof=NOT_BUILT\n";
    return total ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "LIVE_CAPTURE_CHECK=FAIL " << e.what() << "\n";
    return 1;
  }
}
