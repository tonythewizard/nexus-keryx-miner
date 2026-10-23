#pragma once
// Optional extension of the real CUDA/Qwen integration test. Never a live
// miner: this fixture has no submission transport and uses a replay lease.
#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include "keryx/nexus/amd_memory_handoff.hpp"
#include "keryx/nexus/proof_artifact_buffers.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include "keryx/nexus/secondary_work_lane.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include <fstream>
#include <algorithm>
#include <iostream>
#include <numeric>
#include <cstdlib>

namespace keryx::nexus::testing {
class DualGpuAiFixture final {
public:
  DualGpuAiFixture(ProofArtifactContext context,const model::ReadOnlyGguf &model,
      const std::filesystem::path &tree,const std::filesystem::path &oracle,
      const std::filesystem::path &output,const std::string &module,const std::string &sha,unsigned cycles=2)
    :context_(context),model_(model),merkle_(model,tree),verifier_(oracle.string()),
     output_(output),expected_cycles_(cycles),owner_(queue_,context.nonce,65536,1),
     module_(module),sha_(sha),resident_tiles_(
       std::getenv("NEXUS_TEST_AMD_RESIDENT")&&std::string(std::getenv("NEXUS_TEST_AMD_RESIDENT"))=="1"?model.chunk_count()/32:0),
     engine_(make_engine()),
     lane_(owner_,[this](auto ticket,const auto &cancelled,const auto &commit){return execute(std::move(ticket),cancelled,commit);},
       {},1,[this](const auto &cancelled){stage("upload_begin",0);if(cancelled())return false;
           if(!engine_){await_device_release();engine_=make_engine();}
           const bool ready=!engine_->resident()||engine_->load_resident(
           [this](auto first,auto bytes){model_.read_tiles_streaming(first,bytes);},cancelled);
           stage(ready?"upload_complete":"upload_cancelled",0);return ready;}) {
    if(cycles<1||cycles>8)throw std::invalid_argument("Bounded AMD replay requires one to eight cycles");
    TemplateFields f;f.network="mainnet";f.node_synced=true;f.timestamp=context.timestamp;f.daa_score=context.daa;
    // Replay-only envelope identity. It cannot reach AmdCandidatePipeline or a
    // SubmitBlock client; the full PoM trace/context is checked by Rust below.
    f.block_wire.assign(context.capture.begin(),context.capture.end());f.response_wire=f.block_wire;
    f.consensus_identity_wire=f.block_wire;f.pay_address="isolated-ai-fixture";f.extra_data="isolated-ai-fixture";
    queue_.publish(f);job_=queue_.wait_pop();std::filesystem::create_directories(output_);
  }
  ~DualGpuAiFixture(){lane_.request_stop();lane_.join();if(primary_)owner_.finish(*primary_);}
  std::uint64_t begin_primary(std::uint32_t count) {
    lane_.rethrow_if_failed();if(primary_)throw std::runtime_error("Overlapping primary fixture reservation");
    primary_=owner_.reserve(MiningLane::Nvidia,*job_,context_.pph,count);
    if(!primary_)throw std::runtime_error("Primary fixture admission failed");
    return primary_->first_nonce();
  }
  void end_primary(){if(primary_){owner_.finish(*primary_);primary_.reset();}}
  void publish(){lane_.publish(*job_,context_.pph);}
  void wait_active(){const auto expected=rounds_+1;wait([&]{return enqueued_.load()>=expected;});}
  SecondaryWorkLane *lane(){return &lane_;}
  void release_for_inference(){
    if(!engine_->resident())throw std::runtime_error("Resident AMD fixture required for inference handoff");
    lane_.invalidate_preparation();const auto before=engine_->stats().allocated_bytes;
    engine_->release_resident();const auto after=engine_->stats().allocated_bytes;
    if(before<=after||before-after<model_.chunk_count()/32*1024)
      throw std::runtime_error("AMD model banks were not released before inference");
    // Destroy the drained PoM device as well: RADV may retain freed allocation
    // pools for a surviving logical device. Recreate after the AI child exits.
    engine_.reset();await_device_release();
    std::cout<<"AMD_POM_MEMORY_RELEASED bytes="<<before-after<<" remaining="<<after<<" lane_paused=true device_destroyed=true\n"<<std::flush;
  }
  void verified_resume(){++rounds_;wait([&]{return verified_.load()>=rounds_;});}
  void stop(bool verify=true){if(stopped_)return;lane_.request_stop();lane_.join();end_primary();owner_.stop();
    if(verify){lane_.rethrow_if_failed();
      if(verified_!=expected_cycles_||enqueued_<expected_cycles_||owner_.in_flight()!=0)throw std::runtime_error("Dual GPU fixture incomplete or leaked reservation");
      std::cout<<"AMD_AI_LIFECYCLE=PASS amd_cancelled="<<expected_cycles_<<" amd_rust_resumed="<<expected_cycles_<<" shared_nonce_ranges=true mock_only=true\n"<<std::flush;
    }
    stopped_=true;
  }
private:
  void await_device_release(){
    std::cout<<"AMD_DEVICE_RELEASE_VERIFIED wait_ms="<<memory_handoff_.wait_released()<<'\n'<<std::flush;
  }
  std::unique_ptr<consensus::VulkanWalkEngine> make_engine(){
    return std::make_unique<consensus::VulkanWalkEngine>("pci:0000:00:00.0",module_,sha_,1,resident_tiles_);
  }
  static void stage(const char *phase,unsigned sequence){
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::cout<<"AMD_REPLAY_STAGE phase="<<phase<<" sequence="<<sequence<<" monotonic_ns="<<ns<<'\n'<<std::flush;
  }
  template<class Predicate>void wait(Predicate predicate){
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(!predicate()){lane_.rethrow_if_failed();if(std::chrono::steady_clock::now()>until)throw std::runtime_error("Dual GPU fixture timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  }
  bool execute(GpuWorkCoordinator::Ticket ticket,const SecondaryWorkLane::Cancelled &cancelled,const SecondaryWorkLane::Commit &commit) {
    using namespace consensus;const auto sequence=++attempts_;
    // Bounded cancellation/resume cycles. Park after them without more GPU work.
    if(sequence>2*expected_cycles_){while(!cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(5));return false;}
    const auto ready=[&]{return !cancelled()&&owner_.check(ticket)==WorkGate::Ready;};
    auto c=context_;c.nonce=ticket.first_nonce();c.seed=block_seed(c.pph,c.timestamp,c.nonce,c.daa);c.generation=ticket.job().generation;
    stage("gather_begin",sequence);
    const auto input=gather_v4_model_tiles(c.seed,c.chunks/32,[&](auto index){std::array<std::uint8_t,1024> tile{};model_.read_tile_into(index,tile);return tile;});
    stage("gather_complete",sequence);
    std::array<std::uint64_t,1> seeds{c.seed};std::array<std::uint32_t,256> slots{};std::iota(slots.begin(),slots.end(),0);
    VulkanWalkBatch gpu;const auto hold=(sequence&1U)!=0;
    const auto queued=[&]{if(hold){++enqueued_;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
      while(!cancelled()){if(std::chrono::steady_clock::now()>until)throw std::runtime_error("AMD did not receive AI cancellation");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}};
    if(!engine_->run(c.chunks/32,seeds,
        engine_->resident()?std::span<const std::uint8_t>{}:std::span<const std::uint8_t>(input.tiles),
        engine_->resident()?std::span<const std::uint32_t>{}:std::span<const std::uint32_t>(slots),true,gpu,[&]{return !ready();},queued))return false;
    stage("kernel_complete",sequence);
    if(!ready())return false;
    if(gpu.offsets!=input.offsets||gpu.diagnostic_states.size()!=257*1024)throw std::runtime_error("Combined AMD witness mismatch");
    Walk walk;walk.offsets=gpu.offsets;walk.states.resize(257);
    for(std::size_t i=0;i<257;++i)std::copy_n(gpu.diagnostic_states.data()+i*1024,1024,walk.states[i].data());
    c.final=final_state(walk);c.pow=pom_pow_value_h3(c.final,c.pph);
    const auto proof=prove_v4_from_walk(walk,c.tier,input.tiles,[&](auto offset){return merkle_.tile_path(offset);});
    const auto wire=borsh_v4_container(proof,c.final,c.pow);const auto a=encode_proof_artifacts(c,walk,wire.size());
    stage("rust_begin",sequence);
    const auto verdict=verifier_.verify(a.context_prefix,a.trace,wire);
    stage("rust_complete",sequence);
    if(!ready())return false;
    return commit([&]{
      const auto prefix=output_/std::to_string(c.nonce);
      save(prefix.string()+".proof",wire);save(prefix.string()+".trace",a.trace);
      auto full=a.context_prefix;full.insert(full.end(),wire.begin(),wire.end());save(prefix.string()+".context",full);
      ++verified_;std::cout<<"DUAL_GPU_AMD_RESUMED=PASS nonce="<<c.nonce<<" rust=PASS proof_bytes="<<wire.size()<<" submit=DISABLED\n"<<std::flush;
    });
  }
  static void save(const std::filesystem::path &p,std::span<const std::uint8_t> bytes){
    if(std::filesystem::exists(p))throw std::runtime_error("Dual GPU proof would overwrite evidence");
    std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(!f)throw std::runtime_error("Dual GPU proof write failed");
  }
  ProofArtifactContext context_;
  const model::ReadOnlyGguf &model_;
  model::SparseMerkleIndex merkle_;
  RustProofVerifier verifier_;
  std::filesystem::path output_;
  const unsigned expected_cycles_;
  LiveJobQueue queue_;
  GpuWorkCoordinator owner_;
  std::optional<TemplateJob> job_;
  std::optional<GpuWorkCoordinator::Ticket> primary_;
  std::string module_,sha_;
  std::uint64_t resident_tiles_;
  AmdMemoryHandoff memory_handoff_;
  std::unique_ptr<consensus::VulkanWalkEngine> engine_;
  SecondaryWorkLane lane_;
  unsigned attempts_{},rounds_{};
  bool stopped_{};
  std::atomic_uint enqueued_{},verified_{};
};
}
