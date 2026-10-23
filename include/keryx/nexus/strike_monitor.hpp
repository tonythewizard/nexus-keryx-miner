#pragma once

#include "keryx/nexus/ai_service.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace keryx::nexus::ai {

class StrikeMonitor final {
public:
  using Poll = std::function<StrikeSnapshot()>;
  using Sink = std::function<void(const StrikeSnapshot &)>;

  StrikeMonitor(std::string service_identity, std::filesystem::path journal,
                Poll poll, Sink sink,
                std::chrono::milliseconds interval = std::chrono::minutes(1));
  ~StrikeMonitor();
  StrikeMonitor(const StrikeMonitor &) = delete;
  StrikeMonitor &operator=(const StrikeMonitor &) = delete;

  void start();
  void stop();
  StrikeSnapshot snapshot() const;

private:
  void run();
  void recover_journal();
  void persist(const StrikeSnapshot &snapshot);

  std::string identity_;
  std::filesystem::path journal_;
  Poll poll_;
  Sink sink_;
  std::chrono::milliseconds interval_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  StrikeSnapshot snapshot_;
  std::thread thread_;
  bool stopping_{};
  bool started_{};
  bool recovered_{};
};

} // namespace keryx::nexus::ai
