#pragma once
#include "keryx/nexus/amd_candidate_pipeline.hpp"
#include <optional>
#include <string>

namespace keryx::nexus {
// Caller serializes prepare/execute/release with GpuPreemptionCoordinator.
// The primary lane owns one model allocation; inference must release this
// entire device before loading Qwen. No CUDA types or calls exist here.
class PrimaryVulkanWorkload final {
public:
  static constexpr std::uint32_t batch_limit=3072;
  PrimaryVulkanWorkload(std::string device,std::string module,std::string sha,
      GpuWorkCoordinator &owner,const model::ReadOnlyGguf &model,
      model::SparseMerkleIndex &merkle,RustProofVerifier &verifier,
      std::string payout,std::string identity)
    :device_(std::move(device)),module_(std::move(module)),sha_(std::move(sha)),
     owner_(owner),model_(model),merkle_(merkle),verifier_(verifier),
     payout_(std::move(payout)),identity_(std::move(identity)) {}
  bool prepare(const std::function<bool()> &cancelled={}) {
    if(cancelled&&cancelled())return false;
    if(!engine_) {
      engine_=std::make_unique<consensus::VulkanWalkEngine>(device_,module_,sha_,batch_limit,model_.chunk_count()/32);
      pipeline_=std::make_unique<AmdCandidatePipeline>(owner_,*engine_,model_,merkle_,verifier_,payout_,identity_,MiningLane::Nvidia);
    }
    if(loaded_)return true;
    loaded_=engine_->load_resident([&](auto first,auto bytes){model_.read_tiles_streaming(first,bytes);},cancelled);
    return loaded_;
  }
  std::unique_ptr<AmdVerifiedCandidate> execute(GpuWorkCoordinator::Ticket ticket,
      const std::function<bool()> &cancelled={}, const std::function<void()> &after_enqueue={}) {
    if(!pipeline_)throw std::logic_error("Primary Vulkan workload not prepared");
    return pipeline_->execute(std::move(ticket),cancelled,after_enqueue);
  }
  void release() {
    pipeline_.reset();
    engine_.reset();
    loaded_ = false;
  }
  bool allocated() const noexcept {return bool(engine_);}
  consensus::VulkanEngineStats stats() const {return engine_?engine_->stats():consensus::VulkanEngineStats{};}
private:
  std::string device_,module_,sha_;
  GpuWorkCoordinator &owner_;
  const model::ReadOnlyGguf &model_;
  model::SparseMerkleIndex &merkle_;
  RustProofVerifier &verifier_;
  std::string payout_,identity_;
  std::unique_ptr<consensus::VulkanWalkEngine> engine_;
  std::unique_ptr<AmdCandidatePipeline> pipeline_;
  bool loaded_{false};
};
}
