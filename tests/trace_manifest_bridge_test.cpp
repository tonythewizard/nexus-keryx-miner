#include "keryx/nexus/trace_manifest_bridge.h"
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <cstring>
void check(bool ok){if(!ok)throw std::runtime_error("Native trace manifest bridge failed");}
int main(){try{
 for(unsigned network:{0U,1U})for(unsigned tier=0;tier<5;++tier){
  const auto daa=network ? 3500ULL:140115000ULL;
  NexusTraceManifestInfo info{};std::size_t bytes=0,rows=0;
  check(nexus_ai_h15_manifest(network,tier,daa,nullptr,0,&bytes,nullptr,0,&rows,&info)==2);
  check(info.application_abi==9&&info.position_leaves>2*info.n_products);
  check(rows==info.n_attn+info.n_recur);
  std::vector<std::uint8_t> manifest(bytes+8,0xaa);std::vector<std::uint32_t> layout(rows*5+8,0xabababab);
  check(nexus_ai_h15_manifest(network,tier,daa,manifest.data(),bytes-1,&bytes,layout.data(),rows,&rows,&info)==2);
  check(manifest.front()==0xaa&&layout.front()==0xabababab);
  check(nexus_ai_h15_manifest(network,tier,daa,manifest.data(),bytes,&bytes,layout.data(),rows,&rows,&info)==0);
  check(manifest[bytes]==0xaa&&layout[rows*5]==0xabababab);
  check(std::memcmp(manifest.data(),"KXMANIF2",8)==0);
  if(network)check(rows>0&&info.heads>0&&info.interval>0);
 }
 check(nexus_ai_attention_phase_window(0,0,0,0)==3000);
 check(nexus_ai_attention_phase_window(2,1000,30000,30900)==7800);
 check(nexus_ai_attention_phase_window(4,8000,0,5000)==19384);
 check(nexus_ai_attention_phase_window(3,100,2000,9000)==3500);
 check(nexus_ai_attention_phase_window(4,UINT32_MAX,UINT32_MAX,UINT32_MAX)==16107);
 NexusTraceManifestInfo info{};std::size_t bytes=17,rows=19;
 check(nexus_ai_h15_manifest(0,0,3500,nullptr,0,&bytes,nullptr,0,&rows,&info)==-10);
 check(bytes==17&&rows==19);
 std::cout<<"TRACE_MANIFEST_NATIVE_ABI=PASS tiers=5 network_policy=PASS gpu=NOT_USED\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
