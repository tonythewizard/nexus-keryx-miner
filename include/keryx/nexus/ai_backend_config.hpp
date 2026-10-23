#pragma once
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
namespace keryx::nexus::opoi {
struct VulkanAiConfig {std::string helper,helper_sha,library,library_sha;};
// No GPU or environment mutation. CUDA remains the default; no silent fallback.
inline std::optional<VulkanAiConfig> ai_backend_config(const char *backend,const char *helper,
    const char *helper_sha,const char *library,const char *library_sha,bool resident_lane) {
  const auto present=[](const char *s){return s&&*s;};
  const std::string_view selected=backend?backend:"cuda";
  if(selected=="cuda"){
    if(present(helper)||present(helper_sha)||present(library)||present(library_sha))
      throw std::invalid_argument("Vulkan AI artifacts require explicit backend selection");
    return std::nullopt;
  }
  if(selected!="vulkan")throw std::invalid_argument("NEXUS_AI_BACKEND must be cuda or vulkan");
  const auto digest=[&](const char *s){return present(s)&&std::string_view(s).size()==64&&
    std::all_of(s,s+64,[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});};
  if(!resident_lane||!present(helper)||helper[0]!='/'||!present(library)||library[0]!='/'||!digest(helper_sha)||!digest(library_sha))
    throw std::invalid_argument("Vulkan AI requires a resident Vulkan lane and absolute, SHA-256 pinned helper/library paths");
  return VulkanAiConfig{helper,helper_sha,library,library_sha};
}
}
