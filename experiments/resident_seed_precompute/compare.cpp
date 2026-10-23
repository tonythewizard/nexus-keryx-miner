#include "keryx/nexus/developer_config.hpp"
// Offline A/B: same resident model, templates, nonce ranges and target.
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include "keryx/nexus/scoped_file_cache_guard.hpp"
#include <memory>
#include <optional>
#define private public
#include "keryx/nexus/consensus/cuda_resident.hpp"
#undef private
#include "../../consensus/cuda_resident.cpp"
#include <fstream>
#include <chrono>
#include <csignal>
using namespace keryx::nexus;
volatile std::sig_atomic_t cancelled=0;
void request_cancel(int){cancelled=1;}
std::vector<std::uint8_t> read_file(const char *path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("fixture unavailable");return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){try{
 std::signal(SIGTERM,request_cancel);std::signal(SIGINT,request_cancel);
 if(argc!=10&&argc!=12)throw std::runtime_error("MODEL LLAMA CONTEXT TRACE PROOF ORACLE BASELINE_CUBIN CANDIDATE_CUBIN RESULT [WARPS2_CUBIN WARPS8_CUBIN] required");
 auto context=read_file(argv[3]),trace=read_file(argv[4]),proof=read_file(argv[5]);
 if(context.size()<225)throw std::runtime_error("context truncated");
 RustProofVerifier oracle(argv[6]);auto verified=oracle.verify(std::span(context).first(225),trace,proof);
 std::cout<<verified.summary<<'\n';
 ScopedFileCacheGuard cache_guard;
 model::ReadOnlyGguf gguf(argv[1]);opoi::StockLlamaRuntime rt(argv[2],argv[1]);
 consensus::CudaResidentSearch engine("" NEXUS_PRIMARY_GPU_UUID "",gguf,rt.resident_tensors());
 auto &p=*engine.impl_;CUmodule module{};consensus::check(cuModuleLoad(&module,argv[8]),"seed experiment module");
 CUfunction seeds_fn{},search_fn{};consensus::check(cuModuleGetFunction(&seeds_fn,module,"nexus_h10_seeds"),"seed entry");consensus::check(cuModuleGetFunction(&search_fn,module,"nexus_resident_search_preseeded"),"search entry");
 CUmodule baseline_module{};CUfunction baseline_fn{};
 consensus::check(cuModuleLoad(&baseline_module,argv[7]),"baseline module");
 consensus::check(cuModuleGetFunction(&baseline_fn,baseline_module,"nexus_resident_search"),"baseline entry");
 std::vector<CUmodule> extra_modules;
 std::vector<CUfunction> candidates{search_fn};std::vector<unsigned> candidate_warps{4};
 if(argc==12)for(unsigned j=0;j<2;++j){
  CUmodule m{};CUfunction f{};consensus::check(cuModuleLoad(&m,argv[10+j]),"geometry module");
  consensus::check(cuModuleGetFunction(&f,m,"nexus_resident_search_preseeded"),"geometry entry");
  extra_modules.push_back(m);candidates.push_back(f);candidate_warps.push_back(j==0?2:8);
 }
 consensus::Buffer seeds;seeds.allocate(18432*8);
 consensus::Hash pph{},target{};std::copy_n(context.data()+85,32,pph.data());std::copy_n(context.data()+53,32,target.data());
 const auto timestamp=consensus::le64(context.data()+117),nonce=consensus::le64(context.data()+125),daa=consensus::le64(context.data()+133);
 auto run=[&](bool precomputed,const consensus::Hash &hash,std::uint64_t time,const consensus::Hash &limit,std::uint64_t first,std::uint64_t count,unsigned variant=0){
  const auto state=consensus::seed_sponge_state(hash,time,daa);consensus::check(cuMemcpyHtoD(p.sponge.ptr,state.data(),200),"seed state");
  const std::uint64_t none=UINT64_MAX;consensus::check(cuMemcpyHtoD(p.winner.ptr,&none,8),"reset winner");
  constexpr std::uint64_t salt[4]={0x7C99D381176D4EC4ULL,0xC2E28E3E28118C36ULL,0xD496CE1B129B76CAULL,0x47CF0979FA580BCEULL};
  std::uint64_t words[4],targets[4],unused[4]{};for(unsigned i=0;i<4;++i){words[i]=consensus::le64(hash.data()+8*i)^salt[i];targets[i]=consensus::le64(limit.data()+8*i);}
  std::uint32_t k=256,h10=1,pre=1;
  void *params[]={&p.bases.ptr,&p.prefix.ptr,&p.tensor_count,&p.tiles,&k,&words[0],&words[1],&words[2],&words[3],&unused[0],&unused[1],&unused[2],&unused[3],&time,&targets[0],&targets[1],&targets[2],&targets[3],&first,&count,&p.lut.ptr,&p.shift,&p.reciprocal,&p.winner.ptr,&p.sponge.ptr,&h10,&seeds.ptr,&pre,&p.tile_folds.ptr};
  consensus::check(cuEventRecord(p.resources.begin,p.resources.stream),"begin");
  {void *args[]={&p.sponge.ptr,&first,&count,&seeds.ptr};consensus::check(cuLaunchKernel(seeds_fn,(count+127)/128,1,1,128,1,1,0,p.resources.stream,args,nullptr),"precompute seeds");}
  const unsigned warps=precomputed?candidate_warps.at(variant):4;
  consensus::check(cuLaunchKernel(precomputed?candidates.at(variant):baseline_fn,(count+warps-1)/warps,1,1,warps*32,1,1,warps*(precomputed?3104:3072),p.resources.stream,params,nullptr),"search");
  consensus::check(cuEventRecord(p.resources.end,p.resources.stream),"end");consensus::check(cuEventSynchronize(p.resources.end),"complete");
  std::uint64_t found=none;consensus::check(cuMemcpyDtoH(&found,p.winner.ptr,8),"winner");return found;
 };
 // Exact CPU H10 seeds across changed templates, times and nonce ranges.
 for(unsigned variant=0;variant<4;++variant){auto hash=pph;hash[0]^=variant;const auto time=timestamp+variant;
  (void)run(true,hash,time,target,nonce+variant*97,64);std::array<std::uint64_t,64> gpu{};consensus::check(cuMemcpyDtoH(gpu.data(),seeds.ptr,sizeof(gpu)),"seed parity");
  for(unsigned i=0;i<64;++i)if(gpu[i]!=consensus::block_seed(hash,time,nonce+variant*97+i,daa))throw std::runtime_error("H10 seed mismatch");
 }
 // Boundary target decision after the full 256-step walk, not seed-only parity.
 for(unsigned i=0;i<4;++i){const auto n=nonce+i;auto seed=consensus::block_seed(pph,timestamp,n,daa);
  auto tiles=consensus::gather_v4_model_tiles(seed,gguf.chunk_count()/32,[&](auto off){return engine.read_tile(off);});
  auto walk=consensus::walk_v4_from_tile_sequence(seed,gguf.chunk_count()/32,tiles.tiles);auto pow=consensus::pom_pow_value_h3(consensus::final_state(walk),pph);auto below=pow;
  for(auto &v:below){if(v){--v;break;}v=255;}
  for(unsigned variant=0;variant<candidates.size();++variant)for(bool pre:{false,true})if(run(pre,pph,timestamp,pow,n,1,variant)!=n||run(pre,pph,timestamp,below,n,1,variant)!=UINT64_MAX)throw std::runtime_error("full walk/target parity failed");
 }
 std::cout<<"SEED_PARITY=PASS cases=256 FULL_WALK_TARGET_PARITY=PASS nonces=4 variants=2\n"<<std::flush;
 for(unsigned i=0;i<128&&!cancelled;++i)(void)run(i%2,pph,timestamp,target,nonce+1000000+i*4608,4608);
 std::ofstream result(argv[9]);
 for(unsigned variant=0;variant<candidates.size();++variant){
 for(unsigned i=0;i<64&&!cancelled;++i)(void)run(i%2,pph,timestamp,target,nonce+1000000+i*4608,4608,variant);
 for(unsigned trial=0;trial<8&&!cancelled;++trial)for(unsigned v=0;v<2;++v){const bool pre=(trial+v)%2;
  std::array<std::uint64_t,64> winners{};const auto start=std::chrono::steady_clock::now();
  for(unsigned j=0;j<64&&!cancelled;++j)winners[j]=run(pre,pph,timestamp,target,nonce+2000000+(trial*64+j)*4608,4608,variant);
  const auto wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  result<<"{\"warps\":"<<candidate_warps[variant]<<",\"trial\":"<<trial<<",\"candidate\":"<<(pre?"true":"false")<<",\"attempts_s\":"<<294912/wall<<",\"wall_s\":"<<wall<<"}\n"<<std::flush;
  // Atomic-min winner reduction returns the same nonce for both paths.
  static std::array<std::uint64_t,64> first_winners;
  if(v==0)first_winners=winners;else if(first_winners!=winners)throw std::runtime_error("winner/nonce disagreement in A/B replay");
 }
 }
 consensus::check(cuStreamSynchronize(p.resources.stream),"experiment drain");
 for(auto m:extra_modules)consensus::check(cuModuleUnload(m),"geometry unload");consensus::check(cuModuleUnload(baseline_module),"baseline unload");consensus::check(cuModuleUnload(module),"experiment unload");
 if(cancelled)throw std::runtime_error("experiment cooperatively cancelled");
 std::cout<<"RESIDENT_SEED_REPLAY=PASS mainnet_tx=false production_admitted=false\n";return 0;
}catch(const std::exception &e){std::cerr<<"RESIDENT_SEED_REPLAY=FAIL "<<e.what()<<'\n';return 1;}}
