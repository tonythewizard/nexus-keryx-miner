#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/chain_notice.hpp"

#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/pipeline.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/node_stream_liveness.hpp"
#include "keryx/nexus/opoi.hpp"
#include "messages.grpc.pb.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <grpcpp/grpcpp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <utility>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <syncstream>

namespace keryx::nexus {
namespace {
using Request = protowire::KaspadRequest;
using Response = protowire::KaspadResponse;
std::uint64_t monotonic_ns(){return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());}
double elapsed_ms(std::uint64_t start,std::uint64_t end){
  return static_cast<double>(end-start)/1.0e6;
}

class StopWatcher {
public:
  StopWatcher(const std::atomic_bool &stop, grpc::ClientContext &context)
      : stop_(stop), context_(context), thread_([this] {
          while (!finished_.load(std::memory_order_acquire)) {
            if (stop_.load(std::memory_order_relaxed)) {
              context_.TryCancel();
              return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
          }
        }) {}
  void stop() noexcept {
    finished_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
  }
  ~StopWatcher() { stop(); }
  StopWatcher(const StopWatcher &) = delete;
  StopWatcher &operator=(const StopWatcher &) = delete;
private:
  const std::atomic_bool &stop_;
  grpc::ClientContext &context_;
  std::atomic_bool finished_{false};
  std::thread thread_;
};

const google::protobuf::FieldDescriptor *field(const google::protobuf::Message &m, const char *name) {
  return m.GetDescriptor()->FindFieldByName(name);
}
google::protobuf::Message *set_payload(Request &request, const char *name) {
  const auto *f = field(request, name);
  if (!f || f->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
    throw std::runtime_error(std::string("official protobuf request payload missing: ") + name);
  return request.GetReflection()->MutableMessage(&request, f);
}
Request make_request(const char *payload_name, std::uint64_t id,
                     const std::string *pay = nullptr, const std::string *extra = nullptr,
                     const std::string *inference_result = nullptr) {
  Request request;
  const auto *id_field = field(request, "id");
  if (!id_field) throw std::runtime_error("official KaspadRequest.id field missing");
  request.GetReflection()->SetUInt64(&request, id_field, id);
  auto *payload = set_payload(request, payload_name);
  if (pay) {
    const auto *f = field(*payload, "payAddress");
    if (!f) throw std::runtime_error("official GetBlockTemplate.payAddress missing");
    payload->GetReflection()->SetString(payload, f, *pay);
  }
  if (extra) {
    const auto *f = field(*payload, "extraData");
    if (!f) throw std::runtime_error("official GetBlockTemplate.extraData missing");
    payload->GetReflection()->SetString(payload, f, *extra);
  }
  if (inference_result) {
    const auto *f = field(*payload, "inferenceResult");
    if (!f) throw std::runtime_error("official GetBlockTemplate.inferenceResult missing");
    payload->GetReflection()->SetString(payload, f, *inference_result);
  }
  return request;
}
const google::protobuf::Message *payload(const Response &response, const char *name) {
  const auto *f = field(response, name);
  if (!f || !response.GetReflection()->HasField(response, f)) return nullptr;
  return &response.GetReflection()->GetMessage(response, f);
}
std::uint64_t unsigned_value(const google::protobuf::Message &m, const char *name) {
  const auto *f = field(m, name);
  if (!f) throw std::runtime_error(std::string("official response field missing: ") + name);
  switch (f->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: return m.GetReflection()->GetUInt64(m, f);
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: return m.GetReflection()->GetUInt32(m, f);
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
      const auto v = m.GetReflection()->GetInt64(m, f);
      if (v < 0) throw std::runtime_error(std::string("negative integer field: ") + name);
      return static_cast<std::uint64_t>(v);
    }
    default: throw std::runtime_error(std::string("unexpected protobuf integer type: ") + name);
  }
}
std::string string_value(const google::protobuf::Message &m, const char *name) {
  const auto *f = field(m, name);
  return f ? m.GetReflection()->GetString(m, f) : std::string{};
}
bool bool_value(const google::protobuf::Message &m, const char *name) {
  const auto *f = field(m, name);
  return f && m.GetReflection()->GetBool(m, f);
}
std::string error_message(const google::protobuf::Message &m) {
  const auto *f = field(m, "error");
  return f && m.GetReflection()->HasField(m, f)
      ? string_value(m.GetReflection()->GetMessage(m, f), "message") : std::string{};
}
std::pair<std::string, bool> parse_endpoint(const std::string &endpoint) {
  if (endpoint.rfind("grpcs://", 0) == 0) return {endpoint.substr(8), true};
  if (endpoint.rfind("grpc://", 0) != 0)
    throw std::runtime_error("endpoint scheme must be grpcs:// or loopback-only grpc://");
  const std::string target = endpoint.substr(7);
  std::string host;
  if (!target.empty() && target.front() == '[') {
    const auto close = target.find(']');
    if (close == std::string::npos) throw std::runtime_error("malformed IPv6 endpoint");
    host = target.substr(1, close - 1);
  } else {
    const auto colon = target.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("endpoint port is required");
    host = target.substr(0, colon);
  }
  in_addr ipv4{};
  in6_addr ipv6{};
  const bool loop4 = inet_pton(AF_INET, host.c_str(), &ipv4) == 1 &&
                     (ntohl(ipv4.s_addr) >> 24U) == 127U;
  const bool loop6 = inet_pton(AF_INET6, host.c_str(), &ipv6) == 1 &&
                     IN6_IS_ADDR_LOOPBACK(&ipv6);
  if (!loop4 && !loop6)
    throw std::runtime_error("plaintext grpc:// accepts only numeric loopback addresses");
  return {target, false};
}

class NodeStreamWatchdog final {
public:
  NodeStreamWatchdog(const std::atomic_bool &stop, grpc::ClientContext &context,
                     std::shared_ptr<grpc::Channel> channel, LiveJobQueue *queue)
      : stop_(stop), context_(context), channel_(std::move(channel)), queue_(queue),
        thread_([this] {
          while (!finished_.load(std::memory_order_acquire) && !stop_.load(std::memory_order_relaxed)) {
            const auto action=liveness_.poll(monotonic_ns());
            if(action==NodeStreamLiveness::Action::Reconnect) {
              if(queue_) queue_->invalidate();
              context_.TryCancel();
              std::osyncstream(std::cerr)<<"NODE_STREAM_SILENCE elapsed_s=90 action=RECONNECT stale_lease_cancelled=true\n";
              return;
            }
            if(action==NodeStreamLiveness::Action::Probe) {
              // An independent read-only RPC avoids concurrent writers on the
              // mining stream and cannot change its template/challenge binding.
              // Its answer does not reset the original stream's silence timer.
              bool responded=false;
              try { responded=probe(); } catch(const std::exception &) {}
              std::osyncstream(std::cerr)<<"NODE_STREAM_SILENCE action=READ_ONLY_PROBE responded="
                  <<(responded?"true":"false")<<" original_stream_timer_reset=false\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
          }
        }) {}
  ~NodeStreamWatchdog(){stop();}
  void stop() noexcept {
    finished_.store(true,std::memory_order_release);
    if(thread_.joinable()) thread_.join();
  }
  bool begin_wait(){return liveness_.begin_wait(monotonic_ns());}
  bool end_wait(){return liveness_.end_wait();}
private:
  bool probe() {
    auto stub=protowire::RPC::NewStub(channel_);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now()+std::chrono::seconds(5));
    auto stream=stub->MessageStream(&context);
    if(!stream) return false;
    StopWatcher probe_stop(stop_,context);
    Response response;
    const bool answered=stream->Write(make_request("getInfoRequest",1)) && stream->WritesDone() &&
        stream->Read(&response) && response.id()==1 && response.has_getinforesponse() &&
        !response.getinforesponse().has_error();
    context.TryCancel();
    probe_stop.stop();
    (void)stream->Finish();
    return answered;
  }
  const std::atomic_bool &stop_;
  grpc::ClientContext &context_;
  std::shared_ptr<grpc::Channel> channel_;
  LiveJobQueue *queue_;
  NodeStreamLiveness liveness_;
  std::atomic_bool finished_{false};
  std::thread thread_;
};

TemplateFields read_template(const google::protobuf::Message &template_response,
                             const Response &envelope, const std::string &network,
                             const std::string &pay_address,
                             const std::string &extra_data) {
  const auto *block_field = field(template_response, "block");
  if (!block_field || !template_response.GetReflection()->HasField(template_response, block_field))
    throw std::runtime_error("template response has no block");
  const auto &block = template_response.GetReflection()->GetMessage(template_response, block_field);
  const auto *header_field = field(block, "header");
  if (!header_field || !block.GetReflection()->HasField(block, header_field))
    throw std::runtime_error("template block has no header");
  const auto &header = block.GetReflection()->GetMessage(block, header_field);

  TemplateFields result;
  result.network = network;
  result.pay_address = pay_address;
  result.extra_data = extra_data;
  const auto response_bytes = envelope.SerializeAsString();
  const auto block_bytes = block.SerializeAsString();
  result.response_wire.assign(response_bytes.begin(), response_bytes.end());
  result.block_wire.assign(block_bytes.begin(), block_bytes.end());
  // RpcBlock.verboseData contains diagnostics (difficulty, DAG labels, etc.)
  // which are not consensus inputs. Fingerprint every remaining block field,
  // including the complete header, parent set, transaction bodies, PoM fields,
  // and protobuf unknown fields.
  result.consensus_identity_wire = canonical_consensus_block_identity(block);
  result.daa_score = unsigned_value(header, "daaScore");
  result.compact_target_bits = static_cast<std::uint32_t>(unsigned_value(header, "bits"));
  result.timestamp = unsigned_value(header, "timestamp");
  result.template_nonce = unsigned_value(header, "nonce");
  result.header_hash_field = string_value(header, "hash");
  result.node_synced = bool_value(template_response, "isSynced");
  result.inference_challenge = string_value(template_response, "inferenceChallenge");
  result.received_monotonic_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());

  const auto *parents = field(header, "parents");
  if (parents) {
    const int levels = header.GetReflection()->FieldSize(header, parents);
    for (int i = 0; i < levels; ++i) {
      const auto &level = header.GetReflection()->GetRepeatedMessage(header, parents, i);
      const auto *hashes = field(level, "parentHashes");
      if (!hashes) continue;
      const int count = level.GetReflection()->FieldSize(level, hashes);
      for (int j = 0; j < count; ++j)
        result.parent_hashes.push_back(level.GetReflection()->GetRepeatedString(level, hashes, j));
    }
  }
  return result;
}
}

std::vector<std::uint8_t> canonical_consensus_block_identity(
    const google::protobuf::Message &block) {
  std::unique_ptr<google::protobuf::Message> canonical(block.New());
  canonical->CopyFrom(block);
  if (const auto *verbose = field(*canonical, "verboseData"))
    canonical->GetReflection()->ClearField(canonical.get(), verbose);
  std::string bytes;
  if (!canonical->SerializeToString(&bytes))
    throw std::runtime_error("could not serialize canonical consensus block identity");
  return {bytes.begin(), bytes.end()};
}

namespace {
int run_client(const std::string &endpoint, const std::string &pay_address,
               const std::string &extra_data, const std::string *capture_path,
               LiveJobQueue *live_queue, std::uint64_t max_templates,
               unsigned reconnect_after_seconds, const std::atomic_bool &stop_requested,
               const std::function<std::string(const std::string &)> &inference_handler = {},
               const AiBlockObserver &ai_block_observer = {},
               const AiRemovedChainObserver &ai_removed_chain_observer = {},
               const AiBackfillCompleteObserver &backfill_complete_observer = {}) {
  const auto [target, tls] = parse_endpoint(endpoint);
  std::unique_ptr<CaptureWriter> capture;
  if (capture_path) capture = std::make_unique<CaptureWriter>(*capture_path);
  JobPipeline pipeline;
  std::uint64_t templates_this_run = 0;
  unsigned backoff_ms = 250;

  while (!stop_requested.load(std::memory_order_relaxed) &&
         (max_templates == 0 || templates_this_run < max_templates)) {
    std::shared_ptr<grpc::ChannelCredentials> credentials = tls
        ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
        : grpc::InsecureChannelCredentials();
    grpc::ChannelArguments channel_args;
    channel_args.SetInt("grpc.keepalive_time_ms",15'000);
    channel_args.SetInt("grpc.keepalive_timeout_ms",20'000);
    channel_args.SetInt("grpc.keepalive_permit_without_calls",1);
    auto channel = grpc::CreateCustomChannel(target, credentials, channel_args);
    if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(10))) {
      pipeline.disconnect();
          if (live_queue) live_queue->invalidate();
      std::cerr << "CONNECT_FAILED endpoint=" << target << " timeout_s=10 retry_ms=" << backoff_ms
                << " submit=DISABLED\n" << std::flush;
      std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
      backoff_ms = std::min(backoff_ms * 2U, 5000U);
      continue;
    }

    auto stub = protowire::RPC::NewStub(channel);
    grpc::ClientContext context;
    if (reconnect_after_seconds != 0)
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(reconnect_after_seconds));
    auto stream = stub->MessageStream(&context);
    if (!stream) throw std::runtime_error("could not create official RPC.MessageStream");
    // Declare after the stream so its destructor always joins the cancellation
    // watcher before the stream/context can be destroyed.
    StopWatcher stop_watcher(stop_requested, context);
    NodeStreamWatchdog silence_watchdog(stop_requested, context, channel, live_queue);
    std::uint64_t next_id = 1;
    std::uint64_t network_id = next_id++;
    std::uint64_t info_id = next_id++;
    std::uint64_t template_id = 0;
    std::string template_extra_data = extra_data;
    std::uint64_t template_request_sent_ns = 0;
    std::string network;
    bool template_pending = false;
    bool obsolete_response = false;
    ChainNoticeFilter chain_notices;
    std::uint64_t notification_epoch = 0;
    std::uint64_t request_notification_epoch = 0;
    std::uint64_t dag_info_id = 0;
    AiBackfillTraversal backfill;
    std::unordered_map<std::uint64_t, bool> block_fetches; // request id -> startup backfill
    bool backfill_completion_reported = false;
    std::size_t backfill_blocks_unavailable = 0;
    constexpr std::size_t backfill_inflight_limit = 32;
    std::uint64_t templates_received=0;
    std::uint64_t template_diagnostics_ns=0;

    auto send = [&](Request request) {
      if(!silence_watchdog.begin_wait()) throw std::runtime_error("gRPC stream silence limit reached");
      const bool written=stream->Write(request);
      const bool healthy=silence_watchdog.end_wait();
      if(!healthy) throw std::runtime_error("gRPC stream silence limit reached");
      if (!written) throw std::runtime_error("gRPC request write failed");
    };
    auto get_block_request = [&](std::uint64_t id, const std::string &hash) {
      auto request = make_request("getBlockRequest", id);
      auto *p = set_payload(request, "getBlockRequest");
      const auto *hash_field = field(*p, "hash");
      const auto *transactions_field = field(*p, "includeTransactions");
      if (!hash_field || !transactions_field)
        throw std::runtime_error("official GetBlock hash/includeTransactions fields unavailable");
      p->GetReflection()->SetString(p, hash_field, hash);
      p->GetReflection()->SetBool(p, transactions_field, true);
      return request;
    };
    auto observe_block = [&](const protowire::RpcBlock &block, bool from_backfill) {
      std::cout << "AI_BLOCK_OBSERVED source=" << (from_backfill ? "startup_backfill" : "block_added")
                << " daa=" << (block.has_header() ? block.header().daascore() : 0)
                << " tx_count=" << block.transactions_size() << " submit=DISABLED\n" << std::flush;
      if (!ai_block_observer) return;
      try { ai_block_observer(block, from_backfill); }
      catch (const std::exception &e) {
        std::cerr << "AI_BLOCK_OBSERVER_ERROR backfill=" << (from_backfill ? "true" : "false")
                  << " detail=" << e.what() << "\n" << std::flush;
      }
    };
    auto pump_backfill = [&]() {
      while (block_fetches.size() < backfill_inflight_limit) {
        auto next_hash = backfill.pop();
        if (!next_hash) break;
        auto hash = std::move(*next_hash);
        const auto id = next_id++;
        send(get_block_request(id, hash));
        block_fetches.emplace(id, true);
        std::cout << "AI_BACKFILL_GETBLOCK_SENT id=" << id << " remaining=" << (backfill.scheduled_count())
                  << " in_flight=" << block_fetches.size() << "\n" << std::flush;
      }
    };
    auto request_template = [&]() {
      template_id = next_id++;
      template_extra_data = (live_queue && extra_data.find("/escrow:") != std::string::npos) ? opoi::fresh_coinbase_extra_data(extra_data) : extra_data;
      send(make_request("getBlockTemplateRequest", template_id, &pay_address, &template_extra_data));
      template_request_sent_ns = monotonic_ns();
      template_pending = true;
      obsolete_response = false;
      request_notification_epoch = notification_epoch;
    };
    auto request_template_with_inference = [&](const std::string &result) {
      template_id = next_id++;
      template_extra_data = (live_queue && extra_data.find("/escrow:") != std::string::npos) ? opoi::fresh_coinbase_extra_data(extra_data) : extra_data;
      send(make_request("getBlockTemplateRequest", template_id, &pay_address, &template_extra_data, &result));
      template_request_sent_ns = monotonic_ns();
      template_pending = true;
      obsolete_response = false;
      request_notification_epoch = notification_epoch;
    };

    try {
      send(make_request("getCurrentNetworkRequest", network_id));
      send(make_request("getInfoRequest", info_id));
      Response response;
      while (!stop_requested.load(std::memory_order_relaxed)) {
        if(!silence_watchdog.begin_wait()) break;
        const bool received=stream->Read(&response);
        const bool healthy=silence_watchdog.end_wait();
        if(!received || !healthy) break;
        if (const auto *info = payload(response, "getInfoResponse")) {
          const auto id = unsigned_value(response, "id");
          if (id != 0 && id != info_id) throw std::runtime_error("GetInfo response ID mismatch");
          if (!error_message(*info).empty()) throw std::runtime_error("node GetInfo RPC error: " + error_message(*info));
          std::cout << "NODE_INFO version=" << string_value(*info, "serverVersion")
                    << " synced=" << (bool_value(*info, "isSynced") ? "true" : "false")
                    << " submit=DISABLED\n" << std::flush;
        } else if (const auto *net = payload(response, "getCurrentNetworkResponse")) {
          const auto id = unsigned_value(response, "id");
          if (id != 0 && id != network_id) throw std::runtime_error("GetCurrentNetwork response ID mismatch");
          if (!error_message(*net).empty()) throw std::runtime_error("node GetCurrentNetwork RPC error: " + error_message(*net));
          network = string_value(*net, "currentNetwork");
          std::transform(network.begin(), network.end(), network.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          if (network != "mainnet" && network != "testnet")
            throw std::runtime_error("unknown network returned by node");
          std::cout << "NODE_NETWORK name=" << network << " submit=DISABLED\n" << std::flush;
          send(make_request("notifyNewBlockTemplateRequest", next_id++));
          send(make_request("notifyVirtualChainChangedRequest", next_id++));
          send(make_request("notifyBlockAddedRequest", next_id++));
          dag_info_id = next_id++;
          send(make_request("getBlockDagInfoRequest", dag_info_id));
          if (!template_pending) request_template();
        } else if (const auto *dag = payload(response, "getBlockDagInfoResponse")) {
          const auto id = unsigned_value(response, "id");
          if (id != 0 && id != dag_info_id)
            throw std::runtime_error("GetBlockDagInfo startup backfill response ID mismatch");
          if (!error_message(*dag).empty())
            throw std::runtime_error("GetBlockDagInfo startup backfill seed failed: " + error_message(*dag));
          else {
            const auto tip_daa = unsigned_value(*dag, "virtualDaaScore");
            std::vector<std::string> parents;
            const auto *parents_field = field(*dag, "virtualParentHashes");
            if (!parents_field) throw std::runtime_error("GetBlockDagInfo.virtualParentHashes unavailable");
            const auto *reflection = dag->GetReflection();
            const int count = reflection->FieldSize(*dag, parents_field);
            parents.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i)
              parents.push_back(reflection->GetRepeatedString(*dag, parents_field, i));
            (void)backfill.seed(tip_daa, parents);
            std::cout << "AI_STARTUP_BACKFILL_SEED=PASS source=GetBlockDagInfo daa=" << tip_daa
                      << " cutoff=" << backfill.cutoff_daa() << " virtual_parents=" << parents.size() << '\n' << std::flush;
            pump_backfill();
          }
        } else if (const auto *block_template = payload(response, "getBlockTemplateResponse")) {
          const auto response_received_ns=monotonic_ns();
          ++templates_received;
          if(!template_diagnostics_ns || response_received_ns-template_diagnostics_ns>=NodeStreamLiveness::probe_ns) {
            template_diagnostics_ns=response_received_ns;
            const auto &snapshot=response.getblocktemplateresponse();
            std::cout<<"TEMPLATE_INTAKE received="<<templates_received
                     <<" daa="<<(snapshot.has_block() && snapshot.block().has_header() ? snapshot.block().header().daascore() : 0)
                     <<" synced="<<(snapshot.issynced()?"true":"false")
                     <<" rpc_error="<<(snapshot.has_error()?"true":"false")
                     <<" backfill_inflight="<<block_fetches.size()<<'\n'<<std::flush;
          }
          const auto id = unsigned_value(response, "id");
          if (id != 0 && id != template_id) {
            std::cerr << "STALE_RESPONSE id=" << id << " expected=" << template_id << " ignored=true\n";
            response.Clear();
            continue;
          }
          template_pending = false;
          if (obsolete_response || request_notification_epoch != notification_epoch) {
            std::cout << "LIVE_TEMPLATE_RESPONSE=COALESCED request_id=" << id
                      << " notifications_during_request="
                      << (notification_epoch - request_notification_epoch)
                      << " fetch_latest=true\n" << std::flush;
            request_template();
            response.Clear();
            continue;
          }
          if (!error_message(*block_template).empty()) throw std::runtime_error("node GetBlockTemplate RPC error: " + error_message(*block_template));
          const auto validation_start_ns=monotonic_ns();
          auto fields = read_template(*block_template, response, network,
                                      pay_address, template_extra_data);
          const auto validation_end_ns=monotonic_ns();
          if (!fields.inference_challenge.empty() &&
              opoi::challenge_enabled_for_network(network, fields.daa_score)) {
            if (live_queue) live_queue->invalidate();
            if (!inference_handler) {
              std::cerr << "OPOI_CHALLENGE_PRESENT inference_handler=UNAVAILABLE mining_queue_invalidated=true submit=DISABLED\n";
            } else {
              const auto challenge = opoi::parse_challenge(fields.inference_challenge);
              const auto inference_result = inference_handler(fields.inference_challenge);
              if (inference_result.empty()) throw std::runtime_error("OPoI handler returned an empty result");
              request_template_with_inference(inference_result);
              std::cout << "OPOI_RESPONSE_SENT model=" << challenge.model_id_hex.substr(0, 8)
                        << " nonce_bytes=" << challenge.nonce_hex.size()/2 << " submit=DISABLED\n" << std::flush;
            }
            response.Clear();
            continue;
          }
          if (!fields.inference_challenge.empty())
            std::cerr << "OPOI_CHALLENGE_IGNORED reason=pom_already_active network=" << network
                      << " daa=" << fields.daa_score << " submit=DISABLED\n" << std::flush;
          if (capture) capture->append(network, fields.response_wire);
          TemplateJob job;
          bool duplicate = false;
          const auto publish_start_ns=monotonic_ns();
          if (live_queue) {
            if (!fields.node_synced || network != "mainnet") {
              live_queue->invalidate();
              job.fields = std::move(fields);
              job.generation = 0;
              std::cerr << "LIVE_JOB_INELIGIBLE network=" << network
                        << " synced=" << (fields.node_synced ? "true" : "false")
                        << " queue_publish=BLOCKED submit=DISABLED\n";
            } else {
              const auto queued = live_queue->publish(fields);
              duplicate = queued == LiveJobQueue::PublishResult::Duplicate;
              const auto generation = live_queue->current_generation().value_or(0);
              job = TemplateJob{generation, std::move(fields), JobLease{}};
            }
          } else {
            job = pipeline.accept(std::move(fields));
          }
          const auto publish_end_ns=monotonic_ns();
          ++templates_this_run;
          const auto fingerprint_start_ns=monotonic_ns();
          const auto fingerprint = canonical_job_fingerprint(job.fields);
          const auto fingerprint_end_ns=monotonic_ns();
          const auto response_digest=blake3_digest(job.fields.response_wire.data(),
                                                   job.fields.response_wire.size());
          const auto response_digest_hex=hex(response_digest.data(),response_digest.size());
          std::cout<<"{\"event\":\"LIVE_TEMPLATE_PROFILE\",\"generation\":"<<job.generation
                   <<",\"request_sent_monotonic_ns\":"<<template_request_sent_ns
                   <<",\"response_received_monotonic_ns\":"<<response_received_ns
                   <<",\"validation_start_monotonic_ns\":"<<validation_start_ns
                   <<",\"validation_end_monotonic_ns\":"<<validation_end_ns
                   <<",\"publish_start_monotonic_ns\":"<<publish_start_ns
                   <<",\"publish_end_monotonic_ns\":"<<publish_end_ns
                   <<",\"fingerprint_start_monotonic_ns\":"<<fingerprint_start_ns
                   <<",\"fingerprint_end_monotonic_ns\":"<<fingerprint_end_ns
                   <<",\"grpc_request_ms\":"<<elapsed_ms(template_request_sent_ns,response_received_ns)
                   <<",\"validation_ms\":"<<elapsed_ms(validation_start_ns,validation_end_ns)
                   <<",\"publish_ms\":"<<elapsed_ms(publish_start_ns,publish_end_ns)
                   <<",\"fingerprint_ms\":"<<elapsed_ms(fingerprint_start_ns,fingerprint_end_ns)<<"}\n"<<std::flush;
          std::cout << "LIVE_TEMPLATE generation=" << job.generation
                    << " disposition=" << (job.generation == 0 ? "ineligible" :
                        (duplicate ? "canonical_duplicate" : "published"))
                    << " fingerprint=" << fingerprint
                    << " network=" << job.fields.network
                    << " daa=" << job.fields.daa_score
                    << " compact_target_bits=0x" << std::hex << job.fields.compact_target_bits << std::dec
                    << " timestamp=" << job.fields.timestamp
                    << " template_nonce=" << job.fields.template_nonce
                    << " parents=" << job.fields.parent_hashes.size()
                    << " header_hash_field_present=" << (!job.fields.header_hash_field.empty() ? "true" : "false")
                    << " synced=" << (job.fields.node_synced ? "true" : "false")
                    << " inference_challenge=" << (!job.fields.inference_challenge.empty() ? "present" : "absent")
                    << " response_bytes=" << job.fields.response_wire.size()
                    << " response_blake3=" << response_digest_hex
                    << " lease=active submit=DISABLED\n" << std::flush;
          backoff_ms = 250;
          if (max_templates != 0 && templates_this_run >= max_templates) break;
        } else if (payload(response, "newBlockTemplateNotification")) {
          chain_notices.clear();
          ++notification_epoch;
          if (live_queue) {
            const bool first = live_queue->begin_refresh();
            std::cout << "JOB_REFRESH_PENDING source=new_block_template coalesced="
                      << (first ? "false" : "true")
                      << " generation=" << live_queue->current_generation().value_or(0)
                      << " lease_invalidated=false\n" << std::flush;
          } else {
            pipeline.invalidate();
          }
          if (template_pending) obsolete_response = true;
          else if (!network.empty()) request_template();
        } else if (const auto *chain_notice = payload(response, "virtualChainChangedNotification")) {
          const auto *removed = field(*chain_notice, "removedChainBlockHashes");
          const auto *added = field(*chain_notice, "addedChainBlockHashes");
          const auto *reflection = chain_notice->GetReflection();
          const bool chain_changed = !removed || !added ||
              reflection->FieldSize(*chain_notice, removed)!=0 || reflection->FieldSize(*chain_notice, added)!=0;
          const auto classification=chain_notices.observe(chain_notice->SerializeAsString(),chain_changed);
          if(classification==ChainNoticeFilter::Result::Duplicate) {
            std::cout<<"CHAIN_NOTIFICATION_DUPLICATE action=ignored exact_wire=true\n"<<std::flush;
            response.Clear();continue;
          }
          ++notification_epoch;
          if (ai_removed_chain_observer) {
            const auto *notification = payload(response, "virtualChainChangedNotification");
            const auto *removed_field = notification ? field(*notification, "removedChainBlockHashes") : nullptr;
            std::vector<std::string> removed;
            if (removed_field) {
              const auto *reflection = notification->GetReflection();
              const auto count = reflection->FieldSize(*notification, removed_field);
              removed.reserve(static_cast<std::size_t>(count));
              for (int i = 0; i < count; ++i)
                removed.push_back(reflection->GetRepeatedString(*notification, removed_field, i));
            }
            if (!removed.empty()) {
              try { ai_removed_chain_observer(removed); }
              catch (const std::exception &e) {
                std::cerr << "ESCROW_REORG_OBSERVER_ERROR detail=" << e.what() << "\n" << std::flush;
              }
            }
          }
          if (live_queue) {
            if(chain_changed) {
              live_queue->invalidate_for_virtual_chain_change();
              std::cout << "JOB_INVALIDATED source=virtual_chain_changed reason=selected_parent_chain_changed lease_invalidated=true\n" << std::flush;
            } else {
              (void)live_queue->begin_refresh();
              std::cout<<"JOB_REFRESH_PENDING source=empty_chain_notification lease_invalidated=false\n"<<std::flush;
            }
          } else {
            pipeline.invalidate();
          }
          const auto *notification = payload(response, "virtualChainChangedNotification");
          const auto *added_field = notification ? field(*notification, "addedChainBlockHashes") : nullptr;
          if (added_field) {
            const auto *reflection = notification->GetReflection();
            const auto count = reflection->FieldSize(*notification, added_field);
            for (int i = 0; i < count; ++i) {
              const auto hash = reflection->GetRepeatedString(*notification, added_field, i);
              if (hash.empty()) continue;
              const auto id = next_id++;
              send(get_block_request(id, hash));
              block_fetches.emplace(id, false);
              std::cout << "ESCROW_CHAIN_GETBLOCK_SENT id=" << id << "\n" << std::flush;
            }
          }
          if (template_pending) obsolete_response = true;
          else if (!network.empty()) request_template();
        } else if (const auto *added = payload(response, "blockAddedNotification")) {
          chain_notices.clear();
          ++notification_epoch;
          const auto *block_field = field(*added, "block");
          protowire::RpcBlock added_block;
          if (block_field && added->GetReflection()->HasField(*added, block_field)) {
            const auto &wire_block = added->GetReflection()->GetMessage(*added, block_field);
            if (const auto *typed = dynamic_cast<const protowire::RpcBlock *>(&wire_block)) {
              added_block.CopyFrom(*typed);
              if (added_block.transactions_size() > 0) {
                observe_block(added_block, false);
              } else if (added_block.has_verbosedata() && !added_block.verbosedata().hash().empty()) {
                const auto id = next_id++;
                send(get_block_request(id, added_block.verbosedata().hash()));
                block_fetches.emplace(id, false);
                std::cout << "AI_BLOCK_GETBLOCK_SENT id=" << id << " source=block_added\n" << std::flush;
              }
            }
          }
          pump_backfill();
          if (live_queue) {
            const bool first = live_queue->begin_refresh();
            std::cout << "JOB_REFRESH_PENDING source=block_added coalesced="
                      << (first ? "false" : "true")
                      << " generation=" << live_queue->current_generation().value_or(0)
                      << " lease_invalidated=false\n" << std::flush;
          } else {
            pipeline.invalidate();
          }
          if (template_pending) obsolete_response = true;
          else if (!network.empty()) request_template();
        } else if (payload(response, "getBlockResponse")) {
          const auto id = unsigned_value(response, "id");
          const auto pending = block_fetches.find(id);
          if (pending != block_fetches.end()) {
            const bool from_backfill = pending->second;
            block_fetches.erase(pending);
            const auto *block_payload = payload(response, "getBlockResponse");
            const auto *block_field = block_payload ? field(*block_payload, "block") : nullptr;
            if (block_payload && error_message(*block_payload).empty() && block_field &&
                block_payload->GetReflection()->HasField(*block_payload, block_field)) {
              const auto &wire_block = block_payload->GetReflection()->GetMessage(*block_payload, block_field);
              if (const auto *typed = dynamic_cast<const protowire::RpcBlock *>(&wire_block)) {
                observe_block(*typed, from_backfill);
                if (from_backfill) (void)backfill.note_block(*typed);
              }
            } else if (block_payload && !error_message(*block_payload).empty()) {
              if (from_backfill) ++backfill_blocks_unavailable;
              std::cerr << "AI_BLOCK_FETCH_ERROR backfill=" << (from_backfill ? "true" : "false")
                        << " detail=" << error_message(*block_payload) << "\n" << std::flush;
            } else if (from_backfill) {
              ++backfill_blocks_unavailable;
              std::cerr << "AI_BLOCK_FETCH_ERROR backfill=true detail=block_payload_unavailable\n" << std::flush;
            }
            pump_backfill();
          } else {
            std::cout << "AI_BLOCK_GETBLOCK_UNMATCHED id=" << id << "\n" << std::flush;
          }
        }
        if (!backfill_completion_reported && backfill.seeded() && backfill.drained()) {
          const bool any_backfill_inflight = std::any_of(block_fetches.begin(), block_fetches.end(),
              [](const auto &item) { return item.second; });
          if (!any_backfill_inflight) {
            backfill_completion_reported = true;
            if (backfill_complete_observer) {
              try { backfill_complete_observer(backfill_blocks_unavailable == 0,
                                                backfill.scheduled_count(),
                                                backfill_blocks_unavailable); }
              catch (const std::exception &e) {
                std::cerr << "AI_BACKFILL_COMPLETION_OBSERVER_ERROR detail=" << e.what() << '\n' << std::flush;
              }
            }
            std::cout << "AI_STARTUP_BACKFILL_COMPLETE coverage="
                      << (backfill_blocks_unavailable == 0 ? "COMPLETE" : "INCOMPLETE")
                      << " hashes=" << backfill.scheduled_count()
                      << " unavailable=" << backfill_blocks_unavailable << "\n" << std::flush;
          }
        }
        response.Clear();
      }
      // Read has returned. Join the TryCancel watcher before Finish touches the
      // same ClientContext; otherwise cancellation can race gRPC call teardown.
      silence_watchdog.stop();
      stop_watcher.stop();
      (void)stream->WritesDone();
      const auto status = stream->Finish();
      pipeline.disconnect();
      if (live_queue) live_queue->invalidate();
      std::cerr << "CLIENT_STREAM_CLOSED code=" << status.error_code()
                << " stale_lease_cancelled=true reconnecting="
                << ((max_templates == 0 || templates_this_run < max_templates) && !stop_requested.load() ? "true" : "false")
                << " submit=DISABLED\n" << std::flush;
    } catch (const std::exception &e) {
      context.TryCancel();
      silence_watchdog.stop();
      stop_watcher.stop();
      pipeline.disconnect();
      if (live_queue) live_queue->invalidate();
      std::cerr << "CLIENT_STREAM_ERROR detail=" << e.what() << " stale_lease_cancelled=true submit=DISABLED\n" << std::flush;
      if (std::string(e.what()).find("RPC error:") != std::string::npos) return 1;
    }
    if (stop_requested.load(std::memory_order_relaxed) ||
        (max_templates != 0 && templates_this_run >= max_templates)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    backoff_ms = std::min(backoff_ms * 2U, 5000U);
  }
  pipeline.disconnect();
  if (live_queue) live_queue->invalidate();
  return 0;
}
} // namespace

AiBackfillTraversal::AiBackfillTraversal(std::uint64_t window_daa, std::size_t max_blocks)
    : window_daa_(window_daa), max_blocks_(max_blocks) {
  if (window_daa_ == 0 || max_blocks_ == 0 || max_blocks_ > 50000)
    throw std::invalid_argument("AiRequest backfill bounds must be positive and at most 50000 blocks");
}

bool AiBackfillTraversal::seed(std::uint64_t tip_daa,
                               const std::vector<std::string> &parent_hashes) {
  if (cutoff_daa_) return false;
  cutoff_daa_ = tip_daa > window_daa_ ? tip_daa - window_daa_ : 0;
  for (const auto &hash : parent_hashes) {
    if (hash.empty()) continue;
    if (visited_.size() >= max_blocks_) { at_limit_ = true; break; }
    if (visited_.insert(hash).second) queue_.push_back(hash);
  }
  return true;
}

bool AiBackfillTraversal::seed_from_block(const protowire::RpcBlock &block) {
  if (!block.has_header()) return false;
  if (!cutoff_daa_) {
    std::vector<std::string> parents;
    if (block.header().parents_size() > 0) {
      const auto &level = block.header().parents(0);
      parents.assign(level.parenthashes().begin(), level.parenthashes().end());
    }
    return seed(block.header().daascore(), parents);
  }
  return note_block(block);
}

bool AiBackfillTraversal::enqueue_parents(const protowire::RpcBlock &block) {
  if (!cutoff_daa_ || !block.has_header() || block.header().daascore() <= *cutoff_daa_ ||
      block.header().parents_size() == 0) return false;
  bool added = false;
  const auto &level = block.header().parents(0);
  for (const auto &hash : level.parenthashes()) {
    if (hash.empty() || visited_.contains(hash)) continue;
    if (visited_.size() >= max_blocks_) { at_limit_ = true; break; }
    visited_.insert(hash);
    queue_.push_back(hash);
    added = true;
  }
  return added;
}

bool AiBackfillTraversal::note_block(const protowire::RpcBlock &block) {
  return enqueue_parents(block);
}

std::optional<std::string> AiBackfillTraversal::pop() {
  if (queue_.empty()) return std::nullopt;
  auto value = std::move(queue_.front());
  queue_.pop_front();
  return value;
}

AiBackfillScanResult run_ai_backfill_scan(
    const std::string &endpoint, const AiBackfillScanOptions &options,
    const std::atomic_bool &stop_requested, const AiBackfillBlockObserver &observer) {
  if (options.max_inflight == 0 || options.max_inflight > 8 || options.max_blocks == 0 ||
      options.max_blocks > 50000 || options.timeout <= std::chrono::seconds::zero() ||
      options.settle < std::chrono::milliseconds::zero())
    throw std::invalid_argument("AI backfill audit limits outside bounded range");
  const auto [target, tls] = parse_endpoint(endpoint);
  const auto started = std::chrono::steady_clock::now();
  const auto hard_deadline = started + options.timeout;
  auto channel = grpc::CreateChannel(target, tls
      ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
      : grpc::InsecureChannelCredentials());
  if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(10)))
    throw std::runtime_error("AI backfill audit could not connect before deadline");
  auto stub = protowire::RPC::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + options.timeout);
  auto stream = stub->MessageStream(&context);
  if (!stream) throw std::runtime_error("AI backfill audit could not create RPC.MessageStream");

  AiBackfillScanResult result;
  AiBackfillTraversal traversal(options.window_daa, options.max_blocks);
  constexpr std::uint64_t network_id = 1, info_id = 2, notify_id = 3, dag_id = 4, virtual_chain_id = 5;
  std::uint64_t next_id = 10;
  std::uint64_t watermark_dag_id = 0;
  const auto send = [&](Request request) {
    if (!stream->Write(request)) throw std::runtime_error("AI backfill audit request write failed");
  };
  send(make_request("getCurrentNetworkRequest", network_id));
  send(make_request("getInfoRequest", info_id));
  // Subscribe before taking the virtual-parent snapshot. New blocks arriving
  // during the scan are fed through the same deduplicating traversal.
  send(make_request("notifyBlockAddedRequest", notify_id));
  send(make_request("notifyVirtualChainChangedRequest", virtual_chain_id));
  send(make_request("getBlockDagInfoRequest", dag_id));

  struct PendingFetch { std::string hash; bool backfill; unsigned attempts; };
  struct RetryFetch { std::string hash; bool backfill; unsigned attempts; };
  std::unordered_map<std::uint64_t, PendingFetch> pending;
  std::deque<RetryFetch> retry_queue;
  std::deque<std::string> live_queue;
  std::unordered_set<std::string> observed;
  std::unordered_set<std::string> live_event_hashes;
  std::unordered_set<std::string> selected_chain_event_hashes;
  std::unordered_set<std::string> live_event_reconciled;
  std::unordered_set<std::string> selected_chain_added_hashes;
  std::unordered_set<std::string> selected_chain_event_keys;
  std::unordered_map<std::string, unsigned char> last_selected_chain_action;
  std::unordered_set<std::string> in_flight_hashes;
  bool got_network = false, got_info = false, got_dag = false;
  bool snapshot_watermark_set = false;
  bool watermark_requested = false;
  bool live_event_overflow = false;
  std::size_t in_flight = 0;
  std::atomic_uint64_t drained_since_ns{0};
  const auto mono_ns = [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()); };
  std::atomic_bool cancel_stream{false};
  std::atomic_bool watcher_done{false};
  std::thread cancel_watcher([&] {
    while (!watcher_done.load(std::memory_order_acquire)) {
      if (stop_requested.load(std::memory_order_relaxed)) {
        cancel_stream.store(true, std::memory_order_release);
        context.TryCancel();
        return;
      }
      const auto drained = drained_since_ns.load(std::memory_order_acquire);
      if (drained != 0 && mono_ns() - drained >= static_cast<std::uint64_t>(options.settle.count()) * 1'000'000U) {
        cancel_stream.store(true, std::memory_order_release);
        context.TryCancel();
        return;
      }
      if (std::chrono::steady_clock::now() >= hard_deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  });
  auto finish_watcher = [&] {
    watcher_done.store(true, std::memory_order_release);
    if (cancel_watcher.joinable()) cancel_watcher.join();
  };
  auto schedule_fetch = [&](std::string hash, bool backfill, unsigned attempts) {
    if (hash.empty() || pending.size() >= options.max_inflight ||
        !in_flight_hashes.insert(hash).second) return false;
    const auto id = next_id++;
    auto request = make_request("getBlockRequest", id);
    auto *p = set_payload(request, "getBlockRequest");
    const auto *hash_field = field(*p, "hash");
    const auto *transactions_field = field(*p, "includeTransactions");
    if (!hash_field || !transactions_field) throw std::runtime_error("official GetBlock fields unavailable");
    p->GetReflection()->SetString(p, hash_field, hash);
    p->GetReflection()->SetBool(p, transactions_field, true);
    send(std::move(request));
    pending.emplace(id, PendingFetch{hash, backfill, attempts});
    ++in_flight;
    drained_since_ns.store(0, std::memory_order_release);
    return true;
  };
  auto scan_block = [&](const protowire::RpcBlock &block, bool backfill) {
    std::string hash;
    if (block.has_verbosedata()) hash = block.verbosedata().hash();
    if (hash.empty() && block.has_header()) hash = block.header().hash();
    if (!hash.empty() && !observed.insert(hash).second) {
      return;
    }
    if (backfill) ++result.blocks_recovered;
    if (block.has_header()) result.final_daa = std::max(result.final_daa, block.header().daascore());
    if (observer) observer(block, backfill);
    // Selected-chain replacement blocks may have ancestors absent from the
    // initial sink traversal. Feeding every fetched block to the same bounded,
    // hash-deduplicating traversal catches those parents without restarting the
    // historical window.
    (void)traversal.note_block(block);
  };

  try {
    Response response;
    while (!stop_requested.load(std::memory_order_relaxed) && !cancel_stream.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < hard_deadline && stream->Read(&response)) {
      if (const auto *net = payload(response, "getCurrentNetworkResponse")) {
        if (unsigned_value(response, "id") != network_id || !error_message(*net).empty())
          throw std::runtime_error("GetCurrentNetwork failed or response correlation mismatched");
        result.network = string_value(*net, "currentNetwork");
        std::transform(result.network.begin(), result.network.end(), result.network.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        got_network = true;
      } else if (const auto *info = payload(response, "getInfoResponse")) {
        if (unsigned_value(response, "id") != info_id || !error_message(*info).empty())
          throw std::runtime_error("GetInfo failed or response correlation mismatched");
        result.version = string_value(*info, "serverVersion");
        result.synced = bool_value(*info, "isSynced");
        got_info = true;
      } else if (const auto *dag = payload(response, "getBlockDagInfoResponse")) {
        const auto response_id = unsigned_value(response, "id");
        if (!error_message(*dag).empty())
          throw std::runtime_error("GetBlockDagInfo failed: " + error_message(*dag));
        if (response_id == dag_id) {
          result.initial_daa = unsigned_value(*dag, "virtualDaaScore");
          result.final_daa = result.initial_daa;
          result.initial_selected_sink = string_value(*dag, "sink");
          std::vector<std::string> parents;
          const auto *parents_field = field(*dag, "virtualParentHashes");
          if (!parents_field) throw std::runtime_error("GetBlockDagInfo.virtualParentHashes missing");
          const auto *reflection = dag->GetReflection();
          const int count = reflection->FieldSize(*dag, parents_field);
          parents.reserve(static_cast<std::size_t>(count));
          for (int i = 0; i < count; ++i) parents.push_back(reflection->GetRepeatedString(*dag, parents_field, i));
          (void)traversal.seed(result.initial_daa, parents);
          result.cutoff_daa = traversal.cutoff_daa();
          got_dag = true;
          drained_since_ns.store(mono_ns(), std::memory_order_release);
        } else if (watermark_requested && response_id == watermark_dag_id) {
          result.watermark_daa = unsigned_value(*dag, "virtualDaaScore");
          result.watermark_selected_sink = string_value(*dag, "sink");
          result.final_daa = result.watermark_daa;
          snapshot_watermark_set = true;
          drained_since_ns.store(0, std::memory_order_release);
        } else {
          throw std::runtime_error("GetBlockDagInfo response correlation mismatched");
        }
      } else if (const auto *chain = payload(response, "virtualChainChangedNotification")) {
        const auto *removed_field = field(*chain, "removedChainBlockHashes");
        const auto *added_field = field(*chain, "addedChainBlockHashes");
        AiSelectedChainChange change;
        change.sequence = result.selected_chain_changes.size() + 1;
        const auto *reflection = chain->GetReflection();
        if (removed_field) {
          const int count = reflection->FieldSize(*chain, removed_field);
          for (int i = 0; i < count; ++i)
            change.removed.push_back(reflection->GetRepeatedString(*chain, removed_field, i));
        }
        if (added_field) {
          const int count = reflection->FieldSize(*chain, added_field);
          for (int i = 0; i < count; ++i)
            change.added.push_back(reflection->GetRepeatedString(*chain, added_field, i));
        }
        if (got_dag && !snapshot_watermark_set &&
            (!change.removed.empty() || !change.added.empty())) {
          std::string event_key;
          for (const auto &hash : change.removed) event_key += "-" + hash + ";";
          event_key += "|";
          for (const auto &hash : change.added) event_key += "+" + hash + ";";
          change.duplicate = !selected_chain_event_keys.insert(event_key).second;
          if (!change.duplicate) {
            result.selected_chain_blocks_removed += change.removed.size();
            result.selected_chain_blocks_added += change.added.size();
            if (!change.removed.empty()) {
              ++result.selected_chain_reorgs;
              result.reorg_during_scan = true;
            }
            for (const auto &hash : change.removed) {
              auto &last = last_selected_chain_action[hash];
              if (last == 2) change.out_of_order = true;
              last = 2;
              if (selected_chain_event_hashes.insert(hash).second) {
                if (observed.contains(hash)) live_event_reconciled.insert(hash);
                else if (live_queue.size() < options.max_blocks) live_queue.push_back(hash);
                else { ++result.live_queue_overflow; live_event_overflow = true; }
              }
            }
            for (const auto &hash : change.added) {
              selected_chain_event_hashes.insert(hash);
              auto &last = last_selected_chain_action[hash];
              if (last == 1) change.out_of_order = true;
              if (last == 2) {
                change.reinclusion = true;
                ++result.selected_chain_blocks_reincluded;
              }
              last = 1;
              if (selected_chain_added_hashes.insert(hash).second) {
                if (observed.contains(hash)) live_event_reconciled.insert(hash);
                else if (live_queue.size() < options.max_blocks) live_queue.push_back(hash);
                else { ++result.live_queue_overflow; live_event_overflow = true; }
              }
            }
            if (change.removed.empty()) {
              if (change.reinclusion) ++result.selected_chain_reinclusions;
              else if (!change.out_of_order) ++result.selected_chain_advances;
            }
          }
          if (change.duplicate) ++result.selected_chain_duplicate_events;
          if (change.out_of_order) ++result.selected_chain_out_of_order_events;
          result.selected_chain_changes.push_back(std::move(change));
          drained_since_ns.store(0, std::memory_order_release);
        }
      } else if (const auto *added = payload(response, "blockAddedNotification")) {
        // Keep the immutable ancestor traversal anchored at GetBlockDagInfo,
        // while resolving every unique notification observed before the finite
        // watermark through GetBlock so AiRequest/AiResponse in live arrivals
        // are part of this read-only reconciliation too.
        const auto *block_field = field(*added, "block");
        if (!snapshot_watermark_set && block_field && added->GetReflection()->HasField(*added, block_field)) {
          const auto &wire = added->GetReflection()->GetMessage(*added, block_field);
          const auto *block = dynamic_cast<const protowire::RpcBlock *>(&wire);
          std::string hash;
          if (block) {
            if (block->has_verbosedata()) hash = block->verbosedata().hash();
            if (hash.empty() && block->has_header()) hash = block->header().hash();
          }
          if (hash.empty()) {
            ++result.rpc_errors;
            live_event_overflow = true;
          } else if (live_event_hashes.contains(hash)) {
            ++result.live_duplicate_notifications;
          } else if (live_event_hashes.size() >= options.max_blocks) {
            ++result.live_queue_overflow;
            live_event_overflow = true;
          } else {
            live_event_hashes.insert(hash);
            ++result.live_blocks_seen;
            if (observed.contains(hash)) {
              live_event_reconciled.insert(hash);
              ++result.live_snapshot_deduplicated;
            }
            else if (live_queue.size() < options.max_blocks) live_queue.push_back(std::move(hash));
            else { ++result.live_queue_overflow; live_event_overflow = true; }
          }
          drained_since_ns.store(0, std::memory_order_release);
        }
      } else if (payload(response, "getBlockResponse")) {
        const auto id = unsigned_value(response, "id");
        auto it = pending.find(id);
        if (it != pending.end()) {
          auto fetch = std::move(it->second);
          pending.erase(it);
          in_flight_hashes.erase(fetch.hash);
          --in_flight;
          const auto *block_payload = payload(response, "getBlockResponse");
          const auto error = block_payload ? error_message(*block_payload) : "missing GetBlock response";
          const auto *block_field = block_payload ? field(*block_payload, "block") : nullptr;
          if (error.empty() && block_field && block_payload->GetReflection()->HasField(*block_payload, block_field)) {
            const auto &wire = block_payload->GetReflection()->GetMessage(*block_payload, block_field);
            if (const auto *block = dynamic_cast<const protowire::RpcBlock *>(&wire)) {
              scan_block(*block, fetch.backfill);
              if (!fetch.backfill) ++result.live_blocks_recovered;
              if (live_event_hashes.contains(fetch.hash) || selected_chain_added_hashes.contains(fetch.hash))
                live_event_reconciled.insert(fetch.hash);
            }
            else { ++result.rpc_errors; ++result.blocks_unavailable; }
          } else {
            if (!error.empty()) ++result.rpc_errors;
            std::string lower = error;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool unavailable = lower.find("prun") != std::string::npos || lower.find("not found") != std::string::npos ||
                lower.find("unknown block") != std::string::npos || lower.find("cannot find header") != std::string::npos;
            if (!unavailable && fetch.attempts < 1)
              retry_queue.push_back(RetryFetch{fetch.hash, fetch.backfill, fetch.attempts + 1});
            else {
              ++result.blocks_unavailable;
              if (!fetch.backfill) ++result.live_blocks_unavailable;
            }
          }
        } else {
          ++result.rpc_errors;
        }
      }

      while (pending.size() < options.max_inflight && !retry_queue.empty()) {
        auto item = std::move(retry_queue.front()); retry_queue.pop_front();
        (void)schedule_fetch(std::move(item.hash), item.backfill, item.attempts);
      }
      while (pending.size() < options.max_inflight) {
        auto hash = traversal.pop();
        if (!hash) break;
        (void)schedule_fetch(std::move(*hash), true, 0);
      }
      const bool historical_pending = !traversal.drained() ||
          std::any_of(pending.begin(), pending.end(), [](const auto &item) { return item.second.backfill; }) ||
          std::any_of(retry_queue.begin(), retry_queue.end(), [](const RetryFetch &item) { return item.backfill; });
      if (got_network && got_info && got_dag && !historical_pending && !watermark_requested) {
        watermark_dag_id = next_id++;
        auto watermark_request = make_request("getBlockDagInfoRequest", watermark_dag_id);
        send(std::move(watermark_request));
        watermark_requested = true;
        drained_since_ns.store(0, std::memory_order_release);
      }
      // The second DAG snapshot is the finite live watermark. Notifications
      // before its response are reconciled; later notifications belong to the
      // next live-processing interval and cannot extend this backfill.
      std::unordered_set<std::string> watermark_hashes = live_event_hashes;
      watermark_hashes.insert(selected_chain_event_hashes.begin(), selected_chain_event_hashes.end());
      result.live_watermark_events = watermark_hashes.size();
      if (snapshot_watermark_set && !historical_pending) {
        while (pending.size() < options.max_inflight && !live_queue.empty()) {
          auto hash = std::move(live_queue.front()); live_queue.pop_front();
          if (observed.contains(hash)) {
            live_event_reconciled.insert(hash);
            ++result.live_snapshot_deduplicated;
          }
          else (void)schedule_fetch(std::move(hash), false, 0);
        }
      }
      result.hashes_scheduled = traversal.scheduled_count();
      result.limit_reached = traversal.at_limit();
      const bool live_reconciled = live_event_reconciled.size() >= result.live_watermark_events &&
          live_queue.empty() && std::none_of(pending.begin(), pending.end(),
              [](const auto &item) { return !item.second.backfill; }) &&
          std::none_of(retry_queue.begin(), retry_queue.end(),
              [](const RetryFetch &item) { return !item.backfill; });
      if (got_network && got_info && got_dag && snapshot_watermark_set && pending.empty() &&
          traversal.drained() && retry_queue.empty() && live_reconciled) {
        if (drained_since_ns.load(std::memory_order_acquire) == 0)
          drained_since_ns.store(mono_ns(), std::memory_order_release);
      } else drained_since_ns.store(0, std::memory_order_release);
      response.Clear();
    }
  } catch (...) {
    finish_watcher();
    context.TryCancel();
    (void)stream->Finish();
    throw;
  }
  finish_watcher();
  if (!cancel_stream.load(std::memory_order_acquire) && std::chrono::steady_clock::now() >= hard_deadline)
    result.timed_out = true;
  const auto status = stream->Finish();
  if (!status.ok() && !cancel_stream.load(std::memory_order_acquire) && !stop_requested.load())
    ++result.rpc_errors;
  result.hashes_scheduled = traversal.scheduled_count();
  result.limit_reached = traversal.at_limit();
  result.live_watermark_complete = snapshot_watermark_set && !result.watermark_selected_sink.empty() &&
      live_event_reconciled.size() >= result.live_watermark_events && live_queue.empty() &&
      result.live_blocks_unavailable == 0 && result.live_queue_overflow == 0 && !live_event_overflow &&
      std::none_of(pending.begin(), pending.end(), [](const auto &item) { return !item.second.backfill; }) &&
      std::none_of(retry_queue.begin(), retry_queue.end(), [](const RetryFetch &item) { return !item.backfill; });
  result.complete = got_network && got_info && got_dag && snapshot_watermark_set && pending.empty() && retry_queue.empty() &&
      traversal.drained() && result.blocks_unavailable == 0 && result.rpc_errors == 0 &&
      result.live_watermark_complete &&
      result.network == "mainnet" && result.synced && !result.limit_reached &&
      !result.timed_out && !stop_requested.load();
  if (pending.size() > 0) result.blocks_unavailable += pending.size();
  return result;
}

int run_read_only_client(const std::string &endpoint, const std::string &pay_address,
                         const std::string &extra_data, const std::string &capture_path,
                         std::uint64_t max_templates, unsigned reconnect_after_seconds,
                         const std::atomic_bool &stop_requested) {
  return run_client(endpoint, pay_address, extra_data, &capture_path, nullptr,
                    max_templates, reconnect_after_seconds, stop_requested, {}, {});
}

int run_live_job_client(const std::string &endpoint, const std::string &pay_address,
                        const std::string &extra_data, LiveJobQueue &queue,
                        std::uint64_t max_templates, unsigned reconnect_after_seconds,
                        const std::atomic_bool &stop_requested,
                        const std::function<std::string(const std::string &)> &inference_handler,
                        const AiBlockObserver &ai_block_observer,
                        const AiRemovedChainObserver &ai_removed_chain_observer,
                        const AiBackfillCompleteObserver &backfill_complete_observer) {
  return run_client(endpoint, pay_address, extra_data, nullptr, &queue,
                    max_templates, reconnect_after_seconds, stop_requested, inference_handler,
                    ai_block_observer, ai_removed_chain_observer, backfill_complete_observer);
}

NodeReadiness probe_node_readiness(const std::string &endpoint,
                                   std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("node readiness timeout must be positive");
  const auto [target, tls] = parse_endpoint(endpoint);
  auto channel = grpc::CreateChannel(target, tls
      ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
      : grpc::InsecureChannelCredentials());
  if (!channel->WaitForConnected(std::chrono::system_clock::now() + timeout))
    throw std::runtime_error("node readiness probe could not connect before deadline");
  auto stub = protowire::RPC::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + timeout);
  auto stream = stub->MessageStream(&context);
  if (!stream) throw std::runtime_error("node readiness probe could not create RPC.MessageStream");
  constexpr std::uint64_t network_id = 1;
  constexpr std::uint64_t info_id = 2;
  if (!stream->Write(make_request("getCurrentNetworkRequest", network_id)) ||
      !stream->Write(make_request("getInfoRequest", info_id))) {
    context.TryCancel();
    throw std::runtime_error("node readiness probe request write failed");
  }
  NodeReadiness result;
  bool have_network = false;
  bool have_info = false;
  Response response;
  while (!(have_network && have_info) && stream->Read(&response)) {
    if (const auto *network = payload(response, "getCurrentNetworkResponse")) {
      const auto id = unsigned_value(response, "id");
      if (id != 0 && id != network_id) throw std::runtime_error("GetCurrentNetwork probe response ID mismatch");
      if (!error_message(*network).empty()) throw std::runtime_error("GetCurrentNetwork probe failed: " + error_message(*network));
      result.network = string_value(*network, "currentNetwork");
      std::transform(result.network.begin(), result.network.end(), result.network.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      have_network = true;
    } else if (const auto *info = payload(response, "getInfoResponse")) {
      const auto id = unsigned_value(response, "id");
      if (id != 0 && id != info_id) throw std::runtime_error("GetInfo probe response ID mismatch");
      if (!error_message(*info).empty()) throw std::runtime_error("GetInfo probe failed: " + error_message(*info));
      result.synced = bool_value(*info, "isSynced");
      result.version = string_value(*info, "serverVersion");
      have_info = true;
    }
    response.Clear();
  }
  (void)stream->WritesDone();
  const auto status = stream->Finish();
  if (!(have_network && have_info))
    throw std::runtime_error("node readiness probe ended before both GetInfo and network responses");
  if (!status.ok())
    throw std::runtime_error("node readiness probe RPC ended with status " +
                              std::to_string(status.error_code()));
  return result;
}

bool probe_local_ipfs_api(std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("IPFS readiness timeout must be positive");
#ifdef _WIN32
  struct Winsock {bool ready;Winsock(){WSADATA data{};ready=WSAStartup(MAKEWORD(2,2),&data)==0;}~Winsock(){if(ready)WSACleanup();}} winsock;
  if(!winsock.ready)return false;
  const SOCKET fd=::socket(AF_INET,SOCK_STREAM,0);
  if(fd==INVALID_SOCKET)return false;
  struct SocketGuard{SOCKET fd;~SocketGuard(){closesocket(fd);}} guard{fd};
  const DWORD millis=static_cast<DWORD>(std::min<std::int64_t>(timeout.count(),30000));
  (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&millis),sizeof(millis));
  (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&millis),sizeof(millis));
#else
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  struct SocketGuard { int fd; ~SocketGuard() { if (fd >= 0) ::close(fd); } } guard{fd};
  const auto millis = timeout.count();
  timeval tv{static_cast<time_t>(millis / 1000), static_cast<suseconds_t>((millis % 1000) * 1000)};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(5001);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) return false;
  constexpr char request[] =
      "POST /api/v0/version HTTP/1.1\r\nHost: 127.0.0.1:5001\r\n"
      "Content-Length: 0\r\nConnection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < sizeof(request) - 1) {
    const auto n = ::send(fd, request + sent, static_cast<int>(sizeof(request) - 1 - sent),
#ifdef _WIN32
      0
#else
      MSG_NOSIGNAL
#endif
      );
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  std::string header;
  header.reserve(256);
  char c{};
  while (header.size() < 1024 && ::recv(fd, &c, 1, 0) == 1) {
    header.push_back(c);
    if (header.size() >= 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0) break;
  }
  const auto line_end = header.find("\r\n");
  if (line_end == std::string::npos) return false;
  const auto status = header.substr(0, line_end);
  return status.find(" 200 ") != std::string::npos || status.ends_with(" 200");
}

ai::ServiceRpcSnapshot probe_ai_service_rpc(const std::string &endpoint,
    std::uint64_t provider_window_daa, std::string_view service_identity_hex,
    std::chrono::milliseconds timeout) {
  return probe_ai_service_rpc(endpoint, provider_window_daa, service_identity_hex,
                              timeout, ServiceRpcQuery::All);
}

ai::ServiceRpcSnapshot probe_ai_service_rpc(const std::string &endpoint,
    std::uint64_t provider_window_daa, std::string_view service_identity_hex,
    std::chrono::milliseconds timeout, ServiceRpcQuery query) {
  const bool want_providers = query != ServiceRpcQuery::StrikesOnly;
  const bool want_strikes = query != ServiceRpcQuery::ProvidersOnly;
  if (timeout <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("AI service RPC timeout must be positive");
  if (provider_window_daa > 36000)
    throw std::invalid_argument("GetServiceProviders window must be 0 (node default) or <= 36000 DAA");
  const auto [target, tls] = parse_endpoint(endpoint);
  auto channel = grpc::CreateChannel(target, tls
      ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
      : grpc::InsecureChannelCredentials());
  const auto deadline = std::chrono::system_clock::now() + timeout;
  if (!channel->WaitForConnected(deadline))
    throw std::runtime_error("AI service RPC could not connect before deadline");
  auto stub = protowire::RPC::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(deadline);
  auto stream = stub->MessageStream(&context);
  if (!stream) throw std::runtime_error("could not create AI service RPC stream");

  constexpr std::uint64_t providers_id = 17;
  constexpr std::uint64_t strikes_id = 18;
  auto providers_request = make_request("getServiceProvidersRequest", providers_id);
  auto *providers_payload = set_payload(providers_request, "getServiceProvidersRequest");
  if (provider_window_daa != 0) {
    const auto *window_field = field(*providers_payload, "windowDaa");
    if (!window_field) throw std::runtime_error("official GetServiceProviders.windowDaa field missing");
    providers_payload->GetReflection()->SetUInt64(providers_payload, window_field, provider_window_daa);
  }
  const auto strikes_request = make_request("getServiceStrikesRequest", strikes_id);
  if ((want_providers && !stream->Write(providers_request)) ||
      (want_strikes && !stream->Write(strikes_request)) || !stream->WritesDone()) {
    context.TryCancel();
    (void)stream->Finish();
    throw std::runtime_error("AI service read-only RPC request write failed");
  }

  ai::ServiceRpcSnapshot result;
  bool got_providers = !want_providers, got_strikes = !want_strikes;
  Response response;
  while (!(got_providers && got_strikes) && stream->Read(&response)) {
    const auto id = unsigned_value(response, "id");
    if (const auto *payload_message = payload(response, "getServiceProvidersResponse")) {
      if (id != 0 && id != providers_id) throw std::runtime_error("GetServiceProviders correlation ID mismatch");
      if (error_message(*payload_message).empty()) {
        const auto *virtual_daa = field(*payload_message, "virtualDaaScore");
        if (virtual_daa) result.providers_virtual_daa = payload_message->GetReflection()->GetUInt64(*payload_message, virtual_daa);
        const auto *providers = field(*payload_message, "providers");
        if (providers) {
          const auto *reflection = payload_message->GetReflection();
          const int count = reflection->FieldSize(*payload_message, providers);
          result.providers.reserve(static_cast<std::size_t>(count));
          for (int i = 0; i < count; ++i) {
            const auto &provider = reflection->GetRepeatedMessage(*payload_message, providers, i);
            ai::ServiceProvider item;
            const auto read_u32 = [&](const char *name) {
              const auto *f = field(provider, name);
              return f ? provider.GetReflection()->GetUInt32(provider, f) : 0U;
            };
            const auto read_string = [&](const char *name) {
              const auto *f = field(provider, name);
              return f ? provider.GetReflection()->GetString(provider, f) : std::string{};
            };
            item.tier = read_u32("tier"); item.model_id = read_string("modelId");
            item.identity = read_string("identity"); item.escrow_pubkey = read_string("escrowPubkey");
            result.providers.push_back(std::move(item));
          }
        }
        result.providers_available = true;
      } else result.providers_error = error_message(*payload_message);
      got_providers = true;
    } else if (const auto *payload_message = payload(response, "getServiceStrikesResponse")) {
      if (id != 0 && id != strikes_id) throw std::runtime_error("GetServiceStrikes correlation ID mismatch");
      if (payload_message->GetDescriptor()->FindFieldByName("error") &&
          payload_message->GetReflection()->HasField(*payload_message,
            payload_message->GetDescriptor()->FindFieldByName("error"))) {
        result.strikes.error = error_message(*payload_message);
      } else {
        const auto *concrete = dynamic_cast<const protowire::GetServiceStrikesResponseMessage *>(payload_message);
        if (concrete) result.strikes = ai::strikes_for_identity(*concrete, service_identity_hex);
        else result.strikes.error = "GetServiceStrikes protobuf type mismatch";
      }
      result.strikes_available = result.strikes.availability == ai::StrikeSnapshot::Availability::Available;
      got_strikes = true;
    }
    response.Clear();
  }
  // WritesDone already half-closed the request side. On the normal path the
  // server closes after the requested read-only replies; cancelling here races the
  // gRPC completion path and has caused intermittent allocator corruption.
  const auto rpc_status = stream->Finish();
  if (!rpc_status.ok()) {
    result.providers_available = false;
    if (want_providers) result.providers_error = "GetServiceProviders RPC stream failed: " + rpc_status.error_message();
    result.strikes_available = false;
    if (want_strikes) result.strikes.error = "GetServiceStrikes RPC stream failed: " + rpc_status.error_message();
  }
  if (!got_providers) result.providers_error = "GetServiceProviders response unavailable";
  if (!got_strikes && result.strikes.error.empty()) result.strikes.error = "GetServiceStrikes response unavailable";
  return result;
}

std::optional<bool> probe_block_selected_chain(const std::string &endpoint,
    std::string_view block_hash, std::chrono::milliseconds timeout, std::string *error) {
  if (timeout <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("GetBlock validation timeout must be positive");
  if (block_hash.size() != 64 || !std::all_of(block_hash.begin(), block_hash.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
      })) throw std::invalid_argument("GetBlock validation hash must be 32-byte hex");
  const auto [target, tls] = parse_endpoint(endpoint);
  auto channel = grpc::CreateChannel(target, tls
      ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
      : grpc::InsecureChannelCredentials());
  const auto deadline = std::chrono::system_clock::now() + timeout;
  if (!channel->WaitForConnected(deadline)) {
    if (error) *error = "GetBlock validation connection timeout";
    return std::nullopt;
  }
  auto stub = protowire::RPC::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(deadline);
  auto stream = stub->MessageStream(&context);
  if (!stream) {
    if (error) *error = "GetBlock validation stream creation failed";
    return std::nullopt;
  }
  constexpr std::uint64_t request_id = 0x4e5853455343524fULL;
  auto request = make_request("getBlockRequest", request_id);
  auto *payload_message = set_payload(request, "getBlockRequest");
  const auto *hash_field = field(*payload_message, "hash");
  const auto *transactions_field = field(*payload_message, "includeTransactions");
  if (!hash_field || !transactions_field) throw std::runtime_error("official GetBlock fields unavailable");
  payload_message->GetReflection()->SetString(payload_message, hash_field, std::string(block_hash));
  payload_message->GetReflection()->SetBool(payload_message, transactions_field, false);
  if (!stream->Write(request) || !stream->WritesDone()) {
    context.TryCancel();
    (void)stream->Finish();
    if (error) *error = "GetBlock validation request write failed";
    return std::nullopt;
  }
  Response response;
  std::optional<bool> result;
  while (stream->Read(&response)) {
    const auto id = unsigned_value(response, "id");
    const auto *block_response = payload(response, "getBlockResponse");
    if (id != 0 && id != request_id) { response.Clear(); continue; }
    if (!block_response) { response.Clear(); continue; }
    const auto rpc_error = error_message(*block_response);
    if (!rpc_error.empty()) {
      if (error) *error = rpc_error;
      break;
    }
    const auto *block_field = field(*block_response, "block");
    if (!block_field || !block_response->GetReflection()->HasField(*block_response, block_field)) {
      if (error) *error = "GetBlock response omitted block";
      break;
    }
    const auto &message = block_response->GetReflection()->GetMessage(*block_response, block_field);
    const auto *block = dynamic_cast<const protowire::RpcBlock *>(&message);
    if (!block || !block->has_verbosedata() || !block->verbosedata().has_ischainblock()) {
      if (error) *error = "GetBlock response omitted authoritative verbose chain status";
      break;
    }
    result = block->verbosedata().ischainblock();
    break;
  }
  if (result) {
    (void)stream->WritesDone();
    (void)stream->Finish();
  } else {
    context.TryCancel();
    (void)stream->Finish();
    if (error && error->empty()) *error = "GetBlock selected-chain status unavailable";
  }
  return result;
}

static std::vector<BlockChainStatus> probe_blocks_selected_chain_batch(
    const std::string &endpoint, const std::vector<std::string> &block_hashes,
    std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("batched GetBlock validation timeout must be positive");
  std::vector<BlockChainStatus> results;
  results.reserve(block_hashes.size());
  for (const auto &hash : block_hashes) {
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
      throw std::invalid_argument("batched GetBlock validation contains malformed hash");
    results.push_back({hash, std::nullopt, {}, BlockValidationState::Pending});
  }
  if (block_hashes.empty()) return results;

  const auto [target, tls] = parse_endpoint(endpoint);
  auto channel = grpc::CreateChannel(target, tls
      ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
      : grpc::InsecureChannelCredentials());
  const auto deadline = std::chrono::system_clock::now() + timeout;
  if (!channel->WaitForConnected(deadline)) {
    for (auto &r : results) { r.error = "batched GetBlock validation connection timeout"; r.state = BlockValidationState::Error; }
    return results;
  }
  auto stub = protowire::RPC::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(deadline);
  auto stream = stub->MessageStream(&context);
  if (!stream) {
    for (auto &r : results) { r.error = "batched GetBlock validation stream creation failed"; r.state = BlockValidationState::Error; }
    return results;
  }
  std::unordered_map<std::uint64_t, std::size_t> request_to_result;
  request_to_result.reserve(block_hashes.size());
  for (std::size_t i = 0; i < block_hashes.size(); ++i) {
    const auto id = static_cast<std::uint64_t>(i) + 1;
    auto request = make_request("getBlockRequest", id);
    auto *p = set_payload(request, "getBlockRequest");
    const auto *hash_field = field(*p, "hash");
    const auto *include_field = field(*p, "includeTransactions");
    if (!hash_field || !include_field) throw std::runtime_error("official GetBlock fields unavailable");
    p->GetReflection()->SetString(p, hash_field, block_hashes[i]);
    p->GetReflection()->SetBool(p, include_field, false);
    if (!stream->Write(request)) {
      context.TryCancel();
      (void)stream->Finish();
      for (auto &r : results) if (!r.selected_chain) { r.error = "batched GetBlock request write failed"; r.state = BlockValidationState::Error; }
      return results;
    }
    request_to_result.emplace(id, i);
  }
  if (!stream->WritesDone()) {
    context.TryCancel();
    (void)stream->Finish();
    for (auto &r : results) if (!r.selected_chain) { r.error = "batched GetBlock request half-close failed"; r.state = BlockValidationState::Error; }
    return results;
  }
  Response response;
  std::size_t received = 0;
  while (received < results.size() && stream->Read(&response)) {
    const auto id = unsigned_value(response, "id");
    const auto found = request_to_result.find(id);
    if (found == request_to_result.end()) { response.Clear(); continue; }
    auto &status = results[found->second];
    if (status.selected_chain || !status.error.empty()) { response.Clear(); continue; }
    const auto *payload_message = payload(response, "getBlockResponse");
    if (!payload_message) { status.error = "GetBlock response payload unavailable"; status.state = BlockValidationState::Unknown; }
    else if (const auto rpc_error = error_message(*payload_message); !rpc_error.empty()) {
      status.error = rpc_error;
      std::string normalized = rpc_error;
      std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      const bool pruned_or_unknown =
          normalized.find("cannot find header") != std::string::npos ||
          normalized.find("block not found") != std::string::npos ||
          normalized.find("unknown block") != std::string::npos ||
          normalized.find("block pruned") != std::string::npos ||
          normalized.find("pruned block") != std::string::npos;
      status.state = pruned_or_unknown ? BlockValidationState::Unknown : BlockValidationState::Error;
    } else {
      const auto *block_field = field(*payload_message, "block");
      if (!block_field || !payload_message->GetReflection()->HasField(*payload_message, block_field)) {
        status.error = "GetBlock response omitted block";
        status.state = BlockValidationState::Unknown;
      } else {
        const auto &message = payload_message->GetReflection()->GetMessage(*payload_message, block_field);
        const auto *block = dynamic_cast<const protowire::RpcBlock *>(&message);
        if (!block || !block->has_verbosedata() || !block->verbosedata().has_ischainblock()) {
          status.error = "GetBlock response omitted authoritative selected-chain field";
          status.state = BlockValidationState::Unknown;
        } else {
          status.selected_chain = block->verbosedata().ischainblock();
          status.state = *status.selected_chain ? BlockValidationState::ChainValid : BlockValidationState::OffChain;
        }
      }
    }
    if (status.selected_chain || !status.error.empty()) ++received;
    response.Clear();
  }
  const auto rpc_status = stream->Finish();
  if (!rpc_status.ok()) {
    for (auto &r : results) if (r.state == BlockValidationState::Pending) { r.error = rpc_status.error_message(); r.state = BlockValidationState::Error; }
  } else {
    for (auto &r : results) if (r.state == BlockValidationState::Pending) { r.error = "GetBlock response missing"; r.state = BlockValidationState::Unknown; }
  }
  return results;
}

std::vector<BlockChainStatus> probe_blocks_selected_chain(
    const std::string &endpoint, const std::vector<std::string> &block_hashes,
    std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("selected-chain validation timeout must be positive");
  constexpr std::size_t max_in_flight = 32;
  constexpr unsigned max_retries = 1;
  const auto overall_deadline = std::chrono::steady_clock::now() + timeout;
  std::vector<BlockChainStatus> output;
  output.reserve(block_hashes.size());
  std::unordered_map<std::string, std::size_t> unique_index;
  std::vector<std::string> unique_hashes;
  unique_hashes.reserve(block_hashes.size());
  for (const auto &hash : block_hashes) {
    if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
      throw std::invalid_argument("batched GetBlock validation contains malformed hash");
    if (!unique_index.contains(hash)) {
      unique_index.emplace(hash, unique_hashes.size());
      unique_hashes.push_back(hash);
    }
  }
  std::vector<BlockChainStatus> unique_results(unique_hashes.size());
  for (std::size_t begin = 0; begin < unique_hashes.size(); begin += max_in_flight) {
    const auto count = std::min(max_in_flight, unique_hashes.size() - begin);
    std::vector<std::string> batch(unique_hashes.begin() + static_cast<std::ptrdiff_t>(begin),
                                   unique_hashes.begin() + static_cast<std::ptrdiff_t>(begin + count));
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        overall_deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      for (std::size_t i = 0; i < count; ++i)
        unique_results[begin + i] = {batch[i], std::nullopt, "selected-chain validation overall deadline expired",
                                     BlockValidationState::Error};
      continue;
    }
    auto statuses = probe_blocks_selected_chain_batch(endpoint, batch, remaining);
    std::vector<std::string> retry;
    for (std::size_t i = 0; i < statuses.size(); ++i) {
      if (statuses[i].state == BlockValidationState::Error && max_retries != 0) retry.push_back(batch[i]);
      unique_results[begin + i] = std::move(statuses[i]);
    }
    remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        overall_deadline - std::chrono::steady_clock::now());
    if (!retry.empty() && remaining > std::chrono::milliseconds::zero()) {
      const auto retried = probe_blocks_selected_chain_batch(endpoint, retry, remaining);
      for (const auto &status : retried) {
        const auto index = unique_index.at(status.hash);
        // A retry can only replace an error/unknown observation with a new authoritative
        // verdict. Otherwise retain its explicit final status and error for audit.
        if (status.state == BlockValidationState::ChainValid || status.state == BlockValidationState::OffChain)
          unique_results[index] = status;
        else {
          unique_results[index].state = BlockValidationState::Error;
          if (!status.error.empty()) unique_results[index].error = status.error;
        }
      }
    }
  }
  for (const auto &hash : block_hashes) output.push_back(unique_results.at(unique_index.at(hash)));
  return output;
}

} // namespace keryx::nexus
