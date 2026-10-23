// Isolated experiments: real CUDA search and canonical recorded proof verification.
// There is no node client, signing key or submission endpoint in this replay.
#include "keryx/nexus/consensus/cuda_resident.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include "keryx/nexus/scoped_file_cache_guard.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <numeric>
using namespace keryx::nexus;
using Clock=std::chrono::steady_clock;
static volatile std::sig_atomic_t stopped=0;
static void cancel(int){stopped=1;}
static double ms(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
static auto read(const char *p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing fixture");return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),{});}
static std::uint64_t le(const std::uint8_t *p){std::uint64_t x=0;for(unsigned i=0;i<8;++i)x|=std::uint64_t(p[i])<<(8*i);return x;}
static void check_stop(){if(stopped)throw std::runtime_error("cooperatively cancelled");}
int main(int argc,char **argv){try{
 if(argc!=9)throw std::runtime_error("MODEL LLAMA CONTEXT TRACE PROOF ORACLE UUID OUTPUT required");
 std::signal(SIGINT,cancel);std::signal(SIGTERM,cancel);
 auto context=read(argv[3]),trace=read(argv[4]),proof=read(argv[5]);
 if(context.size()<225||trace.size()!=28+257*1024+256*8)throw std::runtime_error("fixture shape");
 RustProofVerifier oracle(argv[6]);
 std::cout<<oracle.verify(std::span(context).first(225),trace,proof).summary<<'\n'<<std::flush;
 ScopedFileCacheGuard guard;
 model::ReadOnlyGguf gguf(argv[1]);
 opoi::StockLlamaRuntime runtime(argv[2],argv[1]);
 if(runtime.abi_version()!=3)throw std::runtime_error("stock ABI changed");
 consensus::CudaResidentSearch engine(argv[7],gguf,runtime.resident_tensors());
 consensus::Hash pph{},zero{};std::copy_n(context.data()+85,32,pph.data());
 const auto timestamp=le(context.data()+117),nonce=le(context.data()+125),daa=le(context.data()+133);
 const auto tile_count=gguf.chunk_count()/32;
 const bool confirm=std::getenv("NEXUS_EXPERIMENT_CONFIRM")!=nullptr;
 std::ofstream out(argv[8]);if(!out)throw std::runtime_error("result output unavailable");
 // 1. Bounded (one future) CPU verification overlap. Stress cadence is deliberately
 // higher than production: results must be scaled by the observed witness frequency.
 std::vector<std::uint8_t> proof_tiles;
 std::vector<std::vector<consensus::Hash>> proof_paths;
 std::size_t cursor=93;
 auto u32=[&](std::size_t at){if(at+4>proof.size())throw std::runtime_error("truncated proof");std::uint32_t x=0;for(unsigned i=0;i<4;++i)x|=std::uint32_t(proof[at+i])<<(8*i);return x;};
 for(unsigned i=0;i<256;++i){if(u32(cursor)!=1024||cursor+1028>proof.size())throw std::runtime_error("tile size");proof_tiles.insert(proof_tiles.end(),proof.begin()+cursor+4,proof.begin()+cursor+1028);cursor+=1028;}
 if(u32(cursor)!=256)throw std::runtime_error("path count");cursor+=4;
 for(unsigned i=0;i<256;++i){auto count=u32(cursor);cursor+=4;if(count>(proof.size()-cursor)/32)throw std::runtime_error("path size");std::vector<consensus::Hash> path(count);for(auto &h:path){std::copy_n(proof.data()+cursor,32,h.data());cursor+=32;}proof_paths.push_back(std::move(path));}
 if(cursor!=proof.size())throw std::runtime_error("trailing proof");
 auto verify=[&]{
  const auto walk=consensus::walk_v4_from_tile_sequence(le(context.data()+4),tile_count,proof_tiles);
  if(walk.states.size()!=257||std::memcmp(walk.states.data(),trace.data()+28,257*1024))throw std::runtime_error("recorded walk parity");
  unsigned index=0;const auto assembled=consensus::prove_v4_from_walk(walk,context[20],proof_tiles,[&](auto off){if(index>=256||off!=le(trace.data()+28+257*1024+index*8))throw std::runtime_error("path offset");return proof_paths[index++];});
  consensus::Hash pow{};std::copy_n(context.data()+189,32,pow.data());
  const auto wire=consensus::borsh_v4_container(assembled,le(context.data()+181),pow);
  if(wire!=proof)throw std::runtime_error("proof byte parity");
  return oracle.verify(std::span(context).first(225),trace,wire);
 };
 for(unsigned round=0;round<(confirm?0U:6U);++round)for(unsigned pos=0;pos<2;++pos){
  check_stop();const bool overlap=(round+pos)%2;auto start=Clock::now();
  auto future=overlap?std::async(std::launch::async,verify):decltype(std::async(std::launch::async,verify)){};
  if(!overlap)(void)verify();
  double gpu_ms=0;
  for(unsigned i=0;i<64;++i){check_stop();auto result=engine.run(pph,timestamp+round,daa,zero,nonce+1000000+i*4608,4608);if(result.winner)throw std::runtime_error("unexpected zero-target winner");gpu_ms+=result.kernel_ms;}
  if(overlap)(void)future.get();
  out<<"{\"experiment\":\"async\",\"round\":"<<round<<",\"candidate\":"<<overlap<<",\"wall_ms\":"<<ms(start,Clock::now())<<",\"gpu_ms\":"<<gpu_ms<<",\"completed\":"<<64*4608<<"}\n"<<std::flush;
  guard.check();
 }
 // 2. Identical kernels, offsets and DMA; only host destination allocation differs.
 consensus::ResidentWitnessTiles staging;
 for(unsigned round=0;round<(confirm?0U:6U);++round)for(unsigned pos=0;pos<2;++pos){
  check_stop();const bool reuse=(round+pos)%2;const auto start=Clock::now();
  for(unsigned i=0;i<32;++i){
   check_stop();const auto seed=consensus::block_seed(pph,timestamp,nonce+i,daa);
   consensus::ResidentWitnessTiles fresh;
   if(reuse)engine.read_witness_tiles_into(seed,staging);else fresh=engine.read_witness_tiles(seed);
   const auto &w=reuse?staging:fresh;
   if(w.pairs.size()!=256)throw std::runtime_error("witness shape");
   if(round==0){unsigned step=0;auto input=consensus::gather_v4_model_tiles(seed,tile_count,[&](auto offset){if(w.offsets[step]!=offset)throw std::runtime_error("witness CPU offsets");return w.pairs[step++][0];});
    auto walk=consensus::walk_v4_from_tile_sequence(seed,tile_count,input.tiles);
    if(walk.offsets!=input.offsets)throw std::runtime_error("full walk offsets");
   }
  }
  out<<"{\"experiment\":\"reuse\",\"round\":"<<round<<",\"candidate\":"<<reuse<<",\"wall_ms\":"<<ms(start,Clock::now())<<",\"witnesses\":32}\n"<<std::flush;
  guard.check();
 }
 // 3. Nearby static sizes and bounded adaptive policy. All run the same nonce
 // interval per epoch. Per-batch timings support a separate replay against the
 // same recorded invalidation times. No node or freshness gate is changed.
 constexpr std::array<unsigned,4> sizes{3456,4608,5760,6912};
 constexpr std::uint64_t budget=331776;
 for(auto size:sizes)for(unsigned i=0;i<12;++i)(void)engine.run(pph,timestamp,daa,zero,nonce+i*size,size);
 for(unsigned round=0;round<(confirm?24U:8U);++round)for(unsigned pos=0;pos<(confirm?3U:5U);++pos){
  check_stop();const unsigned orders[6][3]={{0,1,4},{1,4,0},{4,0,1},{4,1,0},{1,0,4},{0,4,1}};
  unsigned mode=confirm?orders[round%6][pos]:(round%2?4-pos:pos);unsigned size=mode==4?4608:sizes[mode];
  std::uint64_t offset=0;double gpu_ms=0,max_ms=0;std::vector<double> durations;
  out<<"{\"experiment\":\"batch_begin\",\"round\":"<<round<<",\"mode\":"<<mode<<"}\n";
  const auto start=Clock::now();
  while(offset<budget){
   check_stop();const auto count=std::min<std::uint64_t>(size,budget-offset);auto begin=Clock::now();
   const auto r=engine.run(pph,timestamp+round,daa,zero,nonce+2000000+round*budget+offset,count);
   auto elapsed=ms(begin,Clock::now());if(r.completed!=count||r.winner)throw std::runtime_error("search accounting");
   out<<"{\"experiment\":\"batch_sample\",\"round\":"<<round<<",\"mode\":"<<mode<<",\"count\":"<<count<<",\"wall_ms\":"<<elapsed<<",\"end_ms\":"<<ms(start,Clock::now())<<"}\n";
   gpu_ms+=r.kernel_ms;max_ms=std::max(max_ms,elapsed);durations.push_back(elapsed);offset+=r.completed;
   // Target 3ms admission interval, bounded and no additional speculative work.
   if(mode==4)size=std::clamp<unsigned>(unsigned(count*3.0/std::max(0.1,elapsed))/1152*1152,3456,6912);
  }
  const auto wall=ms(start,Clock::now());std::sort(durations.begin(),durations.end());
  out<<"{\"experiment\":\"batch\",\"round\":"<<round<<",\"mode\":"<<mode<<",\"batch\":"<<(mode==4?0:sizes[mode])<<",\"wall_ms\":"<<wall<<",\"gpu_ms\":"<<gpu_ms<<",\"completed\":"<<offset<<",\"attempts_s\":"<<offset*1000./wall<<",\"batch_p95_ms\":"<<durations[(durations.size()*95-1)/100]<<",\"batch_max_ms\":"<<max_ms<<"}\n"<<std::flush;
  guard.check();
 }
 std::cout<<"WITNESS_BATCH_REPLAY=PASS mock_submission=false no_submission=true\n"<<std::flush;
 return 0;
}catch(const std::exception &e){std::cerr<<"WITNESS_BATCH_REPLAY=FAIL "<<e.what()<<'\n';return 1;}}
