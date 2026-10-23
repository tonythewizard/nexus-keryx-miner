#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
namespace keryx::nexus {
// Account for delayed driver release outside the process/cgroup's charged RAM.
// Create before allocating PoM or AI. Caller holds the exclusive per-PCI lease.
class AmdMemoryHandoff final {
public:
  using Sample=std::array<std::uint64_t,2>;
  using Reader=std::function<Sample()>;
  static Sample device_memory(){
    Sample bytes{};unsigned i=0;
    for(const char *name:{"mem_info_vram_used","mem_info_gtt_used"}){
      std::ifstream input(std::string("/sys/bus/pci/devices/0000:00:00.0/")+name);
      if(!(input>>bytes[i++]))throw std::runtime_error("AMD release accounting unavailable");
    }return bytes;
  }
  explicit AmdMemoryHandoff(Reader reader=device_memory):reader_(std::move(reader)),baseline_(reader_()){}
  double wait_released(const std::function<bool()> &cancelled={},
      std::chrono::milliseconds budget=std::chrono::seconds(5)) const {
    if(budget.count()<=0)throw std::invalid_argument("AMD release wait requires a positive bound");
    const auto began=std::chrono::steady_clock::now(),until=began+budget;unsigned stable=0;
    while(std::chrono::steady_clock::now()<until){
      if(cancelled&&cancelled())throw std::runtime_error("AMD release wait cancelled; PoM admission remains closed");
      const auto now=reader_();
      const auto close=[](auto current,auto baseline){return current<=baseline||current-baseline<=64*1024*1024;};
      stable=close(now[0],baseline_[0])&&close(now[1],baseline_[1])?stable+1:0;
      if(stable>=3)return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("AMD memory did not return to its pre-workload baseline before the release deadline");
  }
private:
  Reader reader_;
  Sample baseline_;
};
}
