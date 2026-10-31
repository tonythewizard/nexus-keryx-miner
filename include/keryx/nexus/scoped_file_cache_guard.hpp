#pragma once
// Optional production counterpart of the measured integration-harness guard.
// Reclaims only the dedicated miner cgroup; no host or node settings change.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
namespace keryx::nexus {
class ScopedFileCacheGuard final {
public:
  ScopedFileCacheGuard() {
#ifdef _WIN32
    throw std::runtime_error("Linux cgroup cache guard is unavailable on Windows");
#else
    std::ifstream input("/proc/self/cgroup");std::string line,path;
    while(std::getline(input,line))if(line.starts_with("0::"))path=line.substr(3);
    if(path.empty())throw std::runtime_error("Dedicated cgroup v2 required for miner cache guard");
    root_=std::filesystem::path("/sys/fs/cgroup")/path.substr(1);
    const auto leaf=root_.filename().string();
    if(leaf!="nexus-v002-production.service" && !(leaf.starts_with("nexus-dual-")&&leaf.ends_with(".service")))
      throw std::runtime_error("Refusing to reclaim a shared/non-miner cgroup");
    const int result=request("0 swappiness=0");
    if(result)throw std::runtime_error("Delegated file reclaim unavailable, errno="+std::to_string(result));
    thread_=std::jthread([this](std::stop_token stop){
      while(!stop.stop_requested()) {
        try {
          std::ifstream stat(root_/"memory.stat");std::string key;std::uint64_t n,file=0,shmem=0;bool found=false;
          while(stat>>key>>n){if(key=="file"){file=n;found=true;}else if(key=="shmem")shmem=n;}
          if(!found)throw std::runtime_error("Cannot read dedicated cgroup file cache");
          if(file>shmem && file-shmem>256ULL*1024*1024) {
            ++requests_;const int error=request("134217728 swappiness=0");
            if(error && error!=EAGAIN){error_.store(error);return;}
          }
        }catch(...){error_.store(EIO);return;}
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    std::cout<<"MINER_FILE_CACHE_GUARD=ACTIVE file_trigger_bytes=268435456 swappiness=0 scope=MINER_ONLY\n"<<std::flush;
#endif
  }
  ~ScopedFileCacheGuard(){thread_.request_stop();if(thread_.joinable())thread_.join();
    std::cout<<"MINER_FILE_CACHE_GUARD=STOP requests="<<requests_.load()<<" errno="<<error_.load()<<'\n'<<std::flush;}
  void check() const {if(const auto error=error_.load())throw std::runtime_error("Miner scoped cache guard failed, errno="+std::to_string(error));}
private:
  int request(const std::string &value) const {
#ifdef _WIN32
    (void)value;return ENOSYS;
#else
    const int fd=::open((root_/"memory.reclaim").c_str(),O_WRONLY|O_CLOEXEC);
    if(fd<0)return errno;
    const auto written=::write(fd,value.data(),value.size());
    const int error=written<0?errno:written!=static_cast<ssize_t>(value.size())?EIO:0;
    ::close(fd);return error;
#endif
  }
  std::filesystem::path root_;
  std::atomic_int error_{};
  std::atomic_uint64_t requests_{};
  std::jthread thread_;
};
}
