#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include <array>
#include <mutex>
#include <chrono>
#include <thread>

namespace keryx::nexus {
enum class MiningLane { Nvidia, Amd };
enum class WorkGate { Ready, Stale, RefreshPending, AiPaused, Stopped, Unknown };
// Only for finishing an already bound, read-only startup proof. This grants
// neither fresh-work accounting nor permission to return/submit a candidate.
inline bool startup_attestation_gate(WorkGate gate) {
  return gate==WorkGate::Ready||gate==WorkGate::Stale||gate==WorkGate::RefreshPending;
}

// A notification is not yet a stale verdict. Yield briefly for its validated
// template, without admitting work on Pending or delaying AI cancellation.
template<class Check,class Cancelled>
bool bounded_work_ready(Check check,Cancelled cancelled) {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(5);
  for(;;) {
    if(cancelled())return false;
    const auto state=check();
    if(state!=WorkGate::RefreshPending)return state==WorkGate::Ready;
    if(std::chrono::steady_clock::now()>=deadline)return false;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

// Development coordinator: both GPU lanes must use ONE instance in a future
// dual-device worker. It does not coordinate with an older running worker.
// No Rust/target verification or submission permission is implied by Ready.
class GpuWorkCoordinator final {
  struct Cancellation { std::atomic_bool requested{false}; };
public:
  class Ticket {
  public:
    std::uint64_t first_nonce() const noexcept { return first_; }
    std::uint32_t count() const noexcept { return count_; }
    MiningLane lane() const noexcept { return lane_; }
    const TemplateJob &job() const noexcept { return *job_; }
    bool cancelled() const noexcept { return !cancel_ || cancel_->requested.load(std::memory_order_acquire) || !job_ || !job_->lease.valid(); }
  private:
    friend class GpuWorkCoordinator;
    Ticket(std::uint64_t id, MiningLane lane, std::shared_ptr<const TemplateJob> job, consensus::Hash pph,
           std::uint64_t first, std::uint32_t count)
      : id_(id),lane_(lane),job_(std::move(job)),pph_(pph),first_(first),count_(count),cancel_(std::make_shared<Cancellation>()) {}
    std::uint64_t id_;
    MiningLane lane_;
    std::shared_ptr<const TemplateJob> job_;
    consensus::Hash pph_;
    std::uint64_t first_;
    std::uint32_t count_;
    std::shared_ptr<Cancellation> cancel_;
  };
  explicit GpuWorkCoordinator(LiveJobQueue &queue, std::uint64_t first_nonce,
                             std::uint32_t nvidia_limit=4608, std::uint32_t amd_limit=64);
  std::optional<Ticket> reserve(MiningLane lane, const TemplateJob &job,
                                const consensus::Hash &pph, std::uint32_t count);
  WorkGate check(const Ticket &ticket) const;
  WorkGate check_candidate(const Ticket &ticket, const consensus::CandidateBinding &binding) const;
  // Call only once GPU completion/cancellation has been acknowledged. No nonce
  // refund, even for stale work, partial batches, cancellation, or retries.
  bool finish(const Ticket &ticket);
  void pause_for_ai();
  bool resume_after_ai();
  void stop();
  std::size_t in_flight() const;
  bool nonce_range_available(std::uint32_t count) const;
private:
  static std::size_t index(MiningLane lane);
  bool owned(const Ticket &ticket) const;
  WorkGate check_locked(const Ticket &ticket) const;
  LiveJobQueue &queue_;
  mutable std::mutex mutex_;
  std::array<std::optional<Ticket>,2> active_;
  // One immutable caller-exact snapshot, shared with both active slots/tickets.
  // Never reuse solely by generation: duplicate RPC envelopes may differ.
  std::shared_ptr<const TemplateJob> snapshot_;
  std::array<std::uint32_t,2> limits_;
  std::uint64_t next_nonce_, next_id_{1};
  bool exhausted_{false},ai_paused_{false},stopped_{false};
};
}
