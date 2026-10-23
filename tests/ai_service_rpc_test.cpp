#include "keryx/nexus/grpc_client.hpp"

#include <grpc/grpc.h>

#include <cstdlib>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
class GrpcRuntime {
 public:
  GrpcRuntime() { grpc_init(); }
  GrpcRuntime(const GrpcRuntime &) = delete;
  GrpcRuntime &operator=(const GrpcRuntime &) = delete;
  ~GrpcRuntime() {
    unsigned shutdown_calls = 0;
    // gRPC C++ can retain a process-level init reference after all local
    // channels/stubs have gone away. Drain the remaining references only at
    // the test process boundary, after run_test() has returned and destroyed
    // every application-owned gRPC object. The final call blocks for worker
    // thread cleanup before OpenSSL's atexit cleanup.
    while (grpc_is_initialized() && shutdown_calls < 16) {
      grpc_shutdown_blocking();
      ++shutdown_calls;
    }
    std::cout << "GRPC_TEST_SHUTDOWN calls=" << shutdown_calls
              << " initialized_after=" << grpc_is_initialized() << '\n';
    if (grpc_is_initialized()) std::abort();
  }
};

int run_test(int argc, char **argv) {
  if (argc != 2) throw std::runtime_error("mock endpoint required");
  const auto result = keryx::nexus::probe_ai_service_rpc(argv[1], 36000, std::string(64, 'b'),
                                                          std::chrono::seconds(5));
  if (!result.providers_available || !result.strikes_available || result.providers.size() != 1 ||
      result.providers.front().tier != 0 ||
      result.providers.front().model_id != "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710" ||
      result.providers.front().identity != std::string(64, 'b') ||
      result.providers.front().escrow_pubkey != std::string(64, 'a'))
    throw std::runtime_error("official GetServiceProviders mapping mismatch");
  using keryx::nexus::ai::ProviderListingStatus;
  const std::string model_id = "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710";
  if (keryx::nexus::ai::provider_listing_status(result, 0, model_id,
        std::string(64, 'b'), std::string(64, 'a')) != ProviderListingStatus::Listed)
    throw std::runtime_error("matching provider cohort entry was not classified LISTED");
  auto absent = result;
  absent.providers.clear();
  if (keryx::nexus::ai::provider_listing_status(absent, 0, model_id,
        std::string(64, 'b'), std::string(64, 'a')) != ProviderListingStatus::NotListed)
    throw std::runtime_error("successful snapshot non-membership was not classified NOT_LISTED");
  absent.providers_available = false;
  if (keryx::nexus::ai::provider_listing_status(absent, 0, model_id,
        std::string(64, 'b'), std::string(64, 'a')) != ProviderListingStatus::Unknown)
    throw std::runtime_error("unavailable provider RPC was not kept UNKNOWN");
  const auto &s = result.strikes;
  if (s.consecutive != 2 || s.lifetime != 7 || s.last_strike_daa != 120000050 ||
      s.suspended_until_daa != 120001000 || s.pending_burns != 1 ||
      s.pending_burn_sompi != 2500000 || s.virtual_daa != 120000100)
    throw std::runtime_error("official GetServiceStrikes mapping mismatch");
  using keryx::nexus::ServiceRpcQuery;
  const auto providers_only = keryx::nexus::probe_ai_service_rpc(argv[1], 36000,
      std::string(64, 'b'), std::chrono::seconds(5), ServiceRpcQuery::ProvidersOnly);
  if (!providers_only.providers_available || providers_only.strikes_available ||
      providers_only.providers.size()!=result.providers.size() ||
      providers_only.providers_virtual_daa!=result.providers_virtual_daa ||
      providers_only.providers.front().identity!=result.providers.front().identity)
    throw std::runtime_error("provider-only snapshot changed authoritative cohort or fabricated strikes");
  const auto strikes_only = keryx::nexus::probe_ai_service_rpc(argv[1], 0,
      std::string(64, 'b'), std::chrono::seconds(5), ServiceRpcQuery::StrikesOnly);
  if (strikes_only.providers_available || !strikes_only.providers.empty() ||
      !strikes_only.strikes_available || strikes_only.strikes.lifetime!=s.lifetime)
    throw std::runtime_error("strike-only snapshot fabricated providers or changed strike state");
  for(const auto window:{35999ULL,35998ULL}) {
    const auto unavailable=keryx::nexus::probe_ai_service_rpc(argv[1],window,
        std::string(64,'b'),std::chrono::seconds(1),ServiceRpcQuery::ProvidersOnly);
    if(unavailable.providers_available || unavailable.providers_error.empty() ||
       keryx::nexus::ai::provider_listing_status(unavailable,0,model_id,
          std::string(64,'b'),std::string(64,'a'))!=ProviderListingStatus::Unknown)
      throw std::runtime_error("missing/error provider snapshot did not remain fail-closed");
  }
  bool correlation_rejected=false;
  try { (void)keryx::nexus::probe_ai_service_rpc(argv[1],35997,std::string(64,'b'),
      std::chrono::seconds(1),ServiceRpcQuery::ProvidersOnly); }
  catch(const std::runtime_error &){correlation_rejected=true;}
  if(!correlation_rejected)throw std::runtime_error("provider-only RPC lost correlation check");
  std::array<double,3> combined_ms{}, providers_ms{};
  for(std::size_t i=0;i<3;++i) {
    const auto measure=[&](ServiceRpcQuery query) {
      const auto start=std::chrono::steady_clock::now();
      const auto snap=keryx::nexus::probe_ai_service_rpc(argv[1],0,std::string(64,'b'),
          std::chrono::seconds(5),query);
      if(!snap.providers_available)throw std::runtime_error("timing fixture lost authoritative providers");
      return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    };
    combined_ms[i]=measure(ServiceRpcQuery::All);
    providers_ms[i]=measure(ServiceRpcQuery::ProvidersOnly);
  }
  std::sort(combined_ms.begin(),combined_ms.end());std::sort(providers_ms.begin(),providers_ms.end());
  std::cout<<"AI_ASSIGNMENT_REPLAY combined_median_ms="<<combined_ms[1]
           <<" providers_only_median_ms="<<providers_ms[1]<<" delayed_strikes_ms=250\n";
  if(combined_ms[1]<240 || providers_ms[1]>=combined_ms[1]/2)
    throw std::runtime_error("assignment remains coupled to delayed strike polling");
  const auto chain = keryx::nexus::probe_blocks_selected_chain(argv[1],
      {std::string(64, '1'), std::string(64, '2'), std::string(64, '3'),
       std::string(64, '4'), std::string(64, '5'), std::string(64, '6'),
       std::string(64, '1'), std::string(64, '4')},
      std::chrono::seconds(5));
  using keryx::nexus::BlockValidationState;
  if (chain.size() != 8 || chain[0].selected_chain != std::optional<bool>(true) ||
      chain[0].state != BlockValidationState::ChainValid ||
      chain[1].selected_chain != std::optional<bool>(false) ||
      chain[1].state != BlockValidationState::OffChain ||
      chain[2].selected_chain || chain[2].state != BlockValidationState::Error || chain[2].error.empty() ||
      chain[3].selected_chain || chain[3].state != BlockValidationState::Unknown ||
      chain[4].selected_chain || chain[4].state != BlockValidationState::Unknown ||
      chain[5].selected_chain || chain[5].state != BlockValidationState::Unknown ||
      chain[6].state != BlockValidationState::ChainValid || chain[7].state != BlockValidationState::Unknown)
    throw std::runtime_error("bounded/correlated GetBlock selected-chain/UNKNOWN mapping mismatch");
  std::cout << "AI_SERVICE_RPC_MOCK=PASS providers=PASS listing=LISTED/NOT_LISTED/UNKNOWN strikes=PASS selected_chain=true/false/UNKNOWN/ERROR "
               "out_of_order=PASS duplicate_reply=ignored duplicate_hash=deduplicated rpc_error_retry=bounded identity_match=PASS\n";
  return 0;
}
}  // namespace

int main(int argc, char **argv) {
  // Ensure process-level gRPC shutdown runs before OpenSSL atexit cleanup.
  GrpcRuntime grpc_runtime;
  return run_test(argc, argv);
}
