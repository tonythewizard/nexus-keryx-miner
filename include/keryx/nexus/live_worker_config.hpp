#pragma once
#include "keryx/nexus/network/worker_mode.hpp"
#include "keryx/nexus/gpu_device_policy.hpp"
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace keryx::nexus {
inline bool primary_pom_selection(const char *value) {
  if (!value || std::string_view(value) == "1") return true;
  if (std::string_view(value) == "0") return false;
  throw std::invalid_argument("NEXUS_PRIMARY_POM_ENABLED must be exactly 0 or 1");
}
struct LiveWorkerConfig {
  std::string endpoint, pay, extra;
  std::filesystem::path model, tree, metadata, oracle, output;
  std::uint64_t nonce_limit;
  std::size_t microbatch;
  network::WorkerSubmissionPolicy policy;
  bool escrow_claims;
  GpuBackend gpu_backend{GpuBackend::CudaDriver};
};

// Pure configuration validation: no GPU, RPC, file writes or secret logging.
inline LiveWorkerConfig parse_live_worker_config(int argc, char **argv,
    const char *visible, std::string_view authorized_uuid,
    const char *backend = nullptr, const char *device_id = nullptr, bool vulkan_worker=false) {
  namespace fs = std::filesystem;
    if(argc!=14&&argc!=15) throw std::runtime_error("usage: nexus-live-cuda-worker ENDPOINT PAY_ADDRESS EXTRA_DATA MODEL.gguf TREE.bin TREE.json ORACLE OUTPUT_DIR NONCE_LIMIT MICROBATCH MODE --no-submit|--enable-submit --enable-ai-response|--no-ai-response [--enable-escrow-claims|--no-escrow-claims]");
    const auto selected_backend = require_production_gpu_backend(backend, device_id, authorized_uuid, vulkan_worker);
    if(selected_backend==GpuBackend::CudaDriver && (!visible||std::string_view(visible)!=authorized_uuid))
      throw std::runtime_error("CUDA_VISIBLE_DEVICES must contain only the authorized CUDA UUID");
    if((selected_backend==GpuBackend::VulkanJit || selected_backend==GpuBackend::VulkanBda) && visible && *visible)
      throw std::runtime_error("Vulkan-only workflow must exclude native CUDA devices");
    const std::string endpoint=argv[1],pay=argv[2],extra=argv[3];
    const fs::path model_path=argv[4],tree_path=argv[5],meta_path=argv[6],oracle=argv[7],outdir=argv[8];
    const auto nonce_limit=std::stoull(argv[9]); const auto microbatch=static_cast<std::size_t>(std::stoul(argv[10]));
    const auto submit_policy=network::parse_worker_policy(argv[11],argv[12],argv[13]);
    const bool submit_enabled=submit_policy.enabled();
    const bool ai_responses_enabled=submit_policy.ai_responses_enabled();
    bool escrow_claims_enabled=false;
    if(argc==15) {
      if(std::string_view(argv[14])=="--enable-escrow-claims") escrow_claims_enabled=true;
      else if(std::string_view(argv[14])!="--no-escrow-claims") throw std::runtime_error("invalid explicit escrow claim policy");
    }
    if(escrow_claims_enabled&&(submit_policy.mode!=network::WorkerMode::Live||!submit_enabled||!ai_responses_enabled))
      throw std::runtime_error("escrow claims require live mode and explicit PoM/AiResponse authorization");
    if((nonce_limit>256)||(nonce_limit==0&&submit_policy.mode!=network::WorkerMode::Live)||microbatch==0||microbatch>4)
      throw std::runtime_error("nonce limit must be 1..256, or 0 only for continuous live mode; microbatch must be 1..4");
    if(submit_enabled && submit_policy.mode!=network::WorkerMode::Live)
      throw std::runtime_error("SubmitBlock is forbidden in shadow mode");
    if(submit_policy.mode==network::WorkerMode::Live && !ai_responses_enabled)
      throw std::runtime_error("productive mining is forbidden without explicit AiResponse transaction authorization");
    const auto hex_field = [](std::string_view value) {
      return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      });
    };
    const auto escrow_pos = extra.find("/escrow:");
    const auto signature_pos = extra.find("/esig:");
    const bool escrow_syntax = escrow_pos != std::string::npos && signature_pos != std::string::npos &&
        signature_pos == escrow_pos + 8 + 64 && extra.size() == signature_pos + 6 + 128 &&
        hex_field(std::string_view(extra).substr(escrow_pos + 8, 64)) &&
        hex_field(std::string_view(extra).substr(signature_pos + 6, 128));
    if(pay.rfind("keryx:", 0) != 0 || !escrow_syntax)
      throw std::runtime_error("payout/escrow configuration is malformed (values redacted)");
    return {endpoint,pay,extra,model_path,tree_path,meta_path,oracle,outdir,
            nonce_limit,microbatch,submit_policy,escrow_claims_enabled,selected_backend};
}
} // namespace keryx::nexus
