#include "keryx/nexus/live_job_queue.hpp"
#include <functional>
#include <iostream>
using namespace keryx::nexus;
int main(){try{
 unsigned cases=0;
 for(bool canonical:{false,true}){
  TemplateFields base;base.network="mainnet";base.node_synced=true;
  base.pay_address=std::string("p\0ay",4);base.extra_data=std::string("d\0eleg",6);base.inference_challenge=std::string("a\0i",3);
  base.response_wire={1,2};base.block_wire.assign(4096,73);if(canonical)base.consensus_identity_wire=base.block_wire;
  LiveJobQueue queue;queue.publish(base);const auto job=*queue.wait_pop();
  std::vector<TemplateFields> changes{base};
  auto mutate=[&](auto fn){auto f=base;fn(f);changes.push_back(std::move(f));};
  mutate([](auto &f){f.network="testnet";});mutate([](auto &f){f.node_synced=false;});
  mutate([](auto &f){f.pay_address.push_back('\0');});mutate([](auto &f){f.extra_data[0]^=1;});
  mutate([](auto &f){f.inference_challenge.clear();});mutate([](auto &f){f.response_wire={3,4};});
  mutate([](auto &f){f.block_wire[0]^=1;});mutate([](auto &f){f.block_wire.back()^=1;});
  mutate([](auto &f){f.consensus_identity_wire=f.block_wire;});
  mutate([](auto &f){f.consensus_identity_wire.clear();});
  if(canonical){mutate([](auto &f){f.consensus_identity_wire[0]^=1;});mutate([](auto &f){f.consensus_identity_wire.back()^=1;});}
  mutate([](auto &f){f.timestamp++;f.daa_score++;f.template_nonce++;}); // Canonical wire remains authoritative.
  mutate([](auto &f){f.pay_address="p";f.extra_data=std::string("ay\0d\0eleg",9);});
  for(const auto &f:changes){
   // publish() retains the original exact, length-prefixed identity implementation.
   // Use its duplicate verdict as an independent equivalence oracle.
   LiveJobQueue reference;reference.publish(base);
   const bool expected=reference.publish(f)==LiveJobQueue::PublishResult::Duplicate;
   auto snapshot=job;snapshot.fields=f;
   if(queue.owns_snapshot(snapshot)!=expected)throw std::runtime_error("ownership differs from original publish identity");
   ++cases;
  }
  LiveJobQueue foreign;foreign.publish(base);const auto other=*foreign.wait_pop();
  if(queue.owns_snapshot(other))throw std::runtime_error("foreign lease admitted");
  (void)queue.begin_refresh();if(!queue.owns_snapshot(job)||queue.admission_allowed(job.generation))throw std::runtime_error("refresh ownership/admission distinction changed");
  queue.publish(base);if(!queue.submission_allowed(job.generation))throw std::runtime_error("equivalent refresh failed to resolve");
  queue.invalidate_for_virtual_chain_change();if(queue.owns_snapshot(job)||job.lease.valid()||queue.submission_allowed(job.generation))throw std::runtime_error("reorg left old job eligible");
  queue.publish(base);if(queue.owns_snapshot(job))throw std::runtime_error("reinclusion accepted old generation");
  auto replacement=*queue.wait_pop();queue.close();if(queue.owns_snapshot(replacement))throw std::runtime_error("closed queue admitted snapshot");
 }
 std::cout<<"TEMPLATE_IDENTITY_EQUIVALENCE=PASS cases="<<cases<<" embedded_null=true foreign_lease_rejected=true reorg=true refresh_fail_closed=true\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
