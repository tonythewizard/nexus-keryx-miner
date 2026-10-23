#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/opoi.hpp"
#include <memory>
#include <optional>
namespace keryx::nexus::consensus {
struct ResidentSearchResult {
  std::optional<std::uint64_t> winner;
  std::uint64_t completed{};
  double kernel_ms{};
};
// Fixed one-proof staging only: no model duplication or speculative nonce queue.
struct ResidentWitnessTiles {
  std::array<std::uint64_t, walk_steps> offsets{};
  std::vector<std::array<std::array<std::uint8_t, tile_bytes>, 2>> pairs;
};
// Borrows ONE stock llama model. Model must outlive this engine. No primary-context reset.
class CudaResidentSearch final {
 public:
  CudaResidentSearch(const std::string &uuid, const model::ReadOnlyGguf &gguf,
                     const std::vector<opoi::ResidentTensor> &tensors);
  ~CudaResidentSearch();
  // Exact canonical bytes from the already verified resident model. Caller
  // must own PoM GPU admission; this does not allocate or launch a kernel.
  std::array<std::uint8_t, tile_bytes> read_tile(std::uint64_t tile);
  // Canonically adjacent Merkle siblings, fetched together without GPU allocation.
  std::array<std::array<std::uint8_t, tile_bytes>, 2> read_tile_pair(std::uint64_t tile);
  // Caller owns exclusive PoM admission. Returned offsets must be checked against
  // the independent CPU gather before any witness can be verified or submitted.
  ResidentWitnessTiles read_witness_tiles(std::uint64_t seed);
  // Reuses caller-owned host staging. GPU admission and lifetime remain with the caller.
  void read_witness_tiles_into(std::uint64_t seed, ResidentWitnessTiles &result);
  // Borrowed until the next gather or engine destruction; exclusive PoM admission required.
  const ResidentWitnessTiles &read_witness_tiles_cached(std::uint64_t seed);
  bool diagnostic_headroom_available() const;
  CudaResidentSearch(const CudaResidentSearch&)=delete;
  CudaResidentSearch& operator=(const CudaResidentSearch&)=delete;
  ResidentSearchResult run(const Hash &pph, std::uint64_t timestamp,
                           std::uint64_t daa, const Hash &target,
                           std::uint64_t start, std::uint64_t count,
                           const std::function<void(bool)> &after_enqueue = {});
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
