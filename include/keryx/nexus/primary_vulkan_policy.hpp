#pragma once
#include <stdexcept>
namespace keryx::nexus {
// Pure routing validation: selecting Vulkan cannot initialize CUDA for AI or
// silently substitute an ICD/device. Mixed-primary support needs its own gate.
inline void validate_primary_vulkan_policy(bool selected,bool primary_enabled,
    bool secondary_configured,bool vulkan_inference,bool independent_lanes=false) {
  if(!selected)return;
  if(!primary_enabled)throw std::runtime_error("Selected NVIDIA Vulkan device is OFF");
  if(secondary_configured&&!independent_lanes)throw std::runtime_error("NVIDIA Vulkan primary requires its exclusive ICD; secondary GPU must remain disabled");
  if(!vulkan_inference)throw std::runtime_error("JIT/Vulkan workflow requires Vulkan inference; native CUDA fallback is forbidden");
}
}
