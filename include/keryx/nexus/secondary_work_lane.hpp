#pragma once
#include "keryx/nexus/gpu_work_coordinator.hpp"
#include <condition_variable>
#include <exception>
#include <functional>
#include <thread>

namespace keryx::nexus {
// One bounded secondary consumer. It never pops the primary's ingress queue.
// The executor must drain submitted GPU work on all exits. The lane retires
// the ticket idempotently after the executor and any candidate destructor.
class SecondaryWorkLane final {
public:
  using Cancelled=std::function<bool()>;
  using Prepare=std::function<bool(const Cancelled &)>;
  using Commit=std::function<bool(const std::function<void()> &)>;
  // Execute returns true for a fully completed batch, including non-winners.
  using Execute=std::function<bool(GpuWorkCoordinator::Ticket,const Cancelled &,const Commit &)>;
  struct Stats {std::uint64_t attempts{},completed{},verified{},discarded{},commits{};bool busy{},preparing{},paused{},stopped{},failed{};};
  SecondaryWorkLane(GpuWorkCoordinator &owner,Execute execute,std::function<bool()> admission_ready={},std::uint32_t batch_size=1,Prepare prepare={});
  ~SecondaryWorkLane();
  SecondaryWorkLane(const SecondaryWorkLane &)=delete;
  SecondaryWorkLane &operator=(const SecondaryWorkLane &)=delete;
  void publish(TemplateJob job,consensus::Hash pre_pow);
  // Changes only future reservations; active tickets retain their exact count.
  void set_batch_size(std::uint32_t count);
  // Nested pauses are counted. No primary ticket or CUDA lock is awaited.
  // A pause must be paired with resume(), including after inference failure.
  void pause_and_drain();
  // Only while paused and drained. Preparation must run again before mining.
  void invalidate_preparation();
  void resume();
  void request_stop() noexcept;
  void join();
  void rethrow_if_failed() const;
  Stats stats() const;
private:
  struct Job {TemplateJob snapshot;consensus::Hash pre_pow;};
  void run() noexcept;
  GpuWorkCoordinator &owner_;
  Execute execute_;
  Prepare prepare_;
  bool prepared_{},preparing_{};
  std::function<bool()> admission_ready_; // Must be bounded and must not call back into this lane.
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<Job> latest_;
  std::exception_ptr failure_;
  std::uint64_t epoch_{},pauses_{};
  bool stop_{},busy_{};
  Stats counters_;
  std::uint32_t batch_size_{1};
  std::thread thread_;
};
class SecondaryLanePause final {
public:
  explicit SecondaryLanePause(SecondaryWorkLane *lane):lane_(lane),exceptions_(std::uncaught_exceptions()){
    if(lane_)lane_->pause_and_drain();
  }
  ~SecondaryLanePause(){if(lane_){
    if(std::uncaught_exceptions()>exceptions_)lane_->request_stop();
    else lane_->resume();
  }}
  SecondaryLanePause(const SecondaryLanePause &)=delete;
  SecondaryLanePause &operator=(const SecondaryLanePause &)=delete;
private:
  SecondaryWorkLane *lane_;
  int exceptions_;
};
}
