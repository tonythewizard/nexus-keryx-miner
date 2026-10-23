#include "keryx/nexus/developer_config.hpp"
#pragma once
// Read-only Vulkan admission. No VkDevice, allocation, model upload or dispatch.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace keryx::nexus {
inline void require_bda_features(VkPhysicalDevice gpu) {
  uint32_t count=0;
  if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,nullptr)!=VK_SUCCESS)
    throw std::runtime_error("Cannot query Vulkan extensions");
  std::vector<VkExtensionProperties> ext(count);
  if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,ext.data())!=VK_SUCCESS)
    throw std::runtime_error("Cannot read Vulkan extensions");
  if(std::none_of(ext.begin(),ext.end(),[](auto &e){return std::strcmp(e.extensionName,VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)==0;}))
    throw std::runtime_error("BDA_UNSUPPORTED: VK_KHR_cooperative_matrix unavailable; select JIT+Vulkan");
  VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
  VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};f11.pNext=&coop;
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.pNext=&f11;
  VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&f12;
  vkGetPhysicalDeviceFeatures2(gpu,&f);
  VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&sub;
  vkGetPhysicalDeviceProperties2(gpu,&props);
  if(!f.features.shaderInt64||!f.features.shaderInt16||!f12.shaderInt8||!f12.bufferDeviceAddress||
     !f12.storageBuffer8BitAccess||!f11.storageBuffer16BitAccess||!coop.cooperativeMatrix||
     sub.subgroupSize!=32||!(sub.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT)||
     !(sub.supportedOperations&VK_SUBGROUP_FEATURE_BASIC_BIT)||!(sub.supportedOperations&VK_SUBGROUP_FEATURE_SHUFFLE_BIT))
    throw std::runtime_error("BDA_UNSUPPORTED: integer/storage/address/subgroup requirements unavailable");

}
inline void require_bda_matrix(VkInstance instance,VkPhysicalDevice gpu) {
  require_bda_features(gpu);
  const auto get=reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(
      vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
  uint32_t count=0;
  if(!get||get(gpu,&count,nullptr)!=VK_SUCCESS)throw std::runtime_error("BDA matrix query unavailable");
  std::vector<VkCooperativeMatrixPropertiesKHR> props(count,{VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
  if(get(gpu,&count,props.data())!=VK_SUCCESS)throw std::runtime_error("BDA matrix query failed");
  if(std::none_of(props.begin(),props.end(),[](auto &p){return p.MSize==16&&p.NSize==8&&p.KSize==32&&
       p.AType==VK_COMPONENT_TYPE_SINT8_KHR&&p.BType==VK_COMPONENT_TYPE_SINT8_KHR&&
       p.CType==VK_COMPONENT_TYPE_SINT32_KHR&&p.ResultType==VK_COMPONENT_TYPE_SINT32_KHR&&
       p.scope==VK_SCOPE_SUBGROUP_KHR;}))
    throw std::runtime_error("BDA_UNSUPPORTED: signed int8 16x8x32 cooperative matrix unavailable");
}
inline void probe_bda_route(const std::string &id) {
  const bool nv=id=="" NEXUS_PRIMARY_GPU_UUID "";
  if(!nv&&id!="" NEXUS_SECONDARY_GPU_ID "")throw std::runtime_error("GPU has no admitted runtime lane");
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;
  struct Owner{VkInstance value{};~Owner(){if(value)vkDestroyInstance(value,nullptr);}} instance;
  if(vkCreateInstance(&ci,nullptr,&instance.value)!=VK_SUCCESS)throw std::runtime_error("Vulkan admission instance failed");
  uint32_t n=0;if(vkEnumeratePhysicalDevices(instance.value,&n,nullptr)!=VK_SUCCESS||n!=1)
    throw std::runtime_error("Admission requires one explicitly scoped Vulkan device");
  VkPhysicalDevice gpu{};if(vkEnumeratePhysicalDevices(instance.value,&n,&gpu)!=VK_SUCCESS)
    throw std::runtime_error("Vulkan admission enumeration failed");
  VkPhysicalDeviceIDProperties uuid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
  VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=nv?(void*)&uuid:(void*)&pci;
  vkGetPhysicalDeviceProperties2(gpu,&p);std::ostringstream actual;actual<<std::hex<<std::setfill('0');
  if(nv){actual<<"GPU-";for(unsigned i=0;i<16;++i){if(i==4||i==6||i==8||i==10)actual<<'-';actual<<std::setw(2)<<unsigned(uuid.deviceUUID[i]);}}
  else actual<<"pci:"<<std::setw(4)<<pci.pciDomain<<':'<<std::setw(2)<<pci.pciBus<<':'<<std::setw(2)<<pci.pciDevice<<'.'<<pci.pciFunction;
  if(actual.str()!=id)throw std::runtime_error("Vulkan admission identity mismatch");
  require_bda_matrix(instance.value,gpu);
}
}
