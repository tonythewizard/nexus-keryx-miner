#pragma once

#include "keryx/nexus/ai_service.hpp"
#include "rpc.pb.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace keryx::nexus::ai {

enum class ResponseState {
  Discovered, Assigned, RequestValidated, Inferencing, ResponsePrepared,
  ResponseSigned, ResponsePublished, TransactionPrepared, Submitting,
  RpcAcknowledged, RpcRejected, UnknownAfterTimeout, ChainObserved,
  Finalized, Expired, Failed, NotAssigned, ResponseReorged
};

const char *response_state_name(ResponseState state) noexcept;

struct ResponseJournalRecord {
  std::string request_id;
  std::string transaction_id;
  ResponseState state{ResponseState::Discovered};
};

// Append-only, checksummed journal. Every state transition is fsynced before a side effect.
// It stores IDs and status only; prompts, private envelopes, keys, and generated answers never
// enter the journal. Ambiguous SUBMITTING records require caller reconciliation before retry.
class ResponseJournal final {
public:
  explicit ResponseJournal(std::filesystem::path path);
  void recover();
  bool begin(std::string request_id);
  // A crash before SUBMITTING has no transaction side effect. Re-run inference and preparation
  // from the immutable request; never reset a record that could already have reached the node.
  bool reconsider_unsubmitted_not_assigned(std::string_view request_id);
  bool reset_pre_submission(std::string_view request_id);
  // Only after the native audit store verifies the persisted signed response.
  bool recover_persisted_response(std::string_view request_id,std::string_view txid);
  bool transition(std::string_view request_id, ResponseState next,
                  std::string transaction_id = {});
  // A selected-chain removal is distinct from RPC acknowledgement. Keep the
  // request identity and old response txid so reinclusion/retry can reconcile.
  bool mark_response_reorged(std::string_view request_id);
  std::optional<ResponseJournalRecord> get(std::string_view request_id) const;
  std::vector<ResponseJournalRecord> unresolved() const;
  // Read-only point-in-time copy for audit/reconciliation tools.
  std::vector<ResponseJournalRecord> snapshot() const;
private:
  void append_locked(const ResponseJournalRecord &record);
  std::filesystem::path path_;
  mutable std::mutex mutex_;
  // Query exact string_view spans without constructing a temporary ID string.
  struct RequestIdHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view id) const noexcept {
      return std::hash<std::string_view>{}(id);
    }
  };
  std::unordered_map<std::string, ResponseJournalRecord, RequestIdHash, std::equal_to<>> records_;
  template<class Index>
  static auto find_record(Index &index, std::string_view id) {
    // The common first-record case needs only an exact byte comparison; avoid
    // hashing a 64-byte ID when there cannot be a competing bucket entry.
    if (index.size() == 1) {
      const auto it = index.begin();
      return it->first == id ? it : index.end();
    }
    return index.find(id);
  }
};

struct AiTracedAnswer {
 std::string answer;
 std::vector<std::uint8_t> sealed_plaintext;
 std::array<std::uint8_t,73> commitment{};
};

struct AiResponseEffects {
  // These functions are injected by production adapters or isolated protocol mocks.
  std::function<void(std::string_view request_id)> assigned;
  // Optional monotonic phase durations; IDs/timings only, never prompt or answer.
  // Observability failures must not change transaction/journal semantics.
  std::function<void(std::string_view phase, std::uint64_t elapsed_ns)> phase_timing;
  std::function<std::string(std::string_view prompt, std::uint32_t max_tokens)> infer;
  std::function<AiTracedAnswer(std::string_view,std::uint32_t,const std::array<std::uint8_t,32>&)> infer_traced;
  // Must fsync trace and audit intent before SUBMITTING; never sends a transaction.
  std::function<void(std::string_view,std::span<const std::uint8_t>,std::uint64_t,std::uint64_t)> persist_trace;
  std::function<std::optional<std::string>(const std::array<std::uint8_t,32>&)> recover_traced;
  std::function<std::array<std::uint8_t, 34>(std::span<const std::uint8_t> body)> publish_and_readback;
  struct SubmitResult {
    enum class State { Acknowledged, Rejected, Unknown } state{State::Unknown};
    std::string detail;
  };
  std::function<SubmitResult(const protowire::RpcTransaction &, std::string_view txid)> submit;
  std::function<bool(std::string_view txid)> reconcile;
  std::function<std::uint64_t()> current_daa;
  // Return false to cancel. Production waits cooperatively; fixtures inject a clock.
  // Never retry an unknown transport outcome or regenerate a signed response.
  std::function<bool(std::uint32_t milliseconds)> wait_before_retry;
  // Only an allowlisted reason and a digest of the node message leave this adapter.
  std::function<void(std::string_view txid, std::uint32_t attempt,
                     std::string_view reason, std::string_view detail_sha256)> submission_diagnostic;
};

struct AiResponseConfig {
  bool native_h15{};
  std::array<std::uint8_t, 32> escrow_secret{};
  std::array<std::uint8_t, 32> escrow_pubkey{};
  std::array<std::uint8_t, 64> delegation_signature{};
  std::array<std::uint8_t, 32> model_id{};
  std::string payout_address;
  std::string service_identity;
  std::string network{"mainnet"};
  std::uint32_t tier{};
  std::uint64_t private_inline_activation_daa{};
  std::uint64_t signed_response_activation_daa{};
  std::filesystem::path journal_path;
};

enum class ServeResult { Submitted, Duplicate, NotAssigned, Expired, WaitingReconciliation, Failed };

class AiResponseService final {
public:
  explicit AiResponseService(AiResponseConfig config);
  void recover();
  // Persist an observed request identity before placing it in the volatile bounded work queue.
  // The node's startup block backfill supplies the immutable transaction again after restart.
  bool discover(const protowire::RpcTransaction &transaction, std::uint64_t containing_daa);
  // Mark only a stock-wire response with a valid responder signature as observed in a block.
  // Block/DAG observation is not selected-chain confirmation or finality.
  std::size_t observe_block(const protowire::RpcBlock &block);
  std::size_t observe_removed_chain_blocks(const std::vector<std::string> &block_hashes);
  ServeResult serve(const protowire::RpcTransaction &transaction,
                    std::uint64_t containing_daa,
                    const std::vector<ServiceProvider> &providers,
                    const AiResponseEffects &effects,
                    std::optional<std::uint64_t> authoritative_deadline = std::nullopt,
                    std::optional<std::uint64_t> current_daa = std::nullopt);
  const ResponseJournal &journal() const noexcept { return journal_; }
private:
  AiResponseConfig config_;
  ResponseJournal journal_;
  mutable std::mutex observed_mutex_;
  std::unordered_map<std::string, std::unordered_set<std::string>> response_blocks_;
  std::unordered_map<std::string, std::unordered_set<std::string>> blocks_by_response_;
};

// Official protocol primitives. Encrypted request bytes remain immutable; responder keys
// stay in Rust. Requester secrets use the upstream 32/33-byte tagged encoding.
// Bounded CPU-only estimate; opens and inflates a private prompt without logging it.
std::optional<std::uint32_t> stock_request_prompt_size(const protowire::RpcTransaction &,
    const std::array<std::uint8_t,32> &escrow_secret);
int stock_open_request(std::span<const std::uint8_t> payload,
                       const std::array<std::uint8_t, 32> &escrow_secret,
                       std::vector<std::uint8_t> &prompt,
                       bool &is_private);
std::vector<std::uint8_t> stock_seal_private_response(
    std::span<const std::uint8_t> request_payload,
    const std::array<std::uint8_t, 32> &request_hash,
    const std::array<std::uint8_t, 32> &escrow_secret,
    std::span<const std::uint8_t> answer);
std::vector<std::uint8_t> stock_build_signed_response(
    const std::array<std::uint8_t, 32> &request_hash,
    const std::array<std::uint8_t, 32> &escrow_secret,
    const std::array<std::uint8_t, 34> &cid,
    std::uint64_t response_daa, std::span<const std::uint8_t> answer,
    std::span<const std::uint8_t> private_body = {});
bool stock_verify_response(std::span<const std::uint8_t> payload,
                           const std::array<std::uint8_t, 32> &request_hash,
                           const std::array<std::uint8_t, 32> &escrow_pubkey,
                           const std::optional<std::vector<std::uint8_t>> &requester_secret,
                           std::vector<std::uint8_t> *opened_answer = nullptr);
// Authenticate the exact sealed bytes after signing; a miner never holds a requester secret.
bool stock_verify_built_response(std::span<const std::uint8_t> payload,
                           const std::array<std::uint8_t, 32> &request_hash,
                           const std::array<std::uint8_t, 32> &escrow_pubkey,
                           std::span<const std::uint8_t> sealed_body);
// Authenticate a public/private response observed on-chain without opening its
// encrypted answer. This does not attest inference correctness or finality.
bool stock_verify_response_signature(std::span<const std::uint8_t> payload,
                           const std::array<std::uint8_t, 32> &request_hash,
                           const std::array<std::uint8_t, 32> &escrow_pubkey);
bool stock_verify_escrow_delegation(std::string_view payout_address,
                                    const std::array<std::uint8_t, 32> &escrow_pubkey,
                                    const std::array<std::uint8_t, 64> &signature);
std::array<std::uint8_t, 32> stock_escrow_pubkey_from_secret(
    const std::array<std::uint8_t, 32> &secret);
std::optional<std::array<std::uint8_t, 32>> stock_response_request_hash(
    std::span<const std::uint8_t> payload);

// Production adapters. Public uploads are read back and byte-verified before CID use.
std::array<std::uint8_t, 34> ipfs_publish_and_readback(
    std::string_view api_url, std::span<const std::uint8_t> body);
// One bounded channel for this verified endpoint; streams and deadlines remain
// per-operation. No automatic transaction replay after ambiguous outcomes.
class AiResponseTransport final {
public:
  explicit AiResponseTransport(std::string_view endpoint);
  ~AiResponseTransport();
  AiResponseTransport(const AiResponseTransport&)=delete;
  AiResponseTransport& operator=(const AiResponseTransport&)=delete;
  AiResponseEffects::SubmitResult submit(const protowire::RpcTransaction &, std::string_view expected_txid);
  bool known(std::string_view txid);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
AiResponseEffects::SubmitResult submit_ai_response_transaction(
    std::string_view endpoint, const protowire::RpcTransaction &transaction,
    std::string_view expected_txid);
bool ai_response_transaction_known(std::string_view endpoint, std::string_view txid);

} // namespace keryx::nexus::ai

namespace keryx::nexus::ai {
// Wire metadata only: a commitment is not proof that inference is correct.
struct ResponseTraceCommitment {
  std::array<std::uint8_t,32> root{};
  std::uint32_t prompt_positions{},total_positions{};
  std::uint8_t stage{};
};
std::optional<ResponseTraceCommitment> stock_response_trace(std::span<const std::uint8_t> payload);
struct ResponseTraceCommitmentV2 {
  ResponseTraceCommitment trace;
  std::optional<std::array<std::uint8_t,32>> tokens_root;
};
std::optional<ResponseTraceCommitmentV2> stock_response_trace_v2(std::span<const std::uint8_t> payload);

}
namespace keryx::nexus::ai {
struct TraceOpeningContext {
  std::array<std::uint8_t,32> model_root{},manifest_root{},trace_root{};
  std::array<std::uint8_t,32> response_txid{},escrow_pubkey{},draw_seed{};
  std::uint64_t chunks{},entries{},products{};
  std::uint32_t prompt_positions{},total_positions{};
};
static_assert(sizeof(TraceOpeningContext)==224);
// Only authenticates/verifies the opening. It never submits or marks an obligation fulfilled.
bool stock_verify_trace_opening(std::span<const std::uint8_t> payload,const TraceOpeningContext& context);
// Prefix parameters must come from the verified tier manifest/network policy.
// This additive verifier does not activate an era, send transactions or run inference.
struct TracePrefixContext {
  std::array<std::uint8_t,32> request_hash{};
  std::array<std::uint32_t,256> tokens{};
  std::uint32_t offset{},embedding_entry{},norm_entry{};
  double epsilon{},embedding_scale{};
};
static_assert(sizeof(TracePrefixContext)==1088);
bool stock_verify_trace_opening_prefix(std::span<const std::uint8_t> payload,
    const TraceOpeningContext& context,const TracePrefixContext& prefix);
struct TraceChainContext {
  std::uint32_t layers{},layer_base{},embedding_entry{},active_experts{};
  double epsilon{},embedding_scale{};
  std::uint8_t trim_last{};
  std::array<std::uint8_t,7> reserved{};
};
static_assert(sizeof(TraceChainContext)==40);
bool stock_verify_trace_opening_chain(std::span<const std::uint8_t> payload,
    const TraceOpeningContext& context,const TracePrefixContext& prefix,const TraceChainContext& chain);

}
