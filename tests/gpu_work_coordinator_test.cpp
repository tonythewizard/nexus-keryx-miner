#include "keryx/nexus/gpu_work_coordinator.hpp"
#include <algorithm>
#include <barrier>
#include <iostream>
#include <limits>
#include <thread>
using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
static unsigned checks{};
static void expect(bool condition,const char* reason) {++checks;if(!condition)throw std::runtime_error(reason);}
template<class F>void rejected(F f,const char* reason){bool caught=false;try{f();}catch(const std::exception&){caught=true;}expect(caught,reason);}
static TemplateFields fields(unsigned marker=1,unsigned envelope=0) {
 TemplateFields f;f.network="mainnet";f.node_synced=true;f.timestamp=123;f.daa_score=126000000;
 f.response_wire={std::uint8_t(envelope?envelope:marker),1};f.block_wire={std::uint8_t(marker),2};
 f.consensus_identity_wire=f.block_wire;f.pay_address="fixture-payout";f.extra_data="fixture-delegation";return f;
}
int main(){try{
 for(const auto gate:{WorkGate::Ready,WorkGate::Stale,WorkGate::RefreshPending})
  expect(startup_attestation_gate(gate),"Read-only proof cannot finish attestation");
 for(const auto gate:{WorkGate::AiPaused,WorkGate::Stopped,WorkGate::Unknown})
  expect(!startup_attestation_gate(gate),"Attestation ignored AI/stop/ownership gate");
 unsigned polls=0;
 // Windows may round a 100us sleep past the entire 5ms budget. Exercise
 // refresh transitions with a virtual clock, independent of host scheduling.
 auto tick=std::chrono::steady_clock::time_point{};
 const auto now=[&]{return tick;};
 const auto wait=[&]{tick+=std::chrono::microseconds(100);};
 expect(bounded_work_ready([&]{return ++polls<3?WorkGate::RefreshPending:WorkGate::Ready;},[]{return false;},now,wait),"Resolved refresh discarded valid work");
 expect(polls==3,"Resolved refresh was not rechecked");
 const auto start=tick;
 expect(!bounded_work_ready([]{return WorkGate::RefreshPending;},[]{return false;},now,wait),"Unresolved virtual refresh admitted");
 expect(tick-start==std::chrono::milliseconds(5),"Refresh exceeded its wait budget");
 unsigned waits=0;
 expect(!bounded_work_ready([]{return WorkGate::RefreshPending;},[&]{return waits==1;},now,[&]{++waits;wait();}),"AI cancellation during refresh ignored");
 expect(waits==1,"AI cancellation waited for the full budget");
 expect(!bounded_work_ready([]{return WorkGate::Stale;},[]{return false;}),"Stale work admitted");
 expect(!bounded_work_ready([]{return WorkGate::Ready;},[]{return true;}),"AI/stop cancellation ignored");
 expect(!bounded_work_ready([]{return WorkGate::RefreshPending;},[]{return false;}),"Unresolved refresh admitted");
 for(const auto gate:{WorkGate::AiPaused,WorkGate::Stopped,WorkGate::Unknown})
  expect(!bounded_work_ready([&]{return gate;},[]{return false;}),"Unsafe gate admitted");
 LiveJobQueue q;q.publish(fields());auto job=*q.wait_pop();Hash pph{};pph.fill(42);
 GpuWorkCoordinator c(q,100,8,4);
 auto n=*c.reserve(MiningLane::Nvidia,job,pph,8),a=*c.reserve(MiningLane::Amd,job,pph,4);
 expect(n.first_nonce()==100 && a.first_nonce()==108,"GPU ranges overlap or unexpected start");
 expect(c.in_flight()==2,"More/fewer than two active slots");
 expect(!c.reserve(MiningLane::Amd,job,pph,1),"Second in-flight AMD job admitted");
 auto binding=bind_candidate(job,111,job.fields.timestamp,pph);
 expect(c.check_candidate(a,binding)==WorkGate::Ready,"Owned candidate rejected");
 for(unsigned mutation=0;mutation<5;++mutation){auto bad=binding;
  if(mutation==0)bad.nonce=112;
  if(mutation==1)bad.pre_pow_hash[0]^=1;
  if(mutation==2)bad.timestamp++;
  if(mutation==3)bad.job_fingerprint[0]^=1;
  if(mutation==4)bad.generation++;
  rejected([&]{c.check_candidate(a,bad);},"Active binding corruption accepted");
 }
 LiveJobQueue foreign;foreign.publish(fields());auto other=*foreign.wait_pop();
 auto bad=binding;bad.lease=other.lease;
 rejected([&]{c.check_candidate(a,bad);},"Foreign lease with same generation accepted");
 expect(c.finish(n) && n.cancelled(),"Completed ticket not retired/cancelled");
 rejected([&]{c.reserve(MiningLane::Nvidia,other,pph,1);},"Foreign queue snapshot accepted");
 auto changed=job;changed.fields.block_wire[0]^=1;changed.fields.consensus_identity_wire[0]^=1;
 rejected([&]{c.reserve(MiningLane::Nvidia,changed,pph,1);},"Altered snapshot accepted");
 expect(q.begin_refresh(),"Refresh did not start");expect(c.check(a)==WorkGate::RefreshPending,"Refresh failed to pause winner checks");
 expect(!c.reserve(MiningLane::Nvidia,job,pph,1),"New work admitted during refresh");
 q.publish(fields(1,77));expect(c.check_candidate(a,binding)==WorkGate::Ready,"Duplicate envelope invalidated valid work");
 c.pause_for_ai();expect(a.cancelled() && c.check(a)==WorkGate::AiPaused,"AI failed to cancel active work");
 expect(!c.resume_after_ai(),"Resumed AI while GPU has not drained");
 expect(!c.reserve(MiningLane::Nvidia,job,pph,1),"PoM admitted during AI");
 expect(c.finish(a) && !c.finish(a),"Completion not idempotent");
 expect(c.check_candidate(a,binding)==WorkGate::Unknown,"Late duplicate candidate accepted");
 expect(c.resume_after_ai(),"Could not resume after both lanes drained");
 auto resumed=*c.reserve(MiningLane::Amd,job,pph,1);expect(resumed.first_nonce()==112,"Cancelled nonce range was reused");
 q.invalidate_for_virtual_chain_change();expect(resumed.cancelled() && c.check(resumed)==WorkGate::Stale,"Reorg left work eligible");
 expect(!c.reserve(MiningLane::Nvidia,job,pph,1),"Stale job admitted after reorg");
 expect(c.finish(resumed),"Could not drain stale ticket");
 q.publish(fields(2));job=*q.wait_pop();auto fresh=*c.reserve(MiningLane::Amd,job,pph,1);
 expect(fresh.first_nonce()==113 && c.check(fresh)==WorkGate::Ready,"New template reset nonce cursor or failed recovery");
 rejected([&]{c.reserve(MiningLane::Nvidia,job,pph,0);},"Zero batch admitted");
 rejected([&]{c.reserve(MiningLane::Amd,job,pph,5);},"Oversized AMD batch admitted");
 c.stop();expect(fresh.cancelled() && c.check(fresh)==WorkGate::Stopped,"Stop not sticky");
 expect(c.finish(fresh) && !c.resume_after_ai() && !c.reserve(MiningLane::Amd,job,pph,1),"Stopped coordinator resumed");
 GpuWorkCoordinator end(q,std::numeric_limits<std::uint64_t>::max()-1,8,4);
 expect(end.nonce_range_available(2) && !end.nonce_range_available(3) && !end.nonce_range_available(0),"Nonce capacity query wraps");
 expect(!end.reserve(MiningLane::Nvidia,job,pph,3),"Nonce range wrapped");
 auto final=*end.reserve(MiningLane::Amd,job,pph,2);expect(end.finish(final),"Final nonces not available");
 expect(!end.reserve(MiningLane::Amd,job,pph,1),"Nonce cursor wrapped to zero");
 expect(!end.nonce_range_available(1),"Exhausted owner reports available nonces");
 for(unsigned condition=0;condition<3;++condition) {
  LiveJobQueue gated;auto f=fields();
  if(condition==0)f.node_synced=false;
  if(condition==1)f.network="testnet";
  if(condition==2)f.inference_challenge="pending-ai";
  gated.publish(f);const auto j=*gated.wait_pop();GpuWorkCoordinator owner(gated,0);
  expect(!owner.reserve(MiningLane::Amd,j,pph,1),"Ineligible template admitted");
 }
 GpuWorkCoordinator other_owner(q,0);
 expect(other_owner.check(fresh)==WorkGate::Unknown,"Ticket from another coordinator accepted");
 rejected([&]{c.reserve(static_cast<MiningLane>(8),job,pph,1);},"Unknown GPU lane admitted");
 // Both threads reserve against one shared monotonic owner. This is CPU-only
 // fixture work; no CUDA/Vulkan engine or production worker is started.
 LiveJobQueue parallel;parallel.publish(fields());const auto pj=*parallel.wait_pop();
 GpuWorkCoordinator concurrent(parallel,0,8,4);std::mutex collected;std::vector<std::uint64_t> nonces;
 std::exception_ptr error;auto worker=[&](MiningLane lane){try{for(unsigned i=0;i<1000;++i){
  auto t=concurrent.reserve(lane,pj,pph,4);if(!t)throw std::runtime_error("Concurrent reservation failed");
  {std::lock_guard lock(collected);for(unsigned k=0;k<t->count();++k)nonces.push_back(t->first_nonce()+k);}
  if(!concurrent.finish(*t))throw std::runtime_error("Concurrent completion failed");
 }}catch(...){std::lock_guard lock(collected);error=std::current_exception();}};
 std::thread one(worker,MiningLane::Nvidia),two(worker,MiningLane::Amd);one.join();two.join();if(error)std::rethrow_exception(error);
 std::sort(nonces.begin(),nonces.end());expect(nonces.size()==8000,"Concurrent work lost");
 for(std::size_t i=0;i<nonces.size();++i)if(nonces[i]!=i)throw std::runtime_error("Duplicate or skipped concurrent nonce");
 expect(concurrent.in_flight()==0,"Completed work records leaked");
 // Race a real queue invalidation against candidate validation. An ordinary
 // selected-parent change must not become a fatal binding-integrity error.
 for(unsigned i=0;i<128;++i){LiveJobQueue race;race.publish(fields());const auto j=*race.wait_pop();GpuWorkCoordinator owner(race,0);
  const auto t=*owner.reserve(MiningLane::Amd,j,pph,1);const auto b=bind_candidate(j,0,j.fields.timestamp,pph);
  std::barrier start(2);std::thread invalidate([&]{start.arrive_and_wait();race.invalidate_for_virtual_chain_change();});
  start.arrive_and_wait();try{const auto gate=owner.check_candidate(t,b);if(gate!=WorkGate::Ready && gate!=WorkGate::Stale && gate!=WorkGate::RefreshPending)throw std::runtime_error("Unexpected race gate");}
  catch(...){invalidate.join();throw;}invalidate.join();expect(owner.check(t)==WorkGate::Stale,"Race left stale candidate eligible");owner.finish(t);
 }
 std::cout<<"GPU_WORK_COORDINATOR=PASS checks="<<checks<<" concurrent_nonces=8000 invalidation_races=128 stale_candidate_gate=BLOCKED ai_drain=PASS gpu_runtime=NOT_STARTED\n";
}catch(const std::exception&e){std::cerr<<"GPU_WORK_COORDINATOR=FAIL "<<e.what()<<'\n';return 1;}}
