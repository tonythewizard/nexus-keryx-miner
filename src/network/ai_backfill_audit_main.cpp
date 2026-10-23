#include "keryx/nexus/ai_response_service.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "keryx/nexus/ai_reorg_reconciliation.hpp"
#include "keryx/nexus/ai_journal_archive.hpp"
#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <cstdlib>
#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;
using namespace keryx::nexus;

namespace {
volatile std::sig_atomic_t audit_interrupted = 0;
void on_signal(int) { audit_interrupted = 1; }
std::vector<std::uint8_t> unhex(std::string_view text) {
  if ((text.size() & 1U) != 0) throw std::invalid_argument("hex input has odd length");
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> bytes(text.size() / 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
    if (hi < 0 || lo < 0) throw std::invalid_argument("hex input contains invalid characters");
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return bytes;
}

fs::path persistent(const char *raw) {
  auto path = fs::absolute(raw).lexically_normal();
  const auto text = path.string();
  if (text == "/tmp" || text.starts_with("/tmp/"))
    throw std::invalid_argument("AI audit artifacts must be outside /tmp");
  return path;
}

const char *response_state(ai::ResponseState state) {
  return ai::response_state_name(state);
}
}

int main(int argc, char **argv) {
  keryx::nexus::network::GrpcRuntime grpc_runtime;
  if (argc != 10) {
    std::cerr << "usage: nexus-ai-backfill-audit ENDPOINT PAYOUT ESCROW_PUBKEY_HEX MODEL_ID_HEX TIER "
                 "CONCURRENCY TIMEOUT_SECONDS RESPONSE_JOURNAL OUTPUT_JSONL\n";
    return 2;
  }
  try {
    const std::string endpoint = argv[1], payout = argv[2], escrow_hex = argv[3], model_hex = argv[4];
    const auto tier = static_cast<std::uint32_t>(std::stoul(argv[5]));
    const auto concurrency = static_cast<std::size_t>(std::stoul(argv[6]));
    const auto timeout_s = static_cast<unsigned>(std::stoul(argv[7]));
    if (tier > 4 || concurrency == 0 || concurrency > 8 || timeout_s == 0 || timeout_s > 1800)
      throw std::invalid_argument("audit tier/concurrency/timeout outside bounded range");
    const auto escrow_bytes = unhex(escrow_hex), model_bytes = unhex(model_hex);
    if (escrow_bytes.size() != 32 || model_bytes.size() != 32)
      throw std::invalid_argument("escrow and model identities must be 32-byte hex");
    std::array<std::uint8_t, 32> escrow{};
    std::copy(escrow_bytes.begin(), escrow_bytes.end(), escrow.begin());
    const auto output_path = persistent(argv[9]);
    const auto journal_path = persistent(argv[8]);
    fs::create_directories(output_path.parent_path());
    std::ofstream log(output_path, std::ios::out | std::ios::trunc);
    if (!log) throw std::runtime_error("cannot create persistent AI audit JSONL");

    std::string identity_error;
    const auto service_identity = ai::service_identity_from_payout(payout, &identity_error);
    if (!service_identity) throw std::runtime_error("payout service identity invalid: " + identity_error);
    ai::ResponseJournal journal(journal_path);
    journal.recover(); // read-only; neither journal nor stock escrow files are modified.
    const auto journal_records = journal.snapshot();
    if (journal_records.size() > 10000)
      throw std::runtime_error("AI response journal exceeds the read-only audit index bound of 10000 records");
    std::map<std::string, ai::ResponseJournalRecord> journal_by_request;
    for (const auto &record : journal_records) journal_by_request.emplace(record.request_id, record);

    const auto providers = probe_ai_service_rpc(endpoint, 0, *service_identity, std::chrono::seconds(8));
    const auto listing = ai::provider_listing_status(providers, tier, model_hex, *service_identity, escrow_hex);
    const auto listing_text = listing == ai::ProviderListingStatus::Listed ? "LISTED" :
        listing == ai::ProviderListingStatus::NotListed ? "NOT_LISTED" : "UNKNOWN";
    std::cout << "AI_AUDIT_IDENTITY payout_valid=true delegation_check=performed_by_launcher "
              << "service_identity=" << *service_identity << " provider_listing=" << listing_text
              << " provider_snapshot=" << (providers.providers_available ? "AVAILABLE" : "UNKNOWN")
              << " strike_snapshot=" << (providers.strikes_available ? "AVAILABLE" : "UNKNOWN")
              << " journal_records=" << journal_records.size() << " transactions=DISABLED cuda=DISABLED\n";

    AiBackfillScanOptions options;
    options.window_daa = ai_audit_minimum_window_daa(tier);
    options.max_inflight = concurrency;
    options.max_blocks = 50000;
    options.timeout = std::chrono::seconds(timeout_s);
    options.settle = std::chrono::milliseconds(1200);
    std::atomic_bool stop{false};
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::jthread signal_watcher([&](std::stop_token token) {
      while (!token.stop_requested()) {
        if (audit_interrupted != 0) {
          stop.store(true, std::memory_order_release);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    });
    struct RequestSeen {
      ai::Request request;
      std::string response_state;
      std::map<std::string, ai::Request> appearances;
    };
    std::map<std::string, RequestSeen> requests;
    std::map<std::string, AiReorgBlockEvidence> block_evidence;
    std::set<std::string> any_responses, own_responses;
    std::map<std::string, std::map<std::string, bool>> response_appearances;
    std::size_t ai_tx_seen = 0, invalid_ai_requests = 0, ai_responses_seen = 0;
    bool audit_index_truncated = false;
    const auto model_matches = [&](const ai::Request &r) {
      return std::equal(r.model_id.begin(), r.model_id.end(), model_bytes.begin());
    };
    const auto response_subnetwork = std::string_view("0400000000000000000000000000000000000000");
    const auto observer = [&](const protowire::RpcBlock &block, bool backfill) {
      const auto daa = block.has_header() ? block.header().daascore() : 0;
      std::string block_hash;
      if (block.has_verbosedata()) block_hash = block.verbosedata().hash();
      if (block_hash.empty() && block.has_header()) block_hash = block.header().hash();
      if (!block_hash.empty()) {
        AiReorgBlockEvidence evidence;
        if (block.has_verbosedata()) {
          evidence.selected_parent = block.verbosedata().selectedparenthash();
          const auto *verbose_descriptor = block.verbosedata().GetDescriptor();
          const auto *chain_field = verbose_descriptor->FindFieldByName("isChainBlock");
          if (chain_field && block.verbosedata().GetReflection()->HasField(block.verbosedata(), chain_field))
            evidence.selected_chain_at_fetch = block.verbosedata().GetReflection()->GetBool(
                block.verbosedata(), chain_field);
        }
        evidence.daa_score = daa;
        block_evidence.insert_or_assign(block_hash, std::move(evidence));
      }
      std::size_t block_ai = 0;
      for (const auto &tx : block.transactions()) {
        if (tx.subnetworkid() == ai::request_subnetwork) {
          ++ai_tx_seen; ++block_ai;
          std::string why;
          const auto parsed = ai::parse_request(tx, daa, &why, "mainnet");
          if (!parsed) { ++invalid_ai_requests; continue; }
          const bool model_match = model_matches(*parsed);
          const auto id = parsed->stable_id;
          if (!requests.contains(id) && requests.size() >= 10000) {
            audit_index_truncated = true;
            ++invalid_ai_requests;
            continue;
          }
          auto [it, inserted] = requests.try_emplace(id, RequestSeen{*parsed, "UNRECONCILED", {}});
          (void)inserted;
          it->second.appearances.emplace(block_hash, *parsed);
          const auto deadline = ai::service_response_deadline(parsed->observed_daa, tier,
              parsed->max_tokens, parsed->observed_daa >= ai::mainnet_signed_response_activation_daa);
          std::string classification = "NOT_FOR_CONFIGURED_MODEL";
          if (model_match) {
            if (parsed->is_private) {
              ai::ServiceProvider synthetic;
              synthetic.tier = tier; synthetic.model_id = model_hex;
              synthetic.identity = *service_identity; synthetic.escrow_pubkey = escrow_hex;
              const auto assignment = ai::classify_assignment(*parsed, *service_identity, escrow_hex,
                                                               tier, {synthetic});
              classification = assignment == ai::Assignment::PrivateRecipient
                  ? "ASSIGNED_PRIVATE_RECIPIENT" : "NOT_ASSIGNED_PRIVATE_RECIPIENT";
            } else {
              // Stock grpc.rs queues public requests for every locally ready matching model;
              // GetAiProviders membership is a separate current-cohort observation.
              classification = "ASSIGNED_STOCK_MODEL_CAPABILITY";
            }
          }
          log << "{\"event\":\"AI_REQUEST\",\"id\":\"" << id << "\",\"txid\":\""
              << ai::hex_encode(parsed->txid.data(), parsed->txid.size()) << "\",\"daa\":" << daa
              << ",\"block\":\"" << block_hash << "\",\"source\":\""
              << (backfill ? "backfill" : "live_during_scan") << "\",\"model_match\":"
              << (model_match ? "true" : "false") << ",\"private\":"
              << (parsed->is_private ? "true" : "false") << ",\"classification\":\""
              << classification << "\",\"service_deadline_daa\":"
              << (deadline ? std::to_string(*deadline) : "null") << ",\"deadline_rule\":\""
              << (parsed->observed_daa >= ai::mainnet_signed_response_activation_daa ? "service_bond_v2" : "service_bond_v1")
              << "\"}\n";
        } else if (tx.subnetworkid() == response_subnetwork) {
          ++ai_responses_seen; ++block_ai;
          std::vector<std::uint8_t> payload_bytes;
          try { payload_bytes = unhex(tx.payload()); } catch (...) { continue; }
          const auto request_hash = ai::stock_response_request_hash(payload_bytes);
          if (!request_hash) continue;
          const auto id = ai::hex_encode(request_hash->data(), request_hash->size());
          if (any_responses.size() < 10000 || any_responses.contains(id)) any_responses.insert(id);
          else audit_index_truncated = true;
          const bool signed_by_own_escrow = ai::stock_verify_response_signature(
              payload_bytes, *request_hash, escrow);
          response_appearances[id].emplace(block_hash, signed_by_own_escrow);
          if (signed_by_own_escrow) {
            if (own_responses.size() < 10000 || own_responses.contains(id)) own_responses.insert(id);
            else audit_index_truncated = true;
          }
          log << "{\"event\":\"AI_RESPONSE\",\"request_id\":\"" << id
              << "\",\"daa\":" << daa << ",\"block\":\"" << block_hash
              << "\",\"signed_by_configured_escrow\":" << (signed_by_own_escrow ? "true" : "false")
              << "}\n";
        }
      }
      log << "{\"event\":\"BLOCK_SCANNED\",\"hash\":\"" << block_hash
          << "\",\"daa\":" << daa << ",\"tx_count\":" << block.transactions_size()
          << ",\"ai_tx_count\":" << block_ai << ",\"source\":\""
          << (backfill ? "backfill" : "live_during_scan") << "\"}\n";
      log.flush();
      if (!log) throw std::runtime_error("persistent AI audit JSONL write failed");
    };

    const auto scan = run_ai_backfill_scan(endpoint, options, stop, observer);
    signal_watcher.request_stop();
    signal_watcher.join();
    for (const auto &change : scan.selected_chain_changes) {
      log << "{\"event\":\"SELECTED_CHAIN_CHANGE\",\"sequence\":" << change.sequence
          << ",\"classification\":\""
          << (!change.removed.empty() ? "REORG" : change.reinclusion ? "REINCLUSION" : "ADVANCE")
          << "\",\"duplicate\":"
          << (change.duplicate ? "true" : "false") << ",\"out_of_order\":"
          << (change.out_of_order ? "true" : "false") << ",\"reinclusion\":"
          << (change.reinclusion ? "true" : "false") << ",\"removed\":[";
      for (std::size_t i = 0; i < change.removed.size(); ++i) {
        if (i) log << ',';
        log << '\"' << change.removed[i] << '\"';
      }
      log << "],\"added\":[";
      for (std::size_t i = 0; i < change.added.size(); ++i) {
        if (i) log << ',';
        log << '\"' << change.added[i] << '\"';
      }
      log << "]}\n";
    }
    const auto reorg = reconcile_ai_selected_chain(scan.initial_selected_sink,
        scan.selected_chain_changes, block_evidence, scan.cutoff_daa, 50000);
    bool reorg_reconciliation_complete = reorg.complete &&
        !scan.watermark_selected_sink.empty() && reorg.final_selected_sink == scan.watermark_selected_sink;
    std::size_t reorg_status_blocks = 0, reorg_status_selected = 0, reorg_status_unknown = 0;
    std::map<std::string, bool> selected = reorg.selected_chain;
    const auto selected_state = [&](const std::string &hash) -> std::optional<bool> {
      const auto event_state = reorg.event_selected_chain.find(hash);
      if (event_state != reorg.event_selected_chain.end()) return event_state->second;
      const auto evidence = block_evidence.find(hash);
      if (evidence == block_evidence.end() || evidence->second.daa_score < scan.cutoff_daa)
        return std::nullopt;
      const auto path_state = selected.find(hash);
      return path_state == selected.end() ? std::nullopt : std::optional<bool>{path_state->second};
    };
    std::set<std::string> relevant_hashes;
    for (const auto &[id, seen] : requests) {
      (void)id;
      for (const auto &[hash, request] : seen.appearances) { (void)request; relevant_hashes.insert(hash); }
    }
    for (const auto &[id, appearances] : response_appearances) {
      (void)id;
      for (const auto &[hash, own] : appearances) { (void)own; relevant_hashes.insert(hash); }
    }
    if (relevant_hashes.size() > 10000) reorg_reconciliation_complete = false;
    for (const auto &hash : relevant_hashes) {
      ++reorg_status_blocks;
      const auto state = selected_state(hash);
      if (!state) { ++reorg_status_unknown; reorg_reconciliation_complete = false; }
      else if (*state) ++reorg_status_selected;
    }
    for (auto it = requests.begin(); it != requests.end();) {
      auto &seen = it->second;
      std::vector<const ai::Request *> active;
      bool unknown = false;
      for (const auto &[hash, request] : seen.appearances) {
        const auto state = selected_state(hash);
        if (!state) unknown = true;
        else if (*state) active.push_back(&request);
      }
      if (active.empty() && !unknown) it = requests.erase(it);
      else if (!active.empty()) {
        std::sort(active.begin(), active.end(), [](const auto *a, const auto *b) {
          return a->observed_daa < b->observed_daa;
        });
        seen.request = *active.front();
        if (active.size() > 1) seen.response_state = "MULTIPLE_SELECTED_APPEARANCES";
        ++it;
      } else ++it;
    }
    any_responses.clear();
    own_responses.clear();
    for (const auto &[id, appearances] : response_appearances) {
      for (const auto &[hash, own] : appearances) {
        const auto state = selected_state(hash);
        if (!state) { reorg_reconciliation_complete = false; continue; }
        if (!*state) continue;
        any_responses.insert(id);
        if (own) own_responses.insert(id);
      }
    }
    log << "{\"event\":\"REORG_TRANSACTION_RECONCILIATION\",\"required\":"
        << (scan.selected_chain_reorgs ? "true" : "false") << ",\"complete\":"
        << (reorg_reconciliation_complete ? "true" : "false") << ",\"blocks_checked\":"
        << reorg_status_blocks << ",\"selected_blocks\":" << reorg_status_selected
        << ",\"unknown_blocks\":" << reorg_status_unknown
        << ",\"initial_sink\":\"" << scan.initial_selected_sink
        << "\",\"reconciled_sink\":\"" << reorg.final_selected_sink
        << "\",\"watermark_sink\":\"" << scan.watermark_selected_sink
        << "\",\"watermark_daa\":" << scan.watermark_daa
        << ",\"common_ancestors\":" << reorg.common_ancestors.size()
        << ",\"path_error\":\"" << (reorg.error.empty() && reorg.final_selected_sink != scan.watermark_selected_sink
              ? "replayed selected sink differs from final watermark" : reorg.error) << "\"}\n";
    std::size_t assigned_open = 0, assigned_expired = 0, indeterminate = 0, already_answered = 0;
    std::size_t journal_only = 0, mempool_known = 0, mempool_queries = 0;
    const auto current_daa = std::max(providers.providers_available ? providers.providers_virtual_daa : 0,
                                      scan.final_daa);
    for (auto &[id, seen] : requests) {
      const auto &req = seen.request;
      const auto deadline = ai::service_response_deadline(req.observed_daa, tier, req.max_tokens,
          req.observed_daa >= ai::mainnet_signed_response_activation_daa);
      const auto ji = journal_by_request.find(id);
      const bool journal_has = ji != journal_by_request.end();
      const bool answered_chain = own_responses.contains(id);
      const bool answered_other = any_responses.contains(id) && !answered_chain;
      std::string state;
      if (answered_chain) { state = "AI_RESPONSE_OBSERVED_ON_CHAIN"; ++already_answered; }
      else if (journal_has && ai_audit_journal_requires_reconciliation(ji->second.state)) {
        if (ji->second.state == ai::ResponseState::RpcAcknowledged) state = "RPC_ACKNOWLEDGED_CHAIN_NOT_OBSERVED";
        else if (ji->second.state == ai::ResponseState::ResponseReorged)
          state = "JOURNAL_RESPONSE_REORGED_RECONCILIATION_REQUIRED";
        else state = "JOURNAL_AMBIGUOUS_RECONCILIATION_REQUIRED";
        ++journal_only;
        if (!ji->second.transaction_id.empty() && mempool_queries < 8) {
          ++mempool_queries;
          if (ai::ai_response_transaction_known(endpoint, ji->second.transaction_id)) ++mempool_known;
        }
      } else if (!std::equal(req.model_id.begin(), req.model_id.end(), model_bytes.begin())) {
        state = "NOT_FOR_CONFIGURED_MODEL";
      } else {
        bool assigned = !req.is_private;
        if (req.is_private) {
          ai::ServiceProvider synthetic;
          synthetic.tier = tier; synthetic.model_id = model_hex;
          synthetic.identity = *service_identity; synthetic.escrow_pubkey = escrow_hex;
          assigned = ai::classify_assignment(req, *service_identity, escrow_hex, tier, {synthetic}) ==
              ai::Assignment::PrivateRecipient;
        }
        if (!assigned) state = "NOT_ASSIGNED_PRIVATE_RECIPIENT";
        else if (!deadline) { state = "AI_PENDING_INDETERMINATE"; ++indeterminate; }
        else if (current_daa <= *deadline) { state = "AI_PENDING_ASSIGNED"; ++assigned_open; }
        else { state = "AI_REQUEST_EXPIRED"; ++assigned_expired; }
      }
      if (state == "NOT_FOR_CONFIGURED_MODEL") continue;
      if (state == "NOT_ASSIGNED_PRIVATE_RECIPIENT") continue;
      if ((state == "AI_PENDING_ASSIGNED" || state == "AI_REQUEST_EXPIRED") && !scan.complete) {
        state += "_COVERAGE_INCOMPLETE";
        ++indeterminate;
      }
      if (journal_has) seen.response_state = response_state(ji->second.state);
      log << "{\"event\":\"AI_REQUEST_RECONCILED\",\"id\":\"" << id
          << "\",\"state\":\"" << state << "\",\"journal_state\":\""
          << (journal_has ? response_state(ji->second.state) : "ABSENT")
          << "\",\"deadline_daa\":" << (deadline ? std::to_string(*deadline) : "null")
          << ",\"other_responder_response_observed\":" << (answered_other ? "true" : "false")
          << ",\"current_daa\":" << current_daa << "}\n";
    }
    // Old immutable requests are fetched by canonical block hash solely to resolve journal
    // dependencies. They do NOT extend or pretend selected-chain coverage at the watermark.
    std::map<std::string,std::string> journal_unassigned_evidence;
    std::set<std::string> journal_selected_receipts;
    std::size_t journal_dependency_blocks_unavailable = 0;
    if (const char *archive = std::getenv("NEXUS_AUDIT_JOURNAL_TX_DIR")) {
      const auto directory = persistent(archive);
      if (!fs::is_directory(directory) || fs::is_symlink(directory))
        throw std::runtime_error("journal transaction archive must be a persistent regular directory");
      std::array<std::uint8_t, 32> configured_model{};
      std::copy(model_bytes.begin(), model_bytes.end(), configured_model.begin());
      std::size_t examined = 0;
      for (const auto &record : journal_records) {
        if (requests.contains(record.request_id) || record.state != ai::ResponseState::Discovered ||
            !record.transaction_id.empty()) continue;
        const auto file = directory / (record.request_id + ".rpc.json");
        if (!fs::exists(file)) continue;
        if (++examined > 128 || fs::is_symlink(file) || !fs::is_regular_file(file) ||
            fs::file_size(file) > 1024 * 1024)
          throw std::runtime_error("journal transaction archive exceeds safe input bounds");
        std::ifstream input(file, std::ios::binary);
        std::string json(1024 * 1024 + 1, '\0');
        input.read(json.data(), static_cast<std::streamsize>(json.size()));
        const auto size = input.gcount();
        if (input.bad() || size < 0 || size > 1024 * 1024)
          throw std::runtime_error("cannot read bounded journal transaction archive");
        json.resize(static_cast<std::size_t>(size));
        protowire::RpcTransaction tx;
        if (!google::protobuf::util::JsonStringToMessage(json, &tx).ok())
          throw std::runtime_error("journal transaction archive has invalid protobuf JSON");
        if (ai_journal_foreign_model_verified(record, tx, configured_model)) {
          journal_unassigned_evidence.emplace(record.request_id, "canonical_transaction_id");
          log << "{\"event\":\"AI_JOURNAL_REQUEST_VERIFIED_UNASSIGNED\",\"id\":\""
              << record.request_id << "\",\"evidence\":\"recomputed_consensus_txid_foreign_model\","
                 "\"journal_modified\":false,\"chain_coverage_extended\":false}\n";
        }
      }
    }
    if(const char *input=std::getenv("NEXUS_AUDIT_JOURNAL_BLOCKS")) {
      std::ifstream hashes(input);std::string h;std::size_t attempts=0;
      if(endpoint!="grpc://127.0.0.1:22110")throw std::runtime_error("supplemental journal RPC requires configured loopback node");
      while(hashes>>h){
        if(++attempts>32||h.size()!=64)throw std::runtime_error("supplemental journal dependency bounds exceeded");
        auto stub=protowire::RPC::NewStub(grpc::CreateChannel("127.0.0.1:22110",grpc::InsecureChannelCredentials()));
        grpc::ClientContext ctx;ctx.set_deadline(std::chrono::system_clock::now()+std::chrono::seconds(5));
        auto stream=stub->MessageStream(&ctx);protowire::KaspadRequest req;req.set_id(1);
        auto *get=req.mutable_getblockrequest();get->set_hash(h);get->set_includetransactions(true);
        if(!stream->Write(req))throw std::runtime_error("supplemental journal dependency RPC write failed");
        protowire::KaspadResponse response;bool recovered=false;
        while(stream->Read(&response)){
          if(!response.has_getblockresponse())continue;
          const auto &reply=response.getblockresponse();
          if(reply.has_error()||!reply.has_block())break;
          const auto &block=reply.block();
          if(!block.has_header()||!block.has_verbosedata()||block.verbosedata().hash()!=h)break;
          recovered=true;
          for(const auto &tx:block.transactions()){
            if (tx.subnetworkid() == response_subnetwork) {
              std::vector<std::uint8_t> payload;
              try { payload = unhex(tx.payload()); } catch (...) { continue; }
              const auto request = ai::stock_response_request_hash(payload);
              if (!request) continue;
              const auto id = ai::hex_encode(request->data(), request->size());
              const auto record = journal_by_request.find(id);
              if (record != journal_by_request.end() &&
                  ai_journal_selected_response_verified(record->second, tx, escrow,
                      block.verbosedata().ischainblock() && block.header().daascore() <= scan.watermark_daa) &&
                  journal_selected_receipts.insert(id).second) {
                log << "{\"event\":\"AI_JOURNAL_RESPONSE_LOCALLY_RECONCILED\",\"id\":\""
                    << id << "\",\"source_block\":\"" << h
                    << "\",\"evidence\":\"fresh_GetBlock_selected_chain_canonical_response_txid_valid_own_signature\","
                       "\"journal_modified\":false,\"chain_coverage_extended\":false}\n";
              }
              continue;
            }
            if(tx.subnetworkid()!=ai::request_subnetwork)continue;
            std::string why;const auto parsed=ai::parse_request(tx,block.header().daascore(),&why,"mainnet");
            if(!parsed||!journal_by_request.contains(parsed->stable_id)||journal_by_request.at(parsed->stable_id).state!=ai::ResponseState::Discovered)continue;
            bool unassigned=!model_matches(*parsed);
            if(!unassigned&&parsed->is_private){
              ai::ServiceProvider provider;provider.tier=tier;provider.model_id=model_hex;
              provider.identity=*service_identity;provider.escrow_pubkey=escrow_hex;
              unassigned=ai::classify_assignment(*parsed,*service_identity,escrow_hex,tier,{provider})==ai::Assignment::NotAssigned;
            }
            if(unassigned)journal_unassigned_evidence.emplace(parsed->stable_id,h);
          }
          break;
        }
        ctx.TryCancel();(void)stream->Finish();
        if(!recovered) {
          ++journal_dependency_blocks_unavailable;
          log << "{\"event\":\"AI_JOURNAL_DEPENDENCY_BLOCK_UNAVAILABLE\",\"hash\":\""
              << h << "\",\"dependencies_remain_unresolved\":true}\n";
        }
      }
    }
    for (const auto &record : journal_records) {
      if (requests.contains(record.request_id)) continue;
      if (journal_selected_receipts.contains(record.request_id)) continue;
      if(const auto evidence=journal_unassigned_evidence.find(record.request_id);evidence!=journal_unassigned_evidence.end()){
        if (evidence->second == "canonical_transaction_id") continue;
        log<<"{\"event\":\"AI_JOURNAL_REQUEST_VERIFIED_UNASSIGNED\",\"id\":\""<<record.request_id
           <<"\",\"source_block\":\""<<evidence->second<<"\",\"evidence\":\"fresh_GetBlock_canonical_request_model_or_private_recipient_mismatch\",\"journal_modified\":false}\n";
        continue;
      }
      if (ai_audit_journal_terminal(record.state)) continue;
      ++indeterminate;
      log << "{\"event\":\"AI_JOURNAL_REQUEST_NOT_IN_SCAN\",\"id\":\""
          << record.request_id << "\",\"state\":\"" << response_state(record.state)
          << "\",\"transaction_id_present\":" << (record.transaction_id.empty() ? "false" : "true")
          << "}\n";
    }
    if (invalid_ai_requests != 0 || audit_index_truncated) ++indeterminate;
    const auto audit_complete = scan.complete && !audit_index_truncated && reorg_reconciliation_complete &&
        indeterminate == 0 && journal_only == 0 && assigned_open == 0;
    const auto pending_class = !audit_complete ? "AI_BACKFILL_INCOMPLETE" :
        (indeterminate || journal_only ? "AI_PENDING_INDETERMINATE" :
         (assigned_open ? "AI_PENDING_ASSIGNED" : "AI_PENDING_NONE_VERIFIED"));
    log << "{\"event\":\"AUDIT_SUMMARY\",\"complete\":" << (audit_complete ? "true" : "false")
        << ",\"journal_selected_receipts_verified\":" << journal_selected_receipts.size()
        << ",\"journal_dependency_blocks_unavailable\":" << journal_dependency_blocks_unavailable
        << ",\"initial_daa\":" << scan.initial_daa << ",\"final_daa\":" << scan.final_daa
        << ",\"initial_selected_sink\":\"" << scan.initial_selected_sink
        << "\",\"watermark_selected_sink\":\"" << scan.watermark_selected_sink
        << "\",\"watermark_daa\":" << scan.watermark_daa
        << ",\"cutoff_daa\":" << scan.cutoff_daa << ",\"hashes_visited\":" << scan.hashes_scheduled
        << ",\"blocks_recovered\":" << scan.blocks_recovered << ",\"blocks_unavailable\":" << scan.blocks_unavailable
        << ",\"rpc_errors\":" << scan.rpc_errors << ",\"live_blocks_seen\":" << scan.live_blocks_seen
        << ",\"live_blocks_recovered\":" << scan.live_blocks_recovered
        << ",\"live_blocks_unavailable\":" << scan.live_blocks_unavailable
        << ",\"live_duplicate_notifications\":" << scan.live_duplicate_notifications
        << ",\"live_snapshot_deduplicated\":" << scan.live_snapshot_deduplicated
        << ",\"live_queue_overflow\":" << scan.live_queue_overflow
        << ",\"live_watermark_events\":" << scan.live_watermark_events
        << ",\"live_watermark_complete\":" << (scan.live_watermark_complete ? "true" : "false")
        << ",\"selected_chain_reorgs\":" << scan.selected_chain_reorgs
        << ",\"selected_chain_advances\":" << scan.selected_chain_advances
        << ",\"selected_chain_reinclusions\":" << scan.selected_chain_reinclusions
        << ",\"selected_chain_blocks_removed\":" << scan.selected_chain_blocks_removed
        << ",\"selected_chain_blocks_added\":" << scan.selected_chain_blocks_added
        << ",\"selected_chain_blocks_reincluded\":" << scan.selected_chain_blocks_reincluded
        << ",\"selected_chain_duplicate_events\":" << scan.selected_chain_duplicate_events
        << ",\"selected_chain_out_of_order_events\":" << scan.selected_chain_out_of_order_events
        << ",\"reorg_during_scan\":" << (scan.reorg_during_scan ? "true" : "false")
        << ",\"ai_request_transactions\":" << ai_tx_seen << ",\"ai_requests_parsed\":" << requests.size()
        << ",\"invalid_ai_requests\":" << invalid_ai_requests << ",\"audit_index_truncated\":"
        << (audit_index_truncated ? "true" : "false")
        << ",\"ai_responses_seen\":" << ai_responses_seen << ",\"assigned_open\":" << assigned_open
        << ",\"assigned_expired\":" << assigned_expired << ",\"already_answered\":" << already_answered
        << ",\"journal_ambiguous\":" << journal_only << ",\"mempool_known\":" << mempool_known
        << ",\"pending_class\":\"" << pending_class << "\",\"provider_listing\":\""
        << listing_text << "\",\"transactions\":\"READ_ONLY\",\"cuda\":\"NOT_LINKED\"}\n";
    log.flush();
    if (!log) throw std::runtime_error("persistent AI audit summary write failed");
    std::cout << "AI_BACKFILL_AUDIT=" << (audit_complete ? "COMPLETE" : "INCOMPLETE")
              << " initial_daa=" << scan.initial_daa << " final_daa=" << scan.final_daa
              << " cutoff_daa=" << scan.cutoff_daa << " window_daa=" << options.window_daa << " hashes=" << scan.hashes_scheduled
              << " blocks_recovered=" << scan.blocks_recovered << " unavailable=" << scan.blocks_unavailable
              << " rpc_errors=" << scan.rpc_errors << " live_blocks=" << scan.live_blocks_seen
              << " ai_request_transactions=" << ai_tx_seen << " ai_requests=" << requests.size()
              << " invalid_requests=" << invalid_ai_requests << " index_truncated=" << audit_index_truncated
              << " ai_responses=" << ai_responses_seen << " assigned_open=" << assigned_open
              << " assigned_expired=" << assigned_expired << " answered=" << already_answered
              << " journal_ambiguous=" << journal_only << " mempool_known=" << mempool_known
              << " pending_state=" << pending_class << " provider_listing=" << listing_text
              << " journal_records=" << journal_records.size() << " cuda=NOT_LINKED transactions=READ_ONLY"
              << " jsonl=" << output_path << '\n';
    return stop.load(std::memory_order_acquire) ? 130 : (audit_complete ? 0 : 4);
  } catch (const std::exception &e) {
    std::cerr << "AI_BACKFILL_AUDIT=ERROR detail=" << e.what() << " transactions=DISABLED cuda=DISABLED\n";
    return 3;
  }
}
