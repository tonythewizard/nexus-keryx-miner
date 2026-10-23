#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/ai_reorg_reconciliation.hpp"
#include "messages.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
class AuditFixture final : public protowire::RPC::Service {
public:
  explicit AuditFixture(bool emit_reorg = false, std::string failing_hash = {})
      : emit_reorg_(emit_reorg), failing_hash_(std::move(failing_hash)) {}
  grpc::Status MessageStream(grpc::ServerContext *,
      grpc::ServerReaderWriter<protowire::KaspadResponse, protowire::KaspadRequest> *stream) override {
    protowire::KaspadRequest request;
    while (stream->Read(&request)) {
      protowire::KaspadResponse response;
      response.set_id(request.id());
      bool emit_post_snapshot = false;
      switch (request.payload_case()) {
        case protowire::KaspadRequest::kGetCurrentNetworkRequest:
          response.mutable_getcurrentnetworkresponse()->set_currentnetwork("Mainnet"); break;
        case protowire::KaspadRequest::kGetInfoRequest:
          response.mutable_getinforesponse()->set_serverversion("audit-fixture");
          response.mutable_getinforesponse()->set_issynced(true); break;
        case protowire::KaspadRequest::kNotifyBlockAddedRequest:
          response.mutable_notifyblockaddedresponse(); break;
        case protowire::KaspadRequest::kNotifyVirtualChainChangedRequest:
          response.mutable_notifyvirtualchainchangedresponse(); break;
        case protowire::KaspadRequest::kGetBlockDagInfoRequest: {
          ++dag_info_requests;
          auto *dag = response.mutable_getblockdaginforesponse();
          dag->set_virtualdaascore(12000);
          dag->set_sink(block_a);
          if (dag_info_requests == 1) {
            dag->add_virtualparenthashes(block_a);
            emit_post_snapshot = true;
          }
          break;
        }
        case protowire::KaspadRequest::kGetBlockRequest: {
          ++get_block_requests;
          const auto &hash = request.getblockrequest().hash();
          auto *block_response = response.mutable_getblockresponse();
          if (hash == failing_hash_) {
            ++failing_hash_requests;
            block_response->mutable_error()->set_message("temporary fixture retrieval error");
            break;
          }
          auto *block = block_response->mutable_block();
          block->mutable_verbosedata()->set_hash(hash);
          if (hash == block_a) {
            block->mutable_header()->set_daascore(11999);
            block->mutable_header()->add_parents()->add_parenthashes(block_b);
          } else if (hash == block_b) {
            block->mutable_header()->set_daascore(6000); // exact lower edge: do not visit parents
          } else if (hash == "live-0" || hash == "live-1" || hash.starts_with("chain-")) {
            block->mutable_header()->set_daascore(hash == "live-0" ? 12001 : 12002);
          } else {
            response.mutable_getblockresponse()->mutable_error()->set_message("unexpected fixture hash");
          }
          break;
        }
        case protowire::KaspadRequest::kGetBlockTemplateRequest:
          ++template_requests;
          response.mutable_getblocktemplateresponse();
          break;
        default:
          continue;
      }
      if (!stream->Write(response)) break;
      if (emit_post_snapshot) {
        for (unsigned i = 0; i < 2; ++i) {
          protowire::KaspadResponse notification;
          auto *added = notification.mutable_blockaddednotification();
          added->mutable_block()->mutable_verbosedata()->set_hash("live-" + std::to_string(i));
          added->mutable_block()->mutable_header()->set_daascore(12001 + i);
          if (!stream->Write(notification)) break;
        }
        protowire::KaspadResponse duplicate;
        auto *added = duplicate.mutable_blockaddednotification();
        added->mutable_block()->mutable_verbosedata()->set_hash("live-0");
        added->mutable_block()->mutable_header()->set_daascore(12001);
        (void)stream->Write(duplicate);
        protowire::KaspadResponse overlaps_snapshot;
        auto *overlap = overlaps_snapshot.mutable_blockaddednotification();
        overlap->mutable_block()->mutable_verbosedata()->set_hash(block_b);
        overlap->mutable_block()->mutable_header()->set_daascore(6000);
        (void)stream->Write(overlaps_snapshot);
        if (emit_reorg_) {
          auto notify = [&](std::initializer_list<std::string> removed,
                            std::initializer_list<std::string> added) {
            protowire::KaspadResponse event;
            auto *changed = event.mutable_virtualchainchangednotification();
            for (const auto &hash : removed) changed->add_removedchainblockhashes(hash);
            for (const auto &hash : added) changed->add_addedchainblockhashes(hash);
            (void)stream->Write(event);
          };
          notify({}, {"chain-advance"});
          notify({"chain-orphan"}, {"chain-replacement"});
          notify({}, {"chain-orphan"}); // same logical request can be re-included
          notify({"chain-orphan"}, {"chain-replacement"}); // exact duplicate event
          notify({}, {"chain-replacement"}); // repeated add without an intervening removal is out of order
        }
      }
      request.Clear();
    }
    return grpc::Status::OK;
  }
  std::atomic_uint dag_info_requests{0}, get_block_requests{0}, template_requests{0}, failing_hash_requests{0};
  bool emit_reorg_{};
  std::string failing_hash_;
  static constexpr const char *block_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  static constexpr const char *block_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
};
}

int main() {
  try {
    using keryx::nexus::AiReorgBlockEvidence;
    using keryx::nexus::AiSelectedChainChange;
    std::map<std::string, AiReorgBlockEvidence> path_blocks{
      {"tip-a", {"common", 15, true}}, {"common", {"base", 14, true}},
      {"base", {"genesis", 12, true}}, {"genesis", {"", 9, true}},
      {"tip-b", {"common", 15, std::nullopt}}, {"tip-c", {"common", 15, true}},
      {"tip-d", {"tip-c", 16, true}}
    };
    AiSelectedChainChange replace;
    replace.sequence = 1; replace.removed = {"tip-a"}; replace.added = {"tip-b"};
    AiSelectedChainChange replace_again;
    replace_again.sequence = 2; replace_again.removed = {"tip-b"}; replace_again.added = {"tip-c"};
    AiSelectedChainChange advance;
    advance.sequence = 3; advance.added = {"tip-d"};
    const auto reconciled = keryx::nexus::reconcile_ai_selected_chain(
        "tip-a", {replace, replace_again, advance}, path_blocks, 10);
    if (!reconciled.complete || reconciled.final_selected_sink != "tip-d" ||
        reconciled.common_ancestors != std::vector<std::string>{"common", "common"} ||
        reconciled.selected_chain.at("tip-a") || reconciled.selected_chain.at("tip-b") ||
        !reconciled.selected_chain.at("tip-c") || !reconciled.selected_chain.at("tip-d"))
      throw std::runtime_error("multiple verified common-ancestor selected-chain replays failed");
    const std::map<std::string, std::vector<std::string>> logical_request_appearances{
      {"canonical-request-id", {"tip-a", "tip-c"}},
      {"orphan-only-request", {"tip-a"}}
    };
    const auto request_is_selected = [&](const std::string &id) {
      for (const auto &hash : logical_request_appearances.at(id)) {
        const auto it = reconciled.selected_chain.find(hash);
        if (it != reconciled.selected_chain.end() && it->second) return true;
      }
      return false;
    };
    const std::map<std::string, std::vector<std::string>> response_appearances{
      {"canonical-request-id", {"tip-a", "tip-c"}}
    };
    std::size_t selected_response_appearances = 0;
    for (const auto &hash : response_appearances.at("canonical-request-id")) {
      const auto it = reconciled.selected_chain.find(hash);
      if (it != reconciled.selected_chain.end() && it->second) ++selected_response_appearances;
    }
    if (logical_request_appearances.size() != 2 || !request_is_selected("canonical-request-id") ||
        request_is_selected("orphan-only-request") || !response_appearances.contains("canonical-request-id") ||
        selected_response_appearances != 1)
      throw std::runtime_error("canonical request/response reconciliation across an orphan failed");
    const auto duplicate_reconciled = keryx::nexus::reconcile_ai_selected_chain(
        "tip-a", {replace, replace}, path_blocks, 10);
    if (!duplicate_reconciled.complete || duplicate_reconciled.final_selected_sink != "tip-b")
      throw std::runtime_error("exact duplicate selected-chain notification was not idempotent");
    const auto no_reorg = keryx::nexus::reconcile_ai_selected_chain("tip-a", {}, path_blocks, 10);
    const auto reorg_before_scan = keryx::nexus::reconcile_ai_selected_chain("tip-c", {}, path_blocks, 10);
    if (!no_reorg.complete || no_reorg.final_selected_sink != "tip-a" ||
        !reorg_before_scan.complete || reorg_before_scan.final_selected_sink != "tip-c")
      throw std::runtime_error("stable selected-chain anchors without in-window reorg failed");
    auto broken = replace;
    broken.sequence = 3; broken.out_of_order = true;
    const auto out_of_order = keryx::nexus::reconcile_ai_selected_chain("tip-a", {broken}, path_blocks, 10);
    const auto missing = keryx::nexus::reconcile_ai_selected_chain("tip-a", {replace}, {}, 10);
    const auto over_bound = keryx::nexus::reconcile_ai_selected_chain("tip-a", {replace}, path_blocks, 10, 1);
    if (out_of_order.complete || missing.complete || missing.error.empty() || over_bound.complete)
      throw std::runtime_error("reorg reconciliation accepted out-of-order or incomplete evidence");
    if (keryx::nexus::ai_audit_journal_terminal(keryx::nexus::ai::ResponseState::Finalized) == false ||
        keryx::nexus::ai_audit_journal_terminal(keryx::nexus::ai::ResponseState::Expired) == false ||
        keryx::nexus::ai_audit_journal_requires_reconciliation(keryx::nexus::ai::ResponseState::Submitting) == false ||
        keryx::nexus::ai_audit_journal_requires_reconciliation(keryx::nexus::ai::ResponseState::UnknownAfterTimeout) == false ||
        keryx::nexus::ai_audit_journal_requires_reconciliation(keryx::nexus::ai::ResponseState::RpcAcknowledged) == false ||
        keryx::nexus::ai_audit_journal_requires_reconciliation(keryx::nexus::ai::ResponseState::ResponseReorged) == false)
      throw std::runtime_error("terminal and unresolved journal states were not distinguished");

    AuditFixture fixture;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&fixture);
    auto server = builder.BuildAndStart();
    if (!server || port == 0) throw std::runtime_error("audit fixture gRPC server failed to start");
    std::atomic_bool stop{false};
    std::size_t backfill_callbacks = 0, live_callbacks = 0;
    keryx::nexus::AiBackfillScanOptions options;
    options.window_daa = 6000;
    options.max_inflight = 1;
    options.max_blocks = 8;
    options.timeout = std::chrono::seconds(5);
    options.settle = std::chrono::milliseconds(100);
    const auto result = keryx::nexus::run_ai_backfill_scan(
        "grpc://127.0.0.1:" + std::to_string(port), options, stop,
        [&](const protowire::RpcBlock &, bool backfill) {
          if (backfill) ++backfill_callbacks;
          else ++live_callbacks;
        });
    server->Shutdown();
    if (!result.complete || !result.synced || result.network != "mainnet" ||
        result.initial_daa != 12000 || result.cutoff_daa != 6000 ||
        result.hashes_scheduled != 2 || result.blocks_recovered != 2 || result.live_blocks_seen != 3 ||
        result.live_blocks_recovered != 2 || result.live_watermark_events != 3 ||
        !result.live_watermark_complete || result.live_duplicate_notifications != 1 ||
        result.live_snapshot_deduplicated != 1 ||
        backfill_callbacks != 2 || live_callbacks != 2 || fixture.dag_info_requests != 2 ||
        fixture.get_block_requests != 4 || fixture.template_requests != 0)
      throw std::runtime_error("read-only bounded audit scan violated coverage or no-template assertions");

    AuditFixture reorg_fixture(true);
    grpc::ServerBuilder reorg_builder;
    int reorg_port = 0;
    reorg_builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &reorg_port);
    reorg_builder.RegisterService(&reorg_fixture);
    auto reorg_server = reorg_builder.BuildAndStart();
    if (!reorg_server || reorg_port == 0) throw std::runtime_error("reorg audit fixture failed to start");
    const auto reorg_result = keryx::nexus::run_ai_backfill_scan(
        "grpc://127.0.0.1:" + std::to_string(reorg_port), options, stop, {});
    reorg_server->Shutdown();
    if (!reorg_result.complete || !reorg_result.reorg_during_scan ||
        reorg_result.selected_chain_reorgs != 1 || reorg_result.selected_chain_advances != 1 ||
        reorg_result.selected_chain_reinclusions != 1 ||
        reorg_result.selected_chain_blocks_removed != 1 || reorg_result.selected_chain_blocks_added != 4 ||
        reorg_result.selected_chain_blocks_reincluded != 1 ||
        reorg_result.selected_chain_duplicate_events != 1 ||
        reorg_result.selected_chain_out_of_order_events != 1 ||
        reorg_result.selected_chain_changes.size() != 5 ||
        reorg_result.live_blocks_recovered != 5 || reorg_result.live_watermark_events != 6 ||
        reorg_result.watermark_daa != 12000 || reorg_result.watermark_selected_sink != AuditFixture::block_a ||
        !reorg_result.live_watermark_complete || reorg_fixture.get_block_requests != 7)
      throw std::runtime_error("selected-chain reconciliation mismatch: complete=" + std::to_string(reorg_result.complete) +
          " reorgs=" + std::to_string(reorg_result.selected_chain_reorgs) +
          " advances=" + std::to_string(reorg_result.selected_chain_advances) +
          " removed=" + std::to_string(reorg_result.selected_chain_blocks_removed) +
          " added=" + std::to_string(reorg_result.selected_chain_blocks_added) +
          " dup=" + std::to_string(reorg_result.selected_chain_duplicate_events) +
          " order=" + std::to_string(reorg_result.selected_chain_out_of_order_events) +
          " events=" + std::to_string(reorg_result.selected_chain_changes.size()) +
          " live=" + std::to_string(reorg_result.live_blocks_recovered) +
          " wm=" + std::to_string(reorg_result.live_watermark_events) +
          " recovered_hash_requests=" + std::to_string(reorg_fixture.get_block_requests));

    AuditFixture retry_fixture(false, AuditFixture::block_a);
    grpc::ServerBuilder retry_builder;
    int retry_port = 0;
    retry_builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &retry_port);
    retry_builder.RegisterService(&retry_fixture);
    auto retry_server = retry_builder.BuildAndStart();
    if (!retry_server || retry_port == 0) throw std::runtime_error("bounded retry fixture failed to start");
    const auto retry_result = keryx::nexus::run_ai_backfill_scan(
        "grpc://127.0.0.1:" + std::to_string(retry_port), options, stop, {});
    retry_server->Shutdown();
    if (retry_result.complete || retry_result.rpc_errors != 2 || retry_result.blocks_unavailable != 1 ||
        retry_fixture.failing_hash_requests != 2)
      throw std::runtime_error("bounded GetBlock retry did not stop fail-closed after exhaustion: complete=" +
          std::to_string(retry_result.complete) + " rpc=" + std::to_string(retry_result.rpc_errors) +
          " unavailable=" + std::to_string(retry_result.blocks_unavailable) +
          " requests=" + std::to_string(retry_fixture.failing_hash_requests));

    keryx::nexus::AiBackfillTraversal bounded(10, 2);
    if (!bounded.seed(100, {std::string(AuditFixture::block_a), std::string(AuditFixture::block_b), "overflow"}) ||
        bounded.scheduled_count() != 2 || !bounded.at_limit())
      throw std::runtime_error("ancestor traversal did not enforce the configured memory bound");
    std::cout << "AI_BACKFILL_AUDIT_TEST=PASS dag_snapshot=true bounded_ancestor_scan=true "
                 "selected_chain_advance=true reorg_removed_added=true reinclusion=true duplicate_out_of_order=true "
                 "getblock=7 live_events_reconciled=6 snapshot_dedup=1 duplicate_notifications=1 "
                 "multiple_reorgs=true common_ancestor_verified=true bounded_retry_exhaustion=true "
                 "templates=0 synced=true\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "AI_BACKFILL_AUDIT_TEST=FAIL detail=" << e.what() << '\n';
    return 1;
  }
}
