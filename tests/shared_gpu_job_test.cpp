#include "keryx/nexus/gpu_work_coordinator.hpp"
#include <iostream>
#include <new>
#include <type_traits>
using namespace keryx::nexus;
static unsigned fail_after=0,allocations=0;
static bool counting=false;
extern "C" void *__real__Znwm(std::size_t);
extern "C" void *__wrap__Znwm(std::size_t size) {
  if(counting)++allocations;
  if(fail_after&&!--fail_after)throw std::bad_alloc();
  return __real__Znwm(size);
}
static void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
static TemplateFields fields(){TemplateFields f;f.network="mainnet";f.node_synced=true;f.timestamp=123;f.daa_score=126000000;
 f.response_wire.assign(4096,1);f.block_wire.assign(4096,2);f.consensus_identity_wire=f.block_wire;return f;}
int main(){try{
 using Ticket=GpuWorkCoordinator::Ticket;
 static_assert(std::is_same_v<decltype(std::declval<const Ticket &>().job()),const TemplateJob &>);
 consensus::Hash pph{};LiveJobQueue q;q.publish(fields());auto job=*q.wait_pop();
 GpuWorkCoordinator owner(q,0,8,4);
 auto one=*owner.reserve(MiningLane::Nvidia,job,pph,4);
 auto two=*owner.reserve(MiningLane::Amd,job,pph,4);
 require(&one.job()==&two.job(),"identical lanes did not share immutable snapshot");
 auto copy=one;require(&copy.job()==&one.job(),"copying a ticket copied its job");
 require(owner.finish(one)&&copy.cancelled()&&!two.cancelled(),"ticket cancellation leaked across lanes");
 auto next=*owner.reserve(MiningLane::Nvidia,job,pph,4);
 require(&next.job()==&copy.job()&&!next.cancelled(),"steady batch failed reuse or inherited cancellation");
 owner.finish(next);owner.finish(two);
 auto changed=job;changed.fields.response_wire[0]=9;changed.fields.received_monotonic_ns=999;
 auto changed_ticket=*owner.reserve(MiningLane::Nvidia,changed,pph,4);
 require(&changed_ticket.job()!=&copy.job()&&changed_ticket.job().fields.response_wire[0]==9&&copy.job().fields.response_wire[0]==1,"RPC refresh overwrote in-flight snapshot");
 changed.fields.response_wire[0]=7;
 require(changed_ticket.job().fields.response_wire[0]==9,"caller retained mutable alias to immutable snapshot");
 require(owner.check(changed_ticket)==WorkGate::Ready,"equivalent canonical refresh changed eligibility");
 owner.finish(changed_ticket);
 changed.fields.timestamp=456;
 auto metadata=*owner.reserve(MiningLane::Nvidia,changed,pph,4);
 require(metadata.job().fields.timestamp==456&&metadata.job().fields.received_monotonic_ns==999,"derived fields lost during reuse");
 owner.pause_for_ai();require(metadata.cancelled()&&!owner.resume_after_ai(),"AI resumed before drain");owner.finish(metadata);require(owner.resume_after_ai(),"AI resume failed");
 auto after_ai=*owner.reserve(MiningLane::Nvidia,changed,pph,4);
 require(&after_ai.job()!=&metadata.job(),"AI did not release coordinator cache reference");
 q.invalidate_for_virtual_chain_change();require(owner.check(after_ai)==WorkGate::Stale&&after_ai.cancelled(),"reorg reused stale snapshot");owner.finish(after_ai);
 q.publish(fields());job=*q.wait_pop();auto fresh=*owner.reserve(MiningLane::Nvidia,job,pph,4);
 require(fresh.job().generation!=copy.job().generation&&copy.job().fields.response_wire[0]==1,"generation replacement corrupted retained snapshot");
 owner.finish(fresh);owner.stop();
 // Fail each cold allocation: no partial ticket or consumed nonce may escape.
 LiveJobQueue fail_queue;fail_queue.publish(fields());const auto fail_job=*fail_queue.wait_pop();
 unsigned cold_allocations=0;
 {GpuWorkCoordinator measured(fail_queue,0);counting=true;allocations=0;auto t=measured.reserve(MiningLane::Nvidia,fail_job,pph,4);counting=false;cold_allocations=allocations;require(bool(t),"allocation measurement failed");measured.finish(*t);}
 require(cold_allocations>=2,"missing allocation instrumentation");
 for(unsigned fail=1;fail<=cold_allocations;++fail){GpuWorkCoordinator c(fail_queue,100);fail_after=fail;bool threw=false;
  try{(void)c.reserve(MiningLane::Nvidia,fail_job,pph,4);}catch(const std::bad_alloc &){threw=true;}
  fail_after=0;require(threw&&c.in_flight()==0,"allocation failure published partial work");
  auto retry=c.reserve(MiningLane::Nvidia,fail_job,pph,4);require(retry&&retry->first_nonce()==100,"allocation failure consumed nonce");c.finish(*retry);
  fail_after=1;threw=false;try{(void)c.reserve(MiningLane::Nvidia,fail_job,pph,4);}catch(const std::bad_alloc &){threw=true;}
  fail_after=0;require(threw&&c.in_flight()==0,"warm cancellation allocation was not atomic");
  retry=c.reserve(MiningLane::Nvidia,fail_job,pph,4);require(retry&&retry->first_nonce()==104,"warm failure consumed nonce");c.finish(*retry);
 }
 std::cout<<"SHARED_GPU_JOB=PASS const_snapshot=true reuse=true exact_envelope=true caller_isolated=true independent_cancel=true reorg=true ai_release=true fault_points="<<cold_allocations<<" nonce_atomic=true\n";return 0;
}catch(const std::exception &e){counting=false;fail_after=0;std::cerr<<e.what()<<'\n';return 1;}}
