#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/amd_memory_handoff.hpp"
#include "keryx/nexus/scoped_file_cache_guard.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
int main(int argc,char **argv){try{
 if(argc!=9)throw std::runtime_error("usage: MODEL MODULE SHA DEVICE PREFIX BATCH REPEATS COPY_OFFSETS");
 ScopedFileCacheGuard guard;
 if(std::string(argv[4])=="pci:0000:00:00.0"){AmdMemoryHandoff handoff;handoff.wait_released();}
 model::ReadOnlyGguf model(argv[1]);const auto n=model.chunk_count()/32;
 const size_t count=std::stoul(argv[6]),reps=std::stoul(argv[7]);const bool copy=std::string(argv[8])=="1";const bool paired=std::string(argv[8])=="paired";
 if(count<2||count>2048||reps>1000)throw std::runtime_error("bounded benchmark exceeded");
 VulkanWalkEngine engine(argv[4],argv[2],argv[3],count,n);
 if(!engine.load_resident([&](auto first,auto bytes){model.read_tiles_streaming(first,bytes);}))throw std::runtime_error("upload cancelled");
 Hash pph{};pph.fill(0x5a);std::vector<uint64_t> seeds(count);
 for(size_t i=0;i<count;++i)seeds[i]=block_seed(pph,1729450000000ULL,100000ULL+i,150000000ULL);
 VulkanWalkBatch out;
 if(!engine.run(n,std::span(seeds).first(2),{},{},true,out))throw std::runtime_error("diagnostic run cancelled");
 std::ofstream(std::string(argv[5])+".trace",std::ios::binary).write((char*)out.diagnostic_states.data(),out.diagnostic_states.size());
 if(!engine.run(n,seeds,{},{},false,out,{}, {},true))throw std::runtime_error("parity run cancelled");
 std::ofstream(std::string(argv[5])+".final",std::ios::binary).write((char*)out.final_states.data(),out.final_states.size());
 std::ofstream(std::string(argv[5])+".offsets",std::ios::binary).write((char*)out.offsets.data(),out.offsets.size()*8);
 std::vector<uint8_t> expected=out.final_states;
 for(int warm=0;warm<120;++warm)engine.run(n,seeds,{},{},false,out,{}, {},copy);
 std::cout<<"MEASURE_BEGIN\n";
 for(size_t i=0;i<reps;++i){
  for(int phase=0;phase<(paired?2:1);++phase){
   if(paired){
    // Alternate A/B order; both paths omit the CPU offset copy.
    const bool force=((phase+(i&1))%2)==0;
    if(force)setenv("NEXUS_BENCH_FORCE_OFFSETS_WRITE","1",1);else unsetenv("NEXUS_BENCH_FORCE_OFFSETS_WRITE");
    std::cout<<"PHASE force_offsets="<<force<<"\n";
   }
   auto t=std::chrono::steady_clock::now();
   if(!engine.run(n,seeds,{},{},false,out,{}, {},copy))throw std::runtime_error("screen cancelled");
   auto dt=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
   if(out.final_states!=expected)throw std::runtime_error("non deterministic final output");
   std::cout<<"SAMPLE batch="<<count<<" ms="<<dt<<" nonces_s="<<count*1000./dt<<"\n";
  }
 }
 guard.check();std::cout<<"BENCH_PASS batch="<<count<<" copy_offsets="<<copy<<"\n";
}catch(const std::exception&e){std::cerr<<"BENCH_FAIL "<<e.what()<<"\n";return 1;}}
