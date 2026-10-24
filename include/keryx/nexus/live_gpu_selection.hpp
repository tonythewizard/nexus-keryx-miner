#include "keryx/nexus/platform/io.hpp"
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <stdexcept>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

namespace keryx::nexus {
// The telemetry thread polls private, atomically published preferences. GPU
// threads read only atomics; no file I/O or CUDA calls occur on their hot path.
class LiveGpuSelection {
public:
  enum Lane { Nvidia, Amd };
  LiveGpuSelection(const char *file, bool primary, bool secondary,
                   bool primary_available, bool secondary_available)
      : file_(file ? file : ""), available_{primary_available, secondary_available},
        state_{std::uint64_t(primary), std::uint64_t(secondary)},
        applied_{std::uint64_t(primary), std::uint64_t(secondary)} {}
  std::uint64_t token(Lane lane) const { return state_[lane].load(std::memory_order_acquire); }
  bool enabled(Lane lane) const { return available_[lane] && (token(lane) & 1); }
  bool allows(Lane lane, std::uint64_t captured) const {
    return available_[lane] && (captured & 1) && token(lane) == captured;
  }
  // Call only after that lane's previous work has drained. OFF is not reported
  // as applied merely because the preferences file changed.
  void acknowledge(Lane lane) { applied_[lane].store(token(lane), std::memory_order_release); }
  void refresh() {
    if (file_.empty()) return; // Legacy/headless fixtures have no live file.
    bool primary=false, secondary=false;
    error_.clear();
    try {
      struct File { int fd; ~File() { if(fd>=0) keryx::nexus::platform::io::close(fd); } } file{
          keryx::nexus::platform::io::open(file_.c_str(), O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
      if(file.fd<0) throw std::runtime_error("PRIVATE_SELECTION_UNAVAILABLE");
      keryx::nexus::platform::io::Stat st{};
      if(keryx::nexus::platform::io::fstat(file.fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=keryx::nexus::platform::io::getuid() ||
          (st.st_mode&077) || st.st_size<1 || st.st_size>4096)
        throw std::runtime_error("INVALID_PRIVATE_SELECTION");
      std::string data;
      char buffer[4097];
      for(;;) {
        const auto n=keryx::nexus::platform::io::read(file.fd,buffer,sizeof(buffer));
        if(n<0 && errno==EINTR) continue;
        if(n<0) throw std::runtime_error("SELECTION_READ_FAILED");
        if(!n) break;
        data.append(buffer,static_cast<std::size_t>(n));
        if(data.size()>4096) throw std::runtime_error("SELECTION_TOO_LARGE");
      }
      google::protobuf::Struct json;
      if(!google::protobuf::util::JsonStringToMessage(data,&json).ok())
        throw std::runtime_error("INVALID_SELECTION_JSON");
      const auto &f=json.fields();
      if(f.size()!=3 || !f.contains("schema_version") || !f.contains("nvidia_pom") || !f.contains("amd_pom") ||
          f.at("schema_version").kind_case()!=google::protobuf::Value::kNumberValue ||
          f.at("schema_version").number_value()!=1 ||
          f.at("nvidia_pom").kind_case()!=google::protobuf::Value::kBoolValue ||
          f.at("amd_pom").kind_case()!=google::protobuf::Value::kBoolValue)
        throw std::runtime_error("INVALID_SELECTION_SCHEMA");
      primary=f.at("nvidia_pom").bool_value();secondary=f.at("amd_pom").bool_value();
    } catch(const std::exception &e) {
      error_=e.what(); // Only fixed error codes; never file contents or keys.
    }
    update(Nvidia,primary);update(Amd,secondary);
  }
  std::string snapshot() const {
    std::string out="{\"event\":\"GPU_POM_SELECTION\",\"schema_version\":1,\"live_control\":";
    out+=file_.empty()?"false":"true";
    for(const auto lane:{Nvidia,Amd}) {
      const auto current=token(lane);
      const std::string name=lane==Nvidia?"nvidia":"amd";
      const auto field=[&](const std::string &key,bool v){out+=",\""+name+"_"+key+"\":"+(v?"true":"false");};
      field("requested",current&1);field("available",available_[lane]);
      field("enabled",available_[lane]&&(current&1));
      field("applied",applied_[lane].load(std::memory_order_acquire)==current && (!(current&1)||available_[lane]));
    }
    // Fail closed on malformed state while preserving AI discovery/service.
    return out+",\"error\":\""+(error_.empty()?"": "SELECTION_INVALID_POM_PAUSED")+"\"}";
  }
private:
  void update(Lane lane,bool enabled) {
    const auto old=token(lane);
    if(bool(old&1)!=enabled) state_[lane].store(((old+2)&~std::uint64_t(1))|std::uint64_t(enabled),std::memory_order_release);
  }
  std::string file_,error_;
  const bool available_[2];
  std::atomic_uint64_t state_[2],applied_[2];
};
}
