#include "keryx/nexus/secondary_work_lane.hpp"
#include "keryx/nexus/secondary_vulkan_config.hpp"
#include <atomic>
#include <iostream>
#include <set>
using namespace keryx::nexus;
using namespace std::chrono_literals;
namespace {
std::atomic_uint checks{};
void expect(bool ok,const char *why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void wait(F f){const auto end=std::chrono::steady_clock::now()+3s;
  while(!f()){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("Fixture synchronization timed out");std::this_thread::sleep_for(1ms);}}
template<class F>void rejected(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}expect(caught,"Expected fail-closed rejection");}
TemplateFields fields(unsigned marker=1){TemplateFields f;f.network="mainnet";f.node_synced=true;
 f.timestamp=123;f.daa_score=126000000;f.response_wire={std::uint8_t(marker),1};f.block_wire={std::uint8_t(marker),2};
 f.consensus_identity_wire=f.block_wire;f.pay_address="fixture-payout";f.extra_data="fixture-identity";return f;}
}
int main(){try{
 const std::string sha(64,'a');
 expect(!secondary_vulkan_config(nullptr,nullptr,nullptr,false),"Secondary mode must default off");
 expect(bool(secondary_vulkan_config("shadow","module",sha.c_str(),false)),"Valid shadow config rejected");
 expect(bool(secondary_vulkan_config("live","module",sha.c_str(),true)),"Valid live config rejected");
 const auto bda=secondary_vulkan_config("resident-live","bda-native","bda-native",true);
 expect(bda&&bda->resident&&bda->live_submit,"Secondary BDA route rejected");
 rejected([&]{secondary_vulkan_config("resident-live","bda-native",sha.c_str(),true);});
 rejected([&]{secondary_vulkan_config("resident-live","module","bda-native",true);});
 rejected([&]{secondary_vulkan_config("live","bda-native","bda-native",true);});
 const auto resident_shadow=secondary_vulkan_config("resident-shadow","module",sha.c_str(),true);
 expect(resident_shadow&&resident_shadow->resident&&!resident_shadow->live_submit,"Resident shadow admitted submission");
 const auto resident_live=secondary_vulkan_config("resident-live","module",sha.c_str(),true);
 expect(resident_live&&resident_live->resident&&resident_live->live_submit,"Authorized resident live rejected");
 const auto resident_disabled=secondary_vulkan_config("resident-live","module",sha.c_str(),false);
 expect(resident_disabled&&!resident_disabled->live_submit,"Resident mode bypassed explicit submit authorization");
 rejected([&]{secondary_vulkan_config("unsupported_mode","module",sha.c_str(),false);});
 rejected([&]{secondary_vulkan_config(nullptr,"module",sha.c_str(),false);});
 rejected([&]{secondary_vulkan_config("shadow","module","invalid",false);});
 {
  LiveJobQueue queue;GpuWorkCoordinator owner(queue,1);
  rejected([&]{SecondaryWorkLane missing(owner,{});});
  SecondaryWorkLane idle(owner,[](auto,const auto &,const auto &){return false;});
  try{SecondaryLanePause pause(&idle);throw std::runtime_error("AI failure");}
  catch(const std::runtime_error &){}
  idle.join();expect(idle.stats().stopped&&idle.stats().attempts==0,"AI exception reopened secondary admission");
  idle.request_stop();idle.join();expect(owner.in_flight()==0,"Repeated stop/join leaked ticket");
 }
 {
  LiveJobQueue queue;queue.publish(fields());auto job=*queue.wait_pop();consensus::Hash pph{};
  GpuWorkCoordinator owner(queue,100,4,1);auto primary=*owner.reserve(MiningLane::Nvidia,job,pph,4);
  std::atomic_uint entered{},effects{};std::atomic_bool permit{false};std::mutex records;
  std::set<std::uint64_t> nonces;std::uint64_t last_generation{};
  SecondaryWorkLane lane(owner,[&](auto ticket,const auto &cancelled,const auto &commit){
    {std::lock_guard lock(records);if(!nonces.insert(ticket.first_nonce()).second)throw std::runtime_error("Nonce reused");last_generation=ticket.job().generation;}
    ++entered;wait([&]{return cancelled()||permit.load();});
    if(cancelled()){expect(!commit([&]{++effects;}),"Cancelled operation committed");return false;}
    permit=false;return commit([&]{++effects;}); // Deliberately leave retirement to lane guard.
  });
  lane.pause_and_drain();lane.publish(job,pph);expect(lane.stats().attempts==0,"Paused startup admitted work");
  lane.resume();wait([&]{return entered.load()==1;});
  expect(owner.in_flight()==2,"Primary/secondary tickets not concurrent");
  lane.publish(job,pph);expect(effects==0,"Duplicate publication forced effect");
  lane.pause_and_drain();expect(owner.in_flight()==1,"AMD pause waited on or retired NVIDIA ticket");
  expect(owner.check(primary)==WorkGate::Ready,"Local AMD pause cancelled primary");
  lane.pause_and_drain();lane.resume();expect(lane.stats().paused,"Nested pause resumed prematurely");
  queue.publish(fields(2));auto replacement=*queue.wait_pop();lane.publish(replacement,pph);
  const auto before=entered.load();std::this_thread::sleep_for(5ms);
  expect(entered==before,"Nested pause admitted replacement");
  lane.resume();wait([&]{return entered.load()==2;});
  {std::lock_guard lock(records);expect(last_generation==replacement.generation,"Latest replacement not selected");expect(*nonces.begin()>=104,"Secondary overlaps NVIDIA range");}
  permit=true;wait([&]{return effects.load()==1;});wait([&]{return entered.load()>=3;});
  lane.request_stop();lane.join();lane.rethrow_if_failed();
  const auto s=lane.stats();expect(s.stopped&&!s.busy&&!s.failed&&s.verified==1&&s.commits==1,"Final lane counters mismatch");
  expect(s.discarded>=2,"Pause/shutdown did not discard results");
  expect(effects==1,"Shutdown caused another commit");expect(owner.in_flight()==1,"Secondary shutdown leaked ticket");
  owner.finish(primary);expect(owner.in_flight()==0,"Primary cleanup failed");
  rejected([&]{lane.resume();});
 }
 for(unsigned failure=0;failure<3;++failure){
  LiveJobQueue queue;queue.publish(fields());auto job=*queue.wait_pop();consensus::Hash pph{};
  GpuWorkCoordinator owner(queue,200);
  SecondaryWorkLane lane(owner,[&](auto,const auto &,const auto &commit)->bool{
    if(failure==0)throw std::runtime_error("Injected GPU/verifier error");
    if(failure==1)commit([]{throw std::runtime_error("Injected transport error");});
    return true;
  });
  if(failure==2){LiveJobQueue foreign;foreign.publish(fields());lane.publish(*foreign.wait_pop(),pph);}
  else lane.publish(job,pph);
  wait([&]{return lane.stats().failed;});lane.join();rejected([&]{lane.rethrow_if_failed();});
  expect(owner.in_flight()==0,"Failure leaked reservation");
  expect(!owner.reserve(MiningLane::Nvidia,job,pph,1),"Fatal error did not stop primary admission");
 }
 // A refresh alone must block side effects without releasing the queue's lease.
 {
  LiveJobQueue queue;queue.publish(fields());auto job=*queue.wait_pop();consensus::Hash pph{};
  GpuWorkCoordinator owner(queue,300);std::atomic_bool entered{},go{};std::atomic_uint effects{};
  SecondaryWorkLane lane(owner,[&](auto,const auto &cancelled,const auto &commit){entered=true;wait([&]{return go.load()||cancelled();});
    const auto ok=commit([&]{++effects;});if(ok)throw std::runtime_error("Refresh committed");return false;});
  lane.publish(job,pph);wait([&]{return entered.load();});expect(queue.begin_refresh(),"Refresh failed");go=true;
  wait([&]{return lane.stats().discarded==1;});lane.request_stop();lane.join();lane.rethrow_if_failed();
  expect(effects==0&&job.lease.valid()&&owner.in_flight()==0,"Refresh gate weakened or leaked ticket");
 }
 // AI discovery closes admission before the inference scope begins draining.
 {
  LiveJobQueue queue;queue.publish(fields());auto job=*queue.wait_pop();consensus::Hash pph{};
  GpuWorkCoordinator owner(queue,400);std::atomic_bool ready{false},entered{};std::atomic_uint effects{};
  SecondaryWorkLane lane(owner,[&](auto,const auto &cancelled,const auto &commit){entered=true;
    wait(cancelled);expect(!commit([&]{++effects;}),"AI discovery allowed commit");return false;},[&]{return ready.load();});
  lane.publish(job,pph);std::this_thread::sleep_for(5ms);expect(lane.stats().attempts==0,"Queued AI admitted GPU work");
  ready=true;wait([&]{return entered.load();});ready=false;
  wait([&]{return lane.stats().discarded==1;});
  expect(owner.in_flight()==0&&effects==0,"AI cancellation leaked work or effect");lane.request_stop();lane.join();lane.rethrow_if_failed();
 }
 for(const auto batch : {16u,32u,64u,128u,256u,1024u,2048u,4096u}) {
  LiveJobQueue queue;queue.publish(fields());auto job=*queue.wait_pop();consensus::Hash pph{};GpuWorkCoordinator owner(queue,500,4,batch);
  const auto primary=owner.reserve(MiningLane::Nvidia,job,pph,4);
  expect(bool(primary),"Primary reservation failed before AMD work");
  std::atomic_bool permit{true};
  unsigned batches=0;
  SecondaryWorkLane lane(owner,[&](auto ticket,const auto&,const auto&){
    const bool first=batches++==0;
    expect(ticket.count()==(first?batch:1)&&ticket.first_nonce()==primary->first_nonce()+4+(first?0:batch),"AMD batch overlaps primary range or ignores new size");
    permit=false;return true;},[&]{return permit.load();},batch);
  lane.publish(job,pph);wait([&]{return lane.stats().completed==batch;});
  const auto stats=lane.stats();expect(stats.attempts==batch&&stats.verified==0&&stats.discarded==0&&stats.commits==0,
      "Completed non-winning batch was mislabeled stale or Rust verified");
  expect(owner.check(*primary)==WorkGate::Ready,"AMD completion invalidated NVIDIA work");
  owner.finish(*primary);
  lane.set_batch_size(1);rejected([&]{lane.set_batch_size(0);});rejected([&]{lane.set_batch_size(4097);});
  permit=true;wait([&]{return lane.stats().completed==batch+1;});
  expect(lane.stats().attempts==batch+1,"Batch change altered completed work or future reservation");
  lane.request_stop();lane.join();lane.rethrow_if_failed();
 }
 {
  LiveJobQueue queue;queue.publish(fields());auto job=*queue.wait_pop();consensus::Hash pph{};
  GpuWorkCoordinator owner(queue,1000,4,16);auto primary=*owner.reserve(MiningLane::Nvidia,job,pph,4);
  std::atomic_uint preparations{};std::atomic_bool permit{true};
  SecondaryWorkLane lane(owner,[&](auto,const auto&,const auto&){permit=false;return true;},
      [&]{return permit.load();},16,[&](const auto &cancelled){
        if(++preparations==1){wait(cancelled);return false;}return true;
      });
  lane.publish(job,pph);wait([&]{return lane.stats().preparing;});
  expect(lane.stats().attempts==0&&owner.in_flight()==1,"Model preparation reserved nonce work");
  lane.pause_and_drain();expect(owner.check(primary)==WorkGate::Ready&&!lane.stats().preparing,
      "Model upload pause waited for or invalidated NVIDIA work");
  lane.resume();wait([&]{return lane.stats().completed==16;});
  expect(preparations==2&&lane.stats().discarded==0,"Upload resume was counted as stale mining work");
  rejected([&]{lane.invalidate_preparation();});
  lane.pause_and_drain();lane.invalidate_preparation();lane.invalidate_preparation();
  permit=true;lane.resume();wait([&]{return lane.stats().completed==32;});
  expect(preparations==3&&lane.stats().discarded==0,"Released banks resumed without preparation or counted upload as work");
  lane.request_stop();lane.join();owner.finish(primary);lane.rethrow_if_failed();
 }
 std::cout<<"SECONDARY_LANE=PASS checks="<<checks.load()<<" primary_overlap=NONE pause_drain=PASS nested_ai=PASS refresh=BLOCKED failures=FAIL_CLOSED gpu_runtime=NOT_STARTED\n";
}catch(const std::exception &e){std::cerr<<"SECONDARY_LANE=FAIL "<<e.what()<<'\n';return 1;}}
