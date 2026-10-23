#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace keryx::nexus;
static std::vector<std::uint8_t> read(const char *name){
  std::ifstream f(name,std::ios::binary);if(!f)throw std::runtime_error("missing proof fixture");
  return {std::istreambuf_iterator<char>(f),{}};
}
static std::uint64_t le(const std::vector<std::uint8_t>&b,std::size_t p,unsigned n){
  std::uint64_t x=0;for(unsigned i=0;i<n;++i)x|=std::uint64_t(b.at(p+i))<<(8*i);return x;
}
int main(int argc,char **argv){try{
  if(argc!=7)throw std::runtime_error("MODEL TREE CONTEXT TRACE PROOF ORACLE required");
  const auto context=read(argv[3]),trace=read(argv[4]),original=read(argv[5]);
  if(context.size()!=225+original.size()||trace.size()!=28+257*1024+256*8||
     std::string(context.begin(),context.begin()+4)!="NXRF"||
     std::string(trace.begin(),trace.begin()+4)!="NXST"||
     le(trace,20,4)!=257||le(trace,24,4)!=256||le(context,221,4)!=original.size())
    throw std::runtime_error("invalid fixture dimensions");
  model::ReadOnlyGguf gguf(argv[1]);model::SparseMerkleIndex merkle(gguf,argv[2]);
  if(gguf.chunk_count()!=le(context,12,8))throw std::runtime_error("fixture/model chunk mismatch");
  consensus::Walk walk;walk.states.resize(257);walk.offsets.resize(256);
  std::memcpy(walk.states.data(),trace.data()+28,257*1024);
  std::vector<std::uint8_t> tiles;tiles.reserve(256*1024);
  std::vector<model::SparseMerkleIndex::TileRootInput> batch;batch.reserve(256);
  const auto ntiles=gguf.chunk_count()/32;
  for(unsigned i=0;i<256;++i){
    const auto offset=le(trace,28+257*1024+i*8,8);walk.offsets[i]=offset;
    std::array<std::uint8_t,1024> tile{},sibling{};gguf.read_tile_into(offset,tile);
    const auto other=(offset^1U)<ntiles?(offset^1U):offset;gguf.read_tile_into(other,sibling);
    tiles.insert(tiles.end(),tile.begin(),tile.end());batch.push_back({other,sibling});
  }
  merkle.cache_tile_roots(batch);
  const auto proof=consensus::prove_v4_from_walk(walk,context[20],tiles,[&](auto offset){return merkle.tile_path(offset);});
  consensus::Hash pow{};std::copy_n(context.begin()+189,32,pow.begin());
  const auto wire=consensus::borsh_v4_container(proof,le(context,181,8),pow);
  if(wire!=original)throw std::runtime_error("batch-root proof differs from recorded official bytes");
  RustProofVerifier oracle(argv[6]);const auto result=oracle.verify(std::span(context).first(225),trace,wire);
  std::cout<<result.summary<<'\n'<<"MERKLE_ROOT_REAL_PROOF=PASS byte_identical=true official_rust=true no_cuda=true no_submit=true\n";
  return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
