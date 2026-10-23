// Offline real GGUF replay only. No RPC, CUDA, wallet or submission transport.
#include "keryx/nexus/gguf_reader.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
namespace fs = std::filesystem;
using namespace keryx::nexus::consensus;
using namespace keryx::nexus::model;
using Bytes = std::vector<std::uint8_t>;
static Bytes read(const fs::path& p) {
 std::ifstream f(p,std::ios::binary|std::ios::ate);
 if(!f || f.tellg()<0 || f.tellg()>8*1024*1024) throw std::runtime_error("Invalid bounded replay artifact");
 f.seekg(0);return Bytes(std::istreambuf_iterator<char>(f),{});
}
static void write(const fs::path& p,const Bytes& b) {
 if(fs::exists(p)) throw std::runtime_error("Refusing to overwrite replay artifact");
 std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());
 if(!f) throw std::runtime_error("Cannot persist replay artifact");
}
static std::uint64_t le(const Bytes& b,std::size_t pos,unsigned width) {
 if(pos>b.size() || width>b.size()-pos) throw std::runtime_error("Truncated replay artifact");
 std::uint64_t v=0;for(unsigned i=0;i<width;++i)v|=std::uint64_t(b[pos+i])<<(8*i);return v;
}
static void number(Bytes& b,std::uint64_t n,unsigned width) {for(unsigned i=0;i<width;++i)b.push_back(n>>(8*i));}
static Hash hash(const Bytes& b,std::size_t off) {
 if(off>b.size() || b.size()-off<32) throw std::runtime_error("Truncated hash");
 Hash out{};std::copy_n(b.data()+off,32,out.data());return out;
}
int main(int argc,char** argv) {
 try {
  if(argc<2)throw std::runtime_error("Expected prepare or complete");
  const std::string mode=argv[1];
  if((mode!="prepare" && mode!="complete") || argc!=(mode=="prepare"?7:8))
   throw std::runtime_error("Usage: real-model-probe prepare MODEL TREE META CONTEXT OUTDIR | complete MODEL TREE META CONTEXT GPU_OUTPUT OUTDIR");
  ReadOnlyGguf model(argv[2]);const auto ctx=read(argv[5]);
  if(ctx.size()<225 || std::memcmp(ctx.data(),"NXRF",4) || ctx.size()!=225+le(ctx,221,4))
   throw std::runtime_error("Invalid recorded mainnet context");
  const auto seed=le(ctx,4,8),chunks=le(ctx,12,8),time=le(ctx,117,8),nonce=le(ctx,125,8),daa=le(ctx,133,8);
  const auto tier=ctx[20];const auto root=hash(ctx,21),target=hash(ctx,53),pph=hash(ctx,85);
  if(daa<h14_activation_daa || le(ctx,141,8)==0 || block_seed(pph,time,nonce,daa)!=seed)
   throw std::runtime_error("Recorded current-era seed/nonce binding invalid");
  if(std::all_of(target.begin(),target.end(),[](auto b){return b==0;}))throw std::runtime_error("Zero recorded target");
  const auto anchor=official_tier_anchor(daa,tier);
  if(anchor.root!=root || anchor.chunks!=chunks || chunks!=model.chunk_count() || chunks%32)
   throw std::runtime_error("Recorded model anchor differs from installed GGUF");
  const Hash model_id{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
  model.verify_sidecar(argv[3],argv[4],model_id,root,chunks);
  const fs::path outdir=argv[argc-1];fs::create_directories(outdir);
  if(mode=="prepare") {
   const auto gathered=gather_v4_model_tiles(seed,chunks/32,[&](std::uint64_t off){
    std::array<std::uint8_t,1024> tile{};model.read_tile_into(off,tile);return tile;});
   Bytes input{'N','X','W','G','1',0,0,0};number(input,1,4);number(input,1,4);number(input,chunks/32,8);number(input,256,4);number(input,seed,8);
   input.insert(input.end(),gathered.tiles.begin(),gathered.tiles.end());
   for(unsigned i=0;i<256;++i)number(input,i,4);
   write(outdir/"model.input",input);
   std::cout<<"GGUF_GATHER=PASS model_chunks="<<chunks<<" logical_tiles="<<chunks/32<<" uploaded_bytes="<<gathered.tiles.size()<<" seed_binding=PASS submission=NOT_LINKED\n";
   return 0;
  }
  const auto gpu=read(argv[6]);constexpr std::size_t traces=8+2048+16+1024+16;
  if(gpu.size()!=traces+257*1024+16 || std::memcmp(gpu.data(),"NXWO1\0\0\0",8))throw std::runtime_error("Invalid single-nonce Vulkan output");
  for(auto guard:std::array<std::size_t,3>{8+2048,traces-16,traces+257*1024})for(unsigned i=0;i<16;++i)
   if(gpu[guard+i]!=0xcd)throw std::runtime_error("GPU output guard damaged");
  Walk walk;walk.offsets.resize(256);walk.states.resize(257);
  for(unsigned i=0;i<256;++i){walk.offsets[i]=le(gpu,8+i*8,8);if(walk.offsets[i]>=chunks/32)throw std::runtime_error("GPU tile offset outside GGUF");}
  for(unsigned i=0;i<257;++i)std::copy_n(gpu.data()+traces+i*1024,1024,walk.states[i].data());
  if(std::memcmp(walk.states.back().data(),gpu.data()+8+2048+16,1024))throw std::runtime_error("GPU final state and trace differ");
  Bytes tiles;tiles.reserve(256*1024);
  for(auto off:walk.offsets){const auto tile=model.read_tile(off);tiles.insert(tiles.end(),tile.begin(),tile.end());}
  SparseMerkleIndex merkle(model,argv[3]);
  const auto proof=prove_v4_from_walk(walk,tier,tiles,[&](std::uint64_t off){return merkle.tile_path(off);});
  const auto state=final_state(walk);const auto pow=pom_pow_value_h3(state,pph);
  const auto wire=borsh_v4_container(proof,state,pow);
  if(state!=le(ctx,181,8) || pow!=hash(ctx,189) || wire.size()!=le(ctx,221,4) ||
     !std::equal(wire.begin(),wire.end(),ctx.begin()+225))throw std::runtime_error("Vulkan candidate differs from recorded verified proof");
  Bytes trace{'N','X','S','T'};number(trace,seed,8);number(trace,nonce,8);number(trace,257,4);number(trace,256,4);
  for(const auto& row:walk.states)trace.insert(trace.end(),row.begin(),row.end());
  for(auto off:walk.offsets)number(trace,off,8);
  write(outdir/"amd.proof",wire);write(outdir/"amd.trace",trace);write(outdir/"amd.context",ctx);
  std::cout<<"GGUF_VULKAN_PROOF=PASS Borsh=BYTE_IDENTICAL proof_bytes="<<wire.size()<<" target_satisfied="<<little_endian_leq(pow,target)<<" replay=true submission=NOT_LINKED\n";
 }catch(const std::exception& e){std::cerr<<"GGUF_REPLAY=FAIL "<<e.what()<<'\n';return 1;}
}
