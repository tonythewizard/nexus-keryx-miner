// Offline bridge: actual Vulkan walk -> existing canonical Borsh proof builder.
// No CUDA, RPC, wallet, model file, target mutation or submission transport.
#include "keryx/nexus/consensus/pom_v4.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace keryx::nexus::consensus;
using Bytes = std::vector<std::uint8_t>;
static Bytes read(const char* name) {
  std::ifstream f(name, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0 || f.tellg() > 8 * 1024 * 1024)
    throw std::runtime_error("Invalid bounded offline fixture size");
  f.seekg(0);
  return Bytes(std::istreambuf_iterator<char>(f), {});
}
static std::uint64_t le(const Bytes& b, std::size_t pos, unsigned width) {
  if (pos > b.size() || width > b.size() - pos) throw std::runtime_error("Truncated fixture");
  std::uint64_t v=0; for(unsigned i=0;i<width;++i) v|=std::uint64_t(b[pos+i])<<(i*8); return v;
}
int main(int argc,char** argv) {
 try {
  if(argc!=4) throw std::runtime_error("Usage: proof-probe OFFICIAL_NXV4 GPU_TWO_NONCE_OUTPUT OUTPUT_BORSH");
  const auto reference=read(argv[1]), gpu=read(argv[2]);
  constexpr std::size_t states_bytes=257*1024, offsets_bytes=256*8;
  constexpr std::size_t header=4+8+8+1+32+8, trace=8+2*offsets_bytes+16+2*1024+16;
  if(reference.size()<header+offsets_bytes+states_bytes+4 || std::memcmp(reference.data(),"NXV4",4))
    throw std::runtime_error("Invalid official vector");
  if(gpu.size()!=trace+2*states_bytes+16 || std::memcmp(gpu.data(),"NXWO1\0\0\0",8))
    throw std::runtime_error("Invalid GPU witness dimensions");
  for(auto guard: {8+2*offsets_bytes,trace-16,trace+2*states_bytes})
    for(unsigned i=0;i<16;++i) if(gpu[guard+i]!=0xcd) throw std::runtime_error("GPU output guard corrupted");
  Walk actual;actual.states.resize(257);actual.offsets.resize(256);
  for(std::size_t i=0;i<256;++i) {
    actual.offsets[i]=le(gpu,8+i*8,8);
    if(actual.offsets[i]!=le(reference,header+i*8,8)) throw std::runtime_error("GPU offset differs from official Rust");
  }
  for(std::size_t i=0;i<257;++i) {
    std::copy_n(gpu.data()+trace+i*1024,1024,actual.states[i].data());
    if(std::memcmp(actual.states[i].data(),reference.data()+header+offsets_bytes+i*1024,1024))
      throw std::runtime_error("GPU state differs from official Rust");
  }
  if(std::memcmp(actual.states.back().data(),gpu.data()+8+2*offsets_bytes+16,1024))
    throw std::runtime_error("GPU final output and trace disagree");
  const auto seed=le(reference,4,8);const auto tier=reference[20];
  const auto blob=make_reference_test_blob();const auto paths=prove_v4(seed,tier,blob);
  Bytes ordered;ordered.reserve(256*1024);
  for(auto offset:actual.offsets) {
    if(offset>=blob.size()/1024) throw std::runtime_error("GPU tile offset outside model fixture");
    ordered.insert(ordered.end(),blob.begin()+offset*1024,blob.begin()+(offset+1)*1024);
  }
  auto proof=prove_v4_from_walk(actual,tier,ordered,[&](std::uint64_t offset) {
    const auto it=std::find(actual.offsets.begin(),actual.offsets.end(),offset);
    if(it==actual.offsets.end()) throw std::runtime_error("Missing Merkle path");
    return paths.paths[std::size_t(it-actual.offsets.begin())];
  });
  Hash pph{};pph.fill(0x42);const auto state=final_state(actual);
  if(state!=le(reference,header-8,8)) throw std::runtime_error("GPU final-state digest differs from Rust");
  const auto wire=borsh_v4_container(proof,state,pom_pow_value_h3(state,pph));
  const auto start=header+offsets_bytes+states_bytes+4;
  if(wire.size()!=le(reference,start-4,4) || reference.size()!=start+wire.size() ||
     !std::equal(wire.begin(),wire.end(),reference.begin()+start)) throw std::runtime_error("GPU-derived Borsh differs from official Rust");
  std::ofstream out(argv[3],std::ios::binary);out.write(reinterpret_cast<const char*>(wire.data()),wire.size());
  if(!out) throw std::runtime_error("Cannot save proof");
  std::cout<<"VULKAN_BORSH_PARITY=PASS proof_bytes="<<wire.size()<<" gpu_states=257 gpu_offsets=256 submission=NOT_LINKED\n";
 }catch(const std::exception& e){std::cerr<<"VULKAN_PROOF=FAIL "<<e.what()<<'\n';return 1;}
}
