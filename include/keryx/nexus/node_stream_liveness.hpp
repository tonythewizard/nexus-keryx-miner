#pragma once
#include <cstdint>
#include <mutex>

namespace keryx::nexus {
// Measure time blocked on the node, not time spent servicing a response or AI.
// End-of-wait and timeout decisions share a lock: a response racing the timeout
// cannot re-arm mining on a connection that was already declared disconnected.
class NodeStreamLiveness final {
public:
  enum class Action { None, Probe, Reconnect };
  static constexpr std::uint64_t probe_ns = 30'000'000'000ULL;
  static constexpr std::uint64_t limit_ns = 90'000'000'000ULL;
  bool begin_wait(std::uint64_t now) {
    const std::lock_guard lock(mutex_);
    if (expired_ || waiting_) return false;
    since_=last_probe_=now;
    waiting_=true;
    return true;
  }
  bool end_wait() {
    const std::lock_guard lock(mutex_);
    waiting_=false;
    return !expired_;
  }
  Action poll(std::uint64_t now) {
    const std::lock_guard lock(mutex_);
    if (!waiting_ || expired_ || now<since_) return Action::None;
    if (now-since_>=limit_ns) {
      expired_=true;
      waiting_=false;
      return Action::Reconnect;
    }
    if (now>=last_probe_ && now-last_probe_>=probe_ns) {
      last_probe_=now;
      return Action::Probe;
    }
    return Action::None;
  }
private:
  std::mutex mutex_;
  std::uint64_t since_{},last_probe_{};
  bool waiting_{false},expired_{false};
};
} // namespace keryx::nexus
