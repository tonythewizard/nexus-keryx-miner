#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace keryx::nexus {
enum class GpuBackend { CudaDriver, VulkanJit, VulkanBda };

inline GpuBackend parse_gpu_backend(const char* value) {
  if (!value || std::string_view(value) == "cuda-driver") return GpuBackend::CudaDriver;
  if (std::string_view(value) == "vulkan-jit") return GpuBackend::VulkanJit;
  if (std::string_view(value) == "vulkan-bda") return GpuBackend::VulkanBda;
  throw std::runtime_error("unknown NEXUS_POM_BACKEND; expected cuda-driver, vulkan-jit, or vulkan-bda");
}

// Routing policy, not a claim that all CUDA or Vulkan devices are validated.
// This native gate cannot be overridden by the Python GPU setup profile.
inline GpuBackend require_production_gpu_backend(const char* backend,
    const char* device_id, std::string_view authorized_cuda_uuid, bool vulkan_worker=false) {
  const auto selected = parse_gpu_backend(backend);
  if ((selected == GpuBackend::VulkanJit || selected == GpuBackend::VulkanBda) && !vulkan_worker)
    throw std::runtime_error("Vulkan backend is offline-only: resident search, inference and production scheduling are not validated; no GPU fallback");
  if (((selected == GpuBackend::VulkanJit || selected == GpuBackend::VulkanBda) && !device_id) ||
      (device_id && std::string_view(device_id) != authorized_cuda_uuid))
    throw std::runtime_error("NEXUS_POM_DEVICE_ID must match the authorized CUDA UUID; no automatic device substitution");
  return selected;
}
} // namespace keryx::nexus
