#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/consensus/verified_walk_view.hpp"
#include <utility>
#include <random>
#include <functional>
#include <iostream>
using namespace keryx::nexus;
// Independent pre-optimization byte serializer, with the exact 16-byte domain.
std::string reference_identity(const TemplateFields &f) {
 std::string out("KERYX-POM-JOB-v1",16);
 auto append=[&](std::string_view v){for(unsigned i=0;i<8;++i)out.push_back(static_cast<char>((static_cast<std::uint64_t>(v.size())>>(8*i))&255));out.append(v);};
 append(f.network);append(f.node_synced?"synced":"unsynced");append(f.pay_address);append(f.extra_data);append(f.inference_challenge);
 const auto &w=f.consensus_identity_wire.empty()?f.block_wire:f.consensus_identity_wire;
 append(std::string_view(reinterpret_cast<const char*>(w.data()),w.size()));return out;
}
template<class T> concept BorrowableWalk = requires(T&& walk,
 std::span<const std::uint64_t> offsets,std::span<const std::uint8_t> states) {
 consensus::verified_walk_view(std::forward<T>(walk),offsets,states);
};
static_assert(BorrowableWalk<consensus::Walk&>);
static_assert(BorrowableWalk<const consensus::Walk&>);
static_assert(!BorrowableWalk<consensus::Walk>);
static_assert(!BorrowableWalk<const consensus::Walk>);
int main(){try{
 std::mt19937 random(0x4e585553);
 for(unsigned i=0;i<128;++i) {
  TemplateFields f;
  auto bytes=[&](std::size_t n){std::string out(n,'\0');for(char& c:out)c=static_cast<char>(random()&255);return out;};
  f.network=bytes(i%20);f.node_synced=i%2;f.pay_address=bytes(i%100);
  f.extra_data=bytes(random()%20000);f.inference_challenge=bytes(random()%20000);
  const auto wire=bytes(random()%65536);f.block_wire.assign(wire.begin(),wire.end());
  if(i%3)f.consensus_identity_wire=f.block_wire;
  const auto old=reference_identity(f);const auto digest=blake3_digest(reinterpret_cast<const std::uint8_t*>(old.data()),old.size());
  if(canonical_job_fingerprint(f)!=hex(digest.data(),digest.size()))throw std::runtime_error("randomized fingerprint framing mismatch");
 }

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
  mutate([](auto &f){f.pay_address.assign(8192,'p');});
  mutate([](auto &f){f.inference_challenge.assign(17000,'i');});
  mutate([](auto &f){f.consensus_identity_wire.assign(1024*1024,42);});
  mutate([](auto &f){f.block_wire[0]^=1;});mutate([](auto &f){f.block_wire.back()^=1;});
  mutate([](auto &f){f.consensus_identity_wire=f.block_wire;});
  mutate([](auto &f){f.consensus_identity_wire.clear();});
  if(canonical){mutate([](auto &f){f.consensus_identity_wire[0]^=1;});mutate([](auto &f){f.consensus_identity_wire.back()^=1;});}
  mutate([](auto &f){f.timestamp++;f.daa_score++;f.template_nonce++;}); // Canonical wire remains authoritative.
  mutate([](auto &f){f.pay_address="p";f.extra_data=std::string("ay\0d\0eleg",9);});
  for(const auto &f:changes){
   const auto old=reference_identity(f);
   const bool expected=reference_identity(base)==old;
   const auto digest=blake3_digest(reinterpret_cast<const std::uint8_t*>(old.data()),old.size());
   if(canonical_job_fingerprint(f)!=hex(digest.data(),digest.size()))throw std::runtime_error("streamed fingerprint differs from original bytes");
   LiveJobQueue reference;reference.publish(base);
   if((reference.publish(f)==LiveJobQueue::PublishResult::Duplicate)!=expected)throw std::runtime_error("publish identity differs from original bytes");
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
