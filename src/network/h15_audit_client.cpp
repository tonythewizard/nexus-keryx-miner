#include "keryx/nexus/h15_audit_client.hpp"
#include "keryx/nexus/h15_bridge.h"
#include "keryx/nexus/trace_manifest_bridge.h"
#include "messages.grpc.pb.h"
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
std::vector<std::vector<std::uint8_t>>
unbundle(std::span<const std::uint8_t> b) {
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
} // namespace
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
  std::optional<bool> awaited_supported;
  bool awaited_probed{};
  std::chrono::steady_clock::time_point next_awaited{};
  std::uint32_t next_prompt{}, next_tokens{};

  struct ChainPage {
    Hash start;
    std::uint64_t daa;
    protowire::GetVirtualChainFromBlockResponseMessage page;
    int at{};
  };
  std::optional<ChainPage> pending_page;
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
    need(remaining > std::chrono::steady_clock::duration::zero(),
         "Audit polling budget exhausted");
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
    need(got, "Audit RPC response timeout");
    return response;
  }
  void poll_awaited() {
    const auto now = std::chrono::steady_clock::now();
    if (now < next_awaited)
      return;
    next_awaited = now + std::chrono::seconds(10);
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
    put(frame, response.openings_size(), 4);
    for (const auto &o : response.openings()) {
      need(queried.contains(o.responsetxid()) && o.attentionpath_size() <= 16,
           "Unsolicited audit ledger row");
      hash(frame, unhex(o.responsetxid()));
      hash(frame, unhex(o.escrowpubkey()));
      hash(frame, unhex(o.seed()));
      put(frame, o.deadlinedaa(), 8);
      put(frame, o.pieces(), 4);
      put(frame, o.attentionpath_size(), 4);
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
             (!selected || b.verbosedata().ischainblock()) &&
             unhex(b.verbosedata().hash()) == h,
         "Audit header is not selected chain");
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
  void configure() {
    need(runtime.abi_version() == 9, "H15 requires ABI9");
    const auto positions =
        std::min<std::uint32_t>(runtime.trace_context_capacity(),
                                (512U << 20) / 32 / info.position_leaves);
    runtime.configure_trace(manifest, positions, info.position_leaves);
    runtime.configure_sequence(layers, info.n_attn, info.n_recur, info.heads,
                               info.interval);
  }
  static int weight(void *u, std::uint64_t index, std::uint8_t *out,
                    std::uint8_t *path, std::size_t *n) {
    try {
      auto &p = *static_cast<Impl *>(u);
      need(index < p.gguf.chunk_count() && out && n, "Audit weight bounds");
      const auto tile = p.gguf.read_tile(index / 32);
      std::memcpy(out, tile.data() + index % 32 * 32, 32);
      if (!path) {
        *n = 0;
        return 0;
      }
      std::vector<Hash> nodes;
      nodes.reserve(32);
      for (std::size_t i = 0; i < 32; ++i)
        nodes.push_back(
            consensus::blake3_hash(std::span(tile.data() + i * 32, 32)));
      std::vector<Hash> proof;
      auto at = index % 32;
      for (unsigned level = 0; level < 5; ++level) {
        proof.push_back(nodes[at ^ 1]);
        std::vector<Hash> next;
        for (std::size_t i = 0; i < nodes.size(); i += 2)
          next.push_back(consensus::hash_pair(nodes[i], nodes[i + 1]));
        nodes = std::move(next);
        at /= 2;
      }
      auto upper = p.merkle.tile_path(index / 32);
      proof.insert(proof.end(), upper.begin(), upper.end());
      need(proof.size() <= *n, "Audit path capacity");
      std::memcpy(path, proof.data(), proof.size() * 32);
      *n = proof.size();
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
      std::vector<std::uint8_t> b;
      p.with_gpu([&] {
        p.configure();
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
      });
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
  p.anchor = p.sink();
}
H15AuditClient::~H15AuditClient() = default;
void H15AuditClient::set_gpu_owner(
    std::function<void(const std::function<void()> &)> owner) {
  impl_->with_gpu = std::move(owner);
}
bool H15AuditClient::restoring() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->poll_failures ||
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
  p.configure();
  std::array<std::int32_t, 256> tokens{};
  std::size_t n = tokens.size();
  std::uint32_t head = 0;
  need(nexus_h15_prefix(0, request.data(), tokens.data(), &n, &head) == 0,
       "H15 request prefix failed");
  auto [first, rest] = opoi::official_qwen35_trace_prompt(prompt);
  opoi::TracePrefix prefix{std::span(tokens.data(), n), head, rest};
  const auto start = std::chrono::steady_clock::now();
  auto g = p.runtime.infer_traced(
      first, &prefix, max_tokens,
      static_cast<std::uint32_t>(prompt.size() / 16), 10);
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
           static_cast<std::uint32_t>(prompt.size()),
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
      p.pending_page = Impl::ChainPage{start, start_block.header().daascore(),
                                       std::move(page)};
    }
    auto &work = *p.pending_page;
    const auto &page = work.page;
    const int end = std::min(work.at + 8, page.addedchainblockhashes_size());
    std::vector<std::uint8_t> b;
    hash(b, work.start);
    put(b, work.daa, 8);
    put(b, 0, 4);
    put(b,
        static_cast<std::uint64_t>(
            work.at == 0 ? page.removedchainblockhashes_size() : 0),
        4);
    if (work.at == 0)
      for (const auto &h : page.removedchainblockhashes())
        hash(b, unhex(h));
    put(b, static_cast<std::uint64_t>(end - work.at), 4);
    Hash last = work.start;
    auto last_daa = work.daa;
    for (int i = work.at; i < end; ++i) {
      const auto &h = page.addedchainblockhashes(i);
      const auto raw_hash = unhex(h);
      const auto block = p.get_block(raw_hash);
      hash(b, raw_hash);
      put(b, block.header().daascore(), 8);
      const protowire::RpcAcceptedTransactionIds *accepted = nullptr;
      for (const auto &ids : page.acceptedtransactionids())
        if (ids.acceptingblockhash() == h) {
          accepted = &ids;
          break;
        }
      need(!accepted || accepted->acceptedtransactionids_size() <= 50000,
           "H15 accepted transaction bounds");
      put(b,
          static_cast<std::uint64_t>(
              accepted ? accepted->acceptedtransactionids_size() : 0),
          4);
      if (accepted)
        for (const auto &id : accepted->acceptedtransactionids())
          hash(b, unhex(id));
      last = raw_hash;
      last_daa = block.header().daascore();
    }
    const bool consumed = end == page.addedchainblockhashes_size();
    put(b, consumed && page.addedchainblockhashes_size() < 100, 1);
    need(nexus_h15_service_chain(p.service, b.data(), b.size()) == 0,
         "H15 canonical restore reconciliation failed");
    p.anchor = last;
    work.start = last;
    work.daa = last_daa;
    work.at = end;
    if (consumed)
      p.pending_page.reset();
    if (consumed)
      p.poll_awaited();
    p.poll_failures = 0;
    p.next_poll = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(consumed ? 1000 : 20);
  } catch (...) {
    p.pending_page.reset();
    p.poll_failures = std::min(p.poll_failures + 1, 5U);
    p.next_poll =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(std::min(60U, 5U << (p.poll_failures - 1)));
    std::cout << "H15_RESTORE=RETRY obligations=PRESERVED gpu=NOT_USED\n";
  }
  p.rpc_budget.reset();
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
  if (p.poll_failures || p.pending_page)
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
void H15AuditClient::response_result(std::string_view id, bool rejected) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  const auto h = unhex(id);
  need(nexus_h15_service_response_result(p.service, h.data(), rejected) == 0,
       "H15 response outcome fsync failed");
}
void H15AuditClient::submit_due(std::uint64_t daa, bool authorized) {
  auto &p = *impl_;
  std::lock_guard lock(p.mutex);
  if (!authorized || p.poll_failures || p.pending_page)
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
      const auto result = p.transport.submit(tx, text);
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
  Buffer responses;
  need(nexus_h15_service_response_due(p.service, daa, ms, &responses.b) == 0,
       "H15 response retry state blocked");
  if (!send(unbundle(std::span(responses.b.data, responses.b.length)), 4))
    return;
  Buffer openings;
  need(nexus_h15_service_due(p.service, daa, ms, &openings.b) == 0,
       "H15 opening retry state blocked");
  send(unbundle(std::span(openings.b.data, openings.b.length)), 6);
}
} // namespace keryx::nexus::ai
