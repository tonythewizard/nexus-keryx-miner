#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/opoi.hpp"
#include "messages.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

class CoalescingNode final : public protowire::RPC::Service {
public:
  explicit CoalescingNode(bool chain_notices=false):chain_notices_(chain_notices){}
  std::atomic_bool inference_result_seen{false};
  grpc::Status MessageStream(grpc::ServerContext *,
      grpc::ServerReaderWriter<protowire::KaspadResponse, protowire::KaspadRequest> *stream) override {
    protowire::KaspadRequest request;
    bool challenge_sent = false;
    unsigned mining_template_requests = 0;
    while (stream->Read(&request)) {
      protowire::KaspadResponse response;
      response.set_id(request.id());
      bool first_job = false;
      bool second_identical_refresh = false;
      switch (request.payload_case()) {
        case protowire::KaspadRequest::kGetCurrentNetworkRequest:
          response.mutable_getcurrentnetworkresponse()->set_currentnetwork("Mainnet");
          break;
        case protowire::KaspadRequest::kGetInfoRequest:
          response.mutable_getinforesponse()->set_serverversion("scheduler-coalescing-mock");
          response.mutable_getinforesponse()->set_issynced(true);
          break;
        case protowire::KaspadRequest::kGetBlockTemplateRequest: {
          const auto &req = request.getblocktemplaterequest();
          auto *t = response.mutable_getblocktemplateresponse();
          if (!challenge_sent && req.inferenceresult().empty()) {
            challenge_sent = true;
            t->set_inferencechallenge("bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710:0011aaff");
            auto *h = t->mutable_block()->mutable_header();
            h->set_daascore(899); h->set_bits(0x207fffffU); h->set_timestamp(1700000999); h->set_nonce(0);
          } else {
            if (!inference_result_seen.load(std::memory_order_acquire)) {
              if (req.inferenceresult() != "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710:0011aaff:mock answer")
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "OPoI inferenceResult wire mismatch");
              inference_result_seen.store(true, std::memory_order_release);
            }
            t->set_issynced(true);
            auto *block = t->mutable_block();
            auto *h = block->mutable_header();
            if (mining_template_requests <= (chain_notices_?1U:2U)) {
              // Initial template plus two responses to a burst of duplicate
              // notifications. Consensus fields stay byte-identical; only
              // node verbose diagnostics change on the second refresh.
              h->set_daascore(901); h->set_bits(0x207fffffU); h->set_timestamp(1700001001); h->set_nonce(1);
              block->mutable_verbosedata()->set_hash(mining_template_requests == (chain_notices_?1U:2U) ? "diagnostic-only-change" : "");
              block->mutable_verbosedata()->set_difficulty(mining_template_requests == (chain_notices_?1U:2U) ? 7.0 : 1.0);
              first_job = mining_template_requests == 0;
              second_identical_refresh = mining_template_requests == (chain_notices_?1U:2U);
            } else {
              // This is a genuine consensus-bearing replacement.
              h->set_daascore(902); h->set_bits(0x207fffffU); h->set_timestamp(1700001002); h->set_nonce(2);
              h->add_parents()->add_parenthashes(std::string(64, '1'));
            }
            ++mining_template_requests;
          }
          break;
        }
        case protowire::KaspadRequest::kNotifyNewBlockTemplateRequest:
          response.mutable_notifynewblocktemplateresponse();
          break;
        case protowire::KaspadRequest::kNotifyVirtualChainChangedRequest:
          response.mutable_notifyvirtualchainchangedresponse();
          break;
        default:
          continue;
      }
      if (!stream->Write(response)) break;
      if (first_job) {
        std::unique_lock lock(mutex_);
        first_sent_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return release_duplicate_burst_; });
        lock.unlock();
        protowire::KaspadResponse one, two;
        if(chain_notices_) {
          one.mutable_virtualchainchangednotification();two.mutable_virtualchainchangednotification();
        } else {
          one.mutable_newblocktemplatenotification();two.mutable_newblocktemplatenotification();
        }
        if (!stream->Write(one) || !stream->Write(two)) break;
      }
      if (second_identical_refresh) {
        std::unique_lock lock(mutex_);
        duplicate_refresh_sent_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return release_real_change_; });
        lock.unlock();
        protowire::KaspadResponse notification;
        if(chain_notices_)notification.mutable_virtualchainchangednotification()->add_removedchainblockhashes(std::string(64,'1'));
        else notification.mutable_newblocktemplatenotification();
        if (!stream->Write(notification)) break;
      }
      request.Clear();
    }
    return grpc::Status::OK;
  }

  void release_duplicate_burst() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return first_sent_; });
    release_duplicate_burst_ = true;
    changed_.notify_all();
  }
  void release_real_change() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return duplicate_refresh_sent_; });
    release_real_change_ = true;
    changed_.notify_all();
  }
private:
  bool chain_notices_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool first_sent_{};
  bool release_duplicate_burst_{};
  bool duplicate_refresh_sent_{};
  bool release_real_change_{};
};

int main() {
  try {
    for(bool chain_notices:{false,true}) {
    CoalescingNode service(chain_notices);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server || port == 0) throw std::runtime_error("could not start scheduler loopback mock");
    keryx::nexus::LiveJobQueue queue;
    std::atomic_bool stop{false};
    std::atomic_int result{-99};
    std::thread client([&] {
      result.store(keryx::nexus::run_live_job_client(
          "grpc://127.0.0.1:" + std::to_string(port), "mock-payout", "", queue, 0, 0, stop,
          [](const std::string &wire) {
            const auto challenge = keryx::nexus::opoi::parse_challenge(wire);
            return keryx::nexus::opoi::format_inference_result(challenge, "mock answer");
          }), std::memory_order_release);
    });
    auto first = queue.wait_pop();
    if (!first || first->generation != 1 || !first->lease.valid() || first->fields.daa_score != 901)
      throw std::runtime_error("initial live gRPC job was not published");
    service.release_duplicate_burst();
    auto resumed = queue.wait_pop();
    if (!resumed || resumed->generation != 1 || !resumed->lease.valid() ||
        resumed->fields.daa_score != 901 || !queue.admission_allowed(1))
      throw std::runtime_error("duplicate notifications changed the lease or failed to resume admission");
    if (resumed->fields.consensus_identity_wire != first->fields.consensus_identity_wire)
      throw std::runtime_error("verbose-only response change altered canonical consensus identity");
    if (queue.duplicate_count() != 1)
      throw std::runtime_error("duplicate template refresh was not counted exactly once");
    service.release_real_change();
    auto second = queue.wait_pop();
    if (!second || second->generation != 2 || !second->lease.valid() || second->fields.daa_score != 902)
      throw std::runtime_error("consensus-bearing template replacement was not installed as generation 2");
    if (first->lease.valid()) throw std::runtime_error("real template change did not stale prior lease");
    if (first->fields.response_wire == second->fields.response_wire)
      throw std::runtime_error("changed templates unexpectedly have identical full response bytes");
    if (!service.inference_result_seen.load(std::memory_order_acquire))
      throw std::runtime_error("OPoI fixture request was not answered through official gRPC field");
    stop.store(true, std::memory_order_release);
    client.join();
    queue.close();
    server->Shutdown();
    if (result.load(std::memory_order_acquire) != 0)
      throw std::runtime_error("cooperative gRPC client shutdown failed");
    std::cout << "LIVE_GRPC_QUEUE=PASS duplicate_notifications_coalesced=2 identical_snapshot_generation_stable=true verbose_data_ignored=true real_change_generation=2 stale_lease_cancelled=true opoi_mock_wire=PASS disk_handoff=false cooperative_stop=true\n";
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "LIVE_GRPC_QUEUE=FAIL " << e.what() << '\n';
    return 1;
  }
}
