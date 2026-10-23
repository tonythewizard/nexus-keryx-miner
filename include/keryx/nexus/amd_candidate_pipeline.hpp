#pragma once
#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/gpu_work_coordinator.hpp"
#include "keryx/nexus/proof_artifact_buffers.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include <array>

namespace keryx::nexus {
// Move-only screening result; only target-met results carry a Rust-verified proof.
// Its nonce ticket remains reserved through final
// freshness checks and transport. Destruction drains the reservation exactly
// once. Coordinator/model/engine/verifier must outlive the result.
class AmdVerifiedCandidate final {
public:
  ~AmdVerifiedCandidate();
  AmdVerifiedCandidate(const AmdVerifiedCandidate &)=delete;
  AmdVerifiedCandidate &operator=(const AmdVerifiedCandidate &)=delete;
  const TemplateJob &job() const noexcept {return ticket_.job();}
  const consensus::Header &header() const noexcept {return header_;}
  const std::vector<std::uint8_t> &proof() const noexcept {return wire_;}
  const ProofArtifactBuffers &artifacts() const noexcept {return artifacts_;}
  const std::string &rust_summary() const noexcept {return rust_summary_;}
  std::uint64_t nonce() const noexcept {return binding_.nonce;}
  std::uint64_t final_state() const noexcept {return final_;}
  bool target_met() const noexcept {return target_met_;}
  WorkGate recheck() const;
private:
  friend class AmdCandidatePipeline;
  AmdVerifiedCandidate(GpuWorkCoordinator &owner,GpuWorkCoordinator::Ticket ticket,
                       consensus::Header header,consensus::CandidateBinding binding)
    :owner_(owner),ticket_(std::move(ticket)),header_(std::move(header)),binding_(std::move(binding)){}
  GpuWorkCoordinator &owner_;
  GpuWorkCoordinator::Ticket ticket_;
  consensus::Header header_;
  consensus::CandidateBinding binding_;
  std::uint64_t final_{};
  bool target_met_{};
  std::vector<std::uint8_t> wire_;
  ProofArtifactBuffers artifacts_;
  std::string rust_summary_;
};
class AmdCandidatePipeline final {
public:
  AmdCandidatePipeline(GpuWorkCoordinator &owner,consensus::VulkanWalkEngine &engine,
                       const model::ReadOnlyGguf &model,model::SparseMerkleIndex &merkle,
                       RustProofVerifier &verifier,std::string payout,std::string extra,
                       MiningLane lane=MiningLane::Amd);
  ~AmdCandidatePipeline();
  AmdCandidatePipeline(const AmdCandidatePipeline &)=delete;
  AmdCandidatePipeline &operator=(const AmdCandidatePipeline &)=delete;
  // Decode the full RPC header internally and check the configured payout/delegation.
  // Consumes a bounded AMD nonce range on every outcome; nullptr means cancelled.
  std::unique_ptr<AmdVerifiedCandidate> execute(GpuWorkCoordinator::Ticket ticket,
       const std::function<bool()> &cancelled={},
       const std::function<void()> &after_enqueue={});
  bool startup_parity_verified() const {
    std::lock_guard lock(execute_mutex_);return startup_parity_done_;
  }
private:
  struct Workspace;
  std::unique_ptr<Workspace> workspace_;
  std::array<std::uint64_t,5> profile_ns_{}; // preparation, GPU call wall, screen, gap, batches
  std::uint64_t last_screen_end_ns_{}, next_profile_ns_{};
  bool startup_parity_done_{}; // Protected by execute_mutex_.
  GpuWorkCoordinator &owner_;
  consensus::VulkanWalkEngine &engine_;
  const model::ReadOnlyGguf &model_;
  model::SparseMerkleIndex &merkle_;
  RustProofVerifier &verifier_;
  std::string payout_,extra_;
  const MiningLane lane_;
  mutable std::mutex execute_mutex_; // Dedicated sequential Rust verifier for this lane.
};
}
