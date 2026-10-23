#pragma once

#include "keryx/nexus/consensus/pom_v4.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace keryx::nexus::consensus {
struct CudaDeviceInfo {
  int ordinal{};
  std::string name;
  std::string uuid;
  int major{};
  int minor{};
  std::uint64_t total_memory{};
  std::uint64_t free_memory{};
};

struct CudaWalkBatch {
  CudaDeviceInfo device;
  std::vector<std::uint64_t> offsets; // nonce-major, 256 per nonce
  std::vector<std::uint8_t> final_states; // nonce-major, 1024 bytes each
  std::vector<std::uint8_t> diagnostic_states; // optional, 257 * 1024 per nonce
  std::size_t unique_tile_count{};
  std::size_t tile_cache_hits{};
  std::size_t tile_cache_misses{};
  std::size_t tile_cache_vram_bytes{};
  std::size_t pinned_host_bytes{};
  std::size_t h2d_bytes{};
  std::size_t cuda_allocated_bytes{};
  double transfer_ms{};
  double cache_scatter_ms{};
  double device_to_host_ms{};
  double kernel_ms{};
  double total_ms{};
};

std::vector<CudaDeviceInfo> enumerate_cuda_devices();

class CudaWalkEngine {
public:
  CudaWalkEngine(const std::string &required_uuid, std::size_t max_nonces);
  CudaWalkEngine(const std::string &required_uuid, std::size_t max_nonces, bool proof_only);
  ~CudaWalkEngine();
  CudaWalkEngine(CudaWalkEngine &&) noexcept;
  CudaWalkEngine &operator=(CudaWalkEngine &&) noexcept;
  CudaWalkEngine(const CudaWalkEngine &) = delete;
  CudaWalkEngine &operator=(const CudaWalkEngine &) = delete;
  void run(std::uint64_t tile_count,
           const std::string &model_cache_identity,
           const std::vector<std::uint64_t> &seeds,
           const std::vector<std::uint8_t> &unique_tiles,
           const std::vector<std::uint64_t> &unique_tile_ids,
           const std::vector<std::uint32_t> &tile_slots,
           bool diagnostic_states, CudaWalkBatch &result,
           const std::function<void(bool)> &after_kernel_enqueue = {});
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace keryx::nexus::consensus
