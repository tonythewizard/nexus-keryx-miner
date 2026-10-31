#include "keryx/nexus/post_submit_audits.hpp"
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <iostream>
using namespace keryx::nexus;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(){
  std::atomic<unsigned> audited=0,errors=0;
  std::promise<void> sent;
  auto completion=sent.get_future().share();
  PostSubmitAudits queue;
  queue.enqueue(completion,[&]{++audited;},[&](auto){++errors;});
  // drain cannot complete until the actual submit attempt has completed.
  std::promise<void> draining;
  auto started=draining.get_future();
  auto drain=std::async(std::launch::async,[&]{draining.set_value();queue.drain();});
  started.wait();
  const bool was_waiting=drain.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
  const bool premature=audited!=0;
  sent.set_value();drain.get();
  require(was_waiting&&!premature,"audit did not wait for submission completion");
  require(audited==1&&errors==0,"ordered audit lost");
  // A transport exception does not suppress the independent proof audit.
  std::promise<void> failed_send;failed_send.set_exception(std::make_exception_ptr(std::runtime_error("RPC failed")));
  queue.enqueue(failed_send.get_future().share(),[&]{++audited;},[&](auto){++errors;});
  queue.drain();require(audited==2,"failed transport skipped proof audit");
  queue.enqueue({},[]{throw std::runtime_error("invalid Rust proof");},[&](auto error){require(bool(error),"missing audit error");++errors;});
  queue.drain();require(queue.failed()&&errors==1,"audit failure did not latch");
  queue.enqueue({},[&]{++audited;},[](auto){});queue.drain();
  require(queue.failed(),"later success cleared audit failure");
  // Fifth scheduling call waits instead of creating an unbounded audit backlog.
  std::promise<void> release;auto ready=release.get_future().share();
  PostSubmitAudits bounded;
  for(unsigned i=0;i<4;++i)bounded.enqueue(ready,[]{},[](auto){});
  std::promise<void> scheduling;auto scheduling_started=scheduling.get_future();
  auto fifth=std::async(std::launch::async,[&]{scheduling.set_value();bounded.enqueue({},[]{},[](auto){});});
  scheduling_started.wait();
  const bool backpressure=fifth.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
  release.set_value();fifth.get();bounded.drain();
  require(backpressure,"audit backlog exceeded four");
  std::promise<void> destroyed;auto destruction=destroyed.get_future();
  {PostSubmitAudits shutdown;shutdown.enqueue({},[&]{destroyed.set_value();},[](auto){});}
  require(destruction.wait_for(std::chrono::seconds(0))==std::future_status::ready,"shutdown lost outstanding audit");
  std::cout<<"POST_SUBMIT_AUDITS=PASS ordered=true bounded=4 failure=sticky drain=joined\n";
}
