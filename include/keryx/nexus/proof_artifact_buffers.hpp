#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include <stdexcept>
#include <vector>
namespace keryx::nexus {
struct ProofArtifactContext {
  std::uint64_t seed{},chunks{},timestamp{},nonce{},daa{},generation{},final{};
  std::uint8_t tier{};
  consensus::Hash root{},target{},pph{},capture{},pow{};
};
struct ProofArtifactBuffers {std::vector<std::uint8_t> context_prefix,trace;};
inline ProofArtifactBuffers encode_proof_artifacts(const ProofArtifactContext &c,
    const consensus::Walk &walk,std::size_t wire_size) {
  if(walk.states.size()!=257 || walk.offsets.size()!=256 || wire_size>2*1024*1024)
    throw std::invalid_argument("bounded official proof artifact dimensions required");
  ProofArtifactBuffers result;auto &prefix=result.context_prefix;auto &trace=result.trace;
  prefix.reserve(225);trace.reserve(28+257*1024+256*8);
  auto append=[](auto &to,const auto &from){to.insert(to.end(),from.begin(),from.end());};
  auto le=[](auto &to,std::uint64_t value,unsigned n){for(unsigned i=0;i<n;++i)to.push_back(static_cast<std::uint8_t>(value>>(8*i)));};
  prefix.insert(prefix.end(),{'N','X','R','F'});le(prefix,c.seed,8);le(prefix,c.chunks,8);prefix.push_back(c.tier);
  append(prefix,c.root);append(prefix,c.target);append(prefix,c.pph);le(prefix,c.timestamp,8);le(prefix,c.nonce,8);
  le(prefix,c.daa,8);le(prefix,c.generation,8);append(prefix,c.capture);le(prefix,c.final,8);append(prefix,c.pow);le(prefix,wire_size,4);
  trace.insert(trace.end(),{'N','X','S','T'});le(trace,c.seed,8);le(trace,c.nonce,8);le(trace,257,4);le(trace,256,4);
  static_assert(sizeof(std::array<std::uint8_t,1024>)==1024);
  const auto *bytes=reinterpret_cast<const std::uint8_t*>(walk.states.data());
  trace.insert(trace.end(),bytes,bytes+257*1024);
  for(auto offset:walk.offsets)le(trace,offset,8);
  return result;
}
}
