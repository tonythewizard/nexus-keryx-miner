// Read-only AMD-only capability probe. No device/queue or shader is created.
#include <vulkan/vulkan_core.h>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(VkResult status) {
  if (status != VK_SUCCESS) throw std::runtime_error("Vulkan capability query failed: " + std::to_string(status));
}
static std::string hex(unsigned value, unsigned width) {
  std::ostringstream out; out << std::hex << std::setfill('0') << std::setw(width) << value; return out.str();
}
static std::string escaped(const char* value) {
  std::string out;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
    if (*p == '"' || *p == '\\') out += '\\';
    if (*p >= 32 && *p < 127) out += static_cast<char>(*p);
  }
  return out;
}
int main(int argc, char** argv) {
  VkInstance instance{};
  try {
    if (argc != 1) throw std::runtime_error("Usage: nexus-vulkan-device-info (AMD-only read-only JSON)");
    (void)argv;
    setenv("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/radeon_icd.json", 1);
    setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/radeon_icd.json", 1);
    setenv("VK_LOADER_LAYERS_DISABLE", "~implicit~", 1);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
    check(vkCreateInstance(&create, nullptr, &instance));
    uint32_t count{}; check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    std::ostringstream result; result << "{\"schema_version\":1,\"probe\":\"AMD_READ_ONLY\",\"devices\":[";
    bool first = true;
    for (auto gpu : devices) {
      VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(gpu, &properties);
      if (properties.vendorID != 0x1002 || properties.deviceID != 0x67df) continue;
      uint32_t extension_count{};
      check(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extension_count, nullptr));
      std::vector<VkExtensionProperties> extensions(extension_count);
      check(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extension_count, extensions.data()));
      bool pci_supported = false;
      for (const auto& ext : extensions) if (std::string(ext.extensionName) == VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) pci_supported = true;
      if (!pci_supported) throw std::runtime_error("PCI identity extension unavailable; refusing ambiguous AMD identity");
      VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
      VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext = &pci;
      vkGetPhysicalDeviceProperties2(gpu, &props);
      VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &f12;
      vkGetPhysicalDeviceFeatures2(gpu, &features);
      VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
      uint64_t local_bytes{};
      for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local_bytes += memory.memoryHeaps[i].size;
      const auto bdf = hex(pci.pciDomain, 4) + ":" + hex(pci.pciBus, 2) + ":" + hex(pci.pciDevice, 2) + "." + hex(pci.pciFunction, 1);
      if (!first) result << ',';
      first = false;
      result << "{\"id\":\"pci:" << bdf << "\",\"vendor_id\":4098,\"device_id\":26591,\"name\":\"" << escaped(properties.deviceName)
        << "\",\"api_version\":" << properties.apiVersion << ",\"vram_bytes\":" << local_bytes
        << ",\"shaderInt64\":" << (features.features.shaderInt64 ? "true" : "false")
        << ",\"shaderInt8\":" << (f12.shaderInt8 ? "true" : "false")
        << ",\"storageBuffer8BitAccess\":" << (f12.storageBuffer8BitAccess ? "true" : "false")
        << ",\"max_workgroup_invocations\":" << properties.limits.maxComputeWorkGroupInvocations
        << ",\"shared_memory_bytes\":" << properties.limits.maxComputeSharedMemorySize
        << ",\"production_ready\":false}";
    }
    result << "]}\n";
    vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE;
    std::cout << result.str(); return 0;
  } catch (const std::exception& error) {
    if (instance) vkDestroyInstance(instance, nullptr);
    std::cerr << "BLOCKED: " << error.what() << '\n'; return 1;
  }
}
