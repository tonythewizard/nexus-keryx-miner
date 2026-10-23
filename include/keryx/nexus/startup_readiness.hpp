#pragma once

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace keryx::nexus {

enum class StartupResult { Pass, Waiting, Degraded, Unknown, Blocked, Fail };

inline constexpr std::string_view startup_stages[] = {
    "BOOT", "CONFIG_VALIDATION", "NODE_SYNC", "IPFS_READY", "MODEL_VERIFIED",
    "CUDA_ENGINE_READY", "INFERENCE_SELF_TEST", "ESCROW_IDENTITY_READY",
    "AI_REQUEST_SERVICE_READY", "AI_RESPONSE_TRANSPORT_READY",
    "PENDING_REQUEST_RECOVERY", "ESCROW_STATE_RECOVERED", "ESCROW_VALIDATION_COMPLETE",
    "ESCROW_CLAIM_ENGINE_READY", "STRIKE_MONITOR_STATUS", "SERVICE_READY", "POM_READY", "MINING"};

class StartupReadiness final {
public:
  using Clock = std::chrono::steady_clock;
  using Sink = std::function<void(std::string_view)>;

  explicit StartupReadiness(Sink sink)
      : sink_(std::move(sink)), started_(Clock::now()), entered_(started_) {}

  void record(std::string_view stage, StartupResult result, std::string_view reason) {
    if (terminal_) throw std::logic_error("startup readiness is terminal");
    if (index_ >= std::size(startup_stages) || stage != startup_stages[index_])
      throw std::logic_error("startup readiness stage out of order");
    const auto now = Clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - entered_).count();
    const auto next = (result != StartupResult::Blocked && result != StartupResult::Fail) &&
                      index_ + 1 < std::size(startup_stages)
        ? startup_stages[index_ + 1] : std::string_view{};
    const auto recovery = result == StartupResult::Pass ? "continue" :
        result == StartupResult::Waiting ? "retry_bounded_then_report_waiting" :
        result == StartupResult::Degraded ? "continue_only_if_service_contract_allows" :
        result == StartupResult::Unknown ? "diagnostic_only_gate_remains_closed" :
        result == StartupResult::Blocked ? "exit_operator_action_required" :
                                           "exit_correct_cause_then_restart";
    sink_(format(stage, result, reason, next, recovery,
                 static_cast<unsigned long long>(duration)));
    results_.push_back(result);
    ++index_;
    entered_ = now;
    if (result == StartupResult::Fail || result == StartupResult::Blocked) terminal_ = true;
  }

  void fail_current(std::string_view reason) {
    if (terminal_ || index_ >= std::size(startup_stages)) return;
    record(startup_stages[index_], StartupResult::Fail, reason);
  }

  [[nodiscard]] bool mining_allowed() const noexcept {
    return !terminal_ && index_ == std::size(startup_stages) &&
           results_.size() == std::size(startup_stages) &&
           service_ready() && results_[std::size(startup_stages) - 2] == StartupResult::Pass &&
           results_.back() == StartupResult::Pass;
  }

  [[nodiscard]] bool service_ready() const noexcept {
    constexpr std::size_t service_ready_index = std::size(startup_stages) - 3;
    constexpr std::size_t strike_status_index = service_ready_index - 1;
    if (terminal_ || results_.size() <= service_ready_index ||
        results_[service_ready_index] != StartupResult::Pass) return false;
    for (std::size_t i = 0; i < service_ready_index; ++i) {
      // Strike values are observability. A failed/unavailable poll remains UNKNOWN or
      // DEGRADED in the UI and audit, but does not disable an otherwise capable AI service.
      if (i == strike_status_index) {
        if (results_[i] == StartupResult::Blocked || results_[i] == StartupResult::Fail) return false;
      } else if (results_[i] != StartupResult::Pass) return false;
    }
    return true;
  }

  [[nodiscard]] static std::string_view result_name(StartupResult result) noexcept {
    switch (result) {
      case StartupResult::Pass: return "PASS";
      case StartupResult::Waiting: return "WAITING";
      case StartupResult::Degraded: return "DEGRADED";
      case StartupResult::Unknown: return "UNKNOWN";
      case StartupResult::Blocked: return "BLOCKED";
      case StartupResult::Fail: return "FAIL";
    }
    return "FAIL";
  }

private:
  static std::string escape(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
      if (c == '"' || c == '\\') out.push_back('\\');
      if (c == '\n' || c == '\r') { out.push_back(' '); continue; }
      out.push_back(c);
    }
    return out;
  }
  static std::string format(std::string_view stage, StartupResult result,
                            std::string_view reason, std::string_view next,
                            std::string_view recovery, unsigned long long duration_us) {
    return "{\"event\":\"STARTUP_STATE\",\"state\":\"" + std::string(stage) +
        "\",\"result\":\"" + std::string(result_name(result)) +
        "\",\"duration_ms\":" + std::to_string(static_cast<double>(duration_us) / 1000.0) +
        ",\"reason\":\"" + escape(reason) + "\",\"next\":\"" + escape(next) +
        "\",\"recovery\":\"" + escape(recovery) + "\"}";
  }
  Sink sink_;
  Clock::time_point started_;
  Clock::time_point entered_;
  std::vector<StartupResult> results_;
  std::size_t index_{};
  bool terminal_{};
};

} // namespace keryx::nexus
