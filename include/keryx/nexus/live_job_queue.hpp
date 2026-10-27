#pragma once

#include "keryx/nexus/pipeline.hpp"

#include <condition_variable>
#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

namespace keryx::nexus {

[[nodiscard]] std::string canonical_job_fingerprint(const TemplateFields &fields);

// A bounded latest-job queue for the live gRPC stream. Only a validated,
// consensus-changing template invalidates the current lease; duplicate snapshots
// preserve its generation. Work already running must check freshness before it
// publishes a candidate or proof.
class LiveJobQueue final {
public:
  enum class PublishResult { Published, Duplicate };

  PublishResult publish(TemplateFields fields);
  // A template notification requests a refresh but does not itself prove that
  // the current block template changed. Admission pauses until a validated
  // snapshot is compared and published.
  [[nodiscard]] bool begin_refresh() noexcept;
  // A virtual-chain notification certifies selected-parent-chain movement and
  // invalidates the current lease immediately while a replacement is fetched.
  void invalidate_for_virtual_chain_change() noexcept;
  void invalidate() noexcept;
  void close() noexcept;
  [[nodiscard]] std::optional<TemplateJob> wait_pop();
  [[nodiscard]] std::optional<TemplateJob> wait_pop_for(std::chrono::milliseconds timeout);
  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] bool admission_allowed(std::uint64_t generation) const noexcept;
  [[nodiscard]] bool wait_refresh_resolved(std::uint64_t generation);
  [[nodiscard]] bool submission_allowed(std::uint64_t generation) const noexcept;
  // Generation numbers alone cannot distinguish leases from different queues.
  [[nodiscard]] bool owns_snapshot(const TemplateJob &job) const;
  [[nodiscard]] bool owns_lease(const JobLease &lease) const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> current_generation() const noexcept;
  [[nodiscard]] std::uint64_t published_count() const noexcept;
  [[nodiscard]] std::uint64_t duplicate_count() const noexcept;
  [[nodiscard]] std::uint64_t virtual_chain_invalidations() const noexcept;

private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<TemplateJob> pending_;
  std::optional<TemplateJob> current_job_;
  std::shared_ptr<std::atomic_bool> active_;
  std::optional<std::uint64_t> generation_;
  std::uint64_t next_generation_{};
  std::uint64_t published_{};
  std::uint64_t duplicates_{};
  std::uint64_t virtual_chain_invalidations_{};
  bool closed_{};
  bool refresh_pending_{};
  std::uint64_t refresh_notifications_{};
};

} // namespace keryx::nexus
