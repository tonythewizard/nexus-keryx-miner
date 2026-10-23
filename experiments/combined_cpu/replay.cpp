// Offline integration replay. Recorded Merkle paths replace disk gathering;
// timing covers host bookkeeping, full CPU walk, proof assembly and Borsh only.
#include "keryx/nexus/gpu_work_coordinator.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace keryx::nexus;
static auto read(const std::filesystem::path &p) {
 std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing fixture");
 return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),{});
}
static std::uint64_t le(std::span<const std::uint8_t>b,std::size_t p,unsigned n){
 if(p>b.size()||n>b.size()-p)throw std::runtime_error("truncated fixture");
 std::uint64_t x=0;for(unsigned i=0;i<n;++i)x|=std::uint64_t(b[p+i])<<(8*i);return x;
}
struct Fixture {
 std::vector<std::uint8_t> context,trace,wire,tiles;
 std::vector<std::vector<consensus::Hash>> paths;
 std::vector<std::uint64_t> offsets;
 explicit Fixture(const std::filesystem::path &p):context(read(p)),trace(read(p.parent_path()/(p.stem().string()+".trace"))),wire(read(p.parent_path()/(p.stem().string()+".proof"))){
  if(context.size()!=225+wire.size()||trace.size()!=28+257*1024+256*8||le(context,221,4)!=wire.size()||le(wire,89,4)!=256||le(context,12,8)%32)throw std::runtime_error("fixture dimensions");
  std::size_t c=93;for(unsigned i=0;i<256;++i){if(le(wire,c,4)!=1024||c+1028>wire.size())throw std::runtime_error("tile bounds");tiles.insert(tiles.end(),wire.begin()+c+4,wire.begin()+c+1028);c+=1028;offsets.push_back(le(trace,28+257*1024+i*8,8));}
  if(le(wire,c,4)!=256)throw std::runtime_error("path count");c+=4;
  for(unsigned i=0;i<256;++i){auto n=le(wire,c,4);c+=4;if(n>(wire.size()-c)/32)throw std::runtime_error("path bounds");std::vector<consensus::Hash> path(n);for(auto &h:path){std::copy_n(wire.begin()+c,32,h.begin());c+=32;}paths.push_back(std::move(path));}
  if(c!=wire.size())throw std::runtime_error("trailing proof bytes");
 }
};
int main(int argc,char **argv){try{
 if(argc!=4)throw std::runtime_error("FIXTURE_DIRECTORY ORACLE OUTPUT required");
 std::vector<std::filesystem::path> names;for(const auto &e:std::filesystem::directory_iterator(argv[1]))if(e.path().extension()==".context")names.push_back(e.path());std::sort(names.begin(),names.end());if(names.size()<32)throw std::runtime_error("32 distinct fixtures required");names.resize(32);
 std::vector<Fixture> fixtures;for(const auto&p:names)fixtures.emplace_back(p);
 RustProofVerifier oracle(argv[2]);std::ofstream out(argv[3]);if(!out)throw std::runtime_error("output unavailable");
 using Clock=std::chrono::steady_clock;auto us=[](auto a,auto b){return std::chrono::duration<double,std::micro>(b-a).count();};
 unsigned verified=0;
 for(auto bytes:{4096U,65536U}){
  TemplateFields fields;fields.network="mainnet";fields.node_synced=true;fields.timestamp=123;fields.daa_score=126000000;fields.pay_address="offline";fields.extra_data="offline";fields.response_wire.assign(bytes,1);fields.block_wire.assign(bytes,2);fields.consensus_identity_wire=fields.block_wire;
  LiveJobQueue q;q.publish(fields);auto job=*q.wait_pop();GpuWorkCoordinator c(q,0,4608,256);consensus::Hash pph{};
  for(unsigned round=0;round<8;++round)for(unsigned j=0;j<fixtures.size();++j){const auto &f=fixtures[j];const auto begin=Clock::now();
   auto ticket=c.reserve(MiningLane::Nvidia,job,pph,4608);if(!ticket||c.check(*ticket)!=WorkGate::Ready||!q.submission_allowed(job.generation))throw std::runtime_error("fresh bookkeeping failed");
   const auto booked=Clock::now();const auto walk=consensus::walk_v4_from_tile_sequence(le(f.context,4,8),le(f.context,12,8)/32,f.tiles);const auto walked=Clock::now();
   unsigned path_index=0;const auto proof=consensus::prove_v4_from_walk(walk,f.context[20],f.tiles,[&](auto offset){if(path_index>=256||offset!=f.offsets[path_index])throw std::runtime_error("walk path mismatch");return f.paths[path_index++];});
   consensus::Hash pow{};std::copy_n(f.context.begin()+189,32,pow.begin());const auto wire=consensus::borsh_v4_container(proof,le(f.context,181,8),pow);
   if(!c.finish(*ticket))throw std::runtime_error("finish failed");const auto ended=Clock::now();
   if(wire!=f.wire||walk.states.size()!=257||std::memcmp(walk.states.data(),f.trace.data()+28,257*1024)||walk.offsets!=f.offsets)throw std::runtime_error("official bytes changed");
   double rust_ms=0;if(round==0){auto trace=f.trace;std::memcpy(trace.data()+28,walk.states.data(),257*1024);auto result=oracle.verify(std::span(f.context).first(225),trace,wire);rust_ms=result.cpu_ms;++verified;}
   out<<"{\"bytes\":"<<bytes<<",\"round\":"<<round<<",\"fixture\":"<<j<<",\"bookkeeping_us\":"<<us(begin,booked)<<",\"walk_us\":"<<us(booked,walked)<<",\"assembly_borsh_finish_us\":"<<us(walked,ended)<<",\"host_total_us\":"<<us(begin,ended)<<",\"rust_ms\":"<<rust_ms<<"}\n";
  }
  unsigned stale=0,ai=0;for(unsigned i=0;i<12;++i){auto t=c.reserve(MiningLane::Nvidia,job,pph,4608);if(!t)throw std::runtime_error("replay reserve failed");if(i%2==0){q.invalidate_for_virtual_chain_change();if(c.check(*t)!=WorkGate::Stale||q.submission_allowed(job.generation))throw std::runtime_error("stale admitted");++stale;c.finish(*t);fields.consensus_identity_wire[0]^=1;q.publish(fields);job=*q.wait_pop();}else{c.pause_for_ai();if(c.check(*t)!=WorkGate::AiPaused||c.resume_after_ai())throw std::runtime_error("AI drain bypass");c.finish(*t);if(!c.resume_after_ai())throw std::runtime_error("AI resume failed");++ai;}}
  std::cout<<"LOGICAL_REPLAY stale_blocked="<<stale<<" ai_drains="<<ai<<" bytes="<<bytes<<'\n';
 }
 std::cout<<"COMBINED_CPU_REPLAY=PASS distinct_fixtures=32 rust_checks="<<verified<<" no_cuda=true no_submit=true recorded_paths=true\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
