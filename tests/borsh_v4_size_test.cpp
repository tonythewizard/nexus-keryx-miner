#include "keryx/nexus/consensus/pom_v4.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace keryx::nexus::consensus;

// Independent field-by-field reader: does not reuse the serializer's size formula.
class Reader {
 public:
  explicit Reader(const std::vector<std::uint8_t> &data) : data_(data) {}
  std::uint64_t number(unsigned bytes) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= std::uint64_t(data_.at(cursor_++)) << (8 * i);
    return value;
  }
  void bytes(std::span<const std::uint8_t> expected) {
    for (auto value : expected) if (data_.at(cursor_++) != value) throw std::runtime_error("Borsh bytes changed");
  }
  bool done() const { return cursor_ == data_.size(); }
 private:
  const std::vector<std::uint8_t> &data_;
  std::size_t cursor_{};
};

int main() {
  try {
    for (unsigned height : {0U, 1U, 16U, 18U, 23U, 64U}) {
      V4Proof proof;
      proof.tier = height % 5;
      proof.tiles.resize(256);
      proof.paths.resize(256);
      Hash pow{};
      for (unsigned i = 0; i < pow.size(); ++i) pow[i] = i * 3;
      for (unsigned i = 0; i < 256; ++i) {
        for (unsigned j = 0; j < tile_bytes; ++j) proof.tiles[i][j] = (i + j) % 256;
        proof.paths[i].resize(i % 2 ? height : height / 2);
        for (unsigned j = 0; j < proof.paths[i].size(); ++j) proof.paths[i][j].fill((i + j) % 256);
      }
      constexpr std::uint64_t final = 0xfedcba9876543210ULL;
      const auto wire = borsh_v4_container(proof, final, pow);
      Reader reader(wire);
      auto require = [](bool ok) { if (!ok) throw std::runtime_error("Borsh field mismatch"); };
      require(reader.number(1) == proof.tier);
      reader.bytes(Hash{});
      reader.bytes(pow);
      require(reader.number(8) == final);
      for (unsigned i = 0; i < 3; ++i) require(reader.number(4) == 0);
      require(reader.number(1) == 0);
      require(reader.number(1) == 0);
      require(reader.number(1) == 1);
      require(reader.number(1) == proof.tier);
      require(reader.number(4) == 256);
      for (const auto &tile : proof.tiles) {
        require(reader.number(4) == 1024);
        reader.bytes(tile);
      }
      require(reader.number(4) == 256);
      for (const auto &path : proof.paths) {
        require(reader.number(4) == path.size());
        for (const auto &hash : path) reader.bytes(hash);
      }
      require(reader.done());
    }
    V4Proof bad;
    bool rejected = false;
    try { (void)borsh_v4_container(bad, 0, Hash{}); }
    catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("Incomplete proof accepted");
    bad.tier = 5;
    rejected = false;
    try { (void)borsh_v4_container(bad, 0, Hash{}); }
    catch (const std::out_of_range &) { rejected = true; }
    if (!rejected) throw std::runtime_error("Invalid tier accepted");
    std::cout << "BORSH_V4_SIZE=PASS field_parity=6 invalid_inputs=2 no_cuda=true\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
