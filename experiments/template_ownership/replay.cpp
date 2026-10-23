#include "keryx/nexus/gpu_work_coordinator.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace keryx::nexus;
static TemplateFields fields(std::size_t bytes,unsigned marker=1) {
  TemplateFields f;f.network="mainnet";f.node_synced=true;f.timestamp=123;f.daa_score=126000000;
  f.pay_address="offline-payout";f.extra_data="offline-delegation";
  f.response_wire.assign(bytes,2);f.block_wire.assign(bytes,marker);f.consensus_identity_wire=f.block_wire;
  return f;
}
int main(int argc,char **argv) {try {
  if(argc!=2)throw std::runtime_error("OUTPUT required");std::ofstream out(argv[1]);if(!out)throw std::runtime_error("output unavailable");
  using Clock=std::chrono::steady_clock;consensus::Hash pph{};pph.fill(42);
  for(std::size_t size:{4096,65536,1048576})for(unsigned round=0;round<8;++round){
    LiveJobQueue q;auto f=fields(size);q.publish(f);auto job=*q.wait_pop();
    auto begin=Clock::now();unsigned accepted=0;
    for(unsigned i=0;i<1000;++i)accepted+=q.owns_snapshot(job);
    const auto own_us=std::chrono::duration<double,std::micro>(Clock::now()-begin).count()/1000;
    if(accepted!=1000)throw std::runtime_error("owned snapshot rejected");
    GpuWorkCoordinator coordinator(q,0,4608,256);begin=Clock::now();
    for(unsigned i=0;i<100;++i){auto ticket=coordinator.reserve(MiningLane::Nvidia,job,pph,4608);
      if(!ticket||coordinator.check(*ticket)!=WorkGate::Ready||!coordinator.finish(*ticket))throw std::runtime_error("batch bookkeeping failed");}
    const auto batch_us=std::chrono::duration<double,std::micro>(Clock::now()-begin).count()/100;
    // Deterministic notification/reorg/AI replay. These are logical fixture
    // counts, not executed GPU kernels, proofs or mainnet eligible attempts.
    unsigned fresh=0,stale=0,refresh_blocked=0;std::uint64_t next=100*4608;
    for(unsigned i=0;i<40;++i){
      auto ticket=coordinator.reserve(MiningLane::Nvidia,job,pph,4608);
      if(!ticket||ticket->first_nonce()!=next)throw std::runtime_error("nonce overlap/reset");next+=4608;
      if(i%4==0){(void)q.begin_refresh();if(coordinator.check(*ticket)!=WorkGate::RefreshPending||q.submission_allowed(job.generation))throw std::runtime_error("unresolved refresh admitted work");++refresh_blocked;
        f.response_wire[0]^=1;q.publish(f);if(!q.owns_snapshot(job)||!job.lease.valid())throw std::runtime_error("equivalent update invalidated lease");(void)q.wait_pop_for(std::chrono::milliseconds(0));}
      if(i%4==1){q.invalidate_for_virtual_chain_change();if(coordinator.check(*ticket)!=WorkGate::Stale||q.submission_allowed(job.generation))throw std::runtime_error("reorg admitted stale work");++stale;
        if(!coordinator.finish(*ticket))throw std::runtime_error("stale drain failed");f.consensus_identity_wire[0]^=1;f.block_wire=f.consensus_identity_wire;q.publish(f);job=*q.wait_pop();continue;}
      if(i%4==2){coordinator.pause_for_ai();if(coordinator.check(*ticket)!=WorkGate::AiPaused||coordinator.resume_after_ai())throw std::runtime_error("AI pause bypassed drain");
        if(!coordinator.finish(*ticket)||!coordinator.resume_after_ai())throw std::runtime_error("AI resume failed");continue;}
      if(coordinator.check(*ticket)!=WorkGate::Ready)throw std::runtime_error("fresh ticket blocked");++fresh;if(!coordinator.finish(*ticket))throw std::runtime_error("fresh drain failed");
    }
    out<<"{\"bytes\":"<<size<<",\"round\":"<<round<<",\"ownership_us\":"<<own_us<<",\"reserve_check_finish_us\":"<<batch_us
       <<",\"logical_fresh\":"<<fresh<<",\"logical_stale\":"<<stale<<",\"logical_refresh_blocked\":"<<refresh_blocked<<"}\n"<<std::flush;
  }
  std::cout<<"TEMPLATE_OWNERSHIP_REPLAY=PASS logical_only=true no_gpu=true no_submit=true\n";return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
