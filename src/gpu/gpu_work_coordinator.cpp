#include "keryx/nexus/gpu_work_coordinator.hpp"
#include <limits>
#include <stdexcept>
#include <tuple>

namespace keryx::nexus {
namespace {
auto full_snapshot_fields(const TemplateFields &fields) {
  // Structured-binding arity makes a future field addition fail compilation
  // until this exact reuse comparison is updated. This is not consensus identity.
  const auto &[network,payout,extra,response,block,canonical,daa,bits,timestamp,
               nonce,parents,header_hash,synced,challenge,received]=fields;
  return std::tie(network,payout,extra,response,block,canonical,daa,bits,timestamp,
                  nonce,parents,header_hash,synced,challenge,received);
}
}
std::size_t GpuWorkCoordinator::index(MiningLane lane) {
  switch(lane) {case MiningLane::Nvidia:return 0;case MiningLane::Amd:return 1;}
  throw std::invalid_argument("Unknown mining lane");
}
GpuWorkCoordinator::GpuWorkCoordinator(LiveJobQueue &queue,std::uint64_t first,
    std::uint32_t nvidia_limit,std::uint32_t amd_limit)
 : queue_(queue),limits_{nvidia_limit,amd_limit},next_nonce_(first) {
  if(nvidia_limit<1 || nvidia_limit>65536 || amd_limit<1 || amd_limit>4096)
    throw std::invalid_argument("GPU batch limits exceed the bounded adapter contract");
}
bool GpuWorkCoordinator::owned(const Ticket &t) const {
  const auto &slot=active_[index(t.lane_)];
  return slot && slot->id_==t.id_ && slot->cancel_==t.cancel_;
}
std::optional<GpuWorkCoordinator::Ticket> GpuWorkCoordinator::reserve(
    MiningLane lane,const TemplateJob &job,const consensus::Hash &pph,std::uint32_t count) {
  std::lock_guard lock(mutex_);const auto slot=index(lane);
  if(count<1 || count>limits_[slot])throw std::invalid_argument("Invalid GPU batch count");
  if(stopped_ || ai_paused_ || active_[slot] || exhausted_ || !job.lease.valid())return std::nullopt;
  if(!queue_.owns_snapshot(job)) {
    if(!job.lease.valid())return std::nullopt;
    throw std::runtime_error("Job snapshot does not belong to this coordinator queue");
  }
  if(!queue_.admission_allowed(job.generation))return std::nullopt;
  try { require_mining_eligible(job.fields); }
  catch(const std::invalid_argument &) { return std::nullopt; }
  constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
  if(count-1>maximum-next_nonce_)return std::nullopt;
  if(next_id_==maximum)throw std::overflow_error("GPU ticket sequence exhausted");
  auto snapshot=snapshot_;
  if(!snapshot || !snapshot->lease.valid() || snapshot->generation!=job.generation ||
      full_snapshot_fields(snapshot->fields)!=full_snapshot_fields(job.fields))
    snapshot=std::make_shared<const TemplateJob>(job);
  Ticket ticket(next_id_,lane,snapshot,pph,next_nonce_,count);
  // Publish only after every potentially throwing allocation/copy succeeds.
  active_[slot]=ticket;
  snapshot_=std::move(snapshot);
  ++next_id_;
  if(count-1==maximum-next_nonce_)exhausted_=true;
  else next_nonce_+=count;
  return ticket;
}
WorkGate GpuWorkCoordinator::check_locked(const Ticket &ticket) const {
  if(!owned(ticket))return WorkGate::Unknown;
  if(stopped_)return WorkGate::Stopped;
  if(ai_paused_ || ticket.cancel_->requested.load(std::memory_order_acquire))return WorkGate::AiPaused;
  if(!ticket.job().lease.valid())return WorkGate::Stale;
  if(!queue_.owns_snapshot(ticket.job())) {
    if(!ticket.job().lease.valid())return WorkGate::Stale;
    throw std::runtime_error("Active GPU job snapshot integrity mismatch");
  }
  if(!queue_.admission_allowed(ticket.job().generation))return WorkGate::RefreshPending;
  return WorkGate::Ready;
}
WorkGate GpuWorkCoordinator::check(const Ticket &ticket) const {
  std::lock_guard lock(mutex_);return check_locked(ticket);
}
WorkGate GpuWorkCoordinator::check_candidate(const Ticket &ticket,const consensus::CandidateBinding &binding) const {
  std::lock_guard lock(mutex_);const auto state=check_locked(ticket);
  if(state!=WorkGate::Ready)return state;
  const bool owner=queue_.owns_lease(binding.lease);
  const auto binding_status=consensus::candidate_binding_status(binding,ticket.job());
  if(!ticket.job().lease.valid())return WorkGate::Stale;
  if(binding.nonce<ticket.first_ || binding.nonce-ticket.first_>=ticket.count_ ||
      binding.timestamp!=ticket.job().fields.timestamp || binding.pre_pow_hash!=ticket.pph_ ||
      !owner || binding_status!=consensus::CandidateBindingStatus::Valid)
    throw std::runtime_error("GPU candidate binding integrity mismatch");
  // A refresh may have arrived while the fingerprint was checked.
  if(!ticket.job().lease.valid())return WorkGate::Stale;
  return queue_.submission_allowed(ticket.job().generation)?WorkGate::Ready:WorkGate::RefreshPending;
}
bool GpuWorkCoordinator::finish(const Ticket &ticket) {
  std::lock_guard lock(mutex_);
  if(!owned(ticket))return false;
  ticket.cancel_->requested.store(true,std::memory_order_release);
  active_[index(ticket.lane_)].reset();
  if(snapshot_&&!snapshot_->lease.valid())snapshot_.reset();
  return true;
}
void GpuWorkCoordinator::pause_for_ai() {
  std::lock_guard lock(mutex_);ai_paused_=true;snapshot_.reset();
  for(auto &slot:active_)if(slot)slot->cancel_->requested.store(true,std::memory_order_release);
}
bool GpuWorkCoordinator::resume_after_ai() {
  std::lock_guard lock(mutex_);
  if(stopped_ || active_[0] || active_[1])return false;
  ai_paused_=false;return true;
}
void GpuWorkCoordinator::stop() {
  std::lock_guard lock(mutex_);stopped_=true;snapshot_.reset();
  for(auto &slot:active_)if(slot)slot->cancel_->requested.store(true,std::memory_order_release);
}
std::size_t GpuWorkCoordinator::in_flight() const {
  std::lock_guard lock(mutex_);return bool(active_[0])+bool(active_[1]);
}
bool GpuWorkCoordinator::nonce_range_available(std::uint32_t count) const {
  std::lock_guard lock(mutex_);
  return count>0 && !exhausted_ && count-1<=std::numeric_limits<std::uint64_t>::max()-next_nonce_;
}
}
