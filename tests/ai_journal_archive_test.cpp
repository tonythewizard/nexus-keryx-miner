#include "keryx/nexus/ai_journal_archive.hpp"
#include <iostream>

using namespace keryx::nexus;
static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
int main() {
  try {
    require(ai_audit_minimum_window_daa(0) == 11192 &&
            ai_audit_minimum_window_daa(3) == 15288 &&
            ai_audit_minimum_window_daa(4) == 19384, "audit omitted a maximum-token response window");
    bool invalid_tier = false;
    try { (void)ai_audit_minimum_window_daa(5); }
    catch (const std::invalid_argument &) { invalid_tier = true; }
    require(invalid_tier, "invalid tier accepted");
    std::array<std::uint8_t, 32> own_model{};
    protowire::RpcTransaction tx;
    tx.set_subnetworkid(std::string(ai::request_subnetwork));
    tx.set_payload(std::string(ai::request_header_bytes * 2, '0'));
    std::string foreign = tx.payload(); foreign[0] = '1'; tx.set_payload(foreign);
    auto hash = ai::consensus_transaction_id(tx);
    require(hash.has_value(), "fixture canonical transaction ID unavailable");
    ai::ResponseJournalRecord record{ai::hex_encode(hash->data(), hash->size()), "", ai::ResponseState::Discovered};
    require(ai_journal_foreign_model_verified(record, tx, own_model), "canonical foreign model evidence rejected");
    auto changed = tx; changed.mutable_payload()->back() = '1';
    require(!ai_journal_foreign_model_verified(record, changed, own_model), "modified transaction accepted");
    changed.mutable_verbosedata()->set_transactionid(record.request_id);
    require(!ai_journal_foreign_model_verified(record, changed, own_model), "spoofed verbose ID bypassed canonical hashing");
    auto same_model = own_model; same_model[0] = 0x10;
    require(!ai_journal_foreign_model_verified(record, tx, same_model), "configured model marked unassigned");
    for (auto state : {ai::ResponseState::RpcAcknowledged, ai::ResponseState::Submitting,
                       ai::ResponseState::UnknownAfterTimeout, ai::ResponseState::ResponseReorged}) {
      auto receipt = record; receipt.state = state;
      require(!ai_journal_foreign_model_verified(receipt, tx, own_model), "archive cleared an ambiguous response receipt");
    }
    auto receipt = record; receipt.transaction_id = std::string(64, 'a');
    require(!ai_journal_foreign_model_verified(receipt, tx, own_model), "transaction-bearing record cleared");
    changed = tx; changed.set_subnetworkid("0400000000000000000000000000000000000000");
    require(!ai_journal_foreign_model_verified(record, changed, own_model), "wrong subnetwork accepted");
    changed = tx; changed.set_payload("00");
    require(!ai_journal_foreign_model_verified(record, changed, own_model), "truncated payload accepted");
    changed = tx; changed.mutable_payload()->back() = 'x';
    require(!ai_journal_foreign_model_verified(record, changed, own_model), "malformed hex accepted");
    std::array<std::uint8_t, 32> secret{}; secret.fill(0x22); // isolated stock fixture identity
    const auto escrow = ai::stock_escrow_pubkey_from_secret(secret);
    std::array<std::uint8_t, 34> cid{}; cid[0] = 0x12; cid[1] = 0x20;
    const std::array<std::uint8_t, 2> answer{'o', 'k'};
    const auto signed_payload = ai::stock_build_signed_response(*hash, secret, cid, 100, answer);
    protowire::RpcTransaction response;
    response.set_subnetworkid("0400000000000000000000000000000000000000");
    response.set_payload(ai::hex_encode(signed_payload.data(), signed_payload.size()));
    const auto response_hash = ai::consensus_transaction_id(response);
    require(response_hash.has_value(), "response fixture canonical ID missing");
    ai::ResponseJournalRecord acknowledged{record.request_id,
        ai::hex_encode(response_hash->data(), response_hash->size()), ai::ResponseState::RpcAcknowledged};
    require(ai_journal_selected_response_verified(acknowledged, response, escrow, true),
            "exact signed selected-chain receipt rejected");
    require(!ai_journal_selected_response_verified(acknowledged, response, escrow, false),
            "ACK or non-selected block treated as selected-chain evidence");
    auto wrong_receipt = acknowledged; wrong_receipt.request_id[0] ^= 1;
    require(!ai_journal_selected_response_verified(wrong_receipt, response, escrow, true),
            "receipt accepted for another request");
    wrong_receipt = acknowledged; wrong_receipt.transaction_id[0] ^= 1;
    require(!ai_journal_selected_response_verified(wrong_receipt, response, escrow, true),
            "another response transaction accepted");
    auto wrong_escrow = escrow; wrong_escrow[0] ^= 1;
    require(!ai_journal_selected_response_verified(acknowledged, response, wrong_escrow, true),
            "another responder signature accepted");
    std::cout << "AI_JOURNAL_ARCHIVE=PASS canonical_id=true verbose_spoof_rejected=true "
                 "model_bound=true response_receipts_preserved=true max_deadline_covered=true "
                 "selected_chain_exact_signed_receipt=true ack_alone_rejected=true\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "AI_JOURNAL_ARCHIVE=FAIL " << error.what() << '\n';
    return 1;
  }
}
