// Deterministic CPU critical-path replay. No CUDA, RPC, wallet or submission.
#include "keryx/nexus/gguf_reader.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <iomanip>
#include <algorithm>
using namespace keryx::nexus;
using namespace consensus;
using Bytes=std::vector<std::uint8_t>;
static std::uint64_t get64(const Bytes& b,std::size_t pos){std::uint64_t v=0;for(unsigned i=0;i<8;i++)v|=std::uint64_t(b.at(pos+i))<<(8*i);return v;}
static void set64(Bytes& b,std::size_t pos,std::uint64_t v){for(unsigned i=0;i<8;i++)b.at(pos+i)=std::uint8_t(v>>(8*i));}
static void append64(Bytes& b,std::uint64_t v){for(unsigned i=0;i<8;i++)b.push_back(std::uint8_t(v>>(8*i)));}
static void append32(Bytes& b,std::uint32_t v){for(unsigned i=0;i<4;i++)b.push_back(std::uint8_t(v>>(8*i)));}
static void save(const std::string& p,const Bytes& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());if(!f)throw std::runtime_error("replay output failed");}
static double ms(std::chrono::steady_clock::time_point a,std::chrono::steady_clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
int main(int argc,char**argv){try{
 if(argc!=6&&argc!=7)throw std::runtime_error("usage: MODEL TREE CAPTURE_CONTEXT OUT_PREFIX COUNT(1..32) [PREFETCH_UPPER(0|1)]");
 const bool optimized=argc==7&&std::string(argv[6])=="1";
 const auto n=std::stoul(argv[5]);if(n==0||n>32)throw std::runtime_error("bounded replay count exceeded");
 std::ifstream f(argv[3],std::ios::binary);Bytes ctx((std::istreambuf_iterator<char>(f)),{});
 if(ctx.size()<225||std::string(ctx.begin(),ctx.begin()+4)!="NXRF")throw std::runtime_error("invalid captured context");
 Hash pph{},target{};std::copy_n(ctx.data()+85,32,pph.data());std::copy_n(ctx.data()+53,32,target.data());
 const auto timestamp=get64(ctx,117),nonce0=get64(ctx,125),daa=get64(ctx,133);const auto tier=ctx[20];
 model::ReadOnlyGguf g(argv[1]);model::SparseMerkleIndex merkle(g,argv[2],optimized);
 if(get64(ctx,12)!=g.chunk_count())throw std::runtime_error("captured model mismatch");
 for(std::size_t i=0;i<n;i++){
  const auto nonce=nonce0+i,seed=block_seed(pph,timestamp,nonce,daa);const auto start=std::chrono::steady_clock::now();
  auto input=gather_v4_model_tiles(seed,g.chunk_count()/32,[&](std::uint64_t off){std::array<std::uint8_t,tile_bytes> tile{};g.read_tile_into(off,tile);if(optimized)merkle.prefetch_tile_path(off);return tile;});
  const auto gathered=std::chrono::steady_clock::now();auto walk=walk_v4_from_tile_sequence(seed,g.chunk_count()/32,input.tiles);
  if(walk.offsets!=input.offsets)throw std::runtime_error("replay offset disagreement");
  const auto walked=std::chrono::steady_clock::now();auto proof=prove_v4_from_walk(walk,tier,input.tiles,[&](std::uint64_t off){return merkle.tile_path(off);});
  const auto proved=std::chrono::steady_clock::now();const auto final=final_state(walk);const auto pow=pom_pow_value_h3(final,pph);auto wire=borsh_v4_container(proof,final,pow);
  const auto serialized=std::chrono::steady_clock::now();Bytes c(ctx.begin(),ctx.begin()+181);set64(c,4,seed);set64(c,125,nonce);append64(c,final);c.insert(c.end(),pow.begin(),pow.end());append32(c,wire.size());c.insert(c.end(),wire.begin(),wire.end());
  Bytes trace{'N','X','S','T'};append64(trace,seed);append64(trace,nonce);append32(trace,257);append32(trace,256);for(const auto& state:walk.states)trace.insert(trace.end(),state.begin(),state.end());for(auto offset:walk.offsets)append64(trace,offset);
  const auto prefix=std::string(argv[4])+"-"+std::to_string(i);save(prefix+".context",c);save(prefix+".trace",trace);save(prefix+".proof",wire);
  const auto digest=blake3_hash(wire);std::cout<<"{\"index\":"<<i<<",\"gather_ms\":"<<ms(start,gathered)<<",\"walk_ms\":"<<ms(gathered,walked)<<",\"merkle_ms\":"<<ms(walked,proved)<<",\"serialize_ms\":"<<ms(proved,serialized)<<",\"total_ms\":"<<ms(start,serialized)<<",\"target_satisfied\":"<<(little_endian_leq(pow,target)?"true":"false")<<",\"proof_blake3\":\"";
  for(auto b:digest)std::cout<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);std::cout<<std::dec<<"\"}\n";
 }
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
