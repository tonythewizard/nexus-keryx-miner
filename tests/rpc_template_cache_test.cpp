#include "keryx/nexus/rpc_template_cache.hpp"
#include "keryx/nexus/capture.hpp"
#include <chrono>
#include <iostream>
#include <string>
using namespace keryx::nexus;
static std::vector<std::uint8_t> bytes(const protowire::KaspadResponse &r) {
  const auto s=r.SerializeAsString();return {s.begin(),s.end()};
}
static void require(bool ok,const char *why) {if(!ok)throw std::runtime_error(why);}
int main(int argc,char **argv) {
 try {
  RpcTemplateCache cache;protowire::KaspadResponse a;
  a.set_id(1);a.mutable_getblocktemplateresponse()->mutable_block()->mutable_header()->set_timestamp(100);
  a.mutable_getblocktemplateresponse()->mutable_block()->add_transactions()->set_payload("first");
  auto first=bytes(a);const auto expected=a.getblocktemplateresponse().block().SerializeAsString();
  for(int i=0;i<1000;++i)require(cache.block(first).SerializeAsString()==expected,"identical wire changed block");
  a.set_id(2);auto duplicate=bytes(a);require(cache.block(duplicate).SerializeAsString()==expected,"RPC ID refresh changed block");
  a.mutable_getblocktemplateresponse()->mutable_block()->mutable_header()->set_timestamp(101);
  auto changed=bytes(a);require(cache.block(changed).header().timestamp()==101,"header update reused old envelope");
  a.mutable_getblocktemplateresponse()->mutable_block()->mutable_transactions(0)->set_payload("replacement");
  changed=bytes(a);require(cache.block(changed).transactions(0).payload()=="replacement","transaction update reused old envelope");
  protowire::KaspadResponse missing;missing.set_id(3);
  for(const auto &bad:std::vector<std::vector<std::uint8_t>>{{},{0xff},bytes(missing)}) {
   bool rejected=false;try{cache.block(bad);}catch(const std::runtime_error &){rejected=true;}
   require(rejected,"malformed or missing block accepted");
   require(cache.block(first).SerializeAsString()==expected,"cache did not recover after rejected snapshot");
  }
  require(cache.block(changed).SerializeAsString()==a.getblocktemplateresponse().block().SerializeAsString(),"replacement serialized bytes differ");
  std::cout<<"RPC_TEMPLATE_CACHE=PASS duplicate_envelope=true header_update=true transaction_update=true malformed_rejected=true recovery=true byte_parity=true\n";
  if(argc==2) {
   const auto records=read_capture(argv[1]);require(!records.empty(),"capture empty");const auto &wire=records.front().response_wire;
   require(cache.block(wire).SerializeAsString()==[&]{protowire::KaspadResponse r;require(r.ParseFromArray(wire.data(),wire.size()),"capture invalid");return r.getblocktemplateresponse().block().SerializeAsString();}(),"recorded template parity failed");
   constexpr int n=10000;std::uint64_t sink=0;
   auto start=std::chrono::steady_clock::now();
   for(int i=0;i<n;++i){protowire::KaspadResponse r;require(r.ParseFromArray(wire.data(),wire.size()),"baseline parse failed");sink+=r.getblocktemplateresponse().block().header().timestamp();sink+=blake3_digest(wire.data(),wire.size())[0];}
   const auto old_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
   start=std::chrono::steady_clock::now();for(int i=0;i<n;++i)sink+=cache.block(wire).header().timestamp();
   const auto new_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
   std::cout<<"CPU_TEMPLATE_COMPONENT_MEASUREMENT iterations="<<n<<" wire_bytes="<<wire.size()<<" old_ns_per_batch="<<old_ns/n<<" cached_ns_per_batch="<<new_ns/n<<" sink="<<sink<<" end_to_end_speedup=NOT_MEASURED\n";
  }
  return 0;
 }catch(const std::exception &e){std::cerr<<"RPC_TEMPLATE_CACHE=FAIL "<<e.what()<<'\n';return 1;}
}
