#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <deque>
#include <unordered_set>
#include <vector>

#include "keryx/nexus/ai_service.hpp"

namespace google::protobuf { class Message; }
namespace protowire { class RpcBlock; }

namespace keryx::nexus { class LiveJobQueue; }

namespace keryx::nexus {
// Called for blocks from the official BlockAdded subscription and startup backfill.
// Implementations must be bounded/nonblocking and must never infer inline.
using AiBlockObserver = std::function<void(const protowire::RpcBlock &, bool backfill)>;
using AiRemovedChainObserver = std::function<void(const std::vector<std::string> &removed_hashes)>;
using AiBackfillCompleteObserver = std::function<void(bool coverage_complete,
                                                       std::size_t hashes_scheduled,
                                                       std::size_t blocks_unavailable)>;

struct NodeReadiness {
  std::string network;
  std::string version;
  bool synced{};
};

enum class BlockValidationState { Pending, ChainValid, OffChain, Unknown, Error };

struct BlockChainStatus {
  std::string hash;
  std::optional<bool> selected_chain;
  std::string error;
  BlockValidationState state{BlockValidationState::Pending};
};

// Shared stock-compatible ancestor traversal used by live startup backfill and
// the read-only audit command. It tracks hashes once, bounds memory, and walks
// level-zero parents only while the block is inside the requested DAA window.
class AiBackfillTraversal final {
public:
  AiBackfillTraversal(std::uint64_t window_daa = 6000,
                      std::size_t max_blocks = 50000);
  bool seed(std::uint64_t tip_daa, const std::vector<std::string> &parent_hashes);
  bool seed_from_block(const protowire::RpcBlock &block);
  bool note_block(const protowire::RpcBlock &block);
  std::optional<std::string> pop();
  [[nodiscard]] bool seeded() const noexcept { return cutoff_daa_.has_value(); }
  [[nodiscard]] bool drained() const noexcept { return seeded() && queue_.empty(); }
  [[nodiscard]] std::uint64_t cutoff_daa() const noexcept { return cutoff_daa_.value_or(0); }
  [[nodiscard]] std::size_t scheduled_count() const noexcept { return visited_.size(); }
  [[nodiscard]] bool at_limit() const noexcept { return at_limit_; }
private:
  bool enqueue_parents(const protowire::RpcBlock &block);
  std::uint64_t window_daa_;
  std::size_t max_blocks_;
  std::optional<std::uint64_t> cutoff_daa_;
  std::deque<std::string> queue_;
  std::unordered_set<std::string> visited_;
  bool at_limit_{};
};

struct AiBackfillScanOptions {
  std::uint64_t window_daa{6000};
  std::size_t max_inflight{2};
  std::size_t max_blocks{50000};
  std::chrono::seconds timeout{600};
  std::chrono::milliseconds settle{1000};
};
struct AiSelectedChainChange {
  std::size_t sequence{};
  std::vector<std::string> removed;
  std::vector<std::string> added;
  bool duplicate{};
  bool out_of_order{};
  bool reinclusion{};
};
struct AiBackfillScanResult {
  std::string network;
  std::string version;
  bool synced{};
  std::uint64_t initial_daa{};
  std::uint64_t final_daa{};
  std::uint64_t cutoff_daa{};
  std::string initial_selected_sink;
  std::uint64_t watermark_daa{};
  std::string watermark_selected_sink;
  std::size_t hashes_scheduled{};
  std::size_t blocks_recovered{};
  std::size_t blocks_unavailable{};
  std::size_t rpc_errors{};
  std::size_t live_blocks_seen{};
  std::size_t live_blocks_recovered{};
  std::size_t live_blocks_unavailable{};
  std::size_t live_duplicate_notifications{};
  std::size_t live_snapshot_deduplicated{};
  std::size_t live_queue_overflow{};
  std::size_t live_watermark_events{};
  bool live_watermark_complete{};
  std::size_t selected_chain_reorgs{};
  std::size_t selected_chain_advances{};
  std::size_t selected_chain_reinclusions{};
  std::size_t selected_chain_blocks_removed{};
  std::size_t selected_chain_blocks_added{};
  std::size_t selected_chain_blocks_reincluded{};
  std::size_t selected_chain_duplicate_events{};
  std::size_t selected_chain_out_of_order_events{};
  std::vector<AiSelectedChainChange> selected_chain_changes;
  bool reorg_during_scan{};
  bool complete{};
  bool limit_reached{};
  bool timed_out{};
};
using AiBackfillBlockObserver = std::function<void(const protowire::RpcBlock &, bool backfill)>;
// Read-only stream: subscribes to block additions before taking its DAG snapshot,
// never asks for templates, and scans the same bounded ancestor graph as startup.
AiBackfillScanResult run_ai_backfill_scan(
    const std::string &endpoint, const AiBackfillScanOptions &options,
    const std::atomic_bool &stop_requested,
    const AiBackfillBlockObserver &observer);

// Bounded read-only GetCurrentNetwork/GetInfo probe. Plaintext endpoints are
// accepted only for numeric loopback addresses, like the live job client.
NodeReadiness probe_node_readiness(const std::string &endpoint,
                                   std::chrono::milliseconds timeout);
// Read-only check of the local Kubo HTTP API. Never starts/stops IPFS.
bool probe_local_ipfs_api(std::chrono::milliseconds timeout);
// Read-only official GetServiceProviders/GetServiceStrikes query. API errors remain
// explicit; callers must not convert an unavailable strike response into strike=0.
ai::ServiceRpcSnapshot probe_ai_service_rpc(const std::string &endpoint,
                                           std::uint64_t provider_window_daa,
                                           std::string_view service_identity_hex,
                                           std::chrono::milliseconds timeout);
// Keep assignment discovery independent of strike-index latency. The existing
// overload continues to fetch both snapshots for callers that require both.
enum class ServiceRpcQuery { All, ProvidersOnly, StrikesOnly };
ai::ServiceRpcSnapshot probe_ai_service_rpc(const std::string &endpoint,
                                           std::uint64_t provider_window_daa,
                                           std::string_view service_identity_hex,
                                           std::chrono::milliseconds timeout,
                                           ServiceRpcQuery query);
// nullopt means node/error could not establish status; bool is the current selected-chain verdict.
std::optional<bool> probe_block_selected_chain(const std::string &endpoint,
                                               std::string_view block_hash,
                                               std::chrono::milliseconds timeout,
                                               std::string *error = nullptr);
std::vector<BlockChainStatus> probe_blocks_selected_chain(
    const std::string &endpoint, const std::vector<std::string> &block_hashes,
    std::chrono::milliseconds timeout);

// Serialize every consensus-bearing RpcBlock field while excluding only the
// node-populated verbose diagnostics. Used by live job identity and regressions.
std::vector<std::uint8_t> canonical_consensus_block_identity(
    const google::protobuf::Message &block);

int run_read_only_client(const std::string &endpoint, const std::string &pay_address,
                         const std::string &extra_data, const std::string &capture_path,
                         std::uint64_t max_templates, unsigned reconnect_after_seconds,
                         const std::atomic_bool &stop_requested);

// Live ingress variant. Templates are delivered directly to a bounded in-memory
// queue; capture_path is not opened or polled in this mode.
int run_live_job_client(const std::string &endpoint, const std::string &pay_address,
                        const std::string &extra_data, LiveJobQueue &queue,
                        std::uint64_t max_templates, unsigned reconnect_after_seconds,
                        const std::atomic_bool &stop_requested,
                        const std::function<std::string(const std::string &)> &inference_handler = {},
                        const AiBlockObserver &ai_block_observer = {},
                        const AiRemovedChainObserver &ai_removed_chain_observer = {},
                        const AiBackfillCompleteObserver &backfill_complete_observer = {});
}
