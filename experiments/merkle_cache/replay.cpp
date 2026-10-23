// Distinct-witness replay; no GPU, inference, keys or node client.
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace keryx::nexus;
static std::vector<std::uint8_t> read(const std::filesystem::path &p) {
  std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing witness");
  return {std::istreambuf_iterator<char>(f),{}};
}
static std::uint64_t le(std::span<const std::uint8_t> b,std::size_t pos,unsigned n) {
  if(pos>b.size()||n>b.size()-pos)throw std::runtime_error("truncated witness");
  std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(b[pos+i])<<(i*8);return v;
}
struct Fixture {
  std::vector<std::uint8_t> context,trace,wire,tiles;
  consensus::Walk walk;
  std::vector<model::SparseMerkleIndex::TileRootInput> siblings;
};
int main(int argc,char **argv) {try {
  if(argc!=7)throw std::runtime_error("MODEL TREE PREFIX_LIST ORACLE OUTPUT READY required");
  model::ReadOnlyGguf gguf(argv[1]);
  std::vector<Fixture> fixtures;std::ifstream list(argv[3]);std::string prefix;
  while(std::getline(list,prefix)) {
    if(prefix.empty())continue;if(fixtures.size()>=32)throw std::runtime_error("fixture limit exceeded");
    Fixture f;f.context=read(prefix+".context");f.trace=read(prefix+".trace");f.wire=read(prefix+".proof");
    if(f.trace.size()!=28+257*1024+256*8||f.context.size()!=225+f.wire.size()||le(f.context,12,8)!=gguf.chunk_count())throw std::runtime_error("fixture dimensions/model mismatch");
    f.walk.states.resize(257);f.walk.offsets.resize(256);std::memcpy(f.walk.states.data(),f.trace.data()+28,257*1024);
    for(unsigned i=0;i<256;++i){
      const auto off=le(f.trace,28+257*1024+i*8,8);f.walk.offsets[i]=off;
      const auto cursor=93+i*1028;if(le(f.wire,cursor,4)!=1024||cursor+1028>f.wire.size())throw std::runtime_error("tile truncation");
      f.tiles.insert(f.tiles.end(),f.wire.begin()+cursor+4,f.wire.begin()+cursor+1028);
      const auto other=(off^1)<gguf.chunk_count()/32?(off^1):off;
      std::array<std::uint8_t,1024> bytes{};gguf.read_tile_into(other,bytes);f.siblings.push_back({other,bytes});
    }
    fixtures.push_back(std::move(f));
  }
  if(fixtures.size()!=32)throw std::runtime_error("exactly 32 distinct recorded witnesses required");
  std::ofstream(argv[6])<<"FIXTURES_LOADED\n";
  std::ofstream output(argv[5]);if(!output)throw std::runtime_error("output unavailable");
  RustProofVerifier oracle(argv[4]);using Clock=std::chrono::steady_clock;
  for(unsigned round=0;round<3;++round){
    // Reset cache for each pass. No duplicate witness is timed within a pass.
    model::SparseMerkleIndex merkle(gguf,argv[2]);
    for(unsigned i=0;i<fixtures.size();++i){const auto &f=fixtures[i];
      merkle.cache_tile_roots(f.siblings);for(auto off:f.walk.offsets)merkle.prefetch_tile_path(off);
      double path_ms=0;const auto before=merkle.path_cache_stats();
      const auto proof=consensus::prove_v4_from_walk(f.walk,f.context[20],f.tiles,[&](auto off){
        auto start=Clock::now();auto path=merkle.tile_path(off);path_ms+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();return path;});
      const auto after=merkle.path_cache_stats();if(after.entries>16384)throw std::runtime_error("cache exceeded bound");
      consensus::Hash pow{};std::copy_n(f.context.begin()+189,32,pow.begin());
      const auto wire=consensus::borsh_v4_container(proof,le(f.context,181,8),pow);
      if(wire!=f.wire)throw std::runtime_error("Merkle path or Borsh byte mismatch");
      if(round==0)(void)oracle.verify(std::span(f.context).first(225),f.trace,wire);
      output<<"{\"round\":"<<round<<",\"fixture\":"<<i<<",\"paths_ms\":"<<path_ms<<",\"hits\":"<<after.hits-before.hits
        <<",\"misses\":"<<after.misses-before.misses<<",\"entries\":"<<after.entries<<"}\n"<<std::flush;
    }
  }
  std::cout<<"MERKLE_DISTINCT_REPLAY=PASS fixtures=32 passes=3 byte_identical=true rust_first_pass=32 no_cuda=true\n";
  return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
