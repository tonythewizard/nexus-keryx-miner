#include "keryx/nexus/startup_readiness.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace keryx::nexus;

int main() {
  std::vector<std::string> events;
  StartupReadiness readiness([&](std::string_view line) { events.emplace_back(line); });
  for (const auto stage : startup_stages) {
    const auto result = stage == "STRIKE_MONITOR_STATUS" ? StartupResult::Degraded : StartupResult::Pass;
    readiness.record(stage, result, "fixture passed or optional telemetry degraded");
  }
  if (!readiness.mining_allowed() || events.size() != std::size(startup_stages))
    throw std::runtime_error("complete startup with optional strike telemetry degraded did not release mining gate");

  bool order_rejected = false;
  try {
    StartupReadiness out_of_order([](std::string_view) {});
    out_of_order.record("CONFIG_VALIDATION", StartupResult::Pass, "invalid skip");
  } catch (const std::logic_error &) { order_rejected = true; }
  if (!order_rejected) throw std::runtime_error("out-of-order startup transition was accepted");

  StartupReadiness unknown([](std::string_view) {});
  for (std::size_t i = 0; i < 7; ++i)
    unknown.record(startup_stages[i], StartupResult::Pass, "ok");
  unknown.record("ESCROW_IDENTITY_READY", StartupResult::Unknown, "not authoritative");
  if (unknown.mining_allowed() || unknown.service_ready())
    throw std::runtime_error("UNKNOWN escrow state released service/mining gate");

  std::string discovery_failure;
  StartupReadiness discovery([&](std::string_view event) { discovery_failure = event; });
  for (std::size_t i = 0; i < 7; ++i)
    discovery.record(startup_stages[i], StartupResult::Pass, "ok");
  discovery.record("ESCROW_IDENTITY_READY", StartupResult::Blocked,
                   "cannot obtain authoritative provider cohort: fixture RPC deadline");
  if (discovery.mining_allowed() || discovery.service_ready() ||
      discovery_failure.find("fixture RPC deadline") == std::string::npos ||
      discovery_failure.find("\"result\":\"BLOCKED\"") == std::string::npos)
    throw std::runtime_error("provider discovery failure lost its cause or released mining");

  std::string selected_backend_failure;
  StartupReadiness selected_backend([&](std::string_view line){selected_backend_failure=line;});
  for(const auto stage:startup_stages){
    if(stage=="INFERENCE_SELF_TEST")break;
    selected_backend.record(stage,StartupResult::Pass,"fixture CUDA model ready for PoM");
  }
  selected_backend.fail_current("selected Vulkan self-test failed");
  if(selected_backend_failure.find("INFERENCE_SELF_TEST")==std::string::npos ||
      selected_backend_failure.find("selected Vulkan self-test failed")==std::string::npos ||
      selected_backend.service_ready() || selected_backend.mining_allowed())
    throw std::runtime_error("Selected inference backend failure was misclassified or released mining");

  StartupReadiness blocked([](std::string_view) {});
  for (const auto stage : startup_stages) {
    if (stage == "SERVICE_READY") break;
    blocked.record(stage, StartupResult::Pass, "ok");
  }
  blocked.record("SERVICE_READY", StartupResult::Blocked, "service lifecycle unavailable");
  if (blocked.mining_allowed() || blocked.service_ready())
    throw std::runtime_error("blocked service state released mining gate");
  std::cout << "STARTUP_READINESS_TEST=PASS order=PASS unknown_fail_closed=PASS blocked_fail_closed=PASS optional_strikes_degraded=PASS\n";
}
