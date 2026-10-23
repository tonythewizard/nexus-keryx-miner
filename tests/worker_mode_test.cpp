#include "keryx/nexus/network/worker_mode.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace keryx::nexus::network;

int main() {
  try {
    (void)setenv("NEXUS_SUBMIT_ENABLED", "1", 1);
    if (parse_worker_policy("shadow", "--no-submit", "--no-ai-response").enabled())
      throw std::runtime_error("shadow mode enabled submit from ambient environment");
    if (parse_worker_policy("shadow", "--no-submit", "--no-ai-response").ai_responses_enabled())
      throw std::runtime_error("shadow mode enabled AiResponse from ambient environment");
    if (parse_worker_policy("live", "--no-submit", "--enable-ai-response").enabled())
      throw std::runtime_error("live without the explicit CLI flag enabled submit");
    if (!parse_worker_policy("live", "--no-submit", "--enable-ai-response").ai_responses_enabled())
      throw std::runtime_error("explicit AI authorization did not enable AiResponse policy");
    if (!parse_worker_policy("live", "--enable-submit", "--enable-ai-response").enabled())
      throw std::runtime_error("explicit PoM authorization did not enable SubmitBlock policy");
    if (!parse_worker_policy("live", "--enable-submit", "--enable-ai-response").ai_responses_enabled())
      throw std::runtime_error("AI authorization was not independent of PoM authorization");
    bool shadow_flag_rejected = false;
    try { (void)parse_worker_policy("shadow", "--enable-submit", "--no-ai-response"); }
    catch (const std::invalid_argument &) { shadow_flag_rejected = true; }
    if (!shadow_flag_rejected)
      throw std::runtime_error("shadow plus enable-submit was not rejected");
    bool shadow_ai_rejected = false;
    try { (void)parse_worker_policy("shadow", "--no-submit", "--enable-ai-response"); }
    catch (const std::invalid_argument &) { shadow_ai_rejected = true; }
    if (!shadow_ai_rejected) throw std::runtime_error("shadow plus enable-ai-response was not rejected");
    bool live_ai_required = false;
    try { (void)parse_worker_policy("live", "--no-submit", "--no-ai-response"); }
    catch (const std::invalid_argument &) { live_ai_required = true; }
    if (!live_ai_required) throw std::runtime_error("live mining was allowed without explicit AiResponse authorization");
    bool invalid_mode_rejected = false;
    try { (void)parse_worker_policy("live", "1", "--enable-ai-response"); }
    catch (const std::invalid_argument &) { invalid_mode_rejected = true; }
    if (!invalid_mode_rejected)
      throw std::runtime_error("environment-like submit value was accepted as a CLI flag");
    std::cout << "WORKER_SUBMISSION_POLICY=PASS ambient_env_ignored=true shadow_forbidden=true "
                 "pom_and_ai_authorizations_separate=true live_requires_ai_optin=true\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "WORKER_SUBMISSION_POLICY=FAIL detail=" << error.what() << '\n';
    return 1;
  }
}
