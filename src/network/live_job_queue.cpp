#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/capture.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace keryx::nexus {
namespace {
const std::vector<std::uint8_t> &identity_wire(const TemplateFields &fields) {
  return fields.consensus_identity_wire.empty() ? fields.block_wire : fields.consensus_identity_wire;
}
// Exact equivalence to the length-prefixed identity below. Do not compare only
// hashes or generation numbers: changed canonical bytes must always be rejected.
bool same_identity(const TemplateFields &a, const TemplateFields &b) {
  return a.network == b.network && a.node_synced == b.node_synced &&
         a.pay_address == b.pay_address && a.extra_data == b.extra_data &&
         a.inference_challenge == b.inference_challenge && identity_wire(a) == identity_wire(b);
}
std::string identity_bytes(const TemplateFields &fields) {
  const auto &wire = identity_wire(fields);
  std::string identity;
  identity.reserve(16 + 64 + fields.network.size() + 8 + fields.pay_address.size() +
                   fields.extra_data.size() + fields.inference_challenge.size() + wire.size());
  identity.append("KERYX-POM-JOB-v1", 16);
  auto append = [&](std::string_view value) {
    const auto size = static_cast<std::uint64_t>(value.size());
    for (unsigned i = 0; i < 8; ++i)
      identity.push_back(static_cast<char>((size >> (i * 8U)) & 0xffU));
    identity.append(value.data(), value.size());
  };
  append(fields.network);
  append(fields.node_synced ? "synced" : "unsynced");
  append(fields.pay_address);
  append(fields.extra_data);
  append(fields.inference_challenge);
  append(std::string_view(reinterpret_cast<const char *>(wire.data()), wire.size()));
  return identity;
}
}

std::string canonical_job_fingerprint(const TemplateFields &fields) {
  const auto identity = identity_bytes(fields);
  const auto digest = blake3_digest(reinterpret_cast<const std::uint8_t *>(identity.data()), identity.size());
  return hex(digest.data(), digest.size());
}

LiveJobQueue::PublishResult LiveJobQueue::publish(TemplateFields fields) {
  if (fields.network.empty() || fields.response_wire.empty() || fields.block_wire.empty())
    throw std::invalid_argument("live job queue requires a complete template snapshot");
  std::lock_guard lock(mutex_);
  if (closed_) throw std::logic_error("cannot publish to a closed live job queue");
  // The response envelope contains a per-request RPC id. Deduplicate the actual
  // consensus job, not that transport id, otherwise an identical template fetched
  // after reconnect is falsely treated as a new generation.
  if (current_job_ && same_identity(fields, current_job_->fields)) {
    ++duplicates_;
    const bool resolving_refresh = refresh_pending_;
    refresh_pending_ = false;
    // A worker can have paused at the admission boundary while the refresh was
    // unresolved. Requeue the same immutable job/lease so it resumes without a
    // generation change or nonce reset.
    if (active_ && current_job_) {
      current_job_->fields = std::move(fields);
      if (resolving_refresh) pending_ = *current_job_;
    }
    if (resolving_refresh) ready_.notify_all();
    return PublishResult::Duplicate;
  }
  if (active_) active_->store(false, std::memory_order_release);
  if (next_generation_ == UINT64_MAX)
    throw std::overflow_error("live job generation exhausted");
  const auto generation = ++next_generation_;
  active_ = std::make_shared<std::atomic_bool>(true);
  generation_ = generation;
  refresh_pending_ = false;
  current_job_.emplace(TemplateJob{generation, std::move(fields), JobLease(generation, active_)});
  pending_ = *current_job_;
  ++published_;
  ready_.notify_all();
  return PublishResult::Published;
}

bool LiveJobQueue::begin_refresh() noexcept {
  std::lock_guard lock(mutex_);
  if (closed_) return false;
  ++refresh_notifications_;
  const bool newly_pending = !refresh_pending_;
  refresh_pending_ = true;
  return newly_pending;
}

void LiveJobQueue::invalidate_for_virtual_chain_change() noexcept {
  std::lock_guard lock(mutex_);
  ++virtual_chain_invalidations_;
  if (active_) active_->store(false, std::memory_order_release);
  active_.reset();
  generation_.reset();
  pending_.reset();
  current_job_.reset();
  refresh_pending_ = !closed_;
  ready_.notify_all();
}

void LiveJobQueue::invalidate() noexcept {
  std::lock_guard lock(mutex_);
  if (active_) active_->store(false, std::memory_order_release);
  active_.reset();
  generation_.reset();
  pending_.reset();
  current_job_.reset();
  refresh_pending_ = false;
  ready_.notify_all();
}

void LiveJobQueue::close() noexcept {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
    if (active_) active_->store(false, std::memory_order_release);
    active_.reset();
    generation_.reset();
    pending_.reset();
    current_job_.reset();
    refresh_pending_ = false;
  }
  ready_.notify_all();
}

std::optional<TemplateJob> LiveJobQueue::wait_pop() {
  std::unique_lock lock(mutex_);
  ready_.wait(lock, [&] { return closed_ || pending_.has_value(); });
  if (closed_ || !pending_) return std::nullopt;
  auto job = std::move(pending_);
  pending_.reset();
  return job;
}

std::optional<TemplateJob> LiveJobQueue::wait_pop_for(std::chrono::milliseconds timeout) {
  if (timeout < std::chrono::milliseconds::zero())
    throw std::invalid_argument("live job queue timeout cannot be negative");
  std::unique_lock lock(mutex_);
  (void)ready_.wait_for(lock, timeout, [&] { return closed_ || pending_.has_value(); });
  if (closed_ || !pending_) return std::nullopt;
  auto job = std::move(pending_);
  pending_.reset();
  return job;
}

bool LiveJobQueue::closed() const noexcept {
  std::lock_guard lock(mutex_);
  return closed_;
}

bool LiveJobQueue::admission_allowed(std::uint64_t generation) const noexcept {
  std::lock_guard lock(mutex_);
  return !closed_ && !refresh_pending_ && generation_ && *generation_ == generation &&
         active_ && active_->load(std::memory_order_acquire);
}

bool LiveJobQueue::wait_refresh_resolved(std::uint64_t generation) {
  std::unique_lock lock(mutex_);
  ready_.wait(lock, [&] {
    return closed_ || !generation_ || *generation_ != generation || !refresh_pending_;
  });
  return !closed_ && !refresh_pending_ && generation_ && *generation_ == generation &&
         active_ && active_->load(std::memory_order_acquire);
}

bool LiveJobQueue::submission_allowed(std::uint64_t generation) const noexcept {
  return admission_allowed(generation);
}

bool LiveJobQueue::owns_lease(const JobLease &lease) const noexcept {
  std::lock_guard lock(mutex_);
  return !closed_ && generation_ && lease.generation_ == *generation_ &&
         lease.active_ == active_ && active_ && active_->load(std::memory_order_acquire);
}

bool LiveJobQueue::owns_snapshot(const TemplateJob &job) const {
  std::lock_guard lock(mutex_);
  return !closed_ && generation_ && job.generation == *generation_ &&
         job.lease.generation_ == *generation_ && job.lease.active_ == active_ &&
         active_ && active_->load(std::memory_order_acquire) &&
         current_job_ && same_identity(job.fields, current_job_->fields);
}

std::optional<std::uint64_t> LiveJobQueue::current_generation() const noexcept {
  std::lock_guard lock(mutex_);
  return generation_;
}

std::uint64_t LiveJobQueue::published_count() const noexcept {
  std::lock_guard lock(mutex_);
  return published_;
}

std::uint64_t LiveJobQueue::duplicate_count() const noexcept {
  std::lock_guard lock(mutex_);
  return duplicates_;
}

std::uint64_t LiveJobQueue::virtual_chain_invalidations() const noexcept {
  std::lock_guard lock(mutex_);
  return virtual_chain_invalidations_;
}

} // namespace keryx::nexus
