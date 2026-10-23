#pragma once
#include "keryx/nexus/ai_response_service.hpp"
#include "rpc.pb.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace keryx::nexus {

inline std::uint64_t ai_audit_minimum_window_daa(std::uint32_t tier) {
  const auto maximum = ai::service_response_deadline(0, tier, 4096, true);
  if (!maximum) throw std::invalid_argument("invalid audit service tier");
  return std::max<std::uint64_t>(6000, *maximum);
}

// A journal ID is already trusted local identity metadata. An untrusted archive
// can supply its missing transaction, but cannot change the committed model
// without changing that ID. Never use archive acceptance/DAA claims as a proof
// of selected-chain coverage or to clear an acknowledged/ambiguous submission.
inline bool ai_journal_foreign_model_verified(const ai::ResponseJournalRecord &record,
    const protowire::RpcTransaction &tx, const std::array<std::uint8_t, 32> &model) {
  if (record.state != ai::ResponseState::Discovered || !record.transaction_id.empty() ||
      tx.subnetworkid() != ai::request_subnetwork ||
      tx.payload().size() < ai::request_header_bytes * 2 ||
      tx.payload().size() > ai::max_request_bytes * 2)
    return false;
  const auto digest = ai::consensus_transaction_id(tx);
  if (!digest || ai::hex_encode(digest->data(), digest->size()) != record.request_id) return false;
  const auto model_hex = ai::hex_encode(model.data(), model.size());
  const auto &payload = tx.payload();
  for (std::size_t i = 0; i < model_hex.size(); ++i) {
    const char ch = payload[i];
    const char lower = ch >= 'A' && ch <= 'F' ? static_cast<char>(ch - 'A' + 'a') : ch;
    if (lower != model_hex[i]) return true;
  }
  return false;
}

// selected_chain must come from the configured node's fresh GetBlock reply,
// never from an archive/indexer flag. This verifies the receipt's exact
// transaction and signature; it does not promote RPC ACK alone to observation.
inline bool ai_journal_selected_response_verified(const ai::ResponseJournalRecord &record,
    const protowire::RpcTransaction &tx, const std::array<std::uint8_t, 32> &escrow,
    bool selected_chain) {
  if (!selected_chain || record.transaction_id.empty() ||
      (record.state != ai::ResponseState::RpcAcknowledged &&
       record.state != ai::ResponseState::UnknownAfterTimeout &&
       record.state != ai::ResponseState::Submitting &&
       record.state != ai::ResponseState::ChainObserved &&
       record.state != ai::ResponseState::ResponseReorged) ||
      tx.subnetworkid() != "0400000000000000000000000000000000000000" ||
      tx.payload().size() > 1024 * 1024)
    return false;
  const auto digest = ai::consensus_transaction_id(tx);
  if (!digest || ai::hex_encode(digest->data(), digest->size()) != record.transaction_id) return false;
  const auto &hex = tx.payload();
  if (hex.size() % 2) return false;
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> payload(hex.size() / 2);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    const int a = nibble(hex[2 * i]), b = nibble(hex[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    payload[i] = static_cast<std::uint8_t>((a << 4) | b);
  }
  const auto request = ai::stock_response_request_hash(payload);
  return request && ai::hex_encode(request->data(), request->size()) == record.request_id &&
      ai::stock_verify_response_signature(payload, *request, escrow);
}

} // namespace keryx::nexus
