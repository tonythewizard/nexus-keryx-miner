#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <span>
#include <functional>
#include <memory>
#include <cstdint>

#include <optional>

namespace keryx::nexus::vulkan {

struct BdaTensorDescriptor {
    std::string name;
    VkDeviceAddress device_address; // 64-bit GPU virtual address
    uint64_t bytes;
    uint64_t chunk_count;
};

struct BdaWalkBatch {
    std::vector<uint64_t> offsets;
    std::vector<uint8_t> final_states;
    std::vector<uint8_t> diagnostic_states;
    std::optional<uint32_t> winner_index;
};

struct BdaEngineStats {
    uint64_t batches_submitted = 0;
    uint64_t batches_completed = 0;
    uint64_t nonces_processed = 0;
    uint64_t preemptions_handled = 0;
    double last_dispatch_ms = 0.0;
};

class VulkanBdaResidentEngine final {
public:
    VulkanBdaResidentEngine(VkDevice device, VkPhysicalDevice physical_device,
                            uint32_t compute_queue_family, VkQueue queue,
                            const std::vector<uint32_t> &spirv_code);
    ~VulkanBdaResidentEngine();

    VulkanBdaResidentEngine(const VulkanBdaResidentEngine &) = delete;
    VulkanBdaResidentEngine &operator=(const VulkanBdaResidentEngine &) = delete;

    // Attach loaded model tensors in VRAM without duplicating weights
    void attach_resident_tensors(const std::vector<BdaTensorDescriptor> &tensors,
                                 uint64_t total_model_tiles);

    // Run resident PoM batch
    bool run_batch(std::span<const uint64_t> seeds,
                   bool diagnostics,
                   bool copy_offsets,
                   BdaWalkBatch &out,
                   uint64_t p0 = 0, uint64_t p1 = 0, uint64_t p2 = 0, uint64_t p3 = 0,
                   uint64_t t0 = 0, uint64_t t1 = 0, uint64_t t2 = 0, uint64_t t3 = 0,
                   const std::function<bool()> &cancelled = {},
                   const std::function<void()> &after_enqueue = {});

    // Instant Zero-Latency Preemption: drains in-flight GPU batch without destroying VRAM
    void pause_and_drain();
    void resume();

    bool is_ready() const noexcept;
    BdaEngineStats get_stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace keryx::nexus::vulkan
