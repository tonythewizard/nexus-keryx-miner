#include "keryx/nexus/developer_config.hpp"
// Offline canonical replay. No RPC, wallet or submission transport is linked.
#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace keryx::nexus::consensus;
namespace fs=std::filesystem;
using Bytes=std::vector<std::uint8_t>;
static Bytes read(const fs::path&p){std::ifstream f(p,std::ios::binary|std::ios::ate);
 if(!f||f.tellg()<0||f.tellg()>8*1024*1024)throw std::runtime_error("Invalid bounded artifact");
 f.seekg(0);return Bytes(std::istreambuf_iterator<char>(f),{});}
static std::uint64_t le(const Bytes&b,size_t p,unsigned n){if(p>b.size()||n>b.size()-p)throw std::runtime_error("Truncated artifact");
 std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[p+i])<<(8*i);return v;}
static void save(const fs::path&p,const Bytes&b){if(fs::exists(p))throw std::runtime_error("Refusing artifact overwrite");
 std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());if(!f)throw std::runtime_error("Artifact write failed");}
static void require(bool b,const char*m){if(!b)throw std::runtime_error(m);}
int main(int argc,char**argv){try{
 const bool tuning=argc==10&&std::string(argv[9])=="--tune-batches";
 if(argc!=9&&!tuning)throw std::runtime_error("Usage: banked-probe SPV SHA MODEL TREE CONTEXT TRACE OUTDIR GATHERED_SPV_SHA_FILE [--tune-batches]");
 const auto ctx=read(argv[5]),trace=read(argv[6]);
 require(ctx.size()>225&&le(ctx,221,4)==ctx.size()-225&&std::string(ctx.begin(),ctx.begin()+4)=="NXRF","Invalid context");
 const auto seed=le(ctx,4,8),chunks=le(ctx,12,8),nonce=le(ctx,125,8);
 keryx::nexus::model::ReadOnlyGguf model(argv[3]);require(chunks==model.chunk_count()&&chunks%32==0,"Canonical model count mismatch");
 const auto logical=chunks/32;
 const char* test_uuid=std::getenv("NEXUS_VULKAN_TEST_UUID");
 const std::string test_device=test_uuid?test_uuid:"" NEXUS_SECONDARY_GPU_ID "";
 VulkanWalkEngine resident(test_device,argv[1],argv[2],tuning?64:16,logical);
 const auto upload_start=std::chrono::steady_clock::now();
 unsigned upload_checks=0;
 require(!resident.load_resident([&](auto first,auto bytes){model.read_tiles_into(first,bytes);},
     [&]{return ++upload_checks>8;}),"Upload did not yield before taking nonce work");
 require(resident.load_resident([&](auto first,auto bytes){model.read_tiles_into(first,bytes);}),"Upload did not resume");
 const auto upload_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-upload_start).count();
 VulkanWalkBatch output;std::array<std::uint64_t,1> seeds{seed};
 require(resident.run(logical,seeds,{}, {},true,output),"Resident dispatch cancelled");
 require(trace.size()==28+257*1024+256*8&&le(trace,4,8)==seed&&le(trace,12,8)==nonce,"Invalid expected trace");
 require(output.diagnostic_states.size()==257*1024&&output.offsets.size()==256,"Wrong witness dimensions");
 require(std::equal(output.diagnostic_states.begin(),output.diagnostic_states.end(),trace.begin()+28),"Resident states differ from official trace");
 bool low=false,high=false,above4g=false;
 for(unsigned i=0;i<256;++i){require(output.offsets[i]==le(trace,28+257*1024+i*8,8),"Resident offset mismatch");
  low|=output.offsets[i]<resident.resident_split_tiles();high|=output.offsets[i]>=resident.resident_split_tiles();above4g|=output.offsets[i]>=4194304;}
 require(low&&high&&above4g,"Reference did not exercise both banks and >4 GiB canonical addresses");
 Walk walk;walk.offsets=output.offsets;walk.states.resize(257);
 for(unsigned i=0;i<257;++i)std::copy_n(output.diagnostic_states.data()+i*1024,1024,walk.states[i].data());
 auto gather=gather_v4_model_tiles(seed,logical,[&](auto off){std::array<std::uint8_t,1024> t{};model.read_tile_into(off,t);return t;});
 keryx::nexus::model::SparseMerkleIndex merkle(model,argv[4]);Hash pph{};std::copy_n(ctx.data()+85,32,pph.data());
 auto final=final_state(walk);auto pow=pom_pow_value_h3(final,pph);
 auto proof=prove_v4_from_walk(walk,ctx[20],gather.tiles,[&](auto off){return merkle.tile_path(off);});
 auto wire=borsh_v4_container(proof,final,pow);
 require(std::equal(wire.begin(),wire.end(),ctx.begin()+225,ctx.end()),"Proof differs from official Borsh bytes");
 const fs::path out=argv[7];fs::create_directories(out);save(out/"resident.proof",wire);
 std::ifstream module(argv[8]);std::string gathered_path,gathered_sha;std::getline(module,gathered_path);std::getline(module,gathered_sha);
 VulkanWalkEngine gathered(test_device,gathered_path,gathered_sha,16);
 double gather_ms=0,gathered_total=0,resident_total=0;
 if(tuning) {
  // Identical distinct nonce sequence for each batch size; rotate test order.
  // One resident allocation, unchanged arithmetic, bounded cancellation drain.
  constexpr unsigned total=192;
  std::vector<std::uint64_t> inputs(total);
  for(unsigned i=0;i<total;++i)inputs[i]=block_seed(pph,le(ctx,117,8),nonce+i,le(ctx,133,8));
  Bytes reference_states;std::vector<std::uint64_t> reference_offsets;
  for(unsigned round=0;round<3;++round)for(unsigned choice=0;choice<3;++choice) {
   const unsigned size=std::array<unsigned,3>{16,32,64}[(round+choice)%3];
   Bytes states;std::vector<std::uint64_t> offsets;double elapsed_ms=0,max_batch_ms=0;
   for(unsigned first=0;first<total;first+=size) {
    VulkanWalkBatch result;const auto start=std::chrono::steady_clock::now();
    require(resident.run(logical,std::span(inputs).subspan(first,size),{}, {},false,result),"Tuning replay cancelled");
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    elapsed_ms+=ms;max_batch_ms=std::max(max_batch_ms,ms);
    states.insert(states.end(),result.final_states.begin(),result.final_states.end());
    offsets.insert(offsets.end(),result.offsets.begin(),result.offsets.end());
   }
   if(reference_states.empty()){reference_states=states;reference_offsets=offsets;}
   else require(states==reference_states&&offsets==reference_offsets,"Batch size changed canonical output");
   bool cancel=false;VulkanWalkBatch discarded;
   const auto start=std::chrono::steady_clock::now();
   require(!resident.run(logical,std::span(inputs).first(size),{}, {},false,discarded,[&]{return cancel;},[&]{cancel=true;}),"Tuning cancelled output escaped");
   const auto drain_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   std::cout<<"BATCH_TUNING round="<<round<<" batch="<<size<<" nonces="<<total
    <<" elapsed_ms="<<elapsed_ms<<" nonce_s="<<total*1000.0/elapsed_ms
    <<" max_batch_ms="<<max_batch_ms<<" cancel_drain_ms="<<drain_ms<<" byte_parity=PASS\n"<<std::flush;
  }
 }
 for(unsigned run=0;!tuning&&run<3;++run){
  std::vector<std::uint64_t> batch;Bytes tiles;std::vector<std::uint32_t> slots;
  const auto begin=std::chrono::steady_clock::now();
  for(unsigned i=0;i<16;++i){auto n=nonce+run*16+i;batch.push_back(block_seed(pph,le(ctx,117,8),n,le(ctx,133,8)));
   auto input=gather_v4_model_tiles(batch.back(),logical,[&](auto off){std::array<std::uint8_t,1024> t{};model.read_tile_into(off,t);return t;});
   tiles.insert(tiles.end(),input.tiles.begin(),input.tiles.end());for(unsigned j=0;j<256;++j)slots.push_back(i*256+j);}
  gather_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
  VulkanWalkBatch baseline,candidate;
  require(gathered.run(logical,batch,tiles,slots,false,baseline),"Gathered replay cancelled");
  gathered_total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
  const auto start=std::chrono::steady_clock::now();require(resident.run(logical,batch,{}, {},false,candidate),"Resident replay cancelled");
  resident_total+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  require(baseline.offsets==candidate.offsets&&baseline.final_states==candidate.final_states,"Batch screening output mismatch");
 }
 const auto before=resident.stats();
 require(!resident.run(logical,seeds,{}, {},false,output,[]{return true;}),"Pre-cancelled dispatch admitted");
 require(resident.stats().submitted==before.submitted,"Pre-cancel reached GPU");
 bool cancel=false;require(!resident.run(logical,seeds,{}, {},false,output,[&]{return cancel;},[&]{cancel=true;}),"Post-enqueue cancellation leaked output");
 require(resident.run(logical,seeds,{}, {},true,output),"Resident resume failed");
 std::cout<<"BANKED_RESIDENT=PASS canonical_tiles="<<logical<<" split="<<resident.resident_split_tiles()
  <<" upload_ms="<<upload_ms<<" proof_bytes="<<wire.size()<<" byte_identical=true both_banks=true above4GiB=true"
  <<" tuning="<<(tuning?"true":"false")<<" replay_nonces="<<(tuning?1728:48)<<" gather_ms="<<gather_ms<<" gathered_total_ms="<<gathered_total<<" resident_total_ms="<<resident_total
  <<" batch_parity=true upload_pause_resume=true cancellation=true resume=true mainnet_transactions=0\n";
}catch(const std::exception&e){std::cerr<<"BANKED_RESIDENT=FAIL "<<e.what()<<'\n';return 1;}}
