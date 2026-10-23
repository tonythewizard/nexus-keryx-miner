#include "keryx/nexus/primary_vulkan_policy.hpp"
#include "keryx/nexus/gpu_device_policy.hpp"
#include "keryx/nexus/vulkan_device_policy.hpp"
#include "keryx/nexus/native_telemetry.hpp"
#include <iostream>
using namespace keryx::nexus;
int main(){try{
  unsigned rejected=0;
  auto reject=[&](auto f){try{f();}catch(const std::exception&){++rejected;return;}throw std::runtime_error("Unsafe routing admitted");};
  const auto uuid=authorized_nvidia_vulkan_uuid;
  if(require_production_gpu_backend("vulkan-jit",uuid.data(),uuid,true)!=GpuBackend::VulkanJit)throw std::runtime_error("Vulkan worker routing rejected");
  reject([&]{require_production_gpu_backend("vulkan-jit",uuid.data(),uuid,false);});
  reject([&]{require_production_gpu_backend("vulkan-jit",nullptr,uuid,true);});
  reject([&]{require_production_gpu_backend("vulkan-jit","wrong",uuid,true);});
  validate_primary_vulkan_policy(true,true,false,true);
  validate_primary_vulkan_policy(true,true,true,true,true);
  validate_primary_vulkan_policy(false,true,true,false);
  reject([]{validate_primary_vulkan_policy(true,false,false,true);});
  reject([]{validate_primary_vulkan_policy(true,true,true,true);});
  reject([]{validate_primary_vulkan_policy(true,true,false,false);});
  NativePomTelemetry counters(1);counters.work_launched(256);counters.work_completed(256*256,256,2);counters.eligible_attempts(256);
  const auto c=counters.counters();
  if(c.pom_kernels_completed!=2||c.gpu_work_units_completed!=256||c.eligible_attempts!=256)throw std::runtime_error("Vulkan diagnostic replay double-counted attempts");
  std::cout<<"PRIMARY_VULKAN_POLICY=PASS negatives="<<rejected<<" cuda_fallback=FORBIDDEN unique_attempts=256 kernels=2 gpu_initialized=false\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
