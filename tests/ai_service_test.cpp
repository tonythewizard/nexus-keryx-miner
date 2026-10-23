#include "keryx/nexus/ai_service.hpp"
#include "keryx/nexus/strike_monitor.hpp"
#include "keryx/nexus/escrow_watcher.hpp"
#include "rpc.pb.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace keryx::nexus::ai;
namespace fs = std::filesystem;

namespace {
void append_le(std::vector<std::uint8_t> &v, std::uint64_t n, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) v.push_back(static_cast<std::uint8_t>(n >> (8U * i)));
}
std::string hex(std::span<const std::uint8_t> v) {
  return hex_encode(v.data(), v.size());
}
protowire::RpcTransaction request(std::span<const std::uint8_t> prompt) {
  std::vector<std::uint8_t> payload(32, 0x42);
  append_le(payload, 128, 4); append_le(payload, 20'000'000, 8); append_le(payload, min_priority_fee, 8);
  payload.insert(payload.end(), prompt.begin(), prompt.end());
  protowire::RpcTransaction tx;
  tx.set_subnetworkid(std::string(request_subnetwork));
  tx.set_payload(hex(payload));
  tx.mutable_verbosedata()->set_transactionid(std::string(64, 'a'));
  return tx;
}
std::vector<std::uint8_t> private_prompt() {
  // The generator point, encoded as a valid compressed ephemeral key and x-only recipient.
  const std::array<std::uint8_t, 32> gx{
    0x79,0xbe,0x66,0x7e,0xf9,0xdc,0xbb,0xac,0x55,0xa0,0x62,0x95,0xce,0x87,0x0b,0x07,
    0x02,0x9b,0xfc,0xdb,0x2d,0xce,0x28,0xd9,0x59,0xf2,0x81,0x5b,0x16,0xf8,0x17,0x98};
  std::vector<std::uint8_t> p{0, 'K', 'X', 'P', 1, 2};
  p.insert(p.end(), gx.begin(), gx.end());
  p.insert(p.end(), 12, 0); p.push_back(1);
  p.insert(p.end(), gx.begin(), gx.end()); p.insert(p.end(), 48, 0x77); p.insert(p.end(), 16, 0x88);
  return p;
}
}

int main(int argc, char **argv) {
  if (argc != 2) throw std::runtime_error("persistent test directory required");
  const fs::path root = fs::absolute(argv[1]);
  fs::create_directories(root);
  auto pub = request(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>("fixture prompt"), 14));
  std::string why;
  auto parsed = parse_request(pub, 120'000'000, &why);
  if (!parsed || parsed->is_private || parsed->max_tokens != 128 || parsed->priority_fee != min_priority_fee)
    throw std::runtime_error("official public AiRequest parsing failed: " + why);
  const auto model_hex = hex_encode(parsed->model_id.data(), parsed->model_id.size());
  const std::string service_identity(64, 'b');
  const auto private_bytes = private_prompt();
  const auto own_key = hex(std::span<const std::uint8_t>(private_bytes.data() + 51, 32));
  std::vector<ServiceProvider> providers{{0, model_hex, service_identity, own_key}};
  if (classify_assignment(*parsed, service_identity, own_key, 0, providers) != Assignment::PublicEligible ||
      classify_assignment(*parsed, std::string(64, 'c'), own_key, 0, providers) != Assignment::NotAssigned ||
      classify_assignment(*parsed, service_identity, own_key, 1, providers) != Assignment::NotAssigned)
    throw std::runtime_error("public provider assignment classification failed");

  auto priv = parse_request(request(private_bytes), 120'000'001, &why);
  if (!priv || !priv->is_private) throw std::runtime_error("official private envelope parse failed: " + why);
  if (parse_request(pub, mainnet_private_inference_gate_daa, &why) ||
      !parse_request(request(private_bytes), mainnet_private_inference_gate_daa, &why))
    throw std::runtime_error("H14 private-request era gate mismatch");
  if (service_response_deadline(100, 0, 128, false) != 656 ||
      service_response_deadline(100, 0, 128, true) != 3356 ||
      service_response_deadline(100, 5, 128, true) ||
      service_response_deadline(UINT64_MAX - 1, 4, 4096, true))
    throw std::runtime_error("official service response-window calculation mismatch");
  providers.front().model_id = hex_encode(priv->model_id.data(), priv->model_id.size());
  if (classify_assignment(*priv, service_identity, own_key, 0, providers) != Assignment::PrivateRecipient ||
      classify_assignment(*priv, service_identity, std::string(64, 'f'), 0, providers) != Assignment::NotAssigned)
    throw std::runtime_error("private recipient assignment classification failed");
  if(classify_assignment(*priv,service_identity,own_key,0,{})!=Assignment::PrivateRecipient)
    throw std::runtime_error("private recipient must remain assigned when current provider window is empty");

  auto pre_h8_a = parse_request(pub, mainnet_request_txid_gate_daa - 1, &why);
  auto changed_payload = pub;
  auto changed_payload_bytes = std::vector<std::uint8_t>(32, 0x42);
  append_le(changed_payload_bytes, 128, 4); append_le(changed_payload_bytes, 20'000'000, 8);
  append_le(changed_payload_bytes, min_priority_fee, 8); changed_payload_bytes.push_back('x');
  changed_payload.set_payload(hex(changed_payload_bytes));
  auto pre_h8_b = parse_request(changed_payload, mainnet_request_txid_gate_daa - 1, &why);
  auto post_h8_a = parse_request(pub, mainnet_request_txid_gate_daa, &why);
  auto post_h8_b = parse_request(changed_payload, mainnet_request_txid_gate_daa, &why);
  if (!pre_h8_a || !pre_h8_b || !post_h8_a || !post_h8_b ||
      pre_h8_a->stable_id == pre_h8_b->stable_id ||
      post_h8_a->stable_id != post_h8_b->stable_id ||
      post_h8_a->stable_id != hex_encode(post_h8_a->txid.data(), post_h8_a->txid.size()))
    throw std::runtime_error("official H8 AiRequest identity transition mismatch");

  auto malformed = private_bytes; malformed[50] = 0;
  if (parse_request(request(malformed), 1, &why)) throw std::runtime_error("malformed private envelope was accepted");
  auto wrong_net = pub; wrong_net.set_subnetworkid(std::string(40, 'f'));
  if (parse_request(wrong_net, 1, &why)) throw std::runtime_error("non-AiRequest subnetwork was accepted");
  auto missing_txid = pub; missing_txid.mutable_verbosedata()->clear_transactionid();
  if (!parse_request(missing_txid, mainnet_request_txid_gate_daa, &why))
    throw std::runtime_error("official txid fallback serialization failed");

  const auto db = root / "requests.snapshot";
  fs::remove(db); fs::remove(db.string() + ".journal"); fs::remove(db.string() + ".new");
  {
    RequestJournal journal(db);
    journal.recover();
    if (!journal.discover(*parsed, true) || journal.discover(*parsed, true) || journal.size() != 1 ||
        journal.pending().front().state != "ASSIGNED")
      throw std::runtime_error("AiRequest deduplication failed");
    if (!journal.transition(parsed->stable_id, "ASSIGNED", "INFERENCE") ||
        !journal.transition(parsed->stable_id, "INFERENCE", "RESPONSE_SUBMITTED") ||
        journal.transition(parsed->stable_id, "ASSIGNED", "CONFIRMED"))
      throw std::runtime_error("AiRequest lifecycle transition enforcement failed");
  }
  {
    auto unassigned = *parsed;
    unassigned.stable_id = std::string(64, 'd');
    const auto unassigned_db = root / "unassigned.snapshot";
    fs::remove(unassigned_db); fs::remove(unassigned_db.string() + ".journal");
    fs::remove(unassigned_db.string() + ".new");
    RequestJournal journal(unassigned_db);
    journal.recover();
    if (!journal.discover(unassigned, false) || !journal.pending().empty() ||
        journal.discover(unassigned, true))
      throw std::runtime_error("definitely unassigned AiRequest was retained as a service obligation");
  }
  {
    RequestJournal recovered(db); recovered.recover();
    const auto pending = recovered.pending();
    if (pending.size() != 1 || pending.front().state != "RESPONSE_SUBMITTED" ||
        pending.front().request_txid != hex_encode(parsed->txid.data(), parsed->txid.size()) ||
        pending.front().response_deadline_daa.has_value())
      throw std::runtime_error("durable pending AiRequest recovery failed");
    if (!recovered.transition(parsed->stable_id, "RESPONSE_SUBMITTED", "CONFIRMED"))
      throw std::runtime_error("response confirmation journal transition failed");
  }

  protowire::GetServiceStrikesResponseMessage strike;
  strike.set_virtualdaascore(120'000'100);
  auto *s = strike.add_strikes(); s->set_miner(std::string(64, 'b')); s->set_consecutivemisses(2); s->set_laststrikedaascore(99);
  auto *total = strike.add_lifetimestrikes(); total->set_miner(std::string(64, 'b')); total->set_strikes(7);
  auto *burn = strike.add_pendingburns(); burn->set_miner(std::string(64, 'b')); burn->set_burnedsompi(1234);
  auto snapshot = strikes_for_identity(strike, std::string(64, 'b'));
  if (snapshot.availability != StrikeSnapshot::Availability::Available || snapshot.consecutive != 2 ||
      snapshot.lifetime != 7 || snapshot.pending_burns != 1 || snapshot.pending_burn_sompi != 1234)
    throw std::runtime_error("authoritative strike snapshot mapping failed");
  auto unknown = strikes_for_identity(strike, "");
  if (unknown.availability != StrikeSnapshot::Availability::Unknown || unknown.consecutive)
    throw std::runtime_error("unknown strike identity was incorrectly shown as zero");
  std::string address_error;
  const auto service_id = service_identity_from_payout(
      "keryx:qrxpcusyrxjxghfdumcxm2rqw4dhe3n9hyqpvgn2wfyldltf99w2xhnajuhte", &address_error);
  if (!service_id || *service_id != "cb79bef02d429e0fc8bb2335bf43d9d0df4f5bd6a25a39747d700b173e766e20")
    throw std::runtime_error("payout-to-service identity does not match the official stock vector: " + address_error);

  {
    std::atomic<unsigned> calls{};
    std::mutex monitor_mutex;
    std::condition_variable monitor_cv;
    unsigned snapshots = 0;
    StrikeMonitor monitor(std::string(64, 'b'), root / "strikes.jsonl", [&] {
      if (calls.fetch_add(1) == 0) {
        StrikeSnapshot value;
        value.identity = std::string(64, 'b');
        value.availability = StrikeSnapshot::Availability::Available;
        value.consecutive = 2; value.lifetime = 7; value.last_strike_daa = 99;
        value.pending_burns = 1; value.pending_burn_sompi = 1234;
        return value;
      }
      throw std::runtime_error("mock strike endpoint unavailable");
    }, [&](const StrikeSnapshot &value) {
      std::lock_guard lock(monitor_mutex);
      ++snapshots;
      if (value.availability == StrikeSnapshot::Availability::Unknown) {}
      monitor_cv.notify_all();
    }, std::chrono::milliseconds(25));
    monitor.start();
    {
      std::unique_lock lock(monitor_mutex);
      if (!monitor_cv.wait_for(lock, std::chrono::seconds(2), [&] { return snapshots >= 2; }))
        throw std::runtime_error("strike monitor did not poll repeatedly");
    }
    monitor.stop();
    const auto last = monitor.snapshot();
    if (last.availability != StrikeSnapshot::Availability::Available || !last.stale ||
        last.consecutive != 2 || last.lifetime != 7 || last.error.empty())
      throw std::runtime_error("strike monitor replaced unavailable telemetry with false zero/default values");
    const auto journal = fs::file_size(root / "strikes.jsonl");
    if (journal == 0) throw std::runtime_error("strike monitor did not persist observations");
  }
  {
    std::mutex recovered_mutex;
    std::condition_variable recovered_cv;
    bool observed = false;
    StrikeMonitor recovered(std::string(64, 'b'), root / "strikes.jsonl", []() -> StrikeSnapshot {
      throw std::runtime_error("fixture endpoint unavailable after restart");
    }, [&](const StrikeSnapshot &value) {
      std::lock_guard lock(recovered_mutex);
      observed = value.availability == StrikeSnapshot::Availability::Available && value.stale &&
                 value.consecutive == 2 && value.lifetime == 7;
      recovered_cv.notify_all();
    }, std::chrono::milliseconds(25));
    recovered.start();
    {
      std::unique_lock lock(recovered_mutex);
      if (!recovered_cv.wait_for(lock, std::chrono::seconds(2), [&] { return observed; }))
        throw std::runtime_error("strike monitor did not restore the last durable observation as stale");
    }
    recovered.stop();
    const auto snapshot = recovered.snapshot();
    if (snapshot.availability != StrikeSnapshot::Availability::Available || !snapshot.stale ||
        snapshot.consecutive != 2 || snapshot.lifetime != 7)
      throw std::runtime_error("strike state was lost across monitor restart");
  }

  {
    const std::array<std::uint8_t, 32> key{};
    auto test_key = key; test_key.fill(0x11);
    const auto state_path = root / "stock-escrow-state.json";
    {
      EscrowWatcher watcher(test_key,
          "keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr", state_path);
      if (!watcher.start_state_validation().empty() || watcher.validation_in_progress())
        throw std::runtime_error("empty stock escrow snapshot did not validate cleanly");
      const auto txid = std::string(64, 'a');
      watcher.track_inference_outpoint(txid, 125, 50'000'000);
      watcher.flush();
    }
    {
      EscrowWatcher watcher(test_key,
          "keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr", state_path);
      protowire::RpcBlock block;
      block.mutable_header()->set_daascore(126);
      const auto result = watcher.handle_block(block);
      if (result.pending_count != 1 || result.pending_amount_sompi != 50'000'000 || result.claim)
        throw std::runtime_error("stock escrow snapshot/journal did not recover without submitting claims");
      watcher.flush();
    }
    const auto corrupt_path = root / "corrupt-escrow-state.json";
    fs::create_directories(corrupt_path.parent_path());
    fs::copy_file(state_path, corrupt_path, fs::copy_options::overwrite_existing);
    if (fs::exists(state_path.string() + ".journal"))
      fs::copy_file(state_path.string() + ".journal", corrupt_path.string() + ".journal",
                    fs::copy_options::overwrite_existing);
    fs::permissions(corrupt_path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    std::ofstream(corrupt_path, std::ios::binary | std::ios::trunc) << "{truncated";
    bool failed_closed = false;
    try { EscrowWatcher invalid(test_key,
        "keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr", corrupt_path); }
    catch (const std::exception &) { failed_closed = true; }
    if (!failed_closed) throw std::runtime_error("corrupt escrow snapshot was not rejected");
  }

  fs::remove(db); fs::remove(db.string() + ".journal"); fs::remove(db.string() + ".new");
  fs::remove(root / "unassigned.snapshot");
  fs::remove((root / "unassigned.snapshot").string() + ".journal");
  fs::remove((root / "unassigned.snapshot").string() + ".new");
  fs::remove(root / "strikes.jsonl");
  fs::remove(root / "stock-escrow-state.json");
  fs::remove(root / "stock-escrow-state.journal");
  fs::remove(root / "corrupt-escrow-state.json");
  fs::remove(root / "corrupt-escrow-state.journal");
  fs::remove(root);
  std::cout << "AI_SERVICE_CORE_TEST=PASS official_payload=PASS H8_identity=PASS H14_gate=PASS service_deadline=PASS provider_cohort=PASS private_recipient=PASS dedupe=PASS durable_recovery=PASS strikes_unknown_safe=PASS payout_service_identity=PASS\n";
}
