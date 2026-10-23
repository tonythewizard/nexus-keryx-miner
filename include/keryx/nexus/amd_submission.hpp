#pragma once
#include "keryx/nexus/amd_candidate_pipeline.hpp"
#include "keryx/nexus/network/submission_pipeline.hpp"
#include "keryx/nexus/prepared_rpc_template.hpp"
namespace keryx::nexus {
class AmdAdmissionChanged final : public std::invalid_argument {
public: using std::invalid_argument::invalid_argument;
};
// The result type cannot be fabricated from an external boolean. This adapter
// still needs an explicit live-submit flag and the final current job lease.
inline network::SubmitBlockResult submit_amd_candidate(network::SubmitBlockClient &client,
    const AmdVerifiedCandidate &candidate,bool enabled,const std::function<bool()> &cancelled={}) {
  if(cancelled&&cancelled())throw AmdAdmissionChanged("AMD submission cancelled before transport");
  if(!enabled)throw std::invalid_argument("AMD submission disabled");
  if(!candidate.target_met())throw std::invalid_argument("AMD candidate does not satisfy target");
  if(candidate.recheck()!=WorkGate::Ready)throw AmdAdmissionChanged("AMD candidate is stale, paused, or no longer owned");
  const auto &job=candidate.job();const auto digest=consensus::blake3_hash(job.fields.response_wire);
  static constexpr char digits[]="0123456789abcdef";std::string id;
  for(auto b:digest){id+=digits[b>>4];id+=digits[b&15];}
  network::VerifiedSubmission verified;
  verified.job_id=std::move(id);verified.generation=job.generation;verified.nonce=candidate.nonce();
  verified.pom_verified=true;verified.model_verified=true;verified.merkle_verified=true;
  verified.header_seed_verified=true;verified.target_satisfied=true;verified.job_fresh=true;
  verified.generation_matches=true;verified.miner_configured=!job.fields.pay_address.empty();
  const std::string proof(reinterpret_cast<const char*>(candidate.proof().data()),candidate.proof().size());
  network::PomWinner winner{std::move(verified),candidate.header().daa_score,candidate.final_state(),candidate.header().pom_tier,proof};
  PreparedRpcTemplateCache rpc;const auto &block=rpc.block(job.fields.response_wire);
  if(candidate.recheck()!=WorkGate::Ready)throw AmdAdmissionChanged("AMD job changed before transport");
  return network::submit_pom_winner(client,block,winner,job.fields.pay_address,job.fields.extra_data,true,std::chrono::seconds(5),cancelled);
}
}
