#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
namespace keryx::nexus::consensus {
struct VulkanWalkBatch {
  std::vector<std::uint64_t> offsets;
  std::vector<std::uint8_t> final_states, diagnostic_states;
  std::optional<std::uint32_t> winner_index;
};
struct VulkanEngineStats {
  std::uint64_t submitted{},completed{},cancelled_before_dispatch{},discarded_after_completion{};
  std::uint64_t allocated_bytes{};
};
// Experimental persistent Vulkan engine. Does not discover jobs or submit proofs.
// Caller must hold the per-PCI device lease and configure the AMD-only ICD
// before any Vulkan calls/threads. NVIDIA offline replay requires its explicit
// test UUID/ICD instead. SHA comes from the validated JIT manifest.
class VulkanWalkEngine final {
public:
  VulkanWalkEngine(const std::string &pci_id,const std::filesystem::path &module,
                   const std::string &expected_sha256,std::size_t capacity,
                   std::uint64_t resident_tiles=0,bool cached_readback=true);
  // Upload canonical tensor bytes in bounded staging chunks, never raw GGUF.
  // False means paused between drained upload chunks; the next call resumes.
  bool load_resident(const std::function<void(std::uint64_t,std::span<std::uint8_t>)> &read,
                     const std::function<bool()> &cancelled={});
  // Drain the lane first. Model banks are recreated on the next upload.
  void release_resident();
  bool resident() const;
  std::uint64_t resident_split_tiles() const;
  ~VulkanWalkEngine();
  VulkanWalkEngine(const VulkanWalkEngine &)=delete;
  VulkanWalkEngine &operator=(const VulkanWalkEngine &)=delete;
  // False means cancelled; output is empty. A submitted kernel always drains
  // before return, even when cancelled. Never interrupt another GPU context.
  bool run(std::uint64_t logical_tiles,std::span<const std::uint64_t> seeds,
           std::span<const std::uint8_t> packed_tiles,std::span<const std::uint32_t> slots,
           bool diagnostics,VulkanWalkBatch &output,
           const std::function<bool()> &cancelled={},
           const std::function<void()> &after_enqueue={},
           bool copy_offsets=true,
           const std::uint64_t *words=nullptr, const std::uint64_t *targets=nullptr);
  VulkanEngineStats stats() const;
  bool screens_target_on_gpu() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
