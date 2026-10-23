#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <mutex>

namespace keryx::nexus::network {

// Fail-closed admission policy for the eventual SubmitBlock transport. This
// object does not perform consensus verification itself: callers must provide
// results produced by the official proof/model/header/target/job verifiers.
struct VerifiedSubmission {
  std::string job_id;
  std::uint64_t generation{};
  std::uint64_t nonce{};
  bool pom_verified{};
  bool model_verified{};
  bool merkle_verified{};
  bool header_seed_verified{};
  bool target_satisfied{};
  bool job_fresh{};
  bool generation_matches{};
  bool miner_configured{};
};

class SubmissionGate final {
 public:
  std::string reserve(const VerifiedSubmission &candidate) {
    require(candidate.pom_verified, "PoM proof not verified");
    require(candidate.model_verified, "model identity not verified");
    require(candidate.merkle_verified, "Merkle proof not verified");
    require(candidate.header_seed_verified, "header/seed binding not verified");
    require(candidate.target_satisfied, "candidate does not satisfy the network target");
    require(candidate.job_fresh, "job is stale");
    require(candidate.generation_matches, "job generation mismatch");
    require(candidate.miner_configured, "authorized miner payout is not configured");
    if (candidate.job_id.empty() || candidate.generation == 0)
      throw std::invalid_argument("submission job identity is incomplete");

    const auto key = candidate.job_id + ":" + std::to_string(candidate.generation) + ":" +
                     std::to_string(candidate.nonce);
    std::lock_guard lock(mutex_);
    if (!reserved_.insert(key).second)
      throw std::invalid_argument("duplicate submission candidate");
    return key;
  }

  std::size_t reserved_count() const { std::lock_guard lock(mutex_); return reserved_.size(); }

 private:
  static void require(bool condition, const char *reason) {
    if (!condition) throw std::invalid_argument(reason);
  }
  mutable std::mutex mutex_;
  std::unordered_set<std::string> reserved_;
};

}  // namespace keryx::nexus::network
