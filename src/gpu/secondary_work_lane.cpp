#include "keryx/nexus/secondary_work_lane.hpp"
#include <chrono>
#include <limits>

namespace keryx::nexus {
SecondaryWorkLane::SecondaryWorkLane(GpuWorkCoordinator &owner,Execute execute,std::function<bool()> admission_ready,std::uint32_t batch_size,Prepare prepare)
 :owner_(owner),execute_(std::move(execute)),prepare_(std::move(prepare)),prepared_(!prepare_),admission_ready_(std::move(admission_ready)),batch_size_(std::max(1u,batch_size)) {
  if(!execute_)throw std::invalid_argument("Secondary lane executor is required");
  thread_=std::thread([this]{run();});
}
SecondaryWorkLane::~SecondaryWorkLane(){request_stop();if(thread_.joinable())thread_.join();}
void SecondaryWorkLane::publish(TemplateJob job,consensus::Hash pre_pow) {
  std::lock_guard lock(mutex_);
  if(stop_)return;
  // The canonical queue lease decides whether an older executing job is stale.
  // A duplicate refresh must not cancel useful work just because it was copied.
  latest_=Job{std::move(job),pre_pow};changed_.notify_all();
}
void SecondaryWorkLane::set_batch_size(std::uint32_t count) {
  if(count<1||count>4096)throw std::invalid_argument("Secondary batch size must be 1..4096");
  std::lock_guard lock(mutex_);batch_size_=count;
}
void SecondaryWorkLane::pause_and_drain() {
  std::unique_lock lock(mutex_);
  if(std::this_thread::get_id()==thread_.get_id())throw std::logic_error("Secondary lane cannot drain itself");
  if(pauses_==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("Secondary pause count exhausted");
  ++pauses_;++epoch_;changed_.notify_all();
  changed_.wait(lock,[&]{return !busy_;});
  if(failure_)std::rethrow_exception(failure_);
}
void SecondaryWorkLane::invalidate_preparation() {
  std::lock_guard lock(mutex_);
  if(!pauses_||busy_||!prepare_||stop_||failure_)
    throw std::logic_error("Preparation invalidation requires a healthy paused lane with a loader");
  prepared_=false;
}
void SecondaryWorkLane::resume() {
  std::lock_guard lock(mutex_);
  if(!pauses_)throw std::logic_error("Secondary lane resume without pause");
  --pauses_;changed_.notify_all();
}
void SecondaryWorkLane::request_stop() noexcept {
  std::lock_guard lock(mutex_);stop_=true;++epoch_;latest_.reset();changed_.notify_all();
}
void SecondaryWorkLane::join() {
  if(thread_.get_id()==std::this_thread::get_id())throw std::logic_error("Secondary lane cannot join itself");
  if(thread_.joinable())thread_.join();
}
void SecondaryWorkLane::rethrow_if_failed() const {
  std::lock_guard lock(mutex_);if(failure_)std::rethrow_exception(failure_);
}
SecondaryWorkLane::Stats SecondaryWorkLane::stats() const {
  std::lock_guard lock(mutex_);auto s=counters_;
  s.busy=busy_;s.preparing=preparing_;s.paused=pauses_!=0;s.stopped=stop_;s.failed=bool(failure_);return s;
}
void SecondaryWorkLane::run() noexcept {
  try {
    std::unique_lock lock(mutex_);
    while(!stop_) {
      changed_.wait(lock,[&]{return stop_||(!pauses_&&latest_);});
      if(stop_)break;
      if(admission_ready_&&!admission_ready_()) {
        changed_.wait_for(lock,std::chrono::milliseconds(5));continue;
      }
      if(!prepared_) {
        // Model bytes are invariant across template refreshes. Preparation
        // owns no nonce reservation and yields to AI/stop between upload chunks.
        busy_=preparing_=true;
        lock.unlock();
        bool ready=false;
        try {ready=prepare_([&]{std::lock_guard guard(mutex_);return stop_||pauses_||(admission_ready_&&!admission_ready_());});}
        catch(...){lock.lock();busy_=preparing_=false;changed_.notify_all();throw;}
        lock.lock();busy_=preparing_=false;prepared_=ready;changed_.notify_all();
        continue; // Re-read the latest template after potentially long I/O.
      }
      const auto job=*latest_;const auto epoch=epoch_;
      std::uint32_t req=batch_size_;
      while(req>1 && !owner_.nonce_range_available(req)) req/=2;
      auto ticket=owner_.reserve(MiningLane::Amd,job.snapshot,job.pre_pow,req);
      if(!ticket) {
        if(!owner_.nonce_range_available(1))throw std::overflow_error("Secondary nonce range exhausted");
        changed_.wait_for(lock,std::chrono::milliseconds(5));continue;
      }
      const auto reserved=*ticket;
      busy_=true;counters_.attempts+=reserved.count();
      // A second guard makes ticket retirement unconditional even if a test or
      // transport callback throws before taking ownership of the ticket.
      lock.unlock();
      bool verified=false;
      try {
        const auto cancelled=[&,epoch]{std::lock_guard guard(mutex_);return stop_||pauses_||epoch_!=epoch||(admission_ready_&&!admission_ready_());};
        const auto commit=[&,epoch](const std::function<void()> &effect){
          std::unique_lock guard(mutex_);
          if(stop_||pauses_||epoch_!=epoch||(admission_ready_&&!admission_ready_())||owner_.check(reserved)!=WorkGate::Ready)return false;
          // Linearize admission only. Transport must recheck freshness and
          // support AI cancellation; never keep the lane mutex across RPC.
          guard.unlock();effect();guard.lock();
          ++counters_.commits;++counters_.verified;return true;
        };
        verified=execute_(std::move(*ticket),cancelled,commit);
      }catch(...){owner_.stop();owner_.finish(reserved);throw;}
      owner_.finish(reserved);
      lock.lock();busy_=false;
      if(verified)counters_.completed+=reserved.count();else counters_.discarded+=reserved.count();
      changed_.notify_all();
    }
  }catch(...) {
    owner_.stop();std::lock_guard lock(mutex_);
    failure_=std::current_exception();stop_=true;busy_=false;latest_.reset();changed_.notify_all();
  }
}
}
