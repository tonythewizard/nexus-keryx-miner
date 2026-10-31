#pragma once
#include <atomic>
#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <vector>

namespace keryx::nexus {
// One caller schedules work. Audit tasks own their inputs and never own CUDA.
// Retire every task before referenced verifier/telemetry resources are destroyed.
class PostSubmitAudits final {
  std::vector<std::future<void>> tasks_;
  std::atomic_bool failed_{false};
public:
  PostSubmitAudits()=default;
  PostSubmitAudits(const PostSubmitAudits&)=delete;
  ~PostSubmitAudits(){drain();}
  bool failed() const noexcept{return failed_.load(std::memory_order_acquire);}
  void reap(){
    std::erase_if(tasks_,[](auto &task){
      if(task.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
      task.get();return true;
    });
  }
  void drain() noexcept{
    for(auto &task:tasks_)if(task.valid()){
      try{task.get();}catch(...){failed_.store(true,std::memory_order_release);}
    }
    tasks_.clear();
  }
  void enqueue(std::shared_future<void> submission_done,
               std::function<void()> audit,
               std::function<void(std::exception_ptr)> on_error){
    reap();
    if(tasks_.size()>=4){tasks_.front().get();tasks_.erase(tasks_.begin());}
    tasks_.push_back(std::async(std::launch::async,
      [this,submission_done=std::move(submission_done),audit=std::move(audit),on_error=std::move(on_error)]{
        // A failed RPC attempt still needs an audit. Waiting is intentional:
        // scheduling an async submit alone does not order a later audit.
        if(submission_done.valid())submission_done.wait();
        try{audit();}catch(...){
          failed_.store(true,std::memory_order_release);
          try{on_error(std::current_exception());}catch(...){/* keep failure sticky */}
        }
      }));
  }
};
}
