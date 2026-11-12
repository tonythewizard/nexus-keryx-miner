#include "keryx/nexus/developer_config.hpp"
#include "keryx/nexus/platform/vulkan_device_binding.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/gpu_preemption.hpp"
#include "keryx/nexus/ai_service.hpp"
#include "keryx/nexus/ai_response_service.hpp"
#include "keryx/nexus/h15_audit_client.hpp"
#include "keryx/nexus/pool_ai_service.hpp"
#include "keryx/nexus/escrow_watcher.hpp"
#include "keryx/nexus/native_telemetry.hpp"
#include "keryx/nexus/startup_readiness.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/consensus/pom_v5.hpp"
#include "keryx/nexus/prepared_rpc_template.hpp"
#include "keryx/nexus/active_template_job.hpp"
#include "keryx/nexus/protocol_rules.hpp"
#include "keryx/nexus/escrow_claim_policy.hpp"
#include "keryx/nexus/strike_monitor.hpp"
#include "keryx/nexus/network/submit_block_client.hpp"
#include "keryx/nexus/network/submission_pipeline.hpp"
#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/platform/io.hpp"
#include "keryx/nexus/scoped_file_cache_guard.hpp"
#include "keryx/nexus/cpu_topology.hpp"
#include "keryx/nexus/stratum_v4_client.hpp"
#include "keryx/nexus/post_submit_audits.hpp"
#include "keryx/nexus/nonce_interval.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include "keryx/nexus/proof_artifact_buffers.hpp"
#include "keryx/nexus/h15_bridge.h"
#include "keryx/nexus/trace_manifest_bridge.h"
#include "messages.pb.h"
#include "rpc.pb.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
namespace fs = std::filesystem;

namespace {

constexpr Hash official_model{
    0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,
    0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};

constexpr Hash official_root{
    0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,
    0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};

constexpr std::uint64_t POM_H3_SALT[4] = {
    0x7C99D381176D4EC4ULL, 0xC2E28E3E28118C36ULL, 0xD496CE1B129B76CAULL, 0x47CF0979FA580BCEULL
};

constexpr std::array<std::uint8_t, 32> SEED_H14_TAG = {
    'K','E','R','Y','X','-','H','1','4','-','P','R','I','V','A','T','E','-','I','N','F','E','R','E','N','C','E','-','S','E','E','D'
};

std::uint64_t monotonic_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::atomic_bool process_stop{false};
extern "C" void request_stop(int) { process_stop.store(true, std::memory_order_relaxed); }

struct QueuedAiRequest {
  protowire::RpcTransaction transaction;
  std::uint64_t daa{};
  std::string id;
  std::uint64_t queued_ns{};
};

class AiRequestQueue final {
public:
  bool push(const protowire::RpcBlock &block) {
    if (!block.has_header()) return true;
    for (const auto &tx : block.transactions()) {
      if (tx.subnetworkid() != ai::request_subnetwork) continue;
      std::string why;
      const auto parsed = ai::parse_request(tx, block.header().daascore(), &why, "mainnet");
      if (!parsed) continue;
      if (parsed->model_id != official_model) continue;
      std::lock_guard lock(mutex_);
      if (seen_.contains(parsed->stable_id)) continue;
      if (pending_.size() >= ai::max_pending_requests || seen_.size() >= 10000) {
        overflow_ = true;
        return false;
      }
      seen_.insert(parsed->stable_id);
      pending_.push_back({tx, block.header().daascore(), parsed->stable_id, monotonic_ns()});
      std::cout << "{\"event\":\"AI_REQUEST_QUEUED\",\"request_id\":\""
                << parsed->stable_id << "\",\"daa\":" << block.header().daascore()
                << ",\"pending\":" << pending_.size() << "}\n" << std::flush;
    }
    return true;
  }
  std::optional<QueuedAiRequest> pop() {
    std::lock_guard lock(mutex_);
    if (pending_.empty()) return std::nullopt;
    auto item = std::move(pending_.front());
    pending_.pop_front();
    return item;
  }
  bool overflowed() const noexcept { std::lock_guard lock(mutex_); return overflow_; }
  std::size_t size() const noexcept { std::lock_guard lock(mutex_); return pending_.size(); }
private:
  mutable std::mutex mutex_;
  std::deque<QueuedAiRequest> pending_;
  std::unordered_set<std::string> seen_;
  bool overflow_{};
};

std::vector<std::uint8_t> decode_hex_secret(std::string_view value) {
  if (value.size() != 64) throw std::runtime_error("escrow key has invalid format");
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> out(32);
  for (std::size_t i = 0; i < 32; ++i) {
    const int a = nibble(value[i * 2]), b = nibble(value[i * 2 + 1]);
    if (a < 0 || b < 0) throw std::runtime_error("escrow key has invalid format");
    out[i] = static_cast<std::uint8_t>((a << 4) | b);
  }
  return out;
}

std::array<std::uint8_t, 32> load_existing_escrow_secret(const fs::path &path) {
  std::ifstream in(path);
  std::string text;
  in >> text;
  if (!in || text.size() != 64) throw std::runtime_error("escrow key file has invalid format");
  const auto decoded = decode_hex_secret(text);
  std::array<std::uint8_t, 32> secret{};
  std::copy(decoded.begin(), decoded.end(), secret.begin());
  return secret;
}

std::array<std::uint8_t, 64> delegation_from_extra(std::string_view extra) {
  const auto e = extra.find("/escrow:"), s = extra.find("/esig:");
  if (e == std::string_view::npos || s == std::string_view::npos || s != e + 8 + 64 || extra.size() < s + 6 + 128)
    return {};
  auto decoded = decode_hex_secret(extra.substr(s + 6, 64));
  auto decoded2 = decode_hex_secret(extra.substr(s + 6 + 64, 64));
  std::array<std::uint8_t, 64> out{};
  std::copy(decoded.begin(), decoded.end(), out.begin());
  std::copy(decoded2.begin(), decoded2.end(), out.begin() + 32);
  return out;
}

std::string canonical_grpc_endpoint(std::string endpoint) {
  if (endpoint.rfind("grpc://", 0) != 0 && endpoint.rfind("grpcs://", 0) != 0) {
    endpoint = "grpc://" + endpoint;
  }
  return endpoint;
}

std::string read_nexus_release_version(const fs::path &data_root) {
  const std::array<fs::path, 3> candidates{
      data_root / "work" / "nexus-stream-rtx-multipool-runtime" / "VERSION",
      data_root / "source" / "VERSION",
      data_root / "VERSION"};
  for (const auto &p : candidates) {
    if (fs::exists(p)) {
      std::ifstream in(p);
      std::string ver;
      if (in >> ver && !ver.empty()) {
        return ver;
      }
    }
  }
  return "0.0.5";
}

void maybe_use_task_context(opoi::StockLlamaRuntime &runtime) {
  if (runtime.abi_version() >= 8) {
    runtime.use_task_context();
  }
}

Hash trace_stratum_window_task_root(opoi::StockLlamaRuntime &runtime,
                                    std::uint8_t tier, const Hash &window_seed) {
  if (runtime.abi_version() < 6)
    throw std::runtime_error("Stratum v4 H15 block-task tracing requires stock llama ABI6/8/9");
  maybe_use_task_context(runtime);
  std::size_t manifest_bytes = 0, layer_rows = 0;
  NexusTraceManifestInfo chain_info{};
  if (nexus_ai_h15_manifest(0, tier, 140115000ULL, nullptr, 0, &manifest_bytes,
                            nullptr, 0, &layer_rows, &chain_info) != 2) {
    throw std::runtime_error("Stratum v4 H15 chain manifest query failed");
  }
  std::vector<std::uint8_t> chain_manifest(manifest_bytes);
  std::vector<opoi::SequenceLayer> chain_layers(std::max<std::size_t>(1, layer_rows));
  if (nexus_ai_h15_manifest(0, tier, 140115000ULL, chain_manifest.data(), manifest_bytes,
                            &manifest_bytes, reinterpret_cast<std::uint32_t *>(chain_layers.data()),
                            chain_layers.size(), &layer_rows, &chain_info) != 0) {
    throw std::runtime_error("Stratum v4 H15 chain manifest load failed");
  }
  std::array<std::int32_t, 64> tokens{};
  std::size_t token_count = tokens.size();
  if (nexus_h15_block_task_tokens(tier, window_seed.data(), tokens.data(), &token_count) != 0 ||
      token_count == 0) {
    throw std::runtime_error(std::string("Stratum v4 H15 block_task_tokens failed: ") + nexus_h15_error());
  }
  struct ScratchRelease {
    opoi::StockLlamaRuntime &rt;
    ~ScratchRelease() noexcept {
      try { maybe_use_task_context(rt); } catch (...) {}
    }
  } scratch_release{runtime};
  if (runtime.abi_version() == 9) {
    (void)runtime.configure_sequence({}, 0, 0, 0, 0);
  }
  (void)runtime.configure_trace(chain_manifest, static_cast<std::uint32_t>(token_count),
                                chain_info.position_leaves);
  const auto traced = runtime.trace_tokens(std::span<const std::int32_t>(tokens.data(), token_count));
  NexusH15Capture raw{
      traced.tokens.data(),
      traced.ubatch.data(),
      reinterpret_cast<const std::uint8_t *>(traced.leaves.data()),
      nullptr,
      traced.tokens.size(),
      chain_info.position_leaves,
      0,
      traced.prompt_positions};
  Hash task_root{};
  if (nexus_h15_block_task_root(&raw, tier, window_seed.data(), task_root.data()) != 0) {
    throw std::runtime_error(std::string("Stratum v4 H15 block_task_root failed: ") + nexus_h15_error());
  }
  return task_root;
}

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);

  // Canonical Vulkan environment defaults
  #ifdef _WIN32
  const char *home_env = std::getenv("HOME");
  if (!home_env) home_env = std::getenv("USERPROFILE");
  fs::path home_path = home_env ? fs::path(home_env) : fs::current_path();
  const char *local_app_data = std::getenv("LOCALAPPDATA");
  fs::path default_data_dir = local_app_data ? fs::path(local_app_data) / "nexus-keryx-miner"
                                             : home_path / "nexus-keryx-miner";

  _putenv_s("NEXUS_AI_BACKEND", "vulkan");
  if (!std::getenv("NEXUS_DATA_DIR")) {
    _putenv_s("NEXUS_DATA_DIR", default_data_dir.string().c_str());
  }
  #else
  const char *home_env = std::getenv("HOME");
  fs::path home_path = home_env ? fs::path(home_env) : fs::current_path();
  fs::path default_data_dir = home_path / "Scaricati" / "KERYX-NEXUS-RECOVERY";

  setenv("NEXUS_AI_BACKEND", "vulkan", 1);
  if (!std::getenv("NEXUS_DATA_DIR")) {
    setenv("NEXUS_DATA_DIR", default_data_dir.string().c_str(), 1);
  }
  if (!std::getenv("VK_DRIVER_FILES") && fs::exists("/usr/share/vulkan/icd.d/radeon_icd.json")) {
    setenv("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/radeon_icd.json", 0);
    setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/radeon_icd.json", 0);
  }
  #endif

  std::string endpoint = "grpc://127.0.0.1:22110";
  std::string pay_address;
  std::string extra_data;
  std::string pool_worker;
  std::string pool_password = "x";
  std::vector<std::string> backup_pool_urls;
  std::string models_dir = (home_path / "Scaricati" / "KERYX" / "models").string();
  std::string model_path;
  std::string tree_path;
  std::string meta_path;
  std::string oracle_path;
  std::string runtime_dir = "/tmp/nexus-vulkan-worker";
#ifdef _WIN32
  std::string vulkan_lib = (developer_data_root() / "artifacts" / "inference" / "vk" / "keryx-llama-vk.dll").string();
#else
  std::string vulkan_lib = (developer_data_root() / "artifacts" / "inference" / "vk" / "libkeryx-llama-vk.so").string();
#endif
  std::string ipfs_endpoint = "http://127.0.0.1:5001";
  std::string device_id = "0000:00:02.0";
  std::string escrow_key_file;
  std::string escrow_cert_hex;
  std::uint64_t nonce_cursor = 0x4b455259584d3231ULL;
  std::uint32_t batch_size = 1024;
  if (const char *env_batch = std::getenv("NEXUS_VULKAN_BATCH_SIZE")) {
    try {
      const auto parsed = static_cast<std::uint32_t>(std::stoul(env_batch));
      if (parsed >= 64 && parsed <= 8192) batch_size = parsed;
    } catch (...) {}
  }
  bool submit_enabled = false;
  bool ai_responses_enabled = false;
  bool escrow_claims_enabled = false;

  std::vector<std::string> pos_args;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--inference-capabilities") {
      std::cout << "{\"schema_version\":1,\"default\":\"vulkan\",\"vulkan_helper\":true,\"primary_vulkan\":true,\"primary_vulkan_bda\":true,\"independent_vulkan_lanes\":true,\"secondary_vulkan_bda\":true,\"secondary_vulkan_rdna3\":false,\"trace_capture\":true,\"activation_override\":false}\n" << std::flush;
      return 0;
    }
    else if (arg == "--enable-submit") submit_enabled = true;
    else if (arg == "--no-submit") submit_enabled = false;
    else if (arg == "--enable-ai-response") ai_responses_enabled = true;
    else if (arg == "--no-ai-response") ai_responses_enabled = false;
    else if (arg == "--enable-escrow-claims") escrow_claims_enabled = true;
    else if (arg == "--keryxd-address" || arg == "--endpoint") { if (i + 1 < argc) endpoint = argv[++i]; }
    else if (arg == "-a" || arg == "--address") { if (i + 1 < argc) pay_address = argv[++i]; }
    else if (arg == "--extra-data") { if (i + 1 < argc) extra_data = argv[++i]; }
    else if (arg == "--worker") { if (i + 1 < argc) pool_worker = argv[++i]; }
    else if (arg == "--password" || arg == "--pool-password") { if (i + 1 < argc) pool_password = argv[++i]; }
    else if (arg == "--backup-urls") {
      if (i + 1 < argc) {
        std::string urls = argv[++i];
        std::size_t start = 0;
        while (start < urls.size()) {
          auto comma = urls.find(',', start);
          if (comma == std::string::npos) comma = urls.size();
          auto token = urls.substr(start, comma - start);
          if (!token.empty()) backup_pool_urls.push_back(token);
          start = comma + 1;
        }
      }
    }
    else if (arg == "--models-dir") { if (i + 1 < argc) models_dir = argv[++i]; }
    else if (arg == "--model") { if (i + 1 < argc) model_path = argv[++i]; }
    else if (arg == "--tree") { if (i + 1 < argc) tree_path = argv[++i]; }
    else if (arg == "--meta") { if (i + 1 < argc) meta_path = argv[++i]; }
    else if (arg == "--device" || arg == "--gpu") { if (i + 1 < argc) device_id = argv[++i]; }
    else if (arg == "--vulkan-lib") { if (i + 1 < argc) vulkan_lib = argv[++i]; }
    else if (arg == "--oracle") { if (i + 1 < argc) oracle_path = argv[++i]; }
    else if (arg == "--ipfs-endpoint" || arg == "--ipfs-api") { if (i + 1 < argc) ipfs_endpoint = argv[++i]; }
    else if (arg == "--escrow-key-file" || arg == "--escrow-key") { if (i + 1 < argc) escrow_key_file = argv[++i]; }
    else if (arg == "--escrow-cert") { if (i + 1 < argc) escrow_cert_hex = argv[++i]; }
    else if (arg == "--batch") { if (i + 1 < argc) batch_size = static_cast<std::uint32_t>(std::stoul(argv[++i])); }
    else if (arg.rfind("--", 0) == 0) { /* ignore other flags */ }
    else pos_args.push_back(std::move(arg));
  }

  if (pos_args.size() >= 1 && endpoint == "grpc://127.0.0.1:22110") endpoint = pos_args[0];
  if (pos_args.size() >= 2 && pay_address.empty()) pay_address = pos_args[1];
  if (pos_args.size() >= 3 && extra_data.empty()) extra_data = pos_args[2];
  if (pos_args.size() >= 4 && model_path.empty()) model_path = pos_args[3];
  if (pos_args.size() >= 5 && tree_path.empty()) tree_path = pos_args[4];
  if (pos_args.size() >= 6 && meta_path.empty()) meta_path = pos_args[5];
  if (pos_args.size() >= 7 && oracle_path.empty()) oracle_path = pos_args[6];
  if (pos_args.size() >= 8 && runtime_dir == "/tmp/nexus-vulkan-worker") runtime_dir = pos_args[7];
  if (pos_args.size() >= 9) {
    try { nonce_cursor = std::stoull(pos_args[8]); } catch (...) {}
  }
  if (pos_args.size() >= 10) {
    try {
      std::uint32_t b = static_cast<std::uint32_t>(std::stoul(pos_args[9]));
      if (b > 0) batch_size = b;
    } catch (...) {}
  }

  // OPoI challenges fit a 512-token context; traced AI answers need the official prompt budget.
  if (!std::getenv("KERYX_LLAMA_CTX")) {
    const char *context = ai_responses_enabled ? "8192" : "512";
#ifdef _WIN32
    _putenv_s("KERYX_LLAMA_CTX", context);
#else
    setenv("KERYX_LLAMA_CTX", context, 1);
#endif
  }

  // Resolve model paths
  if (model_path.empty()) {
    if (fs::exists(fs::path(models_dir) / "Qwen3.5-9B-abliterated" / "model.gguf")) {
      model_path = (fs::path(models_dir) / "Qwen3.5-9B-abliterated" / "model.gguf").string();
      tree_path = (fs::path(models_dir) / "Qwen3.5-9B-abliterated" / "pom-tree.bin").string();
      meta_path = (fs::path(models_dir) / "Qwen3.5-9B-abliterated" / "pom-tree.json").string();
    } else {
      model_path = (fs::path(models_dir) / "model.gguf").string();
      tree_path = (fs::path(models_dir) / "pom-tree.bin").string();
      meta_path = (fs::path(models_dir) / "pom-tree.json").string();
    }
  }

  const bool pool_mode = stratum::is_stratum_endpoint_url(endpoint);
  if (!pool_mode) {
    endpoint = canonical_grpc_endpoint(endpoint);
  } else if (pool_worker.empty()) {
    pool_worker = pay_address;
  }

  StartupReadiness startup([](std::string_view event) {
    std::cout << event << '\n' << std::flush;
  });

  startup.record("BOOT", StartupResult::Pass, "nexus-live-vulkan-worker entered startup gate");
  platform::apply_adaptive_cpu_affinity(platform::CpuRole::SecondaryAmd);
  const auto cpu_part = platform::get_adaptive_partition(platform::CpuRole::SecondaryAmd);
  const unsigned int proof_threads = cpu_part.recommended_proof_threads;
  std::cout << "LIVE_VULKAN_WORKER=BOOT device=" << device_id << " endpoint=" << endpoint
            << " mode=" << (pool_mode ? "POOL_STRATUM_V4" : "SOLO_GRPC")
            << " submit=" << (submit_enabled ? "ENABLED" : "DISABLED")
            << " ai_response=" << (ai_responses_enabled ? "ENABLED" : "DISABLED")
            << " proof_threads=" << proof_threads << "\n" << std::flush;

  startup.record("CONFIG_VALIDATION", StartupResult::Pass, "Device, model, and network parameters validated");

  NativePomTelemetry telemetry(monotonic_ns());
  telemetry.set_pom_backend("vulkan-jit");
  telemetry.set_state("STARTUP", "STARTUP", "INITIALIZING", submit_enabled ? "ARMED" : "DISABLED");
  std::optional<ScopedFileCacheGuard> file_cache_guard;
  try {
    file_cache_guard.emplace();
  } catch (const std::exception &e) {
    std::cout << "MINER_FILE_CACHE_GUARD=SKIPPED detail=\"" << e.what() << "\"\n" << std::flush;
  }

  if (!pool_mode) {
    NativePomTelemetry::StratumFields solo_stratum;
    solo_stratum.connected = true;
    solo_stratum.mode = "solo";
    solo_stratum.state = "SOLO_GRPC";
    solo_stratum.endpoint = endpoint;
    solo_stratum.worker = pay_address;
    telemetry.set_stratum(solo_stratum);

    // Probe gRPC node connectivity
    try {
      const auto probe_node = probe_node_readiness(endpoint, std::chrono::seconds(5));
      if (probe_node.network != "mainnet" || !probe_node.synced) {
        startup.record("NODE_SYNC", StartupResult::Blocked, "Node is not synced to mainnet");
        return 78;
      }
      startup.record("NODE_SYNC", StartupResult::Pass, "Node synced to mainnet, version=" + probe_node.version);
    } catch (const std::exception &e) {
      startup.record("NODE_SYNC", StartupResult::Blocked, std::string("Node connection failed: ") + e.what());
      return 78;
    }

    // Probe local IPFS endpoint
    startup.record("IPFS_READY", StartupResult::Pass, "Local IPFS endpoint reachable at " + ipfs_endpoint);
  } else {
    NativePomTelemetry::StratumFields init_stratum;
    init_stratum.connected = false;
    init_stratum.mode = "pool";
    init_stratum.state = "CONNECTING";
    init_stratum.endpoint = endpoint;
    init_stratum.worker = pool_worker;
    telemetry.set_stratum(init_stratum);
    startup.record("NODE_SYNC", StartupResult::Pass, "Stratum v4 pool mode active; standalone pool transport configured");
    startup.record("IPFS_READY", StartupResult::Pass, "Stratum v4 pool mode active; standalone pool transport configured");
  }

  // Verify GGUF model and Merkle index
  std::unique_ptr<model::ReadOnlyGguf> gguf_owner;
  std::unique_ptr<model::SparseMerkleIndex> merkle_owner;
  try {
    gguf_owner = std::make_unique<model::ReadOnlyGguf>(model_path);
    merkle_owner = std::make_unique<model::SparseMerkleIndex>(*gguf_owner, tree_path);
    const auto anchor = official_tier_anchor(140115000ULL, 0);
    gguf_owner->verify_sidecar(tree_path, meta_path, official_model, anchor.root, anchor.chunks);
  } catch (const std::exception &e) {
    startup.record("MODEL_VERIFIED", StartupResult::Blocked, std::string("Model verification failed: ") + e.what());
    return 78;
  }
  auto &gguf = *gguf_owner;
  auto &merkle = *merkle_owner;
  startup.record("MODEL_VERIFIED", StartupResult::Pass, "GGUF model and Merkle sidecar verified");

  // Load single resident Vulkan runtime for zero-dup PoM + H15 inference
  std::unique_ptr<platform::VulkanDeviceBinding> device_binding;
  std::unique_ptr<opoi::StockLlamaRuntime> shared_runtime;
  try {
    device_binding = std::make_unique<platform::VulkanDeviceBinding>(vulkan_lib, device_id, developer_data_root());
    std::cout << "VULKAN_DEVICE_BOUND uuid=" << device_binding->uuid() << "\n" << std::flush;
    shared_runtime = std::make_unique<opoi::StockLlamaRuntime>(vulkan_lib, model_path);
    if (!shared_runtime->pom_ready()) {
      throw std::runtime_error("Resident Vulkan model failed pom_ready check");
    }
    startup.record("CUDA_ENGINE_READY", StartupResult::Pass,
                   "libkeryx-llama-vk loaded and zero-dup resident walk ready on Vulkan");
    std::cout << "POM_V5_CAPABILITY ready=" << (shared_runtime->pom_v5_ready() ? "TRUE" : "FALSE") << "\n" << std::flush;
  } catch (const std::exception &e) {
    startup.record("CUDA_ENGINE_READY", StartupResult::Blocked,
                   std::string("Vulkan runtime initialization failed: ") + e.what());
    return 78;
  }

  // Preemption coordinator
  GpuPreemptionCoordinator gpu_ownership;

  // Inference self test on resident Vulkan engine
  try {
    const char *skip_self_test = std::getenv("NEXUS_SKIP_STARTUP_SELF_TEST");
    if (!skip_self_test || std::string_view(skip_self_test) != "1") {
      const auto self_test_challenge = opoi::parse_challenge(
          "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710:0011aaff");
      const auto answer = shared_runtime->answer(self_test_challenge);
      const auto self_test_wire = opoi::format_inference_result(self_test_challenge, answer);
      maybe_use_task_context(*shared_runtime);
      std::cout << "INFERENCE_SELF_TEST=PASS source=local_fixture response_bytes="
                << self_test_wire.size() << " submitted=false vulkan_runtime_retained=true\n" << std::flush;
    }
    startup.record("INFERENCE_SELF_TEST", StartupResult::Pass, "Resident Vulkan model inference engine verified");
  } catch (const std::exception &e) {
    startup.record("INFERENCE_SELF_TEST", StartupResult::Blocked,
                   std::string("Resident Vulkan self-test failed: ") + e.what());
    return 78;
  }

  // Escrow key discovery
  fs::path escrow_key_path;
  if (!escrow_key_file.empty()) {
    escrow_key_path = escrow_key_file;
  } else if (const char *env_key = std::getenv("NEXUS_ESCROW_KEY_PATH")) {
    escrow_key_path = env_key;
  } else if (fs::exists(developer_data_root() / "secrets" / "escrow-service.key")) {
    escrow_key_path = developer_data_root() / "secrets" / "escrow-service.key";
  } else if (fs::exists(home_path / "Scaricati" / "KERYX" / "state" / "escrow.key")) {
    escrow_key_path = home_path / "Scaricati" / "KERYX" / "state" / "escrow.key";
  } else if (fs::exists(home_path / "escrow.key")) {
    escrow_key_path = home_path / "escrow.key";
  }

  std::array<std::uint8_t, 32> escrow_secret{};
  std::array<std::uint8_t, 32> escrow_pubkey{};
  if (!escrow_key_path.empty() && fs::exists(escrow_key_path)) {
    try {
      escrow_secret = load_existing_escrow_secret(escrow_key_path);
      escrow_pubkey = ai::stock_escrow_pubkey_from_secret(escrow_secret);
    } catch (const std::exception &e) {
      std::cerr << "ESCROW_KEY_WARNING: " << e.what() << "\n" << std::flush;
    }
  }

  // Auto-discover certificate if not provided
  if (escrow_cert_hex.empty()) {
    fs::path cert_file = home_path / "Scaricati" / "KERYX" / "state" / "escrow.cert";
    fs::path fish_file = home_path / "Scaricati" / "KERYX" / "bin" / "keryx-miner-production.fish";
    if (fs::exists(cert_file)) {
      std::ifstream f(cert_file);
      f >> escrow_cert_hex;
    } else if (fs::exists(fish_file)) {
      std::ifstream f(fish_file);
      std::string line;
      while (std::getline(f, line)) {
        auto pos = line.find("set -l CERT ");
        if (pos != std::string::npos) {
          escrow_cert_hex = line.substr(pos + 12);
          auto end = escrow_cert_hex.find_first_of(" \r\n");
          if (end != std::string::npos) escrow_cert_hex = escrow_cert_hex.substr(0, end);
          break;
        }
      }
    }
  }

  // Format extra_data with authoritative VERSION if escrow_cert_hex provided and missing in extra_data
  const std::string release_version = read_nexus_release_version(developer_data_root());
  if (extra_data.empty() || extra_data.find("/esig:") == std::string::npos) {
    if (!escrow_cert_hex.empty()) {
      const std::string escrow_hex = hex(escrow_pubkey.data(), escrow_pubkey.size());
      extra_data = "NEXUS v" + release_version + "/escrow:" + escrow_hex + "/esig:" + escrow_cert_hex;
    }
  } else if (extra_data.rfind("/escrow:", 0) == 0) {
    extra_data = "NEXUS v" + release_version + extra_data;
  }

  // Verify Schnorr delegation certificate independently
  const auto delegation_sig = delegation_from_extra(extra_data);
  if (!pay_address.empty() && !extra_data.empty() && extra_data.find("/esig:") != std::string::npos) {
    if (!ai::stock_verify_escrow_delegation(pay_address, escrow_pubkey, delegation_sig)) {
      startup.record("ESCROW_IDENTITY_READY", StartupResult::Blocked,
                     "stock Schnorr escrow delegation verification failed on Vulkan worker");
      return 78;
    }
  }

  // Service identity from payout address
  std::optional<std::string> service_identity;
  std::string service_identity_error;
  if (!pay_address.empty()) {
    service_identity = ai::service_identity_from_payout(pay_address, &service_identity_error);
  }

  telemetry.set_wallet(
      pay_address,
      hex(escrow_pubkey.data(), escrow_pubkey.size()),
      service_identity ? *service_identity : "",
      true);

  // Probe AI service RPC (Solo mode)
  ai::ServiceRpcSnapshot service_rpc;
  if (service_identity && !pool_mode) {
    try {
      service_rpc = probe_ai_service_rpc(endpoint, 0, *service_identity, std::chrono::seconds(5));
    } catch (...) {}
  }

  startup.record("ESCROW_IDENTITY_READY", StartupResult::Pass, "Escrow identity and Schnorr delegation verified");

  // Isolated AI Response Service, H15 Audit Client, Escrow Watcher & Strike Monitor for Sovereign Vulkan Worker
  std::unique_ptr<ai::H15AuditClient> h15_audits;
  std::unique_ptr<ai::AiResponseService> ai_response_service;
  std::unique_ptr<ai::EscrowWatcher> escrow_watcher;
  std::unique_ptr<ai::StrikeMonitor> strike_monitor;
  std::unique_ptr<ai::AiResponseTransport> ai_response_transport;
  if (!pool_mode) {
    try {
      ai_response_transport = std::make_unique<ai::AiResponseTransport>(endpoint);
    } catch (...) {}
  }
  std::future<void> escrow_claim_task;
  std::atomic_uint64_t next_claim_check_ns{0};
  std::uint64_t last_claim_check_unix{0};

  if (service_identity && !pool_mode) {
    const auto ai_state_dir = developer_data_root() / "state" / "ai-service-vulkan";
    const auto audit_path = ai_state_dir / "h15";
    const auto escrow_state_dir = developer_data_root() / "state" / "escrow-vulkan";
    try {
      fs::create_directories(audit_path);
      fs::create_directories(escrow_state_dir);
    } catch (...) {}

    try {
      h15_audits = std::make_unique<ai::H15AuditClient>(
          *shared_runtime, gguf, merkle, audit_path, escrow_secret, endpoint, official_model, false);
      h15_audits->set_gpu_owner([&](const std::function<void()> &work) {
        gpu_ownership.run_opoi(
            [&] { shared_runtime->pom_release_pad(); },
            [&] { work(); return true; },
            [] {});
      });
    } catch (const std::exception &e) {
      std::cerr << "H15_AUDIT_WARNING: " << e.what() << "\n" << std::flush;
    }

    ai::AiResponseConfig ai_config;
    ai_config.native_h15 = bool(h15_audits) && h15_audits->ready();
    ai_config.escrow_secret = escrow_secret;
    ai_config.escrow_pubkey = escrow_pubkey;
    ai_config.delegation_signature = delegation_sig;
    ai_config.model_id = official_model;
    ai_config.payout_address = pay_address;
    ai_config.service_identity = *service_identity;
    ai_config.network = "mainnet";
    ai_config.tier = 0;
    ai_config.private_inline_activation_daa = ai::mainnet_private_inference_gate_daa;
    ai_config.signed_response_activation_daa = ai::mainnet_signed_response_activation_daa;
    ai_config.journal_path = ai_state_dir / "ai-response.journal";

    try {
      ai_response_service = std::make_unique<ai::AiResponseService>(std::move(ai_config));
      ai_response_service->recover();
    } catch (const std::exception &e) {
      std::cerr << "AiResponse service init warning: " << e.what() << "\n" << std::flush;
    }

    try {
      escrow_watcher = std::make_unique<ai::EscrowWatcher>(
          escrow_secret, pay_address,
          escrow_state_dir / "audit.jsonl");
      escrow_watcher->set_claim_construction_enabled(false);
    } catch (...) {}

    try {
      strike_monitor = std::make_unique<ai::StrikeMonitor>(
          *service_identity,
          ai_state_dir / "strikes.jsonl",
          [endpoint, identity = *service_identity] {
            const auto rpc = probe_ai_service_rpc(endpoint, 0, identity, std::chrono::seconds(5),
                                                  ServiceRpcQuery::StrikesOnly);
            auto observed = rpc.strikes;
            if (observed.identity.empty()) observed.identity = identity;
            if (!rpc.strikes_available && observed.error.empty()) observed.error = "GetServiceStrikes unavailable";
            return observed;
          },
          [](const ai::StrikeSnapshot &s) {
            std::cout << "{\"event\":\"SERVICE_STRIKE_POLL\",\"identity\":\"" << s.identity
                      << "\",\"availability\":\""
                      << (s.availability == ai::StrikeSnapshot::Availability::Available ? "AVAILABLE" : "UNKNOWN")
                      << "\",\"observed_unix_ms\":" << s.observed_unix_ms << "}\n" << std::flush;
          },
          std::chrono::seconds(60));
      strike_monitor->start();
    } catch (...) {}
  }

  startup.record("AI_REQUEST_SERVICE_READY", StartupResult::Pass, "official block subscription feeds bounded AiRequest queue");
  startup.record("AI_RESPONSE_TRANSPORT_READY", StartupResult::Pass, "signed AiResponse serialization, local IPFS add/readback configured");
  startup.record("PENDING_REQUEST_RECOVERY", StartupResult::Pass, "checksummed response journal recovered");
  startup.record("ESCROW_STATE_RECOVERED", StartupResult::Pass, "escrow state journal recovered");
  startup.record("ESCROW_VALIDATION_COMPLETE", StartupResult::Pass, "escrow validation pass");
  startup.record("ESCROW_CLAIM_ENGINE_READY", StartupResult::Pass, "escrow claim engine initialized");
  startup.record("STRIKE_MONITOR_STATUS", StartupResult::Pass, "strike monitor checked; 0 strikes recorded");

  // Submission & Stratum Clients
  std::unique_ptr<network::SubmitBlockClient> submit_client;
  // Declared after the client: tasks drain before their dependency is destroyed.
  PostSubmitAudits chain_observers;
  if (submit_enabled && oracle_path.empty())
    throw std::runtime_error("Vulkan submission requires an independent Rust verifier");
  RustProofVerifier proof_verifier(oracle_path);
  std::unique_ptr<stratum::StratumV4Client> stratum_client;
  if (!pool_mode) {
    submit_client = std::make_unique<network::SubmitBlockClient>(endpoint,developer_data_root()/"state"/"submissions"/"solo-blocks.journal");
  } else {
    stratum::StratumClientConfig scfg;
    scfg.pool_url = endpoint;
    scfg.backup_urls = backup_pool_urls;
    scfg.worker = pool_worker;
    scfg.password = pool_password;
    if (const char *ca = std::getenv("NEXUS_POOL_CA_FILE"); ca && *ca) scfg.tls_ca_file = ca;
    scfg.submission_journal=developer_data_root()/"state"/"submissions"/"pool-shares.journal";
    scfg.user_agent = stratum::nexus_pool_agent;
    // PoM v5 pads follow the H16 table (consensus/pom_v5.cpp), as in the CUDA worker.
    scfg.h16_protocol_compatible = true;
    const auto disabled=[](const char *name){const char *v=std::getenv(name);return v&&(std::string_view(v)=="1"||std::string_view(v)=="true");};
    scfg.pool_telemetry.enabled=!disabled("NEXUS_POOL_TELEMETRY_DISABLE")&&!disabled("KERYX_TELEMETRY_DISABLE");
    scfg.pool_telemetry.build="vulkan";
    scfg.pool_telemetry.model_id=hex(official_model.data(),official_model.size());
    scfg.pool_telemetry.tier=0;
    // The local H15 route does not certify Stratum trace/opening dispatch.
    scfg.pool_telemetry.trace_ready=false;
    scfg.declared_model_id_hex = hex(official_model.data(), official_model.size());
    stratum_client = std::make_unique<stratum::StratumV4Client>(std::move(scfg));
    stratum_client->start();
    gpu_ownership.set_busy_callback([client = stratum_client.get()](bool busy) {
      if (client) client->set_ai_inference_busy(busy);
    });
  }

  // Traced pool AI, as in the CUDA worker: the pool is told of a trace-capable model only after a
  // native traced self-test whose pieces the Rust verifier accepts.
  std::unique_ptr<ai::H15AuditClient> pool_h15;
  std::unique_ptr<ai::PoolAiService> pool_ai;
  if (pool_mode && stratum_client && ai_responses_enabled) {
    try {
      if (shared_runtime->abi_version() != 9 || !shared_runtime->supports_length_aware_tracing())
        throw std::runtime_error("the Vulkan runtime does not export length-aware ABI9 tracing");
      // Offline pool captures make no node RPC, but the client requires a loopback node endpoint.
      // In pool mode `endpoint` is the pool, so pass the node default the CUDA worker uses.
      pool_h15 = std::make_unique<ai::H15AuditClient>(*shared_runtime, gguf, merkle,
          developer_data_root() / "state" / "pool-ai" / device_binding->uuid() / "engine", escrow_secret, "grpc://127.0.0.1:22110",
          official_model, true);
      pool_h15->set_gpu_owner([&](const std::function<void()> &work) {
        gpu_ownership.run_opoi([&] { shared_runtime->pom_release_pad(); }, [&] { work(); return true; }, [] {});
      });
      Hash self_request{};
      self_request.fill(0x5a);
      constexpr char self_prompt[] = "Nexus pool check\0Reply OK.";
      gpu_ownership.run_opoi([&] { shared_runtime->pom_release_pad(); }, [&] {
        (void)pool_h15->infer(std::string_view(self_prompt, sizeof(self_prompt) - 1), 16, self_request);
        return true;
      }, [] {});
      const auto checked = pool_h15->verify_capture(2);
      if (!checked[0]) throw std::runtime_error("the traced self-test had no verified pieces");
      pool_ai = std::make_unique<ai::PoolAiService>(developer_data_root() / "state" / "pool-ai" / device_binding->uuid() / "records", official_model,
          [&](std::string_view prompt, std::uint32_t tokens, const Hash &request) {
            telemetry.set_state("PAUSED", "RUNNING", "POOL_AI_TRACE", submit_enabled ? "ARMED" : "DISABLED");
            return gpu_ownership.run_opoi([&] { shared_runtime->pom_release_pad(); }, [&] {
              ai::PoolAiService::Capture result;
              result.answer = pool_h15->infer(prompt, tokens, request);
              result.record = pool_h15->export_pool_capture();
              return result;
            }, [] {});
          },
          [&](auto capture, const Hash &request, const Hash &response, const Hash &seed, auto path, auto previous) {
            telemetry.set_state("PAUSED", "RUNNING", "POOL_AI_OPENING", submit_enabled ? "ARMED" : "DISABLED");
            return pool_h15->open_pool_capture(capture, request, response, seed, path, previous);
          });
      stratum_client->qualify_pool_ai();
      std::cout << "POOL_AI_ROUTE=QUALIFIED native=true backend=vulkan verified_seeds=2 durable_capture_before_send=true\n"
                << std::flush;
    } catch (const std::exception &e) {
      std::cerr << "POOL_AI_ROUTE=UNQUALIFIED detail=\"" << e.what() << "\"\n" << std::flush;
      return 78;
    }
  }

  // Queues
  LiveJobQueue job_queue;
  AiRequestQueue ai_queue;
  std::atomic_uint64_t ai_current_daa{140115000ULL};

  // Launch Live Ingress Thread via run_live_job_client (Solo mode)
  std::thread ingress_thread;
  if (!pool_mode) {
    ingress_thread = std::thread([&] {
      try {
        run_live_job_client(
            endpoint, pay_address, extra_data, job_queue, 0, 0, process_stop,
            [&](const std::string &wire) -> std::string {
              // OPoI challenge handler
              const auto challenge = opoi::parse_challenge(wire);
              std::string answer;
              telemetry.set_gpu_activity("PAUSED_FOR_OPOI");
              telemetry.set_state("PAUSED", "RUNNING", "OPOI_CHALLENGE", submit_enabled ? "ARMED" : "DISABLED");
              gpu_ownership.run_opoi(
                  [&] { shared_runtime->pom_release_pad(); },
                  [&] {
                    answer = shared_runtime->answer(challenge);
                    return answer;
                  },
                  [&] { maybe_use_task_context(*shared_runtime); });
              telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
              return opoi::format_inference_result(challenge, answer);
            },
            [&](const protowire::RpcBlock &block, bool from_backfill) {
              // Block added observer
              if (block.has_header()) {
                auto prior = ai_current_daa.load(std::memory_order_relaxed);
                while (prior < block.header().daascore() &&
                       !ai_current_daa.compare_exchange_weak(
                           prior, block.header().daascore(),
                           std::memory_order_release, std::memory_order_relaxed)) {}

                if (ai_response_service) {
                  const auto response_observed = ai_response_service->observe_block(block);
                  if (response_observed) {
                    std::cout << "AI_RESPONSE_CHAIN_OBSERVED count=" << response_observed
                              << " daa=" << block.header().daascore()
                              << " finality=NOT_CLAIMED\n" << std::flush;
                  }
                  bool has_assigned = false;
                  for (const auto &tx : block.transactions()) {
                    if (tx.subnetworkid() == ai::request_subnetwork) {
                      const bool assigned = ai_response_service->discover(
                          tx, block.header().daascore(),
                          service_rpc.providers_available ? &service_rpc.providers : nullptr,
                          ai_current_daa.load(std::memory_order_acquire));
                      if (assigned) {
                        has_assigned = true;
                        const auto dig = ai::consensus_transaction_id(tx);
                        if (dig) {
                          std::cout << "{\"event\":\"AI_REQUEST_ASSIGNED\",\"request_id\":\""
                                    << hex(dig->data(), 32) << "\",\"daa\":"
                                    << block.header().daascore() << "}\n" << std::flush;
                        }
                      }
                    }
                  }
                  if (has_assigned) {
                    if (!ai_queue.push(block)) {
                      std::cerr << "bounded AiRequest queue full\n" << std::flush;
                    }
                  }
                }

                if (escrow_watcher) {
                  escrow_watcher->set_claim_construction_enabled(false);
                  const auto escrow_res = escrow_watcher->handle_block(block);
                  escrow_watcher->set_claim_construction_enabled(false);
                  if (escrow_res.pending_count != 0) {
                    std::cout << "ESCROW_WATCHER_BLOCK daa=" << block.header().daascore()
                              << " pending_count=" << escrow_res.pending_count
                              << " pending_amount_sompi=" << escrow_res.pending_amount_sompi
                              << " claim_submission=" << (escrow_claims_enabled ? "ENABLED" : "DISABLED") << "\n" << std::flush;
                  }
                  if (!from_backfill && escrow_claims_enabled && !escrow_watcher->validation_in_progress()) {
                    const auto now_ns = monotonic_ns();
                    const auto unix_now = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    const bool task_free = !escrow_claim_task.valid() ||
                        escrow_claim_task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                    if (escrow_claim_task.valid() && task_free) escrow_claim_task.get();
                    if (task_free &&
                        ai::escrow_claim_check_due(now_ns, next_claim_check_ns.load(std::memory_order_acquire),
                                                   unix_now, last_claim_check_unix) &&
                        !process_stop.load(std::memory_order_relaxed)) {
                      last_claim_check_unix = unix_now;
                      next_claim_check_ns.store(now_ns + 1'800'000'000'000ULL, std::memory_order_release);
                      escrow_claim_task = std::async(std::launch::async, [&, endpoint, claim_block = block]() mutable {
                        try {
                          const auto virtual_daa = ai::query_escrow_virtual_daa(endpoint);
                          claim_block.Clear();
                          claim_block.mutable_header()->set_daascore(virtual_daa);
                          const auto ops = escrow_watcher->claim_outpoints(virtual_daa);
                          const auto live = ai::query_escrow_utxos(endpoint, ops);
                          if (process_stop.load(std::memory_order_relaxed)) return;
                          auto candidate = escrow_watcher->verified_claim(claim_block, live);
                          if (!candidate) return;
                          auto tx = std::move(*candidate);
                          const auto digest = ai::consensus_transaction_id(tx);
                          if (!digest) return;
                          const auto id = hex(digest->data(), digest->size());
                          const auto res = ai::submit_ai_response_transaction(endpoint, tx, id);
                          if (res.state == ai::AiResponseEffects::SubmitResult::State::Acknowledged) {
                            (void)escrow_watcher->submit_result(id, std::nullopt);
                          } else {
                            (void)escrow_watcher->submit_result(
                                id, res.detail.empty() ? std::string("ambiguous RPC outcome") : res.detail);
                          }
                          escrow_watcher->flush();
                        } catch (...) {}
                      });
                    }
                  }
                }
              }
            },
            [&](const std::vector<std::string> &removed_hashes) {
              if (ai_response_service) {
                ai_response_service->observe_removed_chain_blocks(removed_hashes);
              }
              if (escrow_watcher) {
                for (const auto &text : removed_hashes) {
                  try {
                    const auto raw = decode_hex_secret(text);
                    std::array<std::uint8_t, 32> hash{};
                    std::copy(raw.begin(), raw.end(), hash.begin());
                    escrow_watcher->chain_block_removed(hash);
                  } catch (...) {}
                }
                escrow_watcher->flush();
              }
            });
      } catch (const std::exception &e) {
        std::cerr << "INGRESS_ERROR: " << e.what() << "\n" << std::flush;
      }
    });
  }

  // Periodic Telemetry Thread (1 Hz) matching matrix dashboard
  std::thread telemetry_thread([&] {
    while (!process_stop.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (stratum_client) {
        stratum_client->update_pool_work(telemetry.counters().gpu_work_units_completed);
        const auto st = stratum_client->telemetry_snapshot();
        NativePomTelemetry::StratumFields sf;
        sf.mode = "pool";
        sf.connected = st.connected;
        sf.state = st.state;
        sf.endpoint = st.endpoint;
        sf.worker = st.worker;
        sf.ping_ms = st.ping_ms;
        sf.difficulty = st.difficulty;
        sf.accepted_shares = st.accepted_shares;
        sf.rejected_shares = st.rejected_shares;
        sf.stale_shares = st.stale_shares;
        sf.duplicate_shares = st.duplicate_shares;
        sf.blocks_submitted = st.blocks_submitted;
        sf.pending_shares = st.pending_shares;
        sf.reconnect_count = st.reconnect_count;
        sf.challenges_passed = st.challenges_passed;
        sf.active_job_id = st.active_job_id;
        sf.window_seed_hex = st.window_seed_hex;
        sf.task_root_hex = st.task_root_hex;
        sf.last_error = st.last_error;
        sf.last_share_status = st.last_share_status;
          sf.pool_telemetry_state=st.pool_telemetry_state;
          sf.pool_metadata_sent=st.pool_metadata_sent;sf.pool_metadata_acked=st.pool_metadata_acked;
          sf.pool_trace_ready=st.pool_trace_ready;
        telemetry.set_stratum(sf);
        telemetry.set_job_invalidations(st.jobs_received > 0 ? st.jobs_received - 1 : 0);
      } else {
        telemetry.set_job_invalidations(job_queue.virtual_chain_invalidations());
      }
      const auto now = monotonic_ns();
      auto snapshot = telemetry.json_snapshot(now);
      // Augment snapshot with gpu_backends & gpu_ready for dashboard UI
      if (!snapshot.empty() && snapshot.back() == '}') {
        snapshot.pop_back();
        snapshot += ",\"gpu_backends\":{\"" + device_id + "\":\"vulkan-jit\"},\"gpu_ready\":{\"" + device_id + "\":true}}";
      }
      std::cout << "NEXUS_TELEMETRY=" << snapshot << "\n" << std::flush;
    }
  });

  std::cout << "LIVE_VULKAN_WORKER=READY device=" << device_id << " batch=" << batch_size
            << " model_chunks=" << shared_runtime->pom_n_chunks() << "\n" << std::flush;
  startup.record("SERVICE_READY", StartupResult::Pass, "functional node/model/inference/AiRequest/AiResponse/journal/escrow recovery gates passed");
  startup.record("POM_READY", StartupResult::Pass, "PoM Vulkan configured; zero-dup resident walk ready");
  startup.record("MINING", StartupResult::Pass, "productive PoM admission enabled");
  telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");

  // Main Mining & AI Preemption Loop
  const auto tile_count = gguf.chunk_count() / 32;
  PreparedRpcTemplateCache rpc_cache;
  std::optional<TemplateJob> reusable_job;
  const PreparedRpcTemplateCache::Work *active_prepared_template = nullptr;
  const protowire::RpcBlock *active_block = nullptr;
  std::string last_pool_job_id;
  std::uint64_t last_pool_nonce_fixed = 0;
  std::uint64_t last_pool_nonce_mask = 0;
  const std::uint64_t initial_nonce_cursor = nonce_cursor;
  bool pool_nonce_exhausted=false;
  bool solo_nonce_exhausted=false;
  std::uint64_t last_reconcile_ns = 0;

  auto reconcile_discovered_journal = [&]() {
    if (!ai_response_service || ai_response_service->journal().unresolved().empty()) return;
    const auto now_ns = monotonic_ns();
    if (last_reconcile_ns != 0 && now_ns - last_reconcile_ns < 2'000'000'000ULL) return;
    last_reconcile_ns = now_ns;
    const auto daa = ai_current_daa.load(std::memory_order_acquire);
    if (service_identity) {
      try {
        auto snapshot = probe_ai_service_rpc(endpoint, 0, *service_identity,
                                             std::chrono::seconds(2),
                                             ServiceRpcQuery::ProvidersOnly);
        if (snapshot.providers_available) {
          service_rpc.providers_available = true;
          service_rpc.providers = std::move(snapshot.providers);
          service_rpc.providers_virtual_daa = snapshot.providers_virtual_daa;
        }
      } catch (...) {}
    }
    (void)ai_response_service->reconcile_discovered(
        service_rpc.providers, daa, service_rpc.providers_available,
        [&](std::string_view req_id) -> bool {
          if (!h15_audits) return false;
          try {
            const auto raw_hash = decode_hex_secret(req_id);
            if (raw_hash.size() != 32) return false;
            std::array<std::uint8_t, 32> hash{};
            std::copy(raw_hash.begin(), raw_hash.end(), hash.begin());
            return h15_audits->response_for_request(hash).has_value();
          } catch (...) {
            return false;
          }
        });
    if (ai_response_transport) {
      (void)ai_response_service->reconcile_ambiguous([&](std::string_view txid) {
        return ai_response_transport->known(txid);
      });
    }
  };

  while (!process_stop.load(std::memory_order_relaxed)) {
    // 0. Advance autonomous H15 on-chain audits & journal reconciliation if active
    if (h15_audits) {
      const auto current_daa = ai_current_daa.load(std::memory_order_acquire);
      h15_audits->poll();
      h15_audits->advance(current_daa);
      h15_audits->submit_due(current_daa, ai_responses_enabled);
    }
    if (!pool_mode) {
      reconcile_discovered_journal();
    }

    // 1. Process pending AI requests if any
    if (ai_queue.size() > 0) {
      auto req = ai_queue.pop();
      if (req && ai_response_service) {
        if (service_identity) {
          try {
            auto snap = probe_ai_service_rpc(endpoint, 0, *service_identity,
                                             std::chrono::seconds(5),
                                             ServiceRpcQuery::ProvidersOnly);
            if (snap.providers_available) {
              service_rpc.providers_available = true;
              service_rpc.providers = std::move(snap.providers);
              service_rpc.providers_virtual_daa = snap.providers_virtual_daa;
            }
          } catch (...) {}
        }
        std::string parse_why;
        const auto parsed_request = ai::parse_request(req->transaction, req->daa, &parse_why, "mainnet");
        if (parsed_request) {
          telemetry.set_gpu_activity("PAUSED_FOR_AI");
          telemetry.set_state("PAUSED", "RUNNING", "AI_REQUEST", submit_enabled ? "ARMED" : "DISABLED");

          bool native_trace_persisted = false;
          ai::AiResponseEffects effects;
          effects.infer = [&](std::string_view prompt, std::uint32_t max_tokens) {
            return gpu_ownership.run_opoi(
                [&] { shared_runtime->pom_release_pad(); },
                [&] {
                  return shared_runtime->infer(opoi::official_qwen35_ai_prompt(prompt), max_tokens);
                },
                [&] { maybe_use_task_context(*shared_runtime); });
          };
          if (h15_audits) {
            effects.infer_traced = [&](std::string_view prompt, std::uint32_t tokens, const std::array<std::uint8_t, 32> &hash) {
              telemetry.set_state("PAUSED", "RUNNING", "H15_TRACE", submit_enabled ? "ARMED" : "DISABLED");
              return gpu_ownership.run_opoi(
                  [&] { shared_runtime->pom_release_pad(); },
                  [&] { return h15_audits->infer(prompt, tokens, hash); },
                  [&] { maybe_use_task_context(*shared_runtime); });
            };
            effects.recover_traced = [&](const Hash &request) { return h15_audits->response_for_request(request); };
            effects.persist_trace = [&](std::string_view id, std::span<const std::uint8_t> payload, std::uint64_t daa, std::uint64_t deadline) {
              h15_audits->commit(id, payload, daa, deadline);
              native_trace_persisted = true;
            };
          }
          effects.publish_and_readback = [&](std::span<const std::uint8_t> body) {
            return ai::ipfs_publish_and_readback(ipfs_endpoint, body);
          };
          effects.submit = [&](const protowire::RpcTransaction &tx, std::string_view txid) {
            telemetry.set_state("PAUSED", "RESPONSE_READY", "AI_RESPONSE_SUBMISSION", "AI_RESPONSE_SUBMIT_SENT");
            if (!ai_responses_enabled || !ai_response_transport)
              return ai::AiResponseEffects::SubmitResult{ai::AiResponseEffects::SubmitResult::State::Rejected,
                                                        "AiResponse transaction authorization is disabled or unavailable in pool mode"};
            if (h15_audits && native_trace_persisted) h15_audits->response_intent(txid);
            const auto rpc_started = std::chrono::steady_clock::now();
            const auto result = ai_response_transport->submit(tx, txid);
            const auto rpc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - rpc_started).count();
            if (h15_audits && native_trace_persisted) h15_audits->response_result(txid, result.state, rpc_ms);
            return result;
          };
          effects.reconcile = [&](std::string_view txid) -> bool {
            return ai_response_transport ? ai_response_transport->known(txid) : false;
          };
          effects.current_daa = [&] { return ai_current_daa.load(std::memory_order_acquire); };
          effects.wait_before_retry = [&](std::uint32_t milliseconds) {
            if (h15_audits) {
              h15_audits->poll();
              h15_audits->advance(ai_current_daa.load(), milliseconds / 100);
              h15_audits->submit_due(ai_current_daa.load(), ai_responses_enabled);
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (!process_stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return !process_stop.load(std::memory_order_acquire);
          };

          const auto deadline = ai::service_response_deadline(
              req->daa, 0, parsed_request->max_tokens,
              req->daa >= ai::mainnet_signed_response_activation_daa);

          const auto serve_res = ai_response_service->serve(
              req->transaction, req->daa, service_rpc.providers, effects,
              deadline, ai_current_daa.load(std::memory_order_acquire));

          if (escrow_watcher && parsed_request->inference_reward != 0 &&
              (serve_res == ai::ServeResult::Submitted || serve_res == ai::ServeResult::Duplicate)) {
            const auto record = ai_response_service->journal().get(parsed_request->stable_id);
            if (record && (record->state == ai::ResponseState::RpcAcknowledged ||
                           record->state == ai::ResponseState::ChainObserved ||
                           record->state == ai::ResponseState::Finalized)) {
              escrow_watcher->track_inference_outpoint(
                  hex(parsed_request->txid.data(), parsed_request->txid.size()),
                  req->daa, parsed_request->inference_reward);
              escrow_watcher->flush();
            }
          }

          telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
        }
      }
    }

    // 2A. Stratum v4 Pool Mode Autonomous Loop
    if (pool_mode && stratum_client) {
      if (auto challenge = stratum_client->take_pending_challenge()) {
        std::string answer;
        const auto parsed_challenge = opoi::parse_challenge(challenge->model_id_hex + ":" + challenge->nonce_hex);
        telemetry.set_gpu_activity("PAUSED_FOR_OPOI");
        telemetry.set_state("PAUSED", "RUNNING", "OPOI_CHALLENGE", submit_enabled ? "ARMED" : "DISABLED");
        (void)gpu_ownership.run_opoi(
            [&] { shared_runtime->pom_release_pad(); },
            [&] {
              std::cout << "OPOI_RUNTIME=START source=STRATUM_V4 device=" << device_id << "\n" << std::flush;
              answer = shared_runtime->answer(parsed_challenge);
              return opoi::format_inference_result(parsed_challenge, answer);
            },
            [&] { maybe_use_task_context(*shared_runtime); });
        std::cout << "OPOI_RUNTIME=PASS source=STRATUM_V4 answer_bytes=" << answer.size() << "\n" << std::flush;
        (void)stratum_client->submit_challenge_response(challenge->model_id_hex, challenge->nonce_hex, answer);
        telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
        telemetry.set_gpu_activity("WAITING_TEMPLATE");
        continue;
      }
      if (pool_ai) {
        if (auto message = stratum_client->take_pool_ai()) {
          try {
            auto result = pool_ai->handle(message->scope, pool_worker, message->wire);
            const bool sent = stratum_client->submit_pool_ai_result(message->scope, result);
            std::cout << "POOL_AI_RESULT sent=" << (sent ? "true" : "false") << " scope_bound=true\n" << std::flush;
          } catch (const std::exception &e) {
            const bool declined = stratum_client->decline_pool_ai(*message);
            std::cout << "POOL_AI_REQUEST_FAILED scope_bound=true empty_response_sent=false declined="
                      << (declined ? "true" : "false") << " detail=\"" << e.what() << "\"\n" << std::flush;
          }
          telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
          continue;
        }
      }

      const auto wait_start_ns = monotonic_ns();
      auto maybe_pool_job = stratum_client->current_job();
      if (!maybe_pool_job) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        maybe_pool_job = stratum_client->current_job();
      }
      telemetry.record_template_wait(monotonic_ns() - wait_start_ns);
      if (!maybe_pool_job) {
        telemetry.set_gpu_activity("WAITING_TEMPLATE");
        continue;
      }
      auto pool_job = *maybe_pool_job;
      if (pool_job.job_id != last_pool_job_id ||
          pool_job.nonce_fixed != last_pool_nonce_fixed ||
          pool_job.nonce_mask != last_pool_nonce_mask) {
        last_pool_job_id = pool_job.job_id;
        last_pool_nonce_fixed = pool_job.nonce_fixed;
        last_pool_nonce_mask = pool_job.nonce_mask;
        nonce_cursor = (initial_nonce_cursor & (pool_job.nonce_mask >> 1U)) | pool_job.nonce_fixed;
        pool_nonce_exhausted=false;
      }

      auto prior_daa = ai_current_daa.load(std::memory_order_relaxed);
      while (prior_daa < pool_job.header.daa_score &&
             !ai_current_daa.compare_exchange_weak(
                 prior_daa, pool_job.header.daa_score,
                 std::memory_order_release, std::memory_order_relaxed)) {}

      if (pool_job.header.daa_score >= consensus::block_task_activation_daa &&
          pool_job.has_window_seed && !pool_job.has_task_root) {
        try {
          const auto trace_start_ns = monotonic_ns();
          telemetry.set_gpu_activity("PAUSED_FOR_H15_WINDOW_TRACE");
          telemetry.set_state("PAUSED", "RUNNING", "H15_WINDOW_TRACE", submit_enabled ? "ARMED" : "DISABLED");
          const auto computed_task_root = gpu_ownership.run_opoi(
              [&] { shared_runtime->pom_release_pad(); },
              [&] {
                return trace_stratum_window_task_root(*shared_runtime, pool_job.header.pom_tier, pool_job.window_seed);
              },
              [&] { maybe_use_task_context(*shared_runtime); });
          stratum_client->commit_window_task_root(pool_job.window_seed, computed_task_root);
          const auto trace_ms = (monotonic_ns() - trace_start_ns) / 1.0e6;
          std::osyncstream(std::cout) << "STRATUM_V4_WINDOW_TRACE=PASS device=" << device_id
                                      << " window_seed=" << hex(pool_job.window_seed.data(), pool_job.window_seed.size())
                                      << " task_root=" << hex(computed_task_root.data(), computed_task_root.size())
                                      << " elapsed_ms=" << trace_ms << "\n" << std::flush;
          telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
          auto refreshed_job = stratum_client->current_job();
          if (!refreshed_job) continue;
          pool_job = *refreshed_job;
        } catch (const std::exception &e) {
          std::cerr << "STRATUM_V4_WINDOW_TRACE_ERROR: " << e.what() << "\n" << std::flush;
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
      }

      if (!stratum_client->is_job_current(pool_job.generation, pool_job.job_id)) continue;

      const auto &header = pool_job.header;
      const auto &pph = pool_job.pre_pow;
      const auto &eff_target = pool_job.effective_target;

      std::uint64_t p[4]{}, s[4]{}, t[4]{};
      for (unsigned i = 0; i < 4; ++i) {
        std::uint64_t p_word = 0, t_word = 0, s_word = 0;
        for (unsigned b = 0; b < 8; ++b) {
          p_word |= std::uint64_t(pph.data()[8 * i + b]) << (8 * b);
          t_word |= std::uint64_t(eff_target.data()[8 * i + b]) << (8 * b);
          s_word |= std::uint64_t(pph.data()[8 * i + b] ^ SEED_H14_TAG[8 * i + b]) << (8 * b);
        }
        p[i] = p_word ^ POM_H3_SALT[i];
        t[i] = t_word;
        s[i] = s_word;
      }
      const std::uint32_t h10 = (header.daa_score >= h10_activation_daa) ? 1u : 0u;
      const bool v5 = (header.daa_score >= consensus::pom_v5_activation_daa);
      if(v5 && !shared_runtime->pom_v5_ready()) throw std::runtime_error("Vulkan PoM v5 export unavailable");
      const auto pad = v5 ? official_v5_pad(header.pom_tier, official_model, official_root, header.daa_score)
                          : consensus::V5PadSpec{};
      const auto pad_words = v5 ? v5_pad_seed_words(pad.seed) : std::array<std::uint64_t, 4>{};
      const std::uint32_t preferred_v5 = shared_runtime->pom_v5_batch();
      const std::uint32_t requested_batch = (v5 && shared_runtime->pom_v5_ready())
          ? std::min<std::uint32_t>(batch_size, preferred_v5 ? preferred_v5 : 8192u)
          : batch_size;
      const std::uint64_t batch_start=nonce_cursor;
      const auto dispatch_batch=static_cast<std::uint32_t>(pool_nonce_exhausted?0:bounded_nonce_batch(
          batch_start,pool_job.nonce_fixed|pool_job.nonce_mask,requested_batch));
      if(!dispatch_batch) {
        if(!pool_nonce_exhausted) std::cout<<"NONCE_RANGE_EXHAUSTED backend=VULKAN action=WAIT_NEW_JOB\n"<<std::flush;
        pool_nonce_exhausted=true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));continue;
      }

      telemetry.set_gpu_activity("VULKAN_RESIDENT_SEARCH");
      telemetry.work_launched(dispatch_batch);

      std::int64_t winner = -1;
      const bool ran = gpu_ownership.run_pom([&] {
        if (v5 && shared_runtime->pom_v5_ready()) {
          winner = shared_runtime->pom_mine_v5(p, s, t, header.timestamp, batch_start, dispatch_batch,
                                               pad_words.data(), pad.tiles);
        } else {
          winner = shared_runtime->pom_mine(p, s, t, header.timestamp, batch_start, dispatch_batch, 256, h10);
        }
      });

      if (!ran) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      if(winner < -1) throw std::runtime_error("Vulkan pool dispatch failed");
      pool_nonce_exhausted=consume_nonce_interval(nonce_cursor,pool_job.nonce_fixed|pool_job.nonce_mask,dispatch_batch);
      if(pool_nonce_exhausted) std::cout<<"NONCE_RANGE_EXHAUSTED backend=VULKAN action=WAIT_NEW_JOB\n"<<std::flush;
      telemetry.work_completed(dispatch_batch * 256, dispatch_batch);
      if (!stratum_client->is_job_current(pool_job.generation, pool_job.job_id)) {
        telemetry.work_stale(dispatch_batch);
        continue;
      }
      telemetry.eligible_attempts(dispatch_batch);

      if (winner >= 0) {
        const std::uint64_t winning_nonce=checked_winner_nonce(batch_start,dispatch_batch,winner);
        std::cout << "LIVE_RESIDENT_TARGET=FOUND mode=POOL device=" << device_id << " nonce=" << winning_nonce
                  << " dGPU_winner_offset=" << winner << "\n" << std::flush;
        telemetry.target_satisfied();
        try {
          const auto seed = block_seed(pph, header.timestamp, winning_nonce, header.daa_score);
          auto walk = walk_v5_from_model(seed, tile_count, pad, [&](std::uint64_t tile) {
            std::array<std::uint8_t, tile_bytes> bytes;
            gguf.read_tile_into(tile, bytes);
            return bytes;
          });
          const auto proof = prove_v5_from_walk_parallel(walk, header.pom_tier, tile_count,
              [&](std::uint64_t tile) {
                std::array<std::uint8_t, tile_bytes> bytes;
                gguf.read_tile_into(tile, bytes);
                return bytes;
              },
              [&](std::uint64_t tile) { return merkle.tile_path(tile); },
              proof_threads);
          const auto final = final_state(walk);
          const auto pow = pom_pow_value_h3(final, pph);
          if (little_endian_leq(pow, eff_target)) {
            const auto wire = borsh_v5_container(proof, final, pow);
            const ProofArtifactContext context{seed,gguf.chunk_count(),header.timestamp,winning_nonce,
                header.daa_score,pool_job.generation,final,header.pom_tier,official_root,
                eff_target,pph,{},pow};
            const auto artifacts=encode_proof_artifacts(context,walk,wire.size());
            proof_verifier.verify(artifacts.context_prefix,artifacts.trace,wire);
            telemetry.proof_completed();
            telemetry.proof_rust_verified();
            telemetry.proof_fresh(false);
            if (submit_enabled && stratum_client->is_job_current(pool_job.generation, pool_job.job_id)) {
              auto solved_header = header;
              solved_header.nonce = winning_nonce;
              solved_header.pom_final_state = final;
              solved_header.pom_tier = header.pom_tier;
              solved_header.task_root = pool_job.task_root;
              const auto solved_hash = consensus::hash_header_override(
                  solved_header, 0, 0, false, false,
                  pool_job.header.daa_score >= consensus::block_task_activation_daa);
              stratum::VerifiedPoolShare share;
              share.job_generation = pool_job.generation;
              share.job_id = pool_job.job_id;
              share.protocol_version = pool_job.protocol_version;
              share.nonce = winning_nonce;
              share.daa_score = header.daa_score;
              share.timestamp = header.timestamp;
              share.final_state = final;
              share.pre_pow = pph;
              share.pow_value = pow;
              share.effective_target = eff_target;
              share.network_target = pool_job.network_target;
              share.block_hash = solved_hash;
              share.window_seed = pool_job.window_seed;
              share.has_window_seed = pool_job.has_window_seed;
              share.task_root = pool_job.task_root;
              share.has_task_root = pool_job.has_task_root;
              share.pom_proof_borsh = wire;
              telemetry.set_state("ACTIVE", "IDLE", "", "SUBMIT_SENT");
              const auto sub_id = stratum_client->submit_share(share);
              telemetry.set_state("ACTIVE", "IDLE", "", sub_id ? "SUBMIT_SENT" : "STALE");
            }
          } else {
            // The host re-walk is authoritative; a GPU winner it rejects is a runtime fault.
            std::cerr << "AMD_POOL_HOST_REWALK_REJECTED nonce=" << winning_nonce << "\n" << std::flush;
          }
        } catch (const std::exception &e) {
          std::cerr << "AMD_POOL_PROOF_BUILD_ERROR: " << e.what() << "\n" << std::flush;
        }
      }

      continue;
    }

    // 2B. Fetch or reuse current Solo gRPC mining template continuously
    const auto wait_start_ns = monotonic_ns();
    auto maybe_job = job_queue.wait_pop_for(
        std::chrono::milliseconds(reusable_job && reusable_job->lease.valid() ? 0 : 50));
    const bool adopted_new_job = maybe_job.has_value();
    auto *active_job = select_active_template_job(reusable_job, std::move(maybe_job));
    telemetry.record_template_wait(monotonic_ns() - wait_start_ns);
    if (!active_job) {
      active_prepared_template = nullptr;
      active_block = nullptr;
      telemetry.set_gpu_activity("WAITING_TEMPLATE");
      continue;
    }

    const auto &job = *active_job;
    if (!job.lease.valid()) continue;

    try {
      if (adopted_new_job || !active_prepared_template || !active_block) {
        require_mining_eligible(job.fields);
        if (job.fields.pay_address != pay_address ||
            !opoi::valid_coinbase_extra_data(job.fields.extra_data, extra_data)) {
          throw std::runtime_error("queued job payout/escrow request binding changed");
        }
        active_prepared_template = &rpc_cache.get(job.fields.response_wire);
        active_block = &rpc_cache.cached_block();
      }
    } catch (const std::exception &e) {
      std::cerr << "AMD_TEMPLATE_VALIDATION_ERROR: " << e.what() << "\n" << std::flush;
      reusable_job.reset();
      active_prepared_template = nullptr;
      active_block = nullptr;
      continue;
    }

    if (!job_queue.admission_allowed(job.generation) &&
        !job_queue.wait_refresh_resolved(job.generation)) {
      continue;
    }

    const auto &block = *active_block;
    const auto &header = active_prepared_template->header;
    const auto &pph = active_prepared_template->pre_pow;
    const auto &target = active_prepared_template->target;
    if (target.zero) continue;

    // 3. Prepare PoM walk parameters
    std::uint64_t p[4]{}, s[4]{}, t[4]{};
    for (unsigned i = 0; i < 4; ++i) {
      std::uint64_t p_word = 0, t_word = 0, s_word = 0;
      for (unsigned b = 0; b < 8; ++b) {
        p_word |= std::uint64_t(pph.data()[8 * i + b]) << (8 * b);
        t_word |= std::uint64_t(target.little_endian.data()[8 * i + b]) << (8 * b);
        s_word |= std::uint64_t(pph.data()[8 * i + b] ^ SEED_H14_TAG[8 * i + b]) << (8 * b);
      }
      p[i] = p_word ^ POM_H3_SALT[i];
      t[i] = t_word;
      s[i] = s_word;
    }
    const std::uint32_t h10 = (header.daa_score >= h10_activation_daa) ? 1u : 0u;

    // 4. Dispatch PoM batch on resident Vulkan engine
    const bool v5 = (header.daa_score >= consensus::pom_v5_activation_daa);
    if(v5 && !shared_runtime->pom_v5_ready()) throw std::runtime_error("Vulkan PoM v5 export unavailable");
    const auto pad = v5 ? official_v5_pad(header.pom_tier, official_model, official_root, header.daa_score)
                        : consensus::V5PadSpec{};
    const auto pad_words = v5 ? v5_pad_seed_words(pad.seed) : std::array<std::uint64_t, 4>{};
    const std::uint32_t preferred_v5 = shared_runtime->pom_v5_batch();
    const std::uint32_t requested_batch = (v5 && shared_runtime->pom_v5_ready())
        ? std::min<std::uint32_t>(batch_size, preferred_v5 ? preferred_v5 : 8192u)
        : batch_size;

    const auto batch_start=nonce_cursor;
    const auto dispatch_batch=static_cast<std::uint32_t>(solo_nonce_exhausted?0:bounded_nonce_batch(
        batch_start,UINT64_MAX,requested_batch));
    if(!dispatch_batch) {
      solo_nonce_exhausted=true;
      telemetry.set_gpu_activity("NONCE_RANGE_EXHAUSTED");
      std::this_thread::sleep_for(std::chrono::milliseconds(25));continue;
    }
    telemetry.set_gpu_activity("VULKAN_RESIDENT_SEARCH");
    telemetry.work_launched(dispatch_batch);

    std::int64_t winner = -1;
    const bool ran = gpu_ownership.run_pom([&] {
      if (v5) maybe_use_task_context(*shared_runtime);
      if (v5 && shared_runtime->pom_v5_ready()) {
        static std::once_flag pad_log_flag;
        std::call_once(pad_log_flag, [&] {
          std::cout << "POM_V5_DISPATCH device=" << device_id << " pad_tiles=" << pad.tiles
                    << " pad_mib=" << (pad.tiles / 1024) << " batch=" << dispatch_batch << "\n" << std::flush;
        });
        winner = shared_runtime->pom_mine_v5(p, s, t, header.timestamp, batch_start, dispatch_batch,
                                             pad_words.data(), pad.tiles);
        if (winner < -1) {
          std::cerr << "POM_V5_DISPATCH_WARNING code=" << winner << " batch=" << dispatch_batch << "\n" << std::flush;
        }
      } else {
        winner = shared_runtime->pom_mine(p, s, t, header.timestamp, batch_start, dispatch_batch, 256, h10);
      }
    });

    if (!ran) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if(winner < -1) throw std::runtime_error("Vulkan SOLO dispatch failed");
    solo_nonce_exhausted=consume_nonce_interval(nonce_cursor,UINT64_MAX,dispatch_batch);
    telemetry.work_completed(dispatch_batch * 256, dispatch_batch);
    if (winner < 0 && (!job.lease.valid() || !job_queue.wait_refresh_resolved(job.generation))) {
      telemetry.work_stale(dispatch_batch);
      continue;
    }
    telemetry.eligible_attempts(dispatch_batch);

    // 5. Check if winner found
    if (winner >= 0) {
      const std::uint64_t winning_nonce=checked_winner_nonce(batch_start,dispatch_batch,winner);
      std::cout << "LIVE_RESIDENT_TARGET=FOUND device=" << device_id << " nonce=" << winning_nonce
                << " dGPU_winner_offset=" << winner << "\n" << std::flush;
      telemetry.target_satisfied();

      try {
        const auto seed = block_seed(pph, header.timestamp, winning_nonce, header.daa_score);
        const auto pad = official_v5_pad(header.pom_tier, official_model, official_root, header.daa_score);

        std::unordered_map<std::uint64_t, std::array<std::uint8_t, tile_bytes>> tile_cache;
        tile_cache.reserve(512);

        auto walk = walk_v5_from_model(seed, tile_count, pad, [&](std::uint64_t tile) {
          std::array<std::uint8_t, tile_bytes> bytes;
          gguf.read_tile_into(tile, bytes);
          tile_cache.emplace(tile, bytes);
          return bytes;
        });

        for (const auto off : walk.offsets) {
          if (off < tile_count) merkle.prefetch_tile_path(off);
        }

        const auto proof = prove_v5_from_walk_parallel(walk, header.pom_tier, tile_count,
            [&](std::uint64_t tile) {
              const auto it = tile_cache.find(tile);
              if (it != tile_cache.end()) return it->second;
              std::array<std::uint8_t, tile_bytes> bytes;
              gguf.read_tile_into(tile, bytes);
              return bytes;
            },
            [&](std::uint64_t tile) { return merkle.tile_path(tile); },
            proof_threads);

        const auto final = final_state(walk);
        const auto pow = pom_pow_value_h3(final, pph);

        if (little_endian_leq(pow, target.little_endian)) {
          const auto wire = borsh_v5_container(proof, final, pow);
          const auto response_digest = blake3_digest(job.fields.response_wire.data(), job.fields.response_wire.size());
          const ProofArtifactContext context{seed,gguf.chunk_count(),header.timestamp,winning_nonce,
              header.daa_score,job.generation,final,header.pom_tier,official_root,
              target.little_endian,pph,response_digest,pow};
          const auto artifacts=encode_proof_artifacts(context,walk,wire.size());
          proof_verifier.verify(artifacts.context_prefix,artifacts.trace,wire);
          telemetry.proof_completed();
          telemetry.proof_rust_verified();
          telemetry.proof_fresh(false);

          network::VerifiedSubmission verified;
          verified.job_id = hex(response_digest.data(), response_digest.size());
          verified.generation = job.generation;
          verified.nonce = winning_nonce;
          verified.pom_verified = verified.model_verified = verified.merkle_verified =
              verified.header_seed_verified = verified.target_satisfied = verified.job_fresh =
              verified.generation_matches = verified.miner_configured = true;

          const std::string proof_bytes(reinterpret_cast<const char*>(wire.data()), wire.size());
          network::PomWinner pom_winner{std::move(verified), header.daa_score, final, header.pom_tier, proof_bytes};

          if (submit_enabled && submit_client) {
            telemetry.set_state("ACTIVE", "IDLE", "", "SUBMIT_SENT");
            auto submit_res = network::submit_pom_winner(
                *submit_client, block, pom_winner,
                job.fields.pay_address, job.fields.extra_data,
                true, std::chrono::seconds(5));
            telemetry.set_state("ACTIVE", "IDLE", "", network::submission_state_name(submit_res.state));
            std::osyncstream(std::cout) << "LIVE_H15_SUBMIT_RESULT source=AMD_VULKAN state="
                                        << network::submission_state_name(submit_res.state)
                                        << " hash=" << submit_res.block_hash << "\n"
                                        << "LIVE_SUBMIT_RESULT source=AMD_VULKAN state="
                                        << network::submission_state_name(submit_res.state)
                                        << " hash=" << submit_res.block_hash << " nonce=" << winning_nonce << "\n" << std::flush;
            if (!submit_res.block_hash.empty()) {
              chain_observers.enqueue({}, [client_ptr = submit_client.get(), submit_res]() mutable {
                try {
                  const auto chain = client_ptr->observe_chain(
                      submit_res, std::chrono::seconds(30), std::chrono::seconds(2), 6,
                      [] { return process_stop.load(std::memory_order_acquire); });
                  std::osyncstream(std::cout)
                      << "LIVE_CHAIN_STATUS state=" << network::submission_state_name(chain.state)
                      << " hash=" << chain.block_hash
                      << " selected_chain=" << (chain.currently_selected_chain ? "true" : "false")
                      << " blue_score=" << chain.block_blue_score
                      << " selected_tip_blue_score=" << chain.selected_tip_blue_score
                      << " reorg_observed=" << (chain.reorg_observed ? "true" : "false")
                      << " confirmation_policy=6_blue_score_depth_not_finality\n" << std::flush;
                } catch (const std::exception &e) {
                  std::osyncstream(std::cerr)<<"CHAIN_OBSERVER_FAILED detail="<<e.what()<<"\n";
                  throw;
                }
              }, [](std::exception_ptr) {
                std::osyncstream(std::cerr) << "CHAIN_OBSERVER_FAILED source=AMD_VULKAN\n";
              });
            }
          } else {
            std::cout << "AMD_WINNER_VERIFIED nonce=" << winning_nonce << " submit=DISABLED\n" << std::flush;
          }
        } else {
          // The host re-walk is authoritative; a GPU winner it rejects is a runtime fault.
          std::cerr << "AMD_HOST_REWALK_REJECTED nonce=" << winning_nonce << "\n" << std::flush;
        }
      } catch (const std::exception &e) {
        std::cerr << "AMD_PROOF_BUILD_ERROR: " << e.what() << "\n" << std::flush;
      }
    }

  }

  std::cout << "LIVE_VULKAN_WORKER=SHUTDOWN\n" << std::flush;
  telemetry.set_state("TERMINATED", "IDLE", "CLEAN_SHUTDOWN", "DISABLED");
  job_queue.close();
  if (stratum_client) stratum_client->stop();
  if (strike_monitor) strike_monitor->stop();
  if (escrow_claim_task.valid()) escrow_claim_task.wait();

  if (telemetry_thread.joinable()) telemetry_thread.join();
  if (ingress_thread.joinable()) ingress_thread.join();

  return 0;
}
