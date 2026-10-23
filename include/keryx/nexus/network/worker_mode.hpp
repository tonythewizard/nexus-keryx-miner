#pragma once

#include <stdexcept>
#include <string_view>

namespace keryx::nexus::network {

enum class WorkerMode { Shadow, Live };

struct WorkerSubmissionPolicy {
  WorkerMode mode;
  bool explicit_enable_flag;
  bool explicit_ai_response_enable_flag;

  [[nodiscard]] bool enabled() const noexcept {
    return mode == WorkerMode::Live && explicit_enable_flag;
  }
  [[nodiscard]] bool ai_responses_enabled() const noexcept {
    return mode == WorkerMode::Live && explicit_ai_response_enable_flag;
  }
};

inline WorkerSubmissionPolicy parse_worker_policy(std::string_view mode,
                                                   std::string_view submit_flag,
                                                   std::string_view ai_response_flag) {
  WorkerMode parsed_mode;
  if (mode == "shadow") parsed_mode = WorkerMode::Shadow;
  else if (mode == "live") parsed_mode = WorkerMode::Live;
  else throw std::invalid_argument("worker mode must be shadow or live");

  const bool explicit_enable = submit_flag == "--enable-submit";
  if (submit_flag != "--no-submit" && !explicit_enable)
    throw std::invalid_argument("worker requires --no-submit or --enable-submit");
  if (parsed_mode == WorkerMode::Shadow && explicit_enable)
    throw std::invalid_argument("shadow worker cannot enable SubmitBlock");
  const bool explicit_ai_enable = ai_response_flag == "--enable-ai-response";
  if (ai_response_flag != "--no-ai-response" && !explicit_ai_enable)
    throw std::invalid_argument("worker requires --no-ai-response or --enable-ai-response");
  if (parsed_mode == WorkerMode::Shadow && explicit_ai_enable)
    throw std::invalid_argument("shadow worker cannot enable AiResponse transactions");
  if (parsed_mode == WorkerMode::Live && !explicit_ai_enable)
    throw std::invalid_argument("productive live mode requires explicit --enable-ai-response authorization");
  return {parsed_mode, explicit_enable, explicit_ai_enable};
}

}  // namespace keryx::nexus::network
