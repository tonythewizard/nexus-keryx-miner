#include "keryx/nexus/developer_config.hpp"
#pragma once
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
namespace keryx::nexus {
inline constexpr std::string_view authorized_nvidia_vulkan_uuid="" NEXUS_PRIMARY_GPU_UUID "";
// Environment selection grants no work/submission authority. Native engine and
// helper still verify the physical identity, ICD and exclusive device lease.
inline bool nvidia_vulkan_selected() {
  bool selected=false;
  for(const auto name:{"NEXUS_VULKAN_DEVICE_UUID","NEXUS_VULKAN_TEST_UUID"}) {
    if(const char *value=std::getenv(name)) {
      if(std::string_view(value)!=authorized_nvidia_vulkan_uuid)
        throw std::invalid_argument("Unauthorized NVIDIA Vulkan UUID");
      selected=true;
    }
  }
  return selected;
}
}
