#pragma once
#include <functional>

#include "keryx/nexus/network/submission_gate.hpp"
#include "rpc.pb.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <mutex>

namespace grpc { class Channel; }

namespace keryx::nexus::network {

enum class SubmissionState {
  CandidateValid,
  SubmitSent,
  RpcAcknowledged,
  RpcRejected,
  ChainObserved,
  ChainConfirmed,
  UnknownAfterTimeout,
  UnknownOutcome,
};

const char *submission_state_name(SubmissionState state) noexcept;

struct SubmitBlockResult {
  struct Timing {
    std::optional<double> assembly_ms, validation_ms, connect_ms, stream_ms, prepare_ms,
                          write_ms, response_ms, finish_ms, total_ms;
    bool channel_reused{};
  } timing;
  SubmissionState state{SubmissionState::CandidateValid};
  std::vector<SubmissionState> transitions;
  protowire::SubmitBlockResponseMessage::RejectReason reason{};
  std::string node_error;
  std::string reservation;
  std::string block_hash;
  std::uint64_t block_blue_score{};
  std::uint64_t selected_tip_blue_score{};
  bool currently_selected_chain{};
  bool reorg_observed{};
};

// Production transport for the official v0.5.7 RPC.MessageStream protocol.
// Live submission is opt-in per call and remains unused by the M2.8 shadow path.
class SubmitBlockClient final {
public:
  explicit SubmitBlockClient(std::string endpoint);
  SubmitBlockResult submit(const protowire::RpcBlock &block,
                           const VerifiedSubmission &verified,
                           bool enable_live_submit,
                           std::chrono::seconds timeout = std::chrono::seconds(5),
                           const std::function<bool()> &cancelled = {});

  // Polls official GetBlock/GetBlockDagInfo RPC data. CHAIN_OBSERVED means the
  // node returned the block from its DAG. CHAIN_CONFIRMED is an operational
  // policy: isChainBlock remains true and the selected sink is at least
  // confirmation_blue_score_depth ahead. It is reorgable, not protocol finality.
  SubmitBlockResult observe_chain(SubmitBlockResult result,
                                  std::chrono::seconds timeout = std::chrono::seconds(30),
                                  std::chrono::milliseconds poll_interval = std::chrono::seconds(1),
                                  std::uint64_t confirmation_blue_score_depth = 6);
private:
  std::string endpoint_;
  SubmissionGate gate_;
  // Only submit() accesses this channel; observers retain independent streams.
  std::shared_ptr<grpc::Channel> submit_channel_;
  std::mutex submit_channel_mutex_;
};

} // namespace keryx::nexus::network
