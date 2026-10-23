#pragma once
#include <optional>
#include <string>
#include <stdexcept>
#include <string_view>
#include <algorithm>
#include <cstdint>
namespace keryx::nexus {
// Fixed-workload Polaris replay: padded LDS, 256 threads and 1024 nonces.
// Keep engine capacity, admission limit and lane reservation size identical.
inline constexpr std::uint32_t secondary_vulkan_batch_size = 1024;
struct SecondaryVulkanConfig {
  std::string module, sha256;
  bool live_submit{false};
  bool resident{false};
};
inline std::optional<SecondaryVulkanConfig> secondary_vulkan_config(
    const char *mode, const char *module, const char *sha, bool submit_enabled) {
  const bool active = mode && *mode, has_module = module && *module, has_sha = sha && *sha;
  if (!active && !has_module && !has_sha) return std::nullopt;
  const std::string_view mode_str(mode ? mode : "");
  if (!active || (mode_str != "shadow" && mode_str != "live" && mode_str != "resident-shadow" && mode_str != "resident-live") || !has_module || !has_sha)
    throw std::invalid_argument("Secondary Vulkan requires explicit shadow or live mode, module and SHA-256");
  const std::string digest = sha;
  const bool is_bda = digest == "bda-native" && std::string_view(module) == "bda-native";
  if ((digest == "bda-native") != (std::string_view(module) == "bda-native") ||
      (is_bda && mode_str != "resident-shadow" && mode_str != "resident-live"))
    throw std::invalid_argument("BDA requires matching module/hash sentinels and resident mode");
  if (!is_bda && (digest.size() != 64 || !std::all_of(digest.begin(), digest.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      })))
    throw std::invalid_argument("Secondary Vulkan module SHA-256 must be canonical hex");
  const bool live_submit = (mode_str == "live" || mode_str == "resident-live") && submit_enabled;
  return SecondaryVulkanConfig{module, digest, live_submit,mode_str=="resident-shadow"||mode_str=="resident-live"};
}
}
