#include "keryx/nexus/active_template_job.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus;
static void require(bool ok,const char *message) {if(!ok)throw std::runtime_error(message);}
static TemplateFields fields(std::size_t bytes,std::uint8_t marker) {
  TemplateFields f;f.network="mainnet";f.node_synced=true;
  f.response_wire.assign(bytes,marker);f.block_wire=f.response_wire;
  f.consensus_identity_wire=f.response_wire;return f;
}
int main() {try {
  JobPipeline pipeline;std::optional<TemplateJob> active;
  auto incoming=pipeline.accept(fields(65536,1));const auto *data=incoming.fields.response_wire.data();
  require(select_active_template_job(active,std::move(incoming))->fields.response_wire.data()==data,"adoption copied envelope");
  for(unsigned i=0;i<1000;++i)
    require(select_active_template_job(active,std::nullopt)->fields.response_wire.data()==data,"reuse copied envelope");
  auto same=*active;same.fields.response_wire[0]=99;const auto *replacement=same.fields.response_wire.data();
  require(select_active_template_job(active,std::move(same))->fields.response_wire.data()==replacement,"same-generation refresh not moved");
  require(active->fields.response_wire[0]==99,"same-generation RPC refresh lost");
  pipeline.invalidate();require(!select_active_template_job(active,std::nullopt),"invalidated lease reused");
  auto next=pipeline.accept(fields(65536,2));require(select_active_template_job(active,std::move(next))->generation==2,"replacement generation lost");
  pipeline.disconnect();require(!select_active_template_job(active,std::nullopt),"disconnect lease reused");
  std::uint64_t sink=0;
  for(auto size:{4096U,65536U,1048576U}) {
    active=pipeline.accept(fields(size,3));
    for(unsigned pair=0;pair<6;++pair)for(unsigned v=0;v<2;++v) {
      const bool candidate=(pair+v)%2;constexpr unsigned iterations=100;
      const auto start=std::chrono::steady_clock::now();
      for(unsigned i=0;i<iterations;++i) {
        if(candidate) {const auto *job=select_active_template_job(active,std::nullopt);sink+=job->fields.response_wire[i%size];}
        else {std::optional<TemplateJob> maybe=*active;auto job=std::move(*maybe);active=job;sink+=job.fields.response_wire[i%size];}
      }
      const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
      std::cout<<"JOB_REUSE_MEASUREMENT bytes_per_envelope="<<size<<" pair="<<pair<<" variant="<<(candidate?"candidate":"baseline")<<" ns_per_batch="<<elapsed/iterations<<'\n';
    }
  }
  std::cout<<"ACTIVE_JOB=PASS ownership=true same_generation_refresh=true stale_rejected=true disconnect_rejected=true sink="<<sink<<'\n';
  return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
