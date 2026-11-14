#include "keryx/nexus/platform/vulkan_device_binding.hpp"
#include <iostream>
#include <stdexcept>
using keryx::nexus::platform::VulkanDeviceBinding;
int main(int argc,char**argv)try{
  if(argc!=3)throw std::runtime_error("usage: binding-test fixture persistent-test-root");
  const auto library=std::filesystem::absolute(argv[1]);
  const auto root=std::filesystem::absolute(argv[2]);
  bool refused=false;
  try{VulkanDeviceBinding bad(library,"pci:0000:00:01.0",root);}catch(const std::exception&){refused=true;}
  if(!refused)throw std::runtime_error("identity mismatch admitted");
  {
    VulkanDeviceBinding first(library,"pci:0000:00:00.0",root);
    refused=false;
    try{VulkanDeviceBinding duplicate(library,"pci:0000:00:00.0",root);}catch(const std::exception&){refused=true;}
    if(!refused)throw std::runtime_error("duplicate ownership admitted");
  }
  VulkanDeviceBinding released(library,"pci:0000:00:00.0",root);
  std::cout<<"VULKAN_BINDING=PASS mismatch_refused=true exclusive=true release=true gpu_execution=false\n";
  return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
