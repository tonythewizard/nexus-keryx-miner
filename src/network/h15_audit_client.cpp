#include "keryx/nexus/h15_audit_client.hpp"
#include "keryx/nexus/h15_chain_restore.hpp"
#include "keryx/nexus/h15_bridge.h"
#include "keryx/nexus/trace_manifest_bridge.h"
#include "messages.grpc.pb.h"
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/struct.pb.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
namespace keryx::nexus::ai {
namespace {
using Hash = consensus::Hash;
void need(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::vector<std::uint8_t> buffer_bytes(const NexusH15Buffer &b) {
  return {b.data, b.data + b.length};
}
struct Buffer {
  NexusH15Buffer b{};
  ~Buffer() { nexus_h15_buffer_free(b); }
  Buffer() = default;
  Buffer(const Buffer &) = delete;
};
Hash unhex(std::string_view s) {
  need(s.size() == 64, "Invalid audit hash");
  Hash h{};
  for (std::size_t i = 0; i < 32; ++i) {
    auto n = [](char c) {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      return -1;
    };
    const auto a = n(s[i * 2]), b = n(s[i * 2 + 1]);
    need(a >= 0 && b >= 0, "Invalid audit hash");
    h[i] = std::uint8_t(a * 16 + b);
  }
  return h;
}
void put(std::vector<std::uint8_t> &b, std::uint64_t n, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i)
    b.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
}
void hash(std::vector<std::uint8_t> &b, const Hash &h) {
  b.insert(b.end(), h.begin(), h.end());
}
inline auto unbundle(std::span<const std::uint8_t> b) {
  return H15AuditClient::unbundle(b);
}
} // namespace

std::vector<std::vector<std::uint8_t>>
H15AuditClient::unbundle(std::span<const std::uint8_t> b) {
  if (b.empty())
    return {};
  if (b[0] != 0xb0)
    return {{b.begin(), b.end()}};
  need(b.size() >= 2, "Truncated audit bundle");
  std::size_t at = 2;
  std::vector<std::vector<std::uint8_t>> out;
  for (unsigned i = 0; i < b[1]; ++i) {
    need(at + 4 <= b.size(), "Truncated audit length");
    std::uint32_t n = 0;
    for (unsigned j = 0; j < 4; ++j)
      n |= std::uint32_t(b[at++]) << (8 * j);
    need(n <= 112U * 1024 + 96 && at + n <= b.size(), "Audit piece bounds");
    out.emplace_back(b.data() + at, b.data() + at + n);
    at += n;
  }
  need(at == b.size(), "Trailing audit data");
  return out;
}

H15AuditClient::H15ObligationStatus
H15AuditClient::evaluate_obligation_bundle(std::span<const std::uint8_t> b, const char *name) {
  if (b.data() == nullptr || b.empty()) {
    return {false, std::string("H15_") + name + "_BUNDLE_INVALID: BUFFER_EMPTY"};
  }
  std::vector<std::vector<std::uint8_t>> unbundled;
  try {
    unbundled = unbundle(b);
  } catch (const std::exception &e) {
    return {false, std::string("H15_") + name + "_BUNDLE_CORRUPT: " + e.what()};
  }
  if (b[0] == 0xb0 && unbundled.size() != static_cast<std::size_t>(b[1])) {
    return {false, std::string("H15_") + name + "_BUNDLE_CARDINALITY_ANOMALY"};
  }
  if (!unbundled.empty()) {
    return {false, std::string("H15_") + name + "_DUE_FOR_SUBMISSION"};
  }
  return {true, ""};
}
struct H15AuditClient::Impl {
  opoi::StockLlamaRuntime &runtime;
  const model::ReadOnlyGguf &gguf;
  const model::SparseMerkleIndex &merkle;
  bool diagnostics{};
  void *service{}, *capture{};
  NexusH15Api api{};
  NexusTraceManifestInfo info{};
  std::vector<std::uint8_t> manifest;
  std::vector<opoi::SequenceLayer> layers;
  std::shared_ptr<grpc::Channel> channel;
  AiResponseTransport transport;
  std::mutex mutex;
  Hash anchor{};
  std::chrono::steady_clock::time_point next_poll{};
  unsigned poll_failures{};
  H15PollAdmission poll_admission;
  std::chrono::steady_clock::time_point next_health{};
  std::optional<bool> awaited_supported;
  bool awaited_probed{};
  std::chrono::steady_clock::time_point next_awaited{};
  std::uint32_t next_prompt{}, next_tokens{};

  H15ChainRefresh chain_refresh;
  std::optional<H15ChainRestorePage> pending_page;
  std::optional<std::uint64_t> cached_tile_index;
  std::array<std::uint8_t, 1024> cached_tile_bytes{};
  bool cached_tile_tree_ready{};
  std::array<Hash, 32> cached_tile_l0{};
  std::array<Hash, 16> cached_tile_l1{};
  std::array<Hash, 8> cached_tile_l2{};
  std::array<Hash, 4> cached_tile_l3{};
  std::array<Hash, 2> cached_tile_l4{};
  std::vector<Hash> cached_tile_upper_path;
  std::optional<std::chrono::steady_clock::time_point> rpc_budget;
  std::function<void(const std::function<void()> &)> with_gpu;
  Impl(opoi::StockLlamaRuntime &r, const model::ReadOnlyGguf &g,
       const model::SparseMerkleIndex &m, std::string endpoint)
      : runtime(r), gguf(g), merkle(m), transport(endpoint) {
    const auto prefix = endpoint.starts_with("grpcs://")  ? 8U
                        : endpoint.starts_with("grpc://") ? 7U
                                                          : 0U;
    const auto address = endpoint.substr(prefix);
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(64 << 20);
    channel = grpc::CreateCustomChannel(
        address,
        prefix == 8 ? grpc::SslCredentials(grpc::SslCredentialsOptions{})
                    : grpc::InsecureChannelCredentials(),
        args);
    api.user = this;
    api.weight = &weight;
    api.replay = &replay;
    api.chunks = g.chunk_count();
  }
  ~Impl() {
    if (capture)
      nexus_h15_capture_free(capture);
    if (service)
      nexus_h15_service_free(service);
  }
  protowire::KaspadResponse call(protowire::KaspadRequest req) {
    const auto remaining =
        rpc_budget
            ? std::min(
                  std::chrono::steady_clock::duration(std::chrono::seconds(8)),
                  *rpc_budget - std::chrono::steady_clock::now())
            : std::chrono::steady_clock::duration(std::chrono::seconds(8));
    if(remaining<=std::chrono::steady_clock::duration::zero())throw H15PollBudgetExhausted();
    // MSVC's system clock uses 100 ns ticks; gRPC accepts its exact time_point.
    const auto deadline = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::system_clock::now() + remaining);
    need(channel->WaitForConnected(deadline), "Audit RPC connection timeout");
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline);
    auto stub = protowire::RPC::NewStub(channel);
    auto stream = stub->MessageStream(&ctx);
    req.set_id(1);
    need(stream && stream->Write(req) && stream->WritesDone(),
         "Audit RPC request failed");
    protowire::KaspadResponse response;
    bool got = false;
    while (stream->Read(&response)) {
      if (response.id() == 1) {
        got = true;
        break;
      }
    }
    ctx.TryCancel();
    (void)stream->Finish();
    if(!got && rpc_budget && std::chrono::steady_clock::now()>=*rpc_budget)throw H15PollBudgetExhausted();
    need(got, "Audit RPC response timeout");
    return response;
  }
  void poll_awaited() {
    const auto now = std::chrono::steady_clock::now();
    h15_success_throttled_poll(next_awaited,now,[&] {
    if (!awaited_supported) {
      protowire::KaspadRequest req;
      req.mutable_getinforequest();
      auto r = call(req);
      need(r.has_getinforesponse() && !r.getinforesponse().has_error(),
           "Audit node version unavailable");
      const auto &v = r.getinforesponse().serverversion();
      std::array<unsigned long, 3> parts{};
      std::size_t at = 0;
      for (auto &part : parts) {
        while (at < v.size() &&
               !std::isdigit(static_cast<unsigned char>(v[at])))
          ++at;
        unsigned digits = 0;
        while (at < v.size() &&
               std::isdigit(static_cast<unsigned char>(v[at]))) {
          need(++digits <= 9, "Node version bounds");
          part = part * 10 + unsigned(v[at++] - '0');
        }
      }
      awaited_supported = parts >= std::array<unsigned long, 3>{1, 6, 6};
      if (!*awaited_supported)
        need(nexus_h15_service_local_estimates(service) == 0,
             "Audit legacy estimates recovery failed");
      std::cout << "H15_NODE_LEDGER supported="
                << (*awaited_supported ? "true" : "false")
                << " minimum=1.6.6\n";
    }
    if (!*awaited_supported)
      return;
    protowire::KaspadRequest dag;
    dag.mutable_getblockdaginforequest();
    auto status = call(dag);
    need(status.has_getblockdaginforesponse() &&
             !status.getblockdaginforesponse().has_error(),
         "Audit ledger DAA unavailable");
    Buffer ids;
    need(nexus_h15_service_awaited_ids(
             service, status.getblockdaginforesponse().virtualdaascore(),
             &ids.b) == 0 &&
             ids.b.length % 32 == 0 && ids.b.length <= 1024 * 32,
         "Audit ledger request bounds");
    if (!ids.b.length && awaited_probed)
      return;
    protowire::KaspadRequest req;
    auto *request = req.mutable_getawaitedopeningsrequest();
    std::unordered_set<std::string> queried;
    for (std::size_t at = 0; at < ids.b.length; at += 32) {
      auto id = hex_encode(ids.b.data + at, 32);
      request->add_responsetxids(id);
      queried.insert(id);
    }
    auto reply = call(req);
    need(reply.has_getawaitedopeningsresponse(),
         "Audit ledger response missing");
    const auto &response = reply.getawaitedopeningsresponse();
    need(!response.has_error() && response.openings_size() <= 1024,
         "Audit ledger response failed");
    std::vector<std::uint8_t> frame;
    put(frame, response.virtualdaascore(), 8);
    put(frame, static_cast<std::uint64_t>(response.openings_size()), 4);
    for (const auto &o : response.openings()) {
      need(queried.contains(o.responsetxid()) && o.attentionpath_size() <= 16,
           "Unsolicited audit ledger row");
      hash(frame, unhex(o.responsetxid()));
      hash(frame, unhex(o.escrowpubkey()));
      hash(frame, unhex(o.seed()));
      put(frame, o.deadlinedaa(), 8);
      put(frame, o.pieces(), 4);
      put(frame, static_cast<std::uint64_t>(o.attentionpath_size()), 4);
      for (auto child : o.attentionpath())
        put(frame, child, 8);
      put(frame, o.accepteddaa(), 8);
    }
    need(nexus_h15_service_apply_awaited(service, frame.data(), frame.size()) ==
             0,
         "Audit node ledger reconciliation failed");
    awaited_probed = true;
    std::cout << "H15_NODE_LEDGER reconciled=" << response.openings_size()
              << " queried=" << queried.size() << "\n";
    });
  }
  protowire::RpcBlock get_block(const Hash &h, bool selected = true) {
    protowire::KaspadRequest req;
    req.mutable_getblockrequest()->set_hash(hex_encode(h.data(), h.size()));
    req.mutable_getblockrequest()->set_includetransactions(false);
    auto r = call(req);
    need(r.has_getblockresponse() && !r.getblockresponse().has_error() &&
             r.getblockresponse().has_block(),
         "Audit GetBlock failed");
    auto b = r.getblockresponse().block();
    need(b.has_header() && b.has_verbosedata() &&
             unhex(b.verbosedata().hash()) == h,
         "Audit header is not selected chain");
    if (selected && !b.verbosedata().ischainblock())
      throw H15ChainChanged();
    return b;
  }
  Hash sink() {
    protowire::KaspadRequest req;
    req.mutable_getblockdaginforequest();
    auto r = call(req);
    need(r.has_getblockdaginforesponse() &&
             !r.getblockdaginforesponse().has_error() &&
             r.getblockdaginforesponse().networkname() == "keryx-mainnet",
         "Audit mainnet identity unavailable");
    return unhex(r.getblockdaginforesponse().sink());
  }
  void configure(std::uint32_t minimum_positions=0) {
    need(runtime.abi_version() == 9, "H15 requires ABI9");
    const auto positions =
        std::min<std::uint32_t>(runtime.trace_context_capacity(),
                                (512U << 20) / 32 / info.position_leaves);
    runtime.configure_trace(manifest, positions, info.position_leaves);
    runtime.configure_sequence(layers, info.n_attn, info.n_recur, info.heads,
                               info.interval,minimum_positions);
  }
  static int weight(void *u, std::uint64_t index, std::uint8_t *out,
                    std::uint8_t *path, std::size_t *n) {
    try {
      auto &p = *static_cast<Impl *>(u);
      need(index < p.gguf.chunk_count() && out && n, "Audit weight bounds");
      const auto tile_index = index / 32;
      const auto chunk_in_tile = static_cast<std::size_t>(index % 32);
      if (p.cached_tile_index != tile_index) {
        // Publish bytes and identity together; a failed read must not poison the old tile.
        std::array<std::uint8_t, 1024> tile_bytes;
        p.gguf.read_tile_into(tile_index, tile_bytes);
        p.cached_tile_bytes = tile_bytes;
        p.cached_tile_index = tile_index;
        p.cached_tile_tree_ready = false;
      }
      std::memcpy(out, p.cached_tile_bytes.data() + chunk_in_tile * 32, 32);
      if (!path) {
        *n = 0;
        return 0;
      }
      if (!p.cached_tile_tree_ready) {
        for (std::size_t i = 0; i < 32; ++i)
          p.cached_tile_l0[i] = consensus::blake3_hash(
              std::span(p.cached_tile_bytes.data() + i * 32, 32));
        for (std::size_t i = 0; i < 16; ++i)
          p.cached_tile_l1[i] = consensus::hash_pair(
              p.cached_tile_l0[2 * i], p.cached_tile_l0[2 * i + 1]);
        for (std::size_t i = 0; i < 8; ++i)
          p.cached_tile_l2[i] = consensus::hash_pair(
              p.cached_tile_l1[2 * i], p.cached_tile_l1[2 * i + 1]);
        for (std::size_t i = 0; i < 4; ++i)
          p.cached_tile_l3[i] = consensus::hash_pair(
              p.cached_tile_l2[2 * i], p.cached_tile_l2[2 * i + 1]);
        for (std::size_t i = 0; i < 2; ++i)
          p.cached_tile_l4[i] = consensus::hash_pair(
              p.cached_tile_l3[2 * i], p.cached_tile_l3[2 * i + 1]);
        p.cached_tile_upper_path = p.merkle.tile_path(tile_index);
        p.cached_tile_tree_ready = true;
      }
      const std::size_t total_depth = 5 + p.cached_tile_upper_path.size();
      need(total_depth <= *n, "Audit path capacity");
      auto *dst = reinterpret_cast<Hash *>(path);
      dst[0] = p.cached_tile_l0[chunk_in_tile ^ 1U];
      dst[1] = p.cached_tile_l1[(chunk_in_tile >> 1U) ^ 1U];
      dst[2] = p.cached_tile_l2[(chunk_in_tile >> 2U) ^ 1U];
      dst[3] = p.cached_tile_l3[(chunk_in_tile >> 3U) ^ 1U];
      dst[4] = p.cached_tile_l4[(chunk_in_tile >> 4U) ^ 1U];
      if (!p.cached_tile_upper_path.empty())
        std::memcpy(dst + 5, p.cached_tile_upper_path.data(),
                    p.cached_tile_upper_path.size() * sizeof(Hash));
      *n = total_depth;
      return 0;
    } catch (...) {
      return -1;
    }
  }
  static int replay(void *u, const NexusH15Capture *raw, std::uint32_t kind,
                    const std::uint64_t *plans, std::size_t count,
                    std::uint32_t slot, std::uint32_t max_chunks,
                    std::uint8_t *out, std::size_t cap, std::size_t *n) {
    try {
      auto &p = *static_cast<Impl *>(u);
      need(raw && raw->positions && raw->positions <= 32768 && count <= 32 &&
               out && n,
           "Audit replay bounds");
      need(bool(p.with_gpu), "Audit replay requires exclusive GPU owner");
      const auto replay_started=std::chrono::steady_clock::now();
      auto acquired=replay_started,configured=replay_started,replayed=replay_started;
      std::vector<std::uint8_t> b;
      p.with_gpu([&] {
        acquired=std::chrono::steady_clock::now();
        struct ScratchRelease {
          opoi::StockLlamaRuntime &rt;
          ~ScratchRelease() noexcept {
            try { rt.use_task_context(); } catch (...) {}
          }
        } scratch_release{p.runtime};
        p.configure(static_cast<std::uint32_t>(raw->positions));
        configured=std::chrono::steady_clock::now();
        opoi::TracedGeneration g;
        g.tokens.assign(raw->tokens, raw->tokens + raw->positions);
        g.ubatch.assign(raw->ubatch, raw->ubatch + raw->positions);
        g.prompt_positions = raw->prompt_positions;
        g.leaves.resize(raw->positions * raw->leaves_per_position);
        std::memcpy(g.leaves.data(), raw->leaves, g.leaves.size() * 32);
        g.sequence_roots.emplace(raw->sequence_count);
        std::memcpy(g.sequence_roots->data(), raw->sequence,
                    raw->sequence_count * 32);
        if (kind == 0) {
          std::vector<std::pair<std::uint32_t, std::uint32_t>> points;
          for (std::size_t i = 0; i < count; ++i) {
            need(plans[2 * i] <= UINT32_MAX && plans[2 * i + 1] <= UINT32_MAX,
                 "Audit replay point overflow");
            points.emplace_back(plans[2 * i], plans[2 * i + 1]);
          }
          for (auto &v : p.runtime.replay_trace_multi(g, points, slot)) {
            put(b, v.size(), 4);
            b.insert(b.end(), v.begin(), v.end());
          }
        } else if (kind == 1) {
          std::vector<opoi::SequenceSpan> spans;
          for (std::size_t i = 0; i < count; ++i) {
            need(plans[3 * i] <= UINT32_MAX, "Audit sequence leaf overflow");
            spans.push_back({static_cast<std::uint32_t>(plans[3 * i]),
                             plans[3 * i + 1], plans[3 * i + 2]});
          }
          for (auto &v : p.runtime.replay_sequence(g, spans, max_chunks)) {
            put(b, v.bytes.size(), 8);
            b.insert(b.end(), v.bytes.begin(), v.bytes.end());
            put(b, v.chunk_cvs.size(), 4);
            for (auto &h : v.chunk_cvs)
              hash(b, h);
          }
        } else
          throw std::runtime_error("Unknown audit replay kind");
        replayed=std::chrono::steady_clock::now();
      });
      const auto finished=std::chrono::steady_clock::now();
      const auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
      std::cout<<"{\"event\":\"H15_REPLAY_PROFILE\",\"schema_version\":1,\"kind\":"<<kind
        <<",\"plans\":"<<count<<",\"positions\":"<<raw->positions
        <<",\"owner_wait_ms\":"<<ms(replay_started,acquired)<<",\"configure_ms\":"<<ms(acquired,configured)
        <<",\"replay_pack_ms\":"<<ms(configured,replayed)<<",\"release_ms\":"<<ms(replayed,finished)
        <<",\"total_ms\":"<<ms(replay_started,finished)<<"}\n"<<std::flush;
      need(b.size() <= cap, "Audit replay memory bound");
      std::memcpy(out, b.data(), b.size());
      *n = b.size();
      return 0;
    } catch (...) {
      return -1;
    }
  }
};
H15AuditClient::H15AuditClient(opoi::StockLlamaRuntime &r,
                               const model::ReadOnlyGguf &g,
                               const model::SparseMerkleIndex &m,
                               std::filesystem::path state, const Hash &secret,
                               std::string endpoint, const Hash &model_id,
                               bool offline_diagnostics)
    : impl_(std::make_unique<Impl>(r, g, m, endpoint)) {
  auto &p = *impl_;
  p.diagnostics = offline_diagnostics;
  need(r.abi_version() == 9, "H15 native audit requires ABI9");
  std::size_t bytes = 0, rows = 0;
  need(nexus_ai_h15_manifest(0, 0, 140115000, nullptr, 0, &bytes, nullptr, 0,
                             &rows, &p.info) == 2,
       "H15 manifest query failed");
  p.manifest.resize(bytes);
  p.layers.resize(rows);
  need(nexus_ai_h15_manifest(0, 0, 140115000, p.manifest.data(), bytes, &bytes,
                             reinterpret_cast<std::uint32_t *>(p.layers.data()),
                             rows, &rows, &p.info) == 0,
       "H15 manifest/layout failed");
  const auto root = consensus::official_tier_anchor(140115000, 0).root;
  std::copy(root.begin(), root.end(), p.api.model_root);
  const auto path = state.string();
  need(nexus_h15_service_open(
           reinterpret_cast<const std::uint8_t *>(path.data()), path.size(),
           secret.data(), 0, &p.api, model_id.data(), &p.service) == 0,
       "H15 audit recovery blocked; original records preserved");
  // Unsigned pool capture/replay has no local selected-chain authority.
  // SOLO keeps its mandatory node anchor; offline adapters perform no RPC.
  if(!offline_diagnostics)p.anchor = p.sink();
}
H15AuditClient::~H15AuditClient() = default;
void H15AuditClient::set_gpu_owner(
    std::function<void(const std::function<void()> &)> owner) {
  impl_->with_gpu = std::move(owner);
}
bool H15AuditClient::restoring() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->poll_admission.blocked() || impl_->poll_failures ||
         nexus_h15_service_restoring(impl_->service) != 0;
}
std::array<std::uint32_t, 2>
H15AuditClient::verify_capture(std::uint32_t seeds) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  std::array<std::uint32_t, 2> stats{};
  if (nexus_h15_selftest(p.capture, &p.api, seeds, stats.data()) != 0)
    throw std::runtime_error(std::string("H15 native self-test: ") +
                             nexus_h15_error());
  return stats;
}
std::array<std::uint32_t, 4> H15AuditClient::verify_lifecycle() {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  need(p.diagnostics, "Lifecycle probe is disabled in production");
  std::array<std::uint32_t, 4> stats{};
  if (nexus_h15_service_selftest(p.service, p.capture, &p.api, stats.data()) !=
      0)
    throw std::runtime_error(std::string("H15 lifecycle probe: ") +
                             nexus_h15_error());
  return stats;
}
bool H15AuditClient::ready() const noexcept {
  return impl_ && impl_->service && impl_->runtime.abi_version() == 9;
}
std::vector<std::uint8_t> H15AuditClient::export_pool_capture() {
  auto &p=*impl_;std::lock_guard lock(p.mutex);Buffer out;
  need(p.capture, "Pool capture missing");
  if(nexus_h15_encode_capture(p.capture,&out.b)!=0)
    throw std::runtime_error(std::string("Pool capture encoding failed: ")+nexus_h15_error());
  need(out.b.length<=514U<<20,"Pool capture storage bound exceeded");
  return buffer_bytes(out.b);
}
std::vector<std::uint8_t> H15AuditClient::open_pool_capture(std::span<const std::uint8_t> bytes,
    const Hash &request,const Hash &response,const Hash &seed,
    std::span<const std::uint64_t> path,std::span<const std::uint8_t> previous) {
  auto &p=*impl_;std::lock_guard lock(p.mutex);
  // Borsh Capture starts with tier then the exact request hash. Never replay a
  // persisted capture for a different request, even if its file was misbound.
  need(bytes.size()>=33&&bytes[0]==0&&std::equal(request.begin(),request.end(),bytes.begin()+1),
       "Pool capture request binding mismatch");
  struct CaptureOwner {void *ptr{};~CaptureOwner(){if(ptr)nexus_h15_capture_free(ptr);}} capture;
  need(nexus_h15_decode_capture(bytes.data(),bytes.size(),&capture.ptr)==0,"Pool capture decoding failed");
  Buffer out;
  need(nexus_h15_open(capture.ptr,&p.api,seed.data(),response.data(),path.data(),path.size(),
       previous.data(),previous.size(),&out.b)==0,"Pool opening replay/verification failed");
  auto pieces=unbundle(std::span(out.b.data,out.b.length));
  if(!path.empty()){need(pieces.size()==1,"Pool attention phase cardinality mismatch");return std::move(pieces[0]);}
  return buffer_bytes(out.b);
}
AiTracedAnswer H15AuditClient::infer(std::string_view prompt,
                                     std::uint32_t max_tokens,
                                     const Hash &request) {
  need(prompt.size() <= 1U << 20 && max_tokens > 0 && max_tokens <= 4096,
       "H15 prompt/token bounds");
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  if (p.capture) {
    nexus_h15_capture_free(p.capture);
    p.capture = nullptr;
  }
  struct ScratchRelease {
    opoi::StockLlamaRuntime &rt;
    ~ScratchRelease() noexcept {
      try { rt.use_task_context(); } catch (...) {}
    }
  } scratch_release{p.runtime};
  p.configure();
  std::array<std::int32_t, 256> tokens{};
  std::size_t n = tokens.size();
  std::uint32_t head = 0;
  need(nexus_h15_prefix(0, request.data(), tokens.data(), &n, &head) == 0,
       "H15 request prefix failed");

  std::string working_prompt(prompt);
  const auto context=std::min(8192U,p.runtime.trace_allocated_context());
  const std::size_t budget = opoi::prompt_budget(context, std::min(max_tokens,context/2));
  if (working_prompt.size() > budget && budget > 0) {
    if (auto cut = opoi::head_tail_cut(working_prompt, budget)) {
      working_prompt = std::move(*cut);
    }
  }

  auto [first, rest] = opoi::official_qwen35_trace_prompt(working_prompt);
  opoi::TracePrefix prefix{std::span(tokens.data(), n), head, rest};
  const auto start = std::chrono::steady_clock::now();
  opoi::TracedGeneration g;
  try {
    g = p.runtime.infer_traced(
        first, &prefix, max_tokens,
        static_cast<std::uint32_t>(working_prompt.size() / 16), 10);
  } catch (...) {
    if (auto cut = opoi::head_tail_cut(working_prompt, budget > 256 ? budget / 2 : 256)) {
      working_prompt = std::move(*cut);
      auto [f, r] = opoi::official_qwen35_trace_prompt(working_prompt);
      first = std::move(f);
      rest = std::move(r);
      prefix.rest = rest;
      g = p.runtime.infer_traced(
          first, &prefix, max_tokens,
          static_cast<std::uint32_t>(working_prompt.size() / 16), 10);
    } else {
      throw;
    }
  }
  need(g.sequence_roots.has_value(), "H15 sequence roots missing");
  NexusH15Capture raw{
      g.tokens.data(),
      g.ubatch.data(),
      reinterpret_cast<const std::uint8_t *>(g.leaves.data()),
      reinterpret_cast<const std::uint8_t *>(g.sequence_roots->data()),
      g.tokens.size(),
      p.info.position_leaves,
      g.sequence_roots->size(),
      g.prompt_positions};
  AiTracedAnswer result;
  result.answer = std::move(g.text);
  const auto secs = std::max(
      0.001,
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count());
  need(nexus_h15_capture(
           &raw, 0, request.data(), max_tokens,
           static_cast<std::uint32_t>(working_prompt.size()),
           reinterpret_cast<const std::uint8_t *>(result.answer.data()),
           result.answer.size(), secs, &p.capture,
           result.commitment.data()) == 0,
       "H15 captured trace/prefix/response bounds failed");
  Buffer plain;
  need(nexus_h15_plaintext(p.capture, &plain.b) == 0,
       "H15 token plaintext failed");
  result.sealed_plaintext = buffer_bytes(plain.b);
  return result;
}
void H15AuditClient::commit(std::string_view txid,
                            std::span<const std::uint8_t> payload,
                            std::uint64_t daa, std::uint64_t deadline) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  need(p.capture, "No H15 capture before submission");
  protowire::RpcTransaction tx;
  tx.set_subnetworkid("0400000000000000000000000000000000000000");
  tx.set_payload(hex_encode(payload.data(), payload.size()));
  auto id = consensus_transaction_id(tx);
  need(id && *id == unhex(txid), "H15 signed response identity mismatch");
  need(nexus_h15_service_commit(p.service, p.capture, payload.data(),
                                payload.size(), daa, p.anchor.data(),
                                deadline) == 0,
       "H15 durable capture commit failed");
  nexus_h15_capture_free(p.capture);
  p.capture = nullptr;
}
void H15AuditClient::poll() {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  if (std::chrono::steady_clock::now() < p.next_poll)
    return;
  p.rpc_budget = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto cycle=p.poll_admission.begin();
  try {
    if (!p.pending_page) {
      Buffer anchors;
      need(nexus_h15_service_anchors(p.service, &anchors.b) == 0 &&
               anchors.b.length % 32 == 0,
           "H15 restore anchors invalid");
      if (anchors.b.length == 0) {
        p.anchor = p.sink();
        p.poll_awaited();
        p.poll_failures = 0;
        cycle.complete();
        p.rpc_budget.reset();
        p.next_poll =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        return;
      }
      Hash start;
      std::memcpy(start.data(), anchors.b.data, 32);
      auto start_block = p.get_block(start, false);
      protowire::KaspadRequest req;
      req.mutable_getvirtualchainfromblockrequest()->set_starthash(
          hex_encode(start.data(), 32));
      req.mutable_getvirtualchainfromblockrequest()
          ->set_includeacceptedtransactionids(true);
      auto reply = p.call(req);
      need(reply.has_getvirtualchainfromblockresponse() &&
               !reply.getvirtualchainfromblockresponse().has_error(),
           "H15 chain restore failed");
      auto page = reply.getvirtualchainfromblockresponse();
      need(page.addedchainblockhashes_size() <= 10000 &&
               page.removedchainblockhashes_size() <= 72000,
           "H15 chain page bounds");
      p.pending_page.emplace(start,start_block.header().daascore(),std::move(page));
    }
    auto &work=*p.pending_page;
    const int completed=work.advance([&](const Hash &h){return p.get_block(h);},
        [&](std::span<const std::uint8_t> frame){
          need(nexus_h15_service_chain(p.service,frame.data(),frame.size())==0,
               "H15 canonical restore reconciliation failed");
        },[&]{return std::chrono::steady_clock::now()<*p.rpc_budget;});
    const bool consumed=work.consumed();p.anchor=work.anchor();
    if(!consumed)std::cout<<"H15_RESTORE=PROGRESS blocks="<<completed
                         <<" committed="<<work.committed_blocks()<<" coverage=INCOMPLETE\n"<<std::flush;
    if (consumed)
      p.pending_page.reset();
    if (consumed)
      p.poll_awaited();
    if(consumed){cycle.complete();p.chain_refresh.complete();}
    p.poll_failures = 0;
    p.next_poll = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(consumed ? 1000 : 20);
  } catch (const H15ChainChanged &) {
    p.pending_page.reset();
    const auto delay=p.chain_refresh.retry();
    p.next_poll=std::chrono::steady_clock::now()+delay;
    std::cout<<"H15_RESTORE=REFRESH reason=SELECTED_CHAIN_CHANGED retry_ms="
             <<delay.count()<<" obligations=PRESERVED replay=BLOCKED\n"<<std::flush;
  } catch (const H15PollBudgetExhausted &) {
    // Retain the page and native committed prefix. Yield to requests and resume
    // promptly; a local scheduling budget must not cause 5-60 s failure backoff.
    p.next_poll=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
    std::cout<<"H15_RESTORE=YIELD reason=POLL_BUDGET obligations=PRESERVED\n"<<std::flush;
  } catch (const std::exception &error) {
    p.pending_page.reset();
    p.poll_failures = std::min(p.poll_failures + 1, 5U);
    p.next_poll =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(std::min(60U, 5U << (p.poll_failures - 1)));
    std::cout << "H15_RESTORE=RETRY obligations=PRESERVED gpu=NOT_USED detail="
              << error.what() << "\n" << std::flush;
  }
  p.rpc_budget.reset();
}

std::uint64_t H15AuditClient::estimate_request_work(std::uint32_t prompt, std::uint32_t tokens) const {
  std::lock_guard lock(impl_->mutex);
  return nexus_h15_service_request_work(impl_->service, prompt, tokens);
}
std::uint64_t H15AuditClient::estimate_next_work(std::uint32_t prompt,
                                                 std::uint32_t tokens) const {
  std::lock_guard lock(impl_->mutex);
  impl_->next_prompt = prompt;
  impl_->next_tokens = tokens;
  return nexus_h15_service_estimate(impl_->service, prompt, tokens);
}
void H15AuditClient::advance(std::uint64_t daa,
                             std::optional<std::uint64_t> next) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  if (p.poll_admission.blocked() || p.poll_failures || p.pending_page)
    return; // Hold draws behind incomplete canonical coverage.
  if (next)
    need(nexus_h15_service_next_span(p.service, p.next_prompt, p.next_tokens) ==
             0,
         "H15 next audit span estimate");
  need(nexus_h15_service_advance(p.service, &p.api, daa,
                                 next.value_or(UINT64_MAX)) == 0,
       "H15 audit replay/verification blocked; records preserved");
}
std::optional<std::string>
H15AuditClient::response_for_request(const Hash &request) const {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  Buffer out;
  need(nexus_h15_service_response_for_request(p.service, request.data(),
                                              &out.b) == 0,
       "H15 saved response lookup failed");
  if (out.b.length == 0)
    return std::nullopt;
  need(out.b.length == 32, "H15 saved response ID shape");
  return hex_encode(out.b.data, 32);
}
void H15AuditClient::response_intent(std::string_view id) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  const auto h = unhex(id);
  const auto ms = static_cast<std::uint64_t>(std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count()));
  need(nexus_h15_service_response_intent(p.service, h.data(), ms) == 0,
       "H15 response intent fsync failed");
}
void H15AuditClient::response_result(std::string_view id, AiResponseEffects::SubmitResult::State state, std::uint64_t rpc_ms) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  const auto h = unhex(id);
  need(nexus_h15_service_response_result(p.service, h.data(), state==AiResponseEffects::SubmitResult::State::Rejected) == 0,
       "H15 response outcome fsync failed");
  if(nexus_h15_service_rpc_observed(p.service,h.data(),static_cast<std::uint8_t>(state),rpc_ms)!=0)
    std::cout<<"H15_HEALTH state=UNAVAILABLE rpc_outcome=UNKNOWN tracking=CANONICAL_INTENT\n"<<std::flush;
}
void H15AuditClient::report_health(std::uint64_t daa) {
  auto &p=*impl_;std::lock_guard lock(p.mutex);
  const auto now=std::chrono::steady_clock::now();if(now<p.next_health)return;
  p.next_health=now+std::chrono::seconds(5);Buffer out;
  if(nexus_h15_service_health(p.service,daa,&out.b)!=0){
    std::cout<<"H15_HEALTH state=UNAVAILABLE tracking=CANONICAL_JOURNAL records=PRESERVED\n"<<std::flush;return;
  }
  std::cout.write(reinterpret_cast<const char*>(out.b.data),out.b.length);std::cout<<"\n"<<std::flush;
}
void H15AuditClient::report_exit_health(std::uint64_t daa) {
  auto &p=*impl_;std::lock_guard lock(p.mutex);Buffer out;
  std::cout<<"H15_EXIT_RECOVERY_SNAPSHOT canonical_clearance=UNPROVEN\n";
  if(nexus_h15_service_health(p.service,daa,&out.b)!=0){
    std::cout<<"H15_EXIT_RECOVERY_SNAPSHOT state=UNKNOWN records=PRESERVED recovery=REQUIRED\n"<<std::flush;return;
  }
  std::cout.write(reinterpret_cast<const char*>(out.b.data),out.b.length);std::cout<<"\n"<<std::flush;
}
void H15AuditClient::submit_due(std::uint64_t daa, bool authorized) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  if (!authorized || p.poll_admission.blocked() || p.poll_failures || p.pending_page)
    return;
  const auto ms = static_cast<std::uint64_t>(std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count()));
  auto send = [&](const std::vector<std::vector<std::uint8_t>> &payloads,
                  unsigned subnet) {
    for (std::size_t i = 0; i < payloads.size(); ++i) {
      const auto &payload = payloads[i];
      protowire::RpcTransaction tx;
      std::array<std::uint8_t, 20> sn{};
      sn[0] = static_cast<std::uint8_t>(subnet);
      tx.set_subnetworkid(hex_encode(sn.data(), sn.size()));
      tx.set_payload(hex_encode(payload.data(), payload.size()));
      const auto id = consensus_transaction_id(tx);
      need(id.has_value(), "H15 transaction ID failed");
      const auto text = hex_encode(id->data(), 32);
      if (p.transport.known(text)) {
        need(nexus_h15_service_refund(p.service, id->data(), 1) == 0,
             "H15 mempool reconciliation failed");
        continue;
      }
      const auto rpc_started=std::chrono::steady_clock::now();
      const auto result = p.transport.submit(tx, text);
      const auto rpc_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-rpc_started).count();
      if(nexus_h15_service_rpc_observed(p.service,id->data(),static_cast<std::uint8_t>(result.state),rpc_ms)!=0)
        std::cout<<"H15_HEALTH state=UNAVAILABLE rpc_outcome=UNKNOWN tracking=CANONICAL_INTENT\n"<<std::flush;
      if (subnet == 4)
        need(nexus_h15_service_response_result(
                 p.service, id->data(),
                 result.state ==
                     AiResponseEffects::SubmitResult::State::Rejected) == 0,
             "H15 response outcome fsync failed");
      std::cout << "H15_DURABLE_SUBMIT subnet=" << subnet << " txid=" << text
                << " state=" << static_cast<int>(result.state) << "\n";
      if (result.state == AiResponseEffects::SubmitResult::State::Unknown) {
        std::vector<Hash> unsent;
        for (std::size_t j = i + 1; j < payloads.size(); ++j) {
          protowire::RpcTransaction later;
          later.set_subnetworkid(tx.subnetworkid());
          later.set_payload(hex_encode(payloads[j].data(), payloads[j].size()));
          const auto next = consensus_transaction_id(later);
          need(next.has_value(), "H15 unsent ID");
          unsent.push_back(*next);
        }
        if (!unsent.empty())
          need(nexus_h15_service_refund(
                   p.service,
                   reinterpret_cast<const std::uint8_t *>(unsent.data()),
                   unsent.size()) == 0,
               "H15 unsent accounting failed");
        return false;
      }
    }
    return true;
  };
  Buffer openings;
  need(nexus_h15_service_due(p.service, daa, ms, &openings.b) == 0,
       "H15 opening retry state blocked");
  if(!send(unbundle(std::span(openings.b.data, openings.b.length)),6))return;
  Buffer responses;
  need(nexus_h15_service_response_due(p.service, daa, ms, &responses.b) == 0,
       "H15 response retry state blocked");
  send(unbundle(std::span(responses.b.data, responses.b.length)),4);
}

H15AuditClient::H15ObligationStatus H15AuditClient::check_obligations(std::uint64_t current_daa) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  if (!p.service) {
    return {false, "H15_SERVICE_NOT_INITIALIZED"};
  }
  if (p.poll_failures > 0) {
    return {false, "H15_RPC_POLL_FAILURES_DETECTED: " + std::to_string(p.poll_failures)};
  }
  if(p.poll_admission.blocked())return {false,"H15_CANONICAL_POLL_PENDING"};
  if (p.pending_page.has_value()) {
    return {false, "H15_CANONICAL_PAGE_PENDING"};
  }
  if (nexus_h15_service_restoring(p.service) != 0) {
    return {false, "H15_RESTORING_ACTIVE"};
  }
  if (current_daa == 0) {
    return {false, "CANONICAL_BLOCKCHAIN_DAA_ZERO"};
  }

  const auto ms = static_cast<std::uint64_t>(std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
             .count()));

  Buffer openings;
  if (nexus_h15_service_due(p.service, current_daa, ms, &openings.b) != 0) {
    return {false, "H15_QUERY_DUE_OPENINGS_FAILED"};
  }
  const auto openings_status = evaluate_obligation_bundle(
      std::span<const std::uint8_t>(openings.b.data, openings.b.length), "OPENINGS");
  if (!openings_status.safe) {
    return openings_status;
  }

  Buffer responses;
  if (nexus_h15_service_response_due(p.service, current_daa, ms, &responses.b) != 0) {
    return {false, "H15_QUERY_DUE_RESPONSES_FAILED"};
  }
  const auto responses_status = evaluate_obligation_bundle(
      std::span<const std::uint8_t>(responses.b.data, responses.b.length), "RESPONSES");
  if (!responses_status.safe) {
    return responses_status;
  }

  Buffer health_buf;
  if (nexus_h15_service_health(p.service, current_daa, &health_buf.b) != 0) {
    return {false, "H15_HEALTH_QUERY_FAILED"};
  }
  std::string json_text(reinterpret_cast<const char *>(health_buf.b.data), health_buf.b.length);
  google::protobuf::Struct health_json;
  if (!google::protobuf::util::JsonStringToMessage(json_text, &health_json).ok()) {
    return {false, "H15_HEALTH_JSON_PARSE_ERROR"};
  }

  H15ObligationStatus status;
  const auto &fields = health_json.fields();
  auto rows_it = fields.find("rows");
  if (rows_it == fields.end() || !rows_it->second.has_list_value()) {
    return {false, "H15_HEALTH_MISSING_ROWS"};
  }

  for (const auto &item : rows_it->second.list_value().values()) {
    if (!item.has_struct_value()) continue;
    const auto &row = item.struct_value().fields();
    std::string obligation = "unknown";
    if (auto ob_it = row.find("obligation"); ob_it != row.end() && ob_it->second.has_string_value()) {
      obligation = ob_it->second.string_value();
    }

    if (auto cov_it = row.find("chain_coverage_complete"); cov_it != row.end()) {
      if (!cov_it->second.bool_value()) {
        return {false, "H15_CHAIN_COVERAGE_INCOMPLETE: " + obligation};
      }
    } else {
      return {false, "H15_MISSING_CHAIN_COVERAGE_FIELD: " + obligation};
    }

    if (auto rpc_it = row.find("response_rpc"); rpc_it != row.end() && rpc_it->second.has_string_value()) {
      if (rpc_it->second.string_value() == "UNKNOWN") {
        return {false, "H15_AMBIGUOUS_RESPONSE_RPC: " + obligation};
      }
    }

    std::string row_status;
    if (auto st_it = row.find("status"); st_it != row.end() && st_it->second.has_string_value()) {
      row_status = st_it->second.string_value();
    }
    if (row_status == "OPENING_ACCEPTED") {
      status.accepted_count++;
    } else if (row_status == "OPENING_EXPIRED") {
      status.expired_count++;
    } else {
      status.pending_count++;
      return {false, "H15_OBLIGATION_UNRESOLVED: " + obligation + " status=" + row_status};
    }
  }

  status.safe = true;
  status.reason = "H15_OBLIGATIONS_CLEARED";
  return status;
}
} // namespace keryx::nexus::ai
