#pragma once
// Offline/test adapter. Production admission is deliberately separate.
#include "keryx/nexus/bounded_file_sha256.hpp"
#include <dlfcn.h>
#include <sys/file.h>
#include <memory>
#include <string_view>
#include <vector>

namespace keryx::nexus::opoi {
class ExperimentalVulkanInference final {
  struct Impl {
    int lease{-1}; void *library{}, *model{};
    void (*free)(void *){};
    int (*generate)(void *,const char *,int,char *,int){};
    const char *(*error)(){};
    ~Impl() { if(model&&free)free(model);if(library)dlclose(library);if(lease>=0)::close(lease); }
  };
  std::unique_ptr<Impl> impl_{std::make_unique<Impl>()};
  template<class T> T symbol(const char *name) {
    auto ptr=reinterpret_cast<T>(dlsym(impl_->library,name));
    if(!ptr)throw std::runtime_error("Experimental Vulkan inference ABI symbol missing");
    return ptr;
  }
public:
  ExperimentalVulkanInference(const std::string &library,const std::string &sha,
      const std::string &model,const std::string &lease_path,int context=8192) {
    const char *icd=std::getenv("VK_DRIVER_FILES"),*layers=std::getenv("VK_LOADER_LAYERS_DISABLE");
    if(!icd||std::string_view(icd)!="/usr/share/vulkan/icd.d/radeon_icd.json"||
       !layers||std::string_view(layers)!="~implicit~")
      throw std::runtime_error("Experimental inference requires the authorized AMD-only ICD");
    if(sha.size()!=64||bounded_file_sha256(library)!=sha)
      throw std::runtime_error("Experimental Vulkan library SHA-256 mismatch");
    if(bounded_file_sha256(model)!="946072b16f5d672e60357410f900888f6522b3ae49f241b5bf2142cb89637fb6")
      throw std::runtime_error("Experimental Vulkan inference official model mismatch");
    auto &i=*impl_;
    i.lease=::open(lease_path.c_str(),O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    struct stat st{};
    if(i.lease<0||fstat(i.lease,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)||flock(i.lease,LOCK_EX|LOCK_NB))
      throw std::runtime_error("secondary AMD GPU already reserved or device lease invalid");
    i.library=dlopen(library.c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!i.library)throw std::runtime_error("Experimental Vulkan inference library cannot be loaded");
    if(symbol<int(*)()>("nexus_vulkan_inference_abi")()!=1)
      throw std::runtime_error("Experimental Vulkan inference ABI mismatch");
    const auto load=symbol<void*(*)(const char*,const char*,int)>("nexus_vulkan_inference_load");
    i.free=symbol<void(*)(void*)>("nexus_vulkan_inference_free");
    i.generate=symbol<int(*)(void*,const char*,int,char*,int)>("nexus_vulkan_inference_generate");
    i.error=symbol<const char*(*)()>("nexus_vulkan_inference_error");
    i.model=load(model.c_str(),"0000:00:00.0",context);
    if(!i.model)throw std::runtime_error(std::string("Vulkan inference load failed: ")+i.error());
  }
  std::string infer(std::string_view prompt,std::uint32_t tokens) {
    if(prompt.empty()||prompt.size()>60*1024||prompt.find('\0')!=std::string_view::npos||!tokens||tokens>4096)
      throw std::invalid_argument("Invalid bounded Vulkan inference request");
    const std::string owned(prompt);std::vector<char> answer(65536);
    const int n=impl_->generate(impl_->model,owned.c_str(),tokens,answer.data(),answer.size());
    if(n<=0||n>=int(answer.size()))throw std::runtime_error(std::string("Vulkan inference failed: ")+impl_->error());
    return std::string(answer.data(),n);
  }
};
}
