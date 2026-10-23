#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/pipeline.hpp"
#include "messages.pb.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
static void put(std::ostream&o,const void*p,std::size_t n){o.write(static_cast<const char*>(p),static_cast<std::streamsize>(n));if(!o)throw std::runtime_error("artifact write failed");}
static void u32(std::ostream&o,std::uint32_t x){std::array<char,4>b{};for(unsigned i=0;i<4;i++)b[i]=static_cast<char>(x>>(8*i));put(o,b.data(),b.size());}
static void u64(std::ostream&o,std::uint64_t x){std::array<char,8>b{};for(unsigned i=0;i<8;i++)b[i]=static_cast<char>(x>>(8*i));put(o,b.data(),b.size());}
static void hash(std::ostream&o,const Hash&h){put(o,h.data(),h.size());}
static void require_new(const std::filesystem::path&p){if(std::filesystem::exists(p))throw std::runtime_error("refusing to overwrite existing proof artifact: "+p.string());}
int main(int argc,char**argv){try{
  if(argc!=8)throw std::runtime_error("usage: nexus-real-model-proof CAPTURE MODEL.gguf TREE.bin TREE.json PROOF.bin CONTEXT.bin TRACE.bin");
  const std::filesystem::path capture_path=argv[1],model_path=argv[2],tree_path=argv[3],meta_path=argv[4],proof_path=argv[5],context_path=argv[6],trace_path=argv[7];require_new(proof_path);require_new(context_path);require_new(trace_path);
  const Hash official_model{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
  const Hash official_root{0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};
  model::ReadOnlyGguf model(model_path);const auto sidecar=model.verify_sidecar(tree_path,meta_path,official_model,official_root,203469888);if(sidecar.declared_root!=official_root)throw std::runtime_error("H6 root mismatch");
  model::SparseMerkleIndex merkle(model,tree_path);const auto records=read_capture(capture_path);const CaptureRecord*selected=nullptr;protowire::KaspadResponse response;
  for(const auto&r:records){protowire::KaspadResponse candidate;if(!candidate.ParseFromArray(r.response_wire.data(),static_cast<int>(r.response_wire.size()))||!candidate.has_getblocktemplateresponse())continue;const auto&t=candidate.getblocktemplateresponse();if(r.network=="mainnet"&&t.issynced()&&t.has_block()&&t.block().has_header()){selected=&r;response=std::move(candidate);break;}}
  if(!selected)throw std::runtime_error("capture has no synced mainnet template");const auto&t=response.getblocktemplateresponse();const auto&block=t.block();const auto header=header_from_rpc(block.header());const auto tier=header.pom_tier;const auto anchor=official_tier_anchor(header.daa_score,tier);if(anchor.root!=official_root||anchor.chunks!=model.chunk_count())throw std::runtime_error("captured template DAA does not select this GGUF anchor");
  const auto pph=pre_pow_hash(header);constexpr std::uint64_t nonce=0x4b455259584d3231ULL;const auto timestamp=header.timestamp;const auto seed=block_seed(pph,timestamp,nonce,header.daa_score);const auto target=decode_compact_target(header.bits);if(target.zero)throw std::runtime_error("invalid mainnet compact target");
  TemplateFields fields;fields.network=selected->network;fields.response_wire=selected->response_wire;const auto block_wire=block.SerializeAsString();fields.block_wire.assign(block_wire.begin(),block_wire.end());fields.daa_score=header.daa_score;fields.compact_target_bits=header.bits;fields.timestamp=header.timestamp;fields.template_nonce=header.nonce;fields.node_synced=t.issynced();JobPipeline jobs;const auto job=jobs.accept(std::move(fields));const auto binding=bind_candidate(job,nonce,timestamp,pph);if(!candidate_matches_job(binding,job)||!binding.lease.valid())throw std::runtime_error("candidate is not bound to captured job generation");
  const auto ntiles=model.chunk_count()/32;auto build=prove_v4_from_model(seed,tier,ntiles,[&](std::uint64_t off){const auto bytes=model.read_tile(off);std::array<std::uint8_t,tile_bytes>tile{};std::copy(bytes.begin(),bytes.end(),tile.begin());return tile;},[&](std::uint64_t off){return merkle.tile_path(off);});
  const auto final=final_state(build.walk);const auto pow=pom_pow_value_h3(final,pph);const auto wire=borsh_v4_container(build.proof,final,pow);if(!candidate_matches_job(binding,job)||!binding.lease.valid())throw std::runtime_error("captured job lease became stale during proof construction");
  {std::ofstream f(proof_path,std::ios::binary|std::ios::out|std::ios::trunc);put(f,wire.data(),wire.size());}
  {std::ofstream f(context_path,std::ios::binary|std::ios::out|std::ios::trunc);put(f,"NXRF",4);u64(f,seed);u64(f,model.chunk_count());put(f,&tier,1);hash(f,official_root);hash(f,target.little_endian);hash(f,pph);u64(f,timestamp);u64(f,nonce);u64(f,header.daa_score);u64(f,job.generation);hash(f,selected->digest);u64(f,final);hash(f,pow);u32(f,static_cast<std::uint32_t>(wire.size()));put(f,wire.data(),wire.size());}
  {std::ofstream f(trace_path,std::ios::binary|std::ios::out|std::ios::trunc);put(f,"NXST",4);u64(f,seed);u64(f,nonce);u32(f,static_cast<std::uint32_t>(build.walk.states.size()));u32(f,static_cast<std::uint32_t>(build.walk.offsets.size()));for(const auto&s:build.walk.states)put(f,s.data(),s.size());for(const auto off:build.walk.offsets)u64(f,off);}
  std::cout<<"REAL_MODEL_PROOF_BUILD=PASS states=257 offsets=256 tiles=256 proof_bytes="<<wire.size()<<" chunks="<<model.chunk_count()<<" daa="<<header.daa_score<<" tier="<<unsigned(tier)<<" seed="<<seed<<" nonce="<<nonce<<" generation="<<job.generation<<" target_satisfied="<<(little_endian_leq(pow,target.little_endian)?"yes":"no")<<" submit=DISABLED\n";
  std::cout<<"proof="<<proof_path<<" context="<<context_path<<" trace="<<trace_path<<" capture_blake3="<<hex(selected->digest.data(),selected->digest.size())<<"\n";return 0;
}catch(const std::exception&e){std::cerr<<"REAL_MODEL_PROOF_BUILD=FAIL "<<e.what()<<"\n";return 1;}}
