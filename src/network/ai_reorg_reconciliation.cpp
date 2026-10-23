#include "keryx/nexus/ai_reorg_reconciliation.hpp"

#include <set>
#include <utility>

namespace keryx::nexus {
namespace {
bool fail(AiReorgReconciliation &result, std::string message) {
  result.error = std::move(message);
  result.complete = false;
  return false;
}
}

AiReorgReconciliation reconcile_ai_selected_chain(
    std::string initial_selected_sink,
    const std::vector<AiSelectedChainChange> &changes,
    const std::map<std::string, AiReorgBlockEvidence> &blocks,
    std::uint64_t coverage_cutoff_daa,
    std::size_t max_changed_blocks) {
  AiReorgReconciliation result;
  if (initial_selected_sink.empty()) {
    fail(result, "initial selected-chain sink missing");
    return result;
  }
  std::string tip = std::move(initial_selected_sink);
  std::set<std::string> exact_events;
  std::size_t changed_blocks = 0;
  const auto get = [&](const std::string &hash) -> const AiReorgBlockEvidence * {
    const auto it = blocks.find(hash);
    return it == blocks.end() ? nullptr : &it->second;
  };
  if (!get(tip)) {
    fail(result, "initial selected sink block was not fetched and verified: " + tip);
    return result;
  }
  for (const auto &change : changes) {
    if (change.removed.empty() && change.added.empty()) continue;
    std::string event_key;
    for (const auto &hash : change.removed) event_key += "-" + hash + ";";
    event_key += "|";
    for (const auto &hash : change.added) event_key += "+" + hash + ";";
    if (!exact_events.insert(event_key).second) continue;
    if (change.out_of_order) {
      fail(result, "selected-chain event marked out-of-order at sequence " + std::to_string(change.sequence));
      return result;
    }
    changed_blocks += change.removed.size() + change.added.size();
    if (changed_blocks > max_changed_blocks) {
      fail(result, "selected-chain reconciliation exceeded bounded changed-block limit");
      return result;
    }

    std::string common = tip;
    if (!change.removed.empty()) {
      if (change.removed.front() != tip) {
        fail(result, "removed path does not begin at current selected sink");
        return result;
      }
      for (std::size_t i = 0; i < change.removed.size(); ++i) {
        const auto *evidence = get(change.removed[i]);
        if (!evidence || evidence->selected_parent.empty()) {
          fail(result, "removed block selected-parent evidence missing: " + change.removed[i]);
          return result;
        }
        const auto expected_parent = i + 1 < change.removed.size()
            ? change.removed[i + 1] : evidence->selected_parent;
        if (i + 1 < change.removed.size() && evidence->selected_parent != expected_parent) {
          fail(result, "removed path selected-parent discontinuity");
          return result;
        }
        result.event_selected_chain[change.removed[i]] = false;
        common = evidence->selected_parent;
      }
      if (!get(common)) {
        fail(result, "common ancestor block was not fetched and verified: " + common);
        return result;
      }
      result.common_ancestors.push_back(common);
    }
    for (std::size_t i = 0; i < change.added.size(); ++i) {
      const auto &hash = change.added[i];
      const auto *evidence = get(hash);
      if (!evidence || evidence->selected_parent.empty()) {
        fail(result, "added block selected-parent evidence missing: " + hash);
        return result;
      }
      const auto &expected_parent = i == 0 ? common : change.added[i - 1];
      if (evidence->selected_parent != expected_parent) {
        fail(result, "added path does not join the verified common ancestor or prior addition");
        return result;
      }
      result.event_selected_chain[hash] = true;
      tip = hash;
    }
    if (change.added.empty()) tip = common;
  }
  result.final_selected_sink = std::move(tip);
  // `isChainBlock` is a plain bool in the node's Rust protobuf model. A false
  // value is omitted on the wire, so an optional-bool client sees UNKNOWN.
  // Reconstruct the final selected chain from the node-provided selected-parent
  // links instead; this is hash-based and does not assume consecutive DAA scores.
  for (const auto &[hash, evidence] : blocks) {
    (void)evidence;
    result.selected_chain.emplace(hash, false);
  }
  std::string current = result.final_selected_sink;
  std::size_t path_length = 0;
  while (true) {
    if (++path_length > 50000) {
      fail(result, "selected-parent path exceeded the bounded historical coverage limit");
      return result;
    }
    const auto *evidence = get(current);
    if (!evidence) {
      fail(result, "selected-parent coverage has a missing block: " + current);
      return result;
    }
    result.selected_chain[current] = true;
    if (evidence->daa_score <= coverage_cutoff_daa) break;
    if (evidence->selected_parent.empty()) {
      fail(result, "selected-parent link missing above the coverage cutoff: " + current);
      return result;
    }
    const auto *parent = get(evidence->selected_parent);
    if (!parent) {
      fail(result, "selected-parent ancestor was not recovered: " + evidence->selected_parent);
      return result;
    }
    if (parent->daa_score > evidence->daa_score) {
      fail(result, "selected-parent DAA score increases toward an ancestor");
      return result;
    }
    current = evidence->selected_parent;
  }
  result.complete = true;
  return result;
}

bool ai_audit_journal_terminal(ai::ResponseState state) noexcept {
  using ai::ResponseState;
  return state == ResponseState::Finalized || state == ResponseState::Expired ||
      state == ResponseState::Failed || state == ResponseState::NotAssigned ||
      state == ResponseState::RpcRejected;
}

bool ai_audit_journal_requires_reconciliation(ai::ResponseState state) noexcept {
  using ai::ResponseState;
  return state == ResponseState::Submitting || state == ResponseState::UnknownAfterTimeout ||
      state == ResponseState::RpcAcknowledged || state == ResponseState::ResponseReorged;
}
} // namespace keryx::nexus
