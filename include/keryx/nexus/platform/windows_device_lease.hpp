#pragma once
#ifdef _WIN32
#include "keryx/nexus/platform/windows_process.hpp"
namespace keryx::nexus::platform {
class WindowsDeviceLease final {
 public:
  explicit WindowsDeviceLease(const std::string& uuid) {
    io::LocalMemory descriptor;
    if(!io::private_descriptor(descriptor))throw std::runtime_error("Private device lease ACL unavailable");
    SECURITY_ATTRIBUTES security{sizeof(security),descriptor.value,FALSE};
    std::wstring name=L"Global\\NexusKeryx-"+std::wstring(uuid.begin(),uuid.end());
    mutex_.reset(CreateMutexW(&security,FALSE,name.c_str()));
    if(!mutex_)throw std::runtime_error("Cannot acquire exclusive Windows device lease");
    const auto result=WaitForSingleObject(mutex_.value,0);
    if(result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED)
      throw std::runtime_error("Selected GPU is already owned by another Nexus worker");
    owned_=true;
  }
  ~WindowsDeviceLease(){if(owned_)ReleaseMutex(mutex_.value);}
 private:
  WinHandle mutex_;
  bool owned_{};
};
}
#endif
