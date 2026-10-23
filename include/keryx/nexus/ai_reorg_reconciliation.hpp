#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/ai_response_service.hpp"

namespace keryx::nexus {

struct AiReorgBlockEvidence {
  std::string selected_parent;
  std::uint64_t daa_score{};
  std::optional<bool> selected_chain_at_fetch;
};

struct AiReorgReconciliation {
  bool complete{};
  std::string final_selected_sink;
  std::vector<std::string> common_ancestors;
  std::map<std::string, bool> selected_chain;
  std::map<std::string, bool> event_selected_chain;
  std::string error;
};

// Replays the node's ordered VirtualChainChanged path. The RPC contract orders
// removals tip-to-ancestor and additions ancestor-to-tip. Every edge must be
// demonstrated by the fetched block's selectedParentHash.
AiReorgReconciliation reconcile_ai_selected_chain(
    std::string initial_selected_sink,
    const std::vector<AiSelectedChainChange> &changes,
    const std::map<std::string, AiReorgBlockEvidence> &blocks,
    std::uint64_t coverage_cutoff_daa,
    std::size_t max_changed_blocks = 10000);

bool ai_audit_journal_terminal(keryx::nexus::ai::ResponseState state) noexcept;
bool ai_audit_journal_requires_reconciliation(keryx::nexus::ai::ResponseState state) noexcept;

} // namespace keryx::nexus
