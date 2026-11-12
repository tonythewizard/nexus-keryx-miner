#pragma once
#include "keryx/nexus/platform/shared_library.hpp"
#include "keryx/nexus/platform/windows_device_lease.hpp"
#include <array>
#include <stdexcept>
#include <string>
#include <memory>
#include <regex>
#ifndef _WIN32
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace keryx::nexus::platform {
// Resolve the runtime's actual physical device and acquire ownership before it
// loads model weights. An unbound runtime is not admitted for production.
class VulkanDeviceBinding final {
 public:
  VulkanDeviceBinding(const std::filesystem::path& library, const std::string& identity,
                      const std::filesystem::path& root) {
    void* handle = open_library(std::filesystem::absolute(library));
    if (!handle) throw std::runtime_error("Cannot inspect Vulkan runtime device binding");
    struct Close { void* p; ~Close(){close_library(p);} } close{handle};
    using Select = int(*)(const char*, char*, std::size_t);
    auto select = reinterpret_cast<Select>(library_symbol(handle,"keryx_llama_vk_select_device"));
    std::array<char,64> uuid{};
    if (!select || select(identity.c_str(), uuid.data(), uuid.size()) != 0)
      throw std::runtime_error("Vulkan runtime requires an explicit matching AMD UUID/PCI identity");
    uuid_ = uuid.data();
    if (!std::regex_match(uuid_,std::regex("GPU-[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}")))
      throw std::runtime_error("Vulkan runtime returned an invalid device UUID");
#ifdef _WIN32
    lease_ = std::make_unique<WindowsDeviceLease>(uuid_);
#else
    const auto directory = root / "state/gpu-devices";
    std::filesystem::create_directories(directory);
    const auto path = directory / ("lease-" + uuid_ + ".lock");
    int fd = ::open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) throw std::runtime_error("Cannot open Vulkan device lease");
    struct stat st{};
    if(::fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=::geteuid() || (st.st_mode&0077) || ::flock(fd,LOCK_EX|LOCK_NB)!=0){
      ::close(fd);throw std::runtime_error("Vulkan device lease unsafe or already owned");
    }
    fd_=fd;
#endif
  }
  VulkanDeviceBinding(const VulkanDeviceBinding&)=delete;
  VulkanDeviceBinding& operator=(const VulkanDeviceBinding&)=delete;
  ~VulkanDeviceBinding(){
#ifndef _WIN32
    if(fd_>=0)::close(fd_);
#endif
  }
  const std::string& uuid() const noexcept{return uuid_;}
 private:
  std::string uuid_;
#ifdef _WIN32
  std::unique_ptr<WindowsDeviceLease> lease_;
#else
  int fd_{-1};
#endif
};
}
