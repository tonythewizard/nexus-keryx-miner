#include "keryx/nexus/developer_config.hpp"
// Exclusive deterministic layout/occupancy replay. No RPC or submission calls.
#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/amd_memory_handoff.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace keryx::nexus::consensus;
namespace fs=std::filesystem;using Bytes=std::vector<std::uint8_t>;
static void require(bool b,const char*m){if(!b)throw std::runtime_error(m);}
static Bytes read(const fs::path&p){std::ifstream f(p,std::ios::binary|std::ios::ate);require(bool(f)&&f.tellg()>=0&&f.tellg()<8*1024*1024,"Invalid bounded artifact");f.seekg(0);return Bytes(std::istreambuf_iterator<char>(f),{});}
static std::uint64_t le(const Bytes&b,size_t p,unsigned n){require(p<=b.size()&&n<=b.size()-p,"Truncated artifact");std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[p+i])<<(8*i);return v;}
int main(int argc,char**argv){try{
 require(argc==14,"Usage: layout-probe BASE_SPV BASE_SHA PAD128_SPV SHA PAD256_SPV SHA PAD512_SPV SHA MODEL TREE CONTEXT TRACE OUT");
 auto ctx=read(argv[11]),trace=read(argv[12]);require(ctx.size()>225&&le(ctx,221,4)==ctx.size()-225,"Invalid context");
 auto seed=le(ctx,4,8),nonce=le(ctx,125,8);Hash pph{};std::copy_n(ctx.data()+85,32,pph.data());
 keryx::nexus::model::ReadOnlyGguf model(argv[9]);const auto logical=model.chunk_count()/32;
 keryx::nexus::model::SparseMerkleIndex merkle(model,argv[10]);fs::path out=argv[13];require(!fs::exists(out),"Evidence exists");fs::create_directories(out);
 constexpr unsigned total=4096;std::vector<std::uint64_t> inputs(total);for(unsigned i=0;i<total;++i)inputs[i]=block_seed(pph,le(ctx,117,8),nonce+i,le(ctx,133,8));
 Bytes expected_states;std::vector<std::uint64_t> expected_offsets;
 keryx::nexus::AmdMemoryHandoff handoff;
 for(unsigned phase=0;phase<5;++phase){
  handoff.wait_released();
  const bool candidate=phase>0&&phase<4;const unsigned module=candidate?1+2*phase:1;const unsigned batch=candidate?(32u<<phase):64u;VulkanWalkEngine engine("" NEXUS_SECONDARY_GPU_ID "",argv[module],argv[module+1],batch,logical,candidate);
  require(engine.load_resident([&](auto tile,auto bytes){model.read_tiles_streaming(tile,bytes);}),"Upload cancelled");
  VulkanWalkBatch diag;std::array<std::uint64_t,1> seeds{seed};require(engine.run(logical,seeds,{}, {},true,diag),"Diagnostic cancelled");
  require(trace.size()==28+257*1024+256*8&&diag.diagnostic_states.size()==257*1024,"Trace size mismatch");
  require(std::equal(diag.diagnostic_states.begin(),diag.diagnostic_states.end(),trace.begin()+28),"Full states differ from Rust reference");
  for(unsigned i=0;i<256;++i)require(diag.offsets[i]==le(trace,28+257*1024+i*8,8),"Offset differs from Rust reference");
  Walk walk;walk.offsets=diag.offsets;walk.states.resize(257);for(unsigned i=0;i<257;++i)std::copy_n(diag.diagnostic_states.data()+i*1024,1024,walk.states[i].data());
  auto gather=gather_v4_model_tiles(seed,logical,[&](auto off){std::array<std::uint8_t,1024> t{};model.read_tile_into(off,t);return t;});
  auto final=final_state(walk);auto proof=prove_v4_from_walk(walk,ctx[20],gather.tiles,[&](auto off){return merkle.tile_path(off);});auto wire=borsh_v4_container(proof,final,pom_pow_value_h3(final,pph));
  require(std::equal(wire.begin(),wire.end(),ctx.begin()+225,ctx.end()),"Borsh differs from official reference");
  {std::ofstream f(out/("phase-"+std::to_string(phase)+".proof"),std::ios::binary);f.write(reinterpret_cast<const char*>(wire.data()),wire.size());require(bool(f),"Proof write failed");}
  const std::vector<unsigned> sizes{batch,batch,batch};
  for(auto size:sizes){
   Bytes states;std::vector<std::uint64_t> offsets;states.reserve(total*1024);offsets.reserve(total*256);std::vector<double> timings;
   const auto begin=std::chrono::steady_clock::now();
   for(unsigned first=0;first<total;first+=size){VulkanWalkBatch result;auto start=std::chrono::steady_clock::now();require(engine.run(logical,std::span(inputs).subspan(first,size),{}, {},false,result),"Replay cancelled");
    timings.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());states.insert(states.end(),result.final_states.begin(),result.final_states.end());offsets.insert(offsets.end(),result.offsets.begin(),result.offsets.end());}
   const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
   if(expected_states.empty()){expected_states=states;expected_offsets=offsets;}else require(states==expected_states&&offsets==expected_offsets,"Layout/batch changed canonical output");
   bool cancelled=false;VulkanWalkBatch discarded;auto start=std::chrono::steady_clock::now();require(!engine.run(logical,std::span(inputs).first(size),{}, {},false,discarded,[&]{return cancelled;},[&]{cancelled=true;}),"Cancelled result escaped");
   auto drain=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();std::sort(timings.begin(),timings.end());
   std::cout<<"LAYOUT_REPLAY phase="<<phase<<" layout="<<(candidate?"padded":"baseline")<<" local_size="<<(candidate?(phase==3?256u:64u<<phase):128u)<<" batch="<<size<<" nonces="<<total<<" nonce_s="<<total*1000.0/elapsed<<" median_ms="<<timings[timings.size()/2]<<" p95_ms="<<timings[(timings.size()-1)*95/100]<<" max_ms="<<timings.back()<<" cancel_drain_ms="<<drain<<" byte_parity=PASS\n"<<std::flush;
  }
 }
 std::cout<<"LAYOUT_PARITY=PASS full_states=257 offsets=256 Borsh=BYTE_IDENTICAL cancelled_outputs=BLOCKED mainnet_transactions=0\n";
}catch(const std::exception&e){std::cerr<<"LAYOUT_PARITY=FAIL "<<e.what()<<'\n';return 1;}}
