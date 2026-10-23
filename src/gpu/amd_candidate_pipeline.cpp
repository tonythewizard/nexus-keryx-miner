#include "keryx/nexus/amd_candidate_pipeline.hpp"
#include "keryx/nexus/prepared_rpc_template.hpp"
#include "keryx/nexus/opoi.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <numeric>
#include <syncstream>
#include <thread>
#include <unordered_map>
namespace keryx::nexus {
using namespace consensus;
AmdVerifiedCandidate::~AmdVerifiedCandidate(){owner_.finish(ticket_);}
WorkGate AmdVerifiedCandidate::recheck() const {return owner_.check_candidate(ticket_,binding_);}
namespace {
class Cancelled {};
std::uint64_t clock_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
class WinnerProgress {
public:
  std::uint64_t nonce{},generation{}; bool screened{},returned{};
  const char *stage="SCREEN";
  const std::uint64_t started=clock_ns();
  ~WinnerProgress(){
    if(screened&&!returned)std::osyncstream(std::cout)<<"AMD_WINNER_NOT_SUBMITTED nonce="<<nonce
      <<" generation="<<generation<<" stage="<<stage
      <<" reason="<<(std::uncaught_exceptions()?"EXCEPTION":"ADMISSION_CHANGED")
      <<" elapsed_ms="<<(clock_ns()-started)/1e6<<"\n"<<std::flush;
  }
  void advance(const char *next){stage=next;if(screened)std::osyncstream(std::cout)
    <<"AMD_WINNER_STAGE nonce="<<nonce<<" generation="<<generation<<" stage="<<stage
    <<" elapsed_ms="<<(clock_ns()-started)/1e6<<"\n"<<std::flush;}
};

using u32x8 = std::uint32_t __attribute__((vector_size(32)));
using u64x4 = std::uint64_t __attribute__((vector_size(32)));

constexpr std::uint32_t B3_IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};
constexpr std::uint8_t B3_SCHED[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};
constexpr std::uint32_t B3_ROOT_FLAGS = 1u | 2u | 8u;

inline std::uint32_t load_le32(const std::uint8_t *p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline void b3_g_scalar(std::uint32_t *v, int a, int b, int c, int d,
                        std::uint32_t mx, std::uint32_t my) noexcept {
  v[a] += v[b] + mx;
  v[d] = std::rotr(v[d] ^ v[a], 16);
  v[c] += v[d];
  v[b] = std::rotr(v[b] ^ v[c], 12);
  v[a] += v[b] + my;
  v[d] = std::rotr(v[d] ^ v[a], 8);
  v[c] += v[d];
  v[b] = std::rotr(v[b] ^ v[c], 7);
}

inline void b3_compress_scalar(std::uint32_t out[8],
                               const std::uint32_t m[16],
                               std::uint32_t block_len) noexcept {
  std::uint32_t v[16] = {
      B3_IV[0], B3_IV[1], B3_IV[2], B3_IV[3],
      B3_IV[4], B3_IV[5], B3_IV[6], B3_IV[7],
      B3_IV[0], B3_IV[1], B3_IV[2], B3_IV[3],
      0u,       0u,       block_len, B3_ROOT_FLAGS};
  for (int r = 0; r < 7; ++r) {
    const auto *s = B3_SCHED[r];
    b3_g_scalar(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
    b3_g_scalar(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
    b3_g_scalar(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
    b3_g_scalar(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
    b3_g_scalar(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
    b3_g_scalar(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
    b3_g_scalar(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
    b3_g_scalar(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
  }
  for (int i = 0; i < 8; ++i)
    out[i] = v[i] ^ v[i + 8];
}

std::uint64_t fast_final_state_scalar(const std::uint8_t *bytes) noexcept {
  std::uint32_t tree[32][8];
  std::uint32_t m[16]{};
  for (std::size_t r = 0; r < 32; ++r) {
    std::memcpy(m, bytes + r * 32, 32);
    b3_compress_scalar(tree[r], m, 32u);
  }
  for (std::size_t half = 16; half >= 1; half >>= 1) {
    for (std::size_t i = 0; i < half; ++i)
      b3_compress_scalar(tree[i], &tree[2 * i][0], 64u);
  }
  return static_cast<std::uint64_t>(tree[0][0]) |
         (static_cast<std::uint64_t>(tree[0][1]) << 32);
}

#if defined(__x86_64__)
#define NEXUS_TARGET_AVX2 __attribute__((target("avx2")))
#else
#define NEXUS_TARGET_AVX2
#endif

NEXUS_TARGET_AVX2 inline u32x8 rotr_8x(u32x8 x, unsigned n) noexcept {
  return (x >> n) | (x << (32u - n));
}

NEXUS_TARGET_AVX2 inline u32x8 b3_broadcast_8x(std::uint32_t x) noexcept {
  return u32x8{x, x, x, x, x, x, x, x};
}

NEXUS_TARGET_AVX2 inline void b3_g_8x(u32x8 *v, int a, int b, int c, int d,
                                      u32x8 mx, u32x8 my) noexcept {
  v[a] += v[b] + mx;
  v[d] = rotr_8x(v[d] ^ v[a], 16);
  v[c] += v[d];
  v[b] = rotr_8x(v[b] ^ v[c], 12);
  v[a] += v[b] + my;
  v[d] = rotr_8x(v[d] ^ v[a], 8);
  v[c] += v[d];
  v[b] = rotr_8x(v[b] ^ v[c], 7);
}

NEXUS_TARGET_AVX2 inline void b3_compress_8x(u32x8 out[8], const u32x8 m[16],
                                             std::uint32_t block_len) noexcept {
  u32x8 v[16] = {
      b3_broadcast_8x(B3_IV[0]), b3_broadcast_8x(B3_IV[1]), b3_broadcast_8x(B3_IV[2]), b3_broadcast_8x(B3_IV[3]),
      b3_broadcast_8x(B3_IV[4]), b3_broadcast_8x(B3_IV[5]), b3_broadcast_8x(B3_IV[6]), b3_broadcast_8x(B3_IV[7]),
      b3_broadcast_8x(B3_IV[0]), b3_broadcast_8x(B3_IV[1]), b3_broadcast_8x(B3_IV[2]), b3_broadcast_8x(B3_IV[3]),
      b3_broadcast_8x(0u),       b3_broadcast_8x(0u),       b3_broadcast_8x(block_len), b3_broadcast_8x(B3_ROOT_FLAGS)};
  for (int r = 0; r < 7; ++r) {
    const auto *s = B3_SCHED[r];
    b3_g_8x(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
    b3_g_8x(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
    b3_g_8x(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
    b3_g_8x(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
    b3_g_8x(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
    b3_g_8x(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
    b3_g_8x(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
    b3_g_8x(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
  }
  for (int i = 0; i < 8; ++i)
    out[i] = v[i] ^ v[i + 8];
}

NEXUS_TARGET_AVX2 void fast_final_state_8x(const std::uint8_t *base,
                                           std::uint64_t out[8]) noexcept {
  u32x8 tree[32][8];
  const u32x8 zero{0, 0, 0, 0, 0, 0, 0, 0};
  u32x8 m[16] = {zero, zero, zero, zero, zero, zero, zero, zero,
                 zero, zero, zero, zero, zero, zero, zero, zero};
  for (std::size_t r = 0; r < 32; ++r) {
    const std::size_t row_off = r * 32;
    for (std::size_t w = 0; w < 8; ++w) {
      const std::size_t off = row_off + w * 4;
      m[w] = u32x8{
          load_le32(base + 0 * 1024 + off), load_le32(base + 1 * 1024 + off),
          load_le32(base + 2 * 1024 + off), load_le32(base + 3 * 1024 + off),
          load_le32(base + 4 * 1024 + off), load_le32(base + 5 * 1024 + off),
          load_le32(base + 6 * 1024 + off), load_le32(base + 7 * 1024 + off)};
    }
    b3_compress_8x(tree[r], m, 32u);
  }
  for (std::size_t half = 16; half >= 1; half >>= 1) {
    for (std::size_t i = 0; i < half; ++i)
      b3_compress_8x(tree[i], &tree[2 * i][0], 64u);
  }
  for (int lane = 0; lane < 8; ++lane) {
    out[lane] = static_cast<std::uint64_t>(tree[0][0][lane]) |
                (static_cast<std::uint64_t>(tree[0][1][lane]) << 32);
  }
}

constexpr std::array<std::uint64_t, 24> keccak_rc = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
constexpr std::array<unsigned, 25> keccak_rot = {
    0,  1,  62, 28, 27, 36, 44, 6,  55, 20, 3,  10, 43,
    25, 39, 41, 45, 15, 21, 8,  18, 2,  61, 56, 14};

bool cpu_has_avx2() noexcept {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool supported = __builtin_cpu_supports("avx2") != 0;
  return supported;
#else
  return false;
#endif
}

std::uint64_t seed_from_sponge_scalar(std::array<std::uint64_t, 25> a,
                                      std::uint64_t nonce) noexcept {
  a[9] ^= nonce;
  for (auto rc : keccak_rc) {
    std::uint64_t c[5], d[5];
    for (unsigned x = 0; x < 5; ++x)
      c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
    for (unsigned x = 0; x < 5; ++x)
      d[x] = c[(x + 4) % 5] ^ std::rotl(c[(x + 1) % 5], 1);
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned x = 0; x < 5; ++x)
        a[x + 5 * y] ^= d[x];
    std::uint64_t b[25];
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned x = 0; x < 5; ++x)
        b[y + 5 * ((2 * x + 3 * y) % 5)] = std::rotl(a[x + 5 * y], keccak_rot[x + 5 * y]);
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned x = 0; x < 5; ++x)
        a[x + 5 * y] = b[x + 5 * y] ^ ((~b[(x + 1) % 5 + 5 * y]) & b[(x + 2) % 5 + 5 * y]);
    a[0] ^= rc;
  }
  return a[0];
}

NEXUS_TARGET_AVX2 inline u64x4 rotl_4x(u64x4 x, unsigned n) noexcept {
  return n == 0 ? x : ((x << n) | (x >> (64u - n)));
}

NEXUS_TARGET_AVX2 void seed_from_sponge_4x(const std::array<std::uint64_t, 25> &base,
                                           std::uint64_t first_nonce,
                                           std::uint64_t out[4]) noexcept {
  u64x4 a[25];
  for (unsigned i = 0; i < 25; ++i)
    a[i] = u64x4{base[i], base[i], base[i], base[i]};
  a[9] ^= u64x4{first_nonce, first_nonce + 1, first_nonce + 2, first_nonce + 3};
  for (auto rc : keccak_rc) {
    u64x4 c[5], d[5];
    for (unsigned x = 0; x < 5; ++x)
      c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
    for (unsigned x = 0; x < 5; ++x)
      d[x] = c[(x + 4) % 5] ^ rotl_4x(c[(x + 1) % 5], 1);
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned x = 0; x < 5; ++x)
        a[x + 5 * y] ^= d[x];
    u64x4 b[25];
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned x = 0; x < 5; ++x)
        b[y + 5 * ((2 * x + 3 * y) % 5)] = rotl_4x(a[x + 5 * y], keccak_rot[x + 5 * y]);
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned x = 0; x < 5; ++x)
        a[x + 5 * y] = b[x + 5 * y] ^ ((~b[(x + 1) % 5 + 5 * y]) & b[(x + 2) % 5 + 5 * y]);
    a[0] ^= u64x4{rc, rc, rc, rc};
  }
  for (int i = 0; i < 4; ++i)
    out[i] = a[0][i];
}

class ScreenPool {
public:
  explicit ScreenPool(unsigned threads = 3) : stop_(false), gen_(0), active_(0) {
    workers_.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
      workers_.emplace_back([this, t, threads] {
        std::uint64_t seen = 0;
        for (;;) {
          std::function<void(unsigned, unsigned)> fn;
          {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [&] { return stop_ || gen_ != seen; });
            if (stop_) return;
            seen = gen_;
            fn = task_;
          }
          fn(t + 1, threads + 1);
          if (active_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard lock(mu_);
            done_cv_.notify_one();
          }
        }
      });
    }
  }
  ~ScreenPool() {
    {
      std::lock_guard lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto &w : workers_) if (w.joinable()) w.join();
  }
  template <class F>
  void run(F &&fn) {
    const unsigned n = static_cast<unsigned>(workers_.size());
    if (n == 0) {
      fn(0, 1);
      return;
    }
    {
      std::lock_guard lock(mu_);
      task_ = fn;
      active_.store(n, std::memory_order_release);
      ++gen_;
    }
    cv_.notify_all();
    fn(0, n + 1);
    std::unique_lock lock(mu_);
    done_cv_.wait(lock, [&] { return active_.load(std::memory_order_acquire) == 0; });
  }
private:
  std::mutex mu_;
  std::condition_variable cv_, done_cv_;
  bool stop_;
  std::uint64_t gen_;
  std::atomic<unsigned> active_;
  std::function<void(unsigned, unsigned)> task_;
  std::vector<std::thread> workers_;
};
} // namespace

struct AmdCandidatePipeline::Workspace {
  PreparedRpcTemplateCache decoded;
  std::vector<std::uint8_t> validated_response_wire;
  std::vector<std::uint8_t> validated_block_wire;
  std::vector<std::uint64_t> seeds;
  VulkanWalkBatch gpu;
  std::vector<std::uint64_t> next_seeds;
  std::array<std::uint64_t,25> next_sponge{};
  std::uint64_t next_nonce{};
  bool next_valid{};
  std::uint64_t seed_cache_hits{}, seed_cache_misses{};
  std::unique_ptr<ScreenPool> pool;
};

AmdCandidatePipeline::AmdCandidatePipeline(
    GpuWorkCoordinator &owner, consensus::VulkanWalkEngine &engine,
    const model::ReadOnlyGguf &model, model::SparseMerkleIndex &merkle,
    RustProofVerifier &verifier, std::string payout, std::string extra,
    MiningLane lane)
    : workspace_(std::make_unique<Workspace>()),
      owner_(owner), engine_(engine), model_(model), merkle_(merkle),
      verifier_(verifier), payout_(std::move(payout)),
      extra_(std::move(extra)), lane_(lane) {}

AmdCandidatePipeline::~AmdCandidatePipeline() = default;

std::unique_ptr<AmdVerifiedCandidate> AmdCandidatePipeline::execute(
    GpuWorkCoordinator::Ticket ticket,const std::function<bool()> &cancelled,
    const std::function<void()> &after_enqueue) {
  std::lock_guard serialize(execute_mutex_);
  auto &ws = *workspace_;
  const auto preparation_started=clock_ns();
  WinnerProgress progress;
  // Retire the reservation even if header validation/binding throws.
  struct ReservationGuard {GpuWorkCoordinator &owner;GpuWorkCoordinator::Ticket &ticket;bool active{true};
    ~ReservationGuard(){if(active)owner.finish(ticket);}} cleanup{owner_,ticket};
  if(ticket.lane()!=lane_ || ticket.count()<1 || ticket.count()>16384)throw std::invalid_argument("Vulkan candidate pipeline requires bounded nonces from its configured lane");
  if(owner_.check(ticket)!=WorkGate::Ready)return nullptr;
  const auto &job=ticket.job();
  if(job.fields.pay_address!=payout_ || !opoi::valid_coinbase_extra_data(job.fields.extra_data,extra_))
    throw std::runtime_error("AMD payout/delegation request binding mismatch");
  if(ws.validated_response_wire != job.fields.response_wire || ws.validated_block_wire != job.fields.block_wire) {
    protowire::KaspadResponse envelope;
    if(!envelope.ParseFromArray(job.fields.response_wire.data(),static_cast<int>(job.fields.response_wire.size())) ||
       !envelope.has_getblocktemplateresponse() || !envelope.getblocktemplateresponse().issynced() ||
       !envelope.getblocktemplateresponse().error().message().empty())
      throw std::runtime_error("AMD RPC template is malformed, unsynced, or errored");
    const auto &block=ws.decoded.block(job.fields.response_wire);
    const auto raw=block.SerializeAsString();
    if(raw.size()!=job.fields.block_wire.size() || !std::equal(raw.begin(),raw.end(),job.fields.block_wire.begin(),[](char a,auto b){return static_cast<unsigned char>(a)==b;}))
      throw std::runtime_error("AMD RPC/block snapshot mismatch");
    ws.validated_response_wire = job.fields.response_wire;
    ws.validated_block_wire = job.fields.block_wire;
  }
  const auto &work=ws.decoded.get(job.fields.response_wire);const auto &header=work.header;
  const auto &pph=work.pre_pow;const auto &target=work.target;
  if(target.zero || header.timestamp!=ticket.job().fields.timestamp || header.daa_score!=ticket.job().fields.daa_score ||
     header.bits!=ticket.job().fields.compact_target_bits)throw std::runtime_error("AMD header/template binding mismatch");
  const auto anchor=official_tier_anchor(header.daa_score,header.pom_tier);
  if(anchor.chunks!=model_.chunk_count() || model_.chunk_count()%32)throw std::runtime_error("AMD model/tier chunk anchor mismatch");
  CandidateBinding binding;
  try {binding=bind_candidate(ticket.job(),ticket.first_nonce(),header.timestamp,pph);}
  catch(const std::invalid_argument &){if(!ticket.job().lease.valid())return nullptr;throw;}
  if(owner_.check_candidate(ticket,binding)!=WorkGate::Ready)return nullptr;
  auto candidate=std::unique_ptr<AmdVerifiedCandidate>(new AmdVerifiedCandidate(owner_,ticket,header,binding));
  cleanup.active=false; // Reservation ownership transferred to result on every failure path.
  // No external verifier/transport is invoked until the GPU witness is complete.
  const auto eligible=[&]{return bounded_work_ready([&]{return candidate->recheck();},
      [&]{return cancelled&&cancelled();});};
  const bool attesting=!startup_parity_done_;
  const auto ready=[&]{
    if(!attesting)return eligible();
    if(cancelled&&cancelled())return false;
    return startup_attestation_gate(candidate->recheck());
  };
  try {
    const std::uint32_t count=ticket.count();
    auto &seeds=ws.seeds;
    seeds.resize(count);
    std::vector<std::uint8_t> all_tiles;
    if(!engine_.resident())all_tiles.reserve(count*256*1024);
    std::vector<std::uint32_t> all_slots;
    if(!engine_.resident())all_slots.reserve(count*256);

    const bool use_avx2 = cpu_has_avx2();
    const char *lookahead_setting=std::getenv("NEXUS_JIT_SEED_LOOKAHEAD");
    const bool lookahead = !lookahead_setting || std::string_view(lookahead_setting)!="0";
    if(engine_.resident() && header.daa_score >= h10_activation_daa) {
      const auto sponge = seed_sponge_state(pph, header.timestamp, header.daa_score);
      const std::uint64_t first = ticket.first_nonce();
      if(lookahead && ws.next_valid && ws.next_nonce==first &&
          ws.next_sponge==sponge && ws.next_seeds.size()==count) {
        seeds.swap(ws.next_seeds); ++ws.seed_cache_hits;
        if(std::getenv("NEXUS_JIT_VERIFY_LOOKAHEAD"))
          for(std::uint32_t j=0;j<count;++j)
            if(seeds[j]!=block_seed(pph,header.timestamp,first+j,header.daa_score))
              throw std::runtime_error("Lookahead seed differs from canonical reference");
      } else {
        ++ws.seed_cache_misses;
        std::uint32_t i = 0;
        if(use_avx2)for(;i+4<=count;i+=4)seed_from_sponge_4x(sponge,first+i,&seeds[i]);
        for(;i<count;++i)seeds[i]=seed_from_sponge_scalar(sponge,first+i);
      }
      ws.next_valid=false;
    } else {
      for(std::uint32_t i=0;i<count;++i) {
        const std::uint64_t nonce=ticket.first_nonce()+i;
        const auto seed=block_seed(pph,header.timestamp,nonce,header.daa_score);
        seeds[i]=seed;
        if(engine_.resident())continue; // No per-nonce CPU gather for resident search.
        const auto input=gather_v4_model_tiles(seed,anchor.chunks/32,[&](std::uint64_t offset){
          if(!ready())throw Cancelled{};
          std::array<std::uint8_t,1024> tile{};model_.read_tile_into(offset,tile);return tile;
        });
        const std::uint32_t base_slot=static_cast<std::uint32_t>(all_tiles.size()/1024);
        all_tiles.insert(all_tiles.end(),input.tiles.begin(),input.tiles.end());
        for(std::uint32_t s=0;s<256;++s) all_slots.push_back(base_slot+s);
      }
    }

    auto &gpu=ws.gpu;
    // Fast search: diagnostics disabled during screening
    const auto gpu_started=clock_ns();
    const auto prepare_next = [&] {
      if(after_enqueue)after_enqueue();
      if(!lookahead || !engine_.resident() || header.daa_score<h10_activation_daa || !ready() ||
          ticket.first_nonce()>UINT64_MAX-2ULL*count+1)return;
      const auto sponge=seed_sponge_state(pph,header.timestamp,header.daa_score);
      const auto first=ticket.first_nonce()+count;
      ws.next_seeds.resize(count);ws.next_valid=false;
      std::uint32_t i=0;
      if(use_avx2)for(;i+4<=count;i+=4){
        if(i%64==0&&!ready())return;
        seed_from_sponge_4x(sponge,first+i,&ws.next_seeds[i]);
      }
      for(;i<count;++i){if(i%64==0&&!ready())return;ws.next_seeds[i]=seed_from_sponge_scalar(sponge,first+i);}
      if(!ready())return;
      ws.next_sponge=sponge;ws.next_nonce=first;ws.next_valid=true;
    };
    constexpr std::uint64_t bda_salt[4]={0x7C99D381176D4EC4ULL,0xC2E28E3E28118C36ULL,0xD496CE1B129B76CAULL,0x47CF0979FA580BCEULL};
    std::uint64_t bda_words[4]{}, bda_targets[4]{};
    for(unsigned i=0;i<4;++i){
      std::uint64_t p_word=0, t_word=0;
      for(unsigned b=0;b<8;++b){
        p_word |= std::uint64_t(pph.data()[8*i+b]) << (8*b);
        t_word |= std::uint64_t(target.little_endian.data()[8*i+b]) << (8*b);
      }
      bda_words[i]=p_word^bda_salt[i];
      bda_targets[i]=t_word;
    }
    const bool gpu_completed=engine_.run(anchor.chunks/32,seeds,all_tiles,all_slots,false,gpu,[&]{return !ready();},prepare_next,false,bda_words,bda_targets);
    const auto gpu_ended=clock_ns();
    const bool screen_ready=gpu_completed&&ready();
    const auto record_profile=[&]{
      const auto end=clock_ns();
      profile_ns_[0]+=gpu_started-preparation_started;
      profile_ns_[1]+=gpu_ended-gpu_started;
      profile_ns_[2]+=end-gpu_ended;
      if(last_screen_end_ns_)profile_ns_[3]+=preparation_started-last_screen_end_ns_;
      ++profile_ns_[4];last_screen_end_ns_=end;
      if(end>=next_profile_ns_){
        std::osyncstream(std::cout)<<"AMD_BATCH_PROFILE batches="<<profile_ns_[4]
          <<" preparation_ns="<<profile_ns_[0]<<" gpu_call_wall_ns="<<profile_ns_[1]
          <<" screen_ns="<<profile_ns_[2]<<" between_batches_ns="<<profile_ns_[3]
          <<" seed_cache_hits="<<ws.seed_cache_hits<<" seed_cache_misses="<<ws.seed_cache_misses
          <<" gpu_call_includes=dispatch_wait_guards_readback_not_kernel_only\n"<<std::flush;
        next_profile_ns_=end+1000000000ULL;
      }
    };
    if(!screen_ready){record_profile();return nullptr;}
    if(!engine_.screens_target_on_gpu() && gpu.final_states.size()!=count*1024)
      throw std::runtime_error("AMD witness dimensions differ");

    if(!startup_parity_done_) {
      if(header.daa_score >= h10_activation_daa) {
        const auto sponge = seed_sponge_state(pph, header.timestamp, header.daa_score);
        const std::uint64_t first = ticket.first_nonce();
        if(seed_from_sponge_scalar(sponge, first) != block_seed(pph, header.timestamp, first, header.daa_score))
          throw std::runtime_error("AMD scalar Keccak sponge seed disagreed with reference");
        if(use_avx2) {
          std::uint64_t check_seeds[4]{};
          seed_from_sponge_4x(sponge, first, check_seeds);
          for(unsigned lane = 0; lane < 4; ++lane) {
            if(check_seeds[lane] != block_seed(pph, header.timestamp, first + lane, header.daa_score))
              throw std::runtime_error("AMD AVX2 4x Keccak sponge seed disagreed with reference");
          }
        }
      }
      if(!gpu.final_states.empty()) {
        std::span<const std::uint8_t, 1024> first_span(gpu.final_states.data(), 1024);
        if(fast_final_state_scalar(gpu.final_states.data()) != consensus::final_state_from_bytes(first_span))
          throw std::runtime_error("AMD fast BLAKE3 reducer disagreed with reference");
        if(use_avx2) {
          std::array<std::uint8_t, 8 * 1024> test_block{};
          for(unsigned lane = 0; lane < 8; ++lane) {
            const auto *src = (lane < count) ? (gpu.final_states.data() + lane * 1024) : gpu.final_states.data();
            for(std::size_t b = 0; b < 1024; ++b)
              test_block[lane * 1024 + b] = static_cast<std::uint8_t>(src[b] ^ (lane * 0x37u + b));
          }
          std::uint64_t vec_out[8]{};
          fast_final_state_8x(test_block.data(), vec_out);
          for(unsigned lane = 0; lane < 8; ++lane) {
            std::span<const std::uint8_t, 1024> lane_span(test_block.data() + lane * 1024, 1024);
            if(vec_out[lane] != consensus::final_state_from_bytes(lane_span))
              throw std::runtime_error("AMD AVX2 8x BLAKE3 reducer disagreed with reference");
          }
        }
      } else {
        std::array<std::uint8_t, 8 * 1024> test_block{};
        for(std::size_t b = 0; b < test_block.size(); ++b)
          test_block[b] = static_cast<std::uint8_t>(b * 0x37u + 0x5au);
        std::span<const std::uint8_t, 1024> first_span(test_block.data(), 1024);
        if(fast_final_state_scalar(test_block.data()) != consensus::final_state_from_bytes(first_span))
          throw std::runtime_error("AMD fast BLAKE3 reducer disagreed with reference");
        if(use_avx2) {
          std::uint64_t vec_out[8]{};
          fast_final_state_8x(test_block.data(), vec_out);
          for(unsigned lane = 0; lane < 8; ++lane) {
            std::span<const std::uint8_t, 1024> lane_span(test_block.data() + lane * 1024, 1024);
            if(vec_out[lane] != consensus::final_state_from_bytes(lane_span))
              throw std::runtime_error("AMD AVX2 8x BLAKE3 reducer disagreed with reference");
          }
        }
      }
    }

    std::optional<std::uint32_t> winner_idx;
    if(engine_.screens_target_on_gpu()) {
      winner_idx = gpu.winner_index;
    } else {
      std::atomic<std::uint32_t> earliest_winner{count};
      const auto update_winner = [&](std::uint32_t idx) noexcept {
        auto cur = earliest_winner.load(std::memory_order_relaxed);
        while(idx < cur && !earliest_winner.compare_exchange_weak(
            cur, idx, std::memory_order_relaxed, std::memory_order_relaxed)) {}
      };
      const std::uint32_t groups = count / 8;
      const auto screen_groups = [&](unsigned tid, unsigned num_threads) {
        const std::uint32_t begin = (groups * tid) / num_threads;
        const std::uint32_t end = (groups * (tid + 1)) / num_threads;
        for(std::uint32_t g = begin; g < end; ++g) {
          const std::uint32_t base_i = g * 8;
          if(base_i >= earliest_winner.load(std::memory_order_relaxed)) break;
          std::uint64_t states[8];
          if(use_avx2) {
            fast_final_state_8x(gpu.final_states.data() + static_cast<std::size_t>(base_i) * 1024, states);
          } else {
            for(std::uint32_t lane = 0; lane < 8; ++lane)
              states[lane] = fast_final_state_scalar(gpu.final_states.data() + static_cast<std::size_t>(base_i + lane) * 1024);
          }
          for(std::uint32_t lane = 0; lane < 8; ++lane) {
            const auto pow = pom_pow_value_h3(states[lane], pph);
            if(little_endian_leq(pow, target.little_endian)) {
              update_winner(base_i + lane);
              break;
            }
          }
        }
      };
      if(groups >= 8) {
        if(!ws.pool) ws.pool = std::make_unique<ScreenPool>(3);
        ws.pool->run(screen_groups);
      } else if(groups > 0) {
        screen_groups(0, 1);
      }
      for(std::uint32_t i = groups * 8; i < count; ++i) {
        if(i >= earliest_winner.load(std::memory_order_relaxed)) break;
        const auto final_val = fast_final_state_scalar(gpu.final_states.data() + static_cast<std::size_t>(i) * 1024);
        const auto pow = pom_pow_value_h3(final_val, pph);
        if(little_endian_leq(pow, target.little_endian)) {
          update_winner(i);
          break;
        }
      }
      if(const auto w = earliest_winner.load(std::memory_order_relaxed); w < count)
        winner_idx = w;
    }

    record_profile();
    if(winner_idx){
      progress.nonce=ticket.first_nonce()+*winner_idx;progress.generation=job.generation;
      progress.screened=true;
      std::osyncstream(std::cout)<<"AMD_TARGET_SCREENED nonce="<<progress.nonce
        <<" generation="<<progress.generation<<" compact_bits="<<header.bits
        <<" rust=PENDING submit=NOT_SENT\n"<<std::flush;
    }
    // CPU target screening also takes time; it cannot certify a lease that
    // expired after GPU completion. Keep stale results out of completed rates.
    if(!ready())return nullptr;

    // Attest an actual GPU walk before trusting fast screening in this engine
    // session, even when the first template produces no winning candidate.
    const bool screened_winner=winner_idx.has_value();
    if(winner_idx || !startup_parity_done_) {
      const std::uint32_t win_i=winner_idx.value_or(0);
      const std::uint64_t win_nonce=ticket.first_nonce()+win_i;
      const std::uint64_t win_seed=seeds[win_i];

      // The first nonce was bound and checked before dispatch. Preserve that
      // immutable binding for read-only attestation after a lease expires.
      CandidateBinding win_binding=binding;
      if(!attesting||win_i!=0) {
        try {win_binding=bind_candidate(ticket.job(),win_nonce,header.timestamp,pph);}
        catch(...){if(!ticket.job().lease.valid())return nullptr;throw;}
      }
      candidate->binding_=win_binding;

      progress.advance("GATHER");
      // Re-run the winning nonce with diagnostics=true for official Merkle trace
      const auto win_input=gather_v4_model_tiles(win_seed,anchor.chunks/32,[&](std::uint64_t offset){
        if(!ready())throw Cancelled{};
        std::array<std::uint8_t,1024> tile{};model_.read_tile_into(offset,tile);return tile;
      });
      std::array<std::uint64_t,1> single_seed{win_seed};
      std::array<std::uint32_t,256> single_slots{};std::iota(single_slots.begin(),single_slots.end(),0);
      VulkanWalkBatch diag_gpu;
      progress.advance("DIAGNOSTICS");
      if(!engine_.run(anchor.chunks/32,single_seed,
          engine_.resident()?std::span<const std::uint8_t>{}:std::span<const std::uint8_t>(win_input.tiles),
          engine_.resident()?std::span<const std::uint32_t>{}:std::span<const std::uint32_t>(single_slots),true,diag_gpu,[&]{return !ready();}))return nullptr;
      if(!ready())return nullptr;
      if(diag_gpu.offsets!=win_input.offsets || diag_gpu.final_states.size()!=1024 ||
         diag_gpu.diagnostic_states.size()!=257*1024)
        throw std::runtime_error("AMD diagnostic witness dimensions or offsets differ");
      if(!engine_.resident() && !std::equal(diag_gpu.final_states.begin(),diag_gpu.final_states.end(),
                     gpu.final_states.begin()+win_i*1024))
        throw std::runtime_error("AMD screening and diagnostic final states differ");

      Walk walk;walk.offsets=std::move(diag_gpu.offsets);walk.states.resize(257);
      for(size_t i=0;i<257;++i)std::copy_n(diag_gpu.diagnostic_states.data()+i*1024,1024,walk.states[i].data());
      if(!std::equal(diag_gpu.final_states.begin(),diag_gpu.final_states.end(),walk.states.back().begin()))
        throw std::runtime_error("AMD final state and trace disagree");

      candidate->final_=consensus::final_state(walk);
      const auto pow=pom_pow_value_h3(candidate->final_,pph);
      candidate->target_met_=little_endian_leq(pow,target.little_endian);
      if(candidate->target_met_!=screened_winner)
        throw std::runtime_error("AMD diagnostic replay target differs from screening");

      progress.advance("MERKLE_BORSH");
      auto proof=prove_v4_from_walk(walk,header.pom_tier,win_input.tiles,[&](std::uint64_t off){
        if(!ready())throw Cancelled{};return merkle_.tile_path(off);
      });
      candidate->wire_=borsh_v4_container(proof,candidate->final_,pow);
      const auto digest=blake3_hash(ticket.job().fields.response_wire);
      const ProofArtifactContext context{win_seed,anchor.chunks,header.timestamp,win_nonce,header.daa_score,
        ticket.job().generation,candidate->final_,header.pom_tier,anchor.root,target.little_endian,pph,digest,pow};
      candidate->artifacts_=encode_proof_artifacts(context,walk,candidate->wire_.size());
      if(!ready())return nullptr;
      progress.advance("RUST_VERIFY");
      candidate->rust_summary_=verifier_.verify(candidate->artifacts_.context_prefix,candidate->artifacts_.trace,candidate->wire_).summary;
      const auto expected=screened_winner?"target_satisfied=true container=PASS":"target_satisfied=false container=TARGET_NOT_MET";
      if(candidate->rust_summary_.find(expected)==std::string::npos)
        throw std::runtime_error("Official Rust did not attest the AMD replay target");
      startup_parity_done_=true;
      if(attesting)std::osyncstream(std::cout)<<"AMD_ENGINE_PARITY=PASS rust=PASS read_only_attestation=true submission_requires_freshness=true\n"<<std::flush;
      if(!eligible())return nullptr;
      progress.advance("VERIFIED_FRESH");progress.returned=true;
      return candidate;
    }

    candidate->target_met_=false;
    return candidate;
  }catch(const Cancelled &){return nullptr;}
  catch(...){owner_.stop();throw;}
}
}
