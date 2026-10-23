#pragma once
#include "keryx/nexus/gpu_work_coordinator.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
// Test-only job envelope around a recorded candidate. This is deliberately
// NOT an RPC template parser or a live freshness attestation.
class WorkReplay {
 using Owner=keryx::nexus::GpuWorkCoordinator;
 keryx::nexus::LiveJobQueue queue_;
 std::unique_ptr<Owner> owner_;
 std::optional<Owner::Ticket> ticket_;
 keryx::nexus::consensus::CandidateBinding binding_;
 std::string event_;
public:
 WorkReplay(const char* context,const char* event,std::uint64_t gpu_seed) : event_(event?event:"none") {
  using namespace keryx::nexus;using namespace consensus;
  if(event_!="none" && event_!="reorg" && event_!="ai")throw std::runtime_error("Unknown replay event");
  std::ifstream f(context,std::ios::binary|std::ios::ate);
  if(!f || f.tellg()<225 || f.tellg()>8*1024*1024)throw std::runtime_error("Invalid replay context size");
  f.seekg(0);std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(f),{});
  auto le=[&](std::size_t off,unsigned n){std::uint64_t x=0;for(unsigned i=0;i<n;++i)x|=std::uint64_t(bytes.at(off+i))<<(i*8);return x;};
  if(std::memcmp(bytes.data(),"NXRF",4) || bytes.size()!=225+le(221,4))throw std::runtime_error("Invalid replay context");
  const auto nonce=le(125,8),time=le(117,8),daa=le(133,8);Hash pph{};std::copy_n(bytes.data()+85,32,pph.data());
  if(nonce<8 || gpu_seed!=le(4,8) || block_seed(pph,time,nonce,daa)!=gpu_seed)throw std::runtime_error("Replay GPU seed/ticket mismatch");
  TemplateFields fields;fields.network="mainnet";fields.node_synced=true;fields.timestamp=time;fields.daa_score=daa;
  fields.response_wire.assign(bytes.begin(),bytes.begin()+225);fields.block_wire=fields.response_wire;
  fields.consensus_identity_wire=fields.block_wire;queue_.publish(std::move(fields));const auto job=*queue_.wait_pop();
  owner_=std::make_unique<Owner>(queue_,nonce-8,8,1);
  auto simulated=owner_->reserve(MiningLane::Nvidia,job,pph,8);
  if(!simulated)throw std::runtime_error("Cannot reserve simulated NVIDIA lane");
  ticket_=owner_->reserve(MiningLane::Amd,job,pph,1);owner_->finish(*simulated);
  if(!ticket_ || ticket_->first_nonce()!=nonce)throw std::runtime_error("AMD nonce ownership mismatch");
  binding_=bind_candidate(job,nonce,time,pph);
 }
 void before_dispatch(){if(owner_->check_candidate(*ticket_,binding_)!=keryx::nexus::WorkGate::Ready)throw std::runtime_error("Replay work not ready");}
 void after_enqueue(){
  if(event_=="reorg")queue_.invalidate_for_virtual_chain_change();
  if(event_=="ai") {
   owner_->pause_for_ai();
   if(owner_->resume_after_ai())throw std::runtime_error("AI resumed before GPU drain");
  }
 }
 bool after_fence(){
  using keryx::nexus::WorkGate;
  const auto gate=owner_->check_candidate(*ticket_,binding_);
  const auto expected=event_=="none"?WorkGate::Ready:event_=="reorg"?WorkGate::Stale:WorkGate::AiPaused;
  if(gate!=expected)throw std::runtime_error("Unexpected post-GPU replay gate");
  if(!owner_->finish(*ticket_) || owner_->in_flight()!=0)throw std::runtime_error("GPU ticket failed to drain");
  if(event_=="ai" && !owner_->resume_after_ai())throw std::runtime_error("PoM failed to resume after GPU drain");
  std::cout<<"WORK_REPLAY=PASS event="<<event_<<" candidate="<<(gate==WorkGate::Ready?"ELIGIBLE_FOR_VERIFICATION":"DISCARDED")
           <<" nvidia_lane=SIMULATED gpu_drain=PASS submission=NOT_LINKED\n";
  return gate==WorkGate::Ready;
 }
};
