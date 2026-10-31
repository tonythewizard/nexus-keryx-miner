#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/pipeline.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <blake2.h>
#include <blake3.h>
#include <cstring>
#include <limits>
#include <stdexcept>
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && !defined(NEXUS_DISABLE_AVX2_WALK)
#include <immintrin.h>
#define NEXUS_HAS_AVX2_WALK 1
#endif

namespace keryx::nexus::consensus {
namespace {
constexpr std::uint64_t s0_salt = 0x03421325594C3C51ULL;
constexpr std::uint64_t first_salt = 0x6D1CCF96AC4D76F9ULL;
constexpr std::uint64_t step_salt = 0x89050E78D34609EFULL;
constexpr std::array<std::uint64_t, 4> v4_salt = {
    0x7D7BC84C8D18DE80ULL, 0xDE48EE16AE3F1541ULL, 0x3305F1952B30384AULL,
    0xF78C133968D388B7ULL};
constexpr std::array<std::uint64_t, 4> h3_salt = {
    0x7C99D381176D4EC4ULL, 0xC2E28E3E28118C36ULL, 0xD496CE1B129B76CAULL,
    0x47CF0979FA580BCEULL};
constexpr std::array<std::uint8_t, 32> h14_tag = {
    'K', 'E', 'R', 'Y', 'X', '-', 'H', '1', '4', '-', 'P',
    'R', 'I', 'V', 'A', 'T', 'E', '-', 'I', 'N', 'F', 'E',
    'R', 'E', 'N', 'C', 'E', '-', 'S', 'E', 'E', 'D'};
void append_u32(std::vector<std::uint8_t> &o, std::uint32_t v) {
  for (unsigned i = 0; i < 4; i++)
    o.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void append_u64(std::vector<std::uint8_t> &o, std::uint64_t v) {
  for (unsigned i = 0; i < 8; i++)
    o.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void append_hash(std::vector<std::uint8_t> &o, const Hash &h) {
  o.insert(o.end(), h.begin(), h.end());
}
std::uint64_t snippet_fold(std::span<const std::uint8_t, 32> s) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; i++) {
    std::uint32_t w = 0;
    for (unsigned j = 0; j < 4; j++)
      w |= std::uint32_t(s[i * 4 + j]) << (j * 8);
    v = mix64(v ^ w);
  }
  return v;
}
std::uint8_t rho8(std::int32_t acc, std::uint32_t tweak) {
  std::uint32_t z = std::bit_cast<std::uint32_t>(acc) ^ tweak;
  z *= 0x9E3779B9U;
  z ^= z >> 16;
  z *= 0x85EBCA6BU;
  z ^= z >> 13;
  return static_cast<std::uint8_t>(z);
}
std::uint32_t rho_tweak(std::uint32_t step, std::uint32_t row,
                        std::uint32_t col) {
  return step * 0x9E3779B9U + row * 0xC2B2AE35U + col * 0x85EBCA6BU;
}
// Both step implementations use signed int8 operands and exact int32 sums.
// The largest dot product magnitude is 32*128*128=524288: no saturation,
// narrowing or floating point is permitted before the existing rho8 mapping.
using V4Step = void (*)(const std::uint8_t*, std::uint8_t*, const std::uint8_t*, std::uint32_t);
void v4_step_scalar(const std::uint8_t *src, std::uint8_t *dst,
                    const std::uint8_t *tile, std::uint32_t step) {
  for (std::uint32_t r = 0; r < 32; ++r)
    for (std::uint32_t c = 0; c < 32; ++c) {
      std::int32_t acc = 0;
      for (std::uint32_t k = 0; k < 32; ++k)
        acc += static_cast<std::int32_t>(std::bit_cast<std::int8_t>(src[r * 32 + k])) *
               static_cast<std::int32_t>(std::bit_cast<std::int8_t>(tile[c * 32 + k]));
      dst[r * 32 + c] = rho8(acc, rho_tweak(step, r, c));
    }
}
#ifdef NEXUS_HAS_AVX2_WALK
__attribute__((target("avx2")))
void v4_step_avx2(const std::uint8_t *src, std::uint8_t *dst,
                  const std::uint8_t *tile, std::uint32_t step) {
  // Expand this tile once; every state row reuses the same signed columns.
  alignas(32) std::int16_t columns[32 * 32];
  for (unsigned c = 0; c < 32; ++c) {
    _mm256_store_si256(reinterpret_cast<__m256i*>(columns + c * 32),
        _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(tile + c * 32))));
    _mm256_store_si256(reinterpret_cast<__m256i*>(columns + c * 32 + 16),
        _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(tile + c * 32 + 16))));
  }
  for (unsigned r = 0; r < 32; ++r) {
    const auto a = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + r * 32)));
    const auto b = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(src + r * 32 + 16)));
    for (unsigned c = 0; c < 32; ++c) {
      const auto products = _mm256_add_epi32(
          _mm256_madd_epi16(a, _mm256_load_si256(reinterpret_cast<const __m256i*>(columns + c * 32))),
          _mm256_madd_epi16(b, _mm256_load_si256(reinterpret_cast<const __m256i*>(columns + c * 32 + 16))));
      auto sum = _mm_add_epi32(_mm256_castsi256_si128(products), _mm256_extracti128_si256(products, 1));
      sum = _mm_hadd_epi32(sum, sum);
      sum = _mm_hadd_epi32(sum, sum);
      dst[r * 32 + c] = rho8(_mm_cvtsi128_si32(sum), rho_tweak(step, r, c));
    }
  }
}
#endif
V4Step v4_step_dispatch() {
#ifdef NEXUS_HAS_AVX2_WALK
  // GCC/Clang's runtime probe includes operating-system AVX state support.
  static const auto step = __builtin_cpu_supports("avx2") ? v4_step_avx2 : v4_step_scalar;
  return step;
#else
  return v4_step_scalar;
#endif
}
std::uint64_t load_le64(const std::uint8_t *p) {
  std::uint64_t x = 0;
  for (unsigned i = 0; i < 8; i++)
    x |= std::uint64_t(p[i]) << (8 * i);
  return x;
}
void store_le64(std::uint8_t *p, std::uint64_t x) {
  for (unsigned i = 0; i < 8; i++)
    p[i] = static_cast<std::uint8_t>(x >> (8 * i));
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
void keccak_f(std::array<std::uint64_t, 25> &a) {
  for (auto rc : keccak_rc) {
    std::array<std::uint64_t, 5> c{}, d{};
    for (unsigned x = 0; x < 5; x++)
      c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
    for (unsigned x = 0; x < 5; x++)
      d[x] = c[(x + 4) % 5] ^ std::rotl(c[(x + 1) % 5], 1);
    for (unsigned y = 0; y < 5; y++)
      for (unsigned x = 0; x < 5; x++)
        a[x + 5 * y] ^= d[x];
    std::array<std::uint64_t, 25> b{};
    for (unsigned y = 0; y < 5; y++)
      for (unsigned x = 0; x < 5; x++)
        b[y + 5 * ((2 * x + 3 * y) % 5)] =
            std::rotl(a[x + 5 * y], static_cast<int>(keccak_rot[x + 5 * y]));
    for (unsigned y = 0; y < 5; y++)
      for (unsigned x = 0; x < 5; x++)
        a[x + 5 * y] =
            b[x + 5 * y] ^ ((~b[(x + 1) % 5 + 5 * y]) & b[(x + 2) % 5 + 5 * y]);
    a[0] ^= rc;
  }
}
std::uint64_t h10_impl(const Hash &pph, std::uint64_t timestamp,
                       std::uint64_t nonce) {
  constexpr std::array<std::uint64_t, 25> initial = {
      1242148031264380989ULL,  3008272977830772284ULL,  2188519011337848018ULL,
      1992179434288343456ULL,  8876506674959887717ULL,  5399642050693751366ULL,
      1745875063082670864ULL,  8605242046444978844ULL,  17936695144567157056ULL,
      3343109343542796272ULL,  1123092876221303306ULL,  4963925045340115282ULL,
      17037383077651887893ULL, 16629644495023626889ULL, 12833675776649114147ULL,
      3784524041015224902ULL,  1082795874807940378ULL,  13952716920571277634ULL,
      13411128033953605860ULL, 15060696040649351053ULL, 9928834659948351306ULL,
      5237849264682708699ULL,  12825353012139217522ULL, 6706187291358897596ULL,
      196324915476054915ULL};
  auto s = initial;
  for (unsigned i = 0; i < 4; i++)
    s[i] ^= load_le64(pph.data() + 8 * i);
  s[4] ^= timestamp;
  s[9] ^= nonce;
  keccak_f(s);
  return s[0];
}
Hash blake2_block(std::span<const std::uint8_t> bytes) {
  Hash out{};
  blake2b_state s{};
  constexpr char key[] = "BlockHash";
  if (blake2b_init_key(&s, 32, key, sizeof(key) - 1) != 0 ||
      blake2b_update(&s, bytes.data(), bytes.size()) != 0 ||
      blake2b_final(&s, out.data(), out.size()) != 0)
    throw std::runtime_error("libb2 BLAKE2b operation failed");
  return out;
}
std::vector<Hash> fold(std::vector<Hash> level) {
  std::vector<Hash> next;
  next.reserve((level.size() + 1) / 2);
  for (std::size_t i = 0; i < level.size(); i += 2)
    next.push_back(
        hash_pair(level[i], i + 1 < level.size() ? level[i + 1] : level[i]));
  return next;
}
Hash compute_job_fingerprint(const TemplateFields &fields) noexcept {
  Hash out{};
  blake3_hasher h;
  blake3_hasher_init(&h);
  std::array<std::uint8_t, 8> len_buf{};
  store_le64(len_buf.data(), fields.response_wire.size());
  blake3_hasher_update(&h, len_buf.data(), len_buf.size());
  if (!fields.response_wire.empty())
    blake3_hasher_update(&h, fields.response_wire.data(), fields.response_wire.size());
  store_le64(len_buf.data(), fields.block_wire.size());
  blake3_hasher_update(&h, len_buf.data(), len_buf.size());
  if (!fields.block_wire.empty())
    blake3_hasher_update(&h, fields.block_wire.data(), fields.block_wire.size());
  blake3_hasher_finalize(&h, out.data(), out.size());
  return out;
}
} // namespace
std::uint64_t mix64(std::uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}
SeedEra select_seed_era(std::uint64_t daa) {
  if (daa >= h14_activation_daa)
    return SeedEra::h14;
  if (daa >= h10_activation_daa)
    return SeedEra::h10;
  if (daa >= pom_v4_activation_daa)
    return SeedEra::v4;
  throw std::invalid_argument("DAA precedes PoM v4 activation");
}
TierAnchor official_tier_anchor(std::uint64_t daa, std::uint8_t tier) {
  if (daa < h6_activation_daa)
    throw std::invalid_argument(
        "this build contains only official H6/H14 tier anchors");
  if (tier >= 5)
    throw std::out_of_range("official PoM tier index outside 0..4");
  static const auto h6 = [] {
    std::array<TierAnchor, 5> a{};
    a[0].root = {0x2c, 0x49, 0x71, 0x64, 0xea, 0xf2, 0x00, 0x78,
                 0xad, 0xd2, 0x0e, 0x82, 0xae, 0x4e, 0x1b, 0x0f,
                 0xdb, 0x27, 0xd3, 0xfd, 0xd5, 0xea, 0xef, 0xc1,
                 0xc4, 0x8f, 0x20, 0x41, 0x11, 0xe1, 0x4e, 0x88};
    a[0].chunks = 203469888;
    a[1].root = {0x1b, 0xa8, 0xb8, 0xb1, 0x34, 0x41, 0x03, 0xfa,
                 0xa0, 0xa7, 0x47, 0x89, 0xd9, 0x39, 0xc3, 0x3c,
                 0x23, 0xba, 0x5c, 0x3c, 0x41, 0xbb, 0x1a, 0x89,
                 0x5a, 0xb6, 0xe8, 0xbf, 0xec, 0xb0, 0x78, 0x7d};
    a[1].chunks = 258040832;
    a[2].root = {0x8e, 0x4d, 0x5b, 0xe3, 0xaa, 0x7c, 0x3a, 0xb9,
                 0x35, 0x83, 0x5f, 0xf5, 0xe1, 0x9d, 0x7a, 0x3d,
                 0xfa, 0x11, 0x8a, 0xf3, 0x24, 0xd5, 0xba, 0x65,
                 0x16, 0x29, 0xd6, 0xed, 0x16, 0x1a, 0x1e, 0x37};
    a[2].chunks = 305318656;
    a[3].root = {0x85, 0x23, 0xf4, 0x14, 0x8d, 0x22, 0xc7, 0x71,
                 0x3b, 0xfc, 0x11, 0x32, 0xb4, 0xaf, 0x3d, 0x4b,
                 0x97, 0x61, 0xa2, 0x03, 0xfb, 0x33, 0xf1, 0x8e,
                 0xe7, 0x55, 0x67, 0xbd, 0xee, 0x51, 0x2b, 0x0a};
    a[3].chunks = 516762688;
    a[4].root = {0x95, 0x74, 0x71, 0x0f, 0xfa, 0xb6, 0x78, 0xf0,
                 0x68, 0xb4, 0xe6, 0x5a, 0xbe, 0x72, 0x40, 0x86,
                 0x2d, 0xa1, 0x5b, 0xb1, 0x6e, 0xa8, 0x2f, 0xd1,
                 0x62, 0xa9, 0x35, 0x1a, 0x10, 0x51, 0x99, 0x59};
    a[4].chunks = 927994064;
    return a;
  }();
  static const auto h14 = [] {
    auto a = h6;
    a[3].root = {0x40, 0x6c, 0x19, 0x56, 0xf9, 0xf5, 0xdd, 0x13,
                 0x4d, 0x34, 0x61, 0xc6, 0x19, 0x11, 0x32, 0xa3,
                 0xb1, 0x57, 0x2c, 0xc1, 0x6f, 0x39, 0x5a, 0x2b,
                 0xc2, 0xf1, 0xc6, 0x69, 0xfa, 0xe3, 0x74, 0xa1};
    a[3].chunks = 524991232;
    return a;
  }();
  return (daa >= h14_activation_daa ? h14 : h6)[tier];
}
CandidateBinding bind_candidate(const TemplateJob &job, std::uint64_t nonce,
                                std::uint64_t timestamp, const Hash &pph) {
  if (!job.lease.valid() || job.lease.generation() != job.generation)
    throw std::invalid_argument(
        "cannot bind a candidate to an inactive template lease");
  CandidateBinding c;
  c.generation = job.generation;
  c.nonce = nonce;
  c.timestamp = timestamp;
  c.pre_pow_hash = pph;
  c.job_fingerprint = compute_job_fingerprint(job.fields);
  c.lease = job.lease;
  return c;
}
bool candidate_matches_job(const CandidateBinding &c,
                           const TemplateJob &job) noexcept {
  return candidate_binding_status(c, job) == CandidateBindingStatus::Valid;
}
CandidateBindingStatus candidate_binding_status(const CandidateBinding &c,
                                                const TemplateJob &job) noexcept {
  if (!c.lease.valid() || !job.lease.valid())
    return CandidateBindingStatus::StaleLease;
  if (c.generation != job.generation ||
      c.lease.generation() != job.lease.generation())
    return CandidateBindingStatus::IntegrityMismatch;
  return compute_job_fingerprint(job.fields) == c.job_fingerprint
      ? CandidateBindingStatus::Valid
      : CandidateBindingStatus::IntegrityMismatch;
}

std::array<std::uint64_t,25> seed_sponge_state(const Hash &original_pph, std::uint64_t timestamp, std::uint64_t daa) {
  if (daa < h10_activation_daa) throw std::invalid_argument("resident solver requires the H10/H14 seed era");
  auto pph = original_pph;
  if (daa >= h14_activation_daa)
    for (std::size_t i=0;i<pph.size();++i) pph[i] ^= h14_tag[i];
  constexpr std::array<std::uint64_t, 25> initial = {
      1242148031264380989ULL,  3008272977830772284ULL,  2188519011337848018ULL,
      1992179434288343456ULL,  8876506674959887717ULL,  5399642050693751366ULL,
      1745875063082670864ULL,  8605242046444978844ULL,  17936695144567157056ULL,
      3343109343542796272ULL,  1123092876221303306ULL,  4963925045340115282ULL,
      17037383077651887893ULL, 16629644495023626889ULL, 12833675776649114147ULL,
      3784524041015224902ULL,  1082795874807940378ULL,  13952716920571277634ULL,
      13411128033953605860ULL, 15060696040649351053ULL, 9928834659948351306ULL,
      5237849264682708699ULL,  12825353012139217522ULL, 6706187291358897596ULL,
      196324915476054915ULL};
  auto s = initial;
  for (unsigned i = 0; i < 4; i++)
    s[i] ^= load_le64(pph.data() + 8 * i);
  s[4] ^= timestamp;
  return s;
}
std::uint64_t seed_h10(const Hash &p, std::uint64_t t, std::uint64_t n) {
  return h10_impl(p, t, n);
}
std::uint64_t seed_h14(const Hash &p, std::uint64_t t, std::uint64_t n) {
  Hash tagged = p;
  for (std::size_t i = 0; i < tagged.size(); i++)
    tagged[i] ^= h14_tag[i];
  return h10_impl(tagged, t, n);
}
std::uint64_t block_seed(const Hash &p, std::uint64_t t, std::uint64_t n,
                         std::uint64_t daa) {
  switch (select_seed_era(daa)) {
  case SeedEra::h14:
    return seed_h14(p, t, n);
  case SeedEra::h10:
    return seed_h10(p, t, n);
  case SeedEra::v4: {
    std::uint64_t s = mix64(n ^ 0x4B65727978531ULL);
    s = mix64(s ^ t);
    for (unsigned i = 0; i < 4; i++)
      s = mix64(s ^ (load_le64(p.data() + 8 * i) ^ v4_salt[i]));
    return s;
  }
  }
  throw std::logic_error("unreachable seed era");
}
CompactTarget decode_compact_target(std::uint32_t bits) {
  const auto exponent = bits >> 24;
  std::uint32_t mant = bits & 0xFFFFFFU;
  if (exponent <= 3)
    mant >>= 8U * (3U - exponent);
  if (mant > 0x7FFFFFU)
    return {{}, true}; /* Match the official fixed-width Uint256 shift operator,
                          including its modulo-width shift semantics. */
  const unsigned shift = exponent <= 3 ? 0U : (8U * (exponent - 3U)) % 256U;
  CompactTarget out{};
  for (unsigned i = 0; i < 3; i++) {
    const unsigned bit = shift + i * 8U;
    if (bit < 256)
      out.little_endian[bit / 8] |= static_cast<std::uint8_t>(mant >> (i * 8U));
  }
  out.zero = std::all_of(out.little_endian.begin(), out.little_endian.end(),
                         [](auto b) { return b == 0; });
  return out;
}
bool little_endian_leq(const Hash &a, const Hash &b) noexcept {
  for (int i = 31; i >= 0; i--) {
    if (a[static_cast<std::size_t>(i)] < b[static_cast<std::size_t>(i)])
      return true;
    if (a[static_cast<std::size_t>(i)] > b[static_cast<std::size_t>(i)])
      return false;
  }
  return true;
}
Hash blake3_hash(std::span<const std::uint8_t> b) {
  Hash out{};
  blake3_hasher h;
  blake3_hasher_init(&h);
  blake3_hasher_update(&h, b.data(), b.size());
  blake3_hasher_finalize(&h, out.data(), out.size());
  return out;
}
Hash hash_pair(const Hash &a, const Hash &b) {
  std::array<std::uint8_t, 64> bytes{};
  std::copy(a.begin(), a.end(), bytes.begin());
  std::copy(b.begin(), b.end(), bytes.begin() + 32);
  return blake3_hash(bytes);
}
Hash merkle_root(std::span<const std::uint8_t> blob) {
  if (blob.empty() || blob.size() % 32)
    throw std::invalid_argument(
        "canonical blob must contain complete 32-byte chunks");
  std::vector<Hash> level;
  level.reserve(blob.size() / 32);
  for (std::size_t i = 0; i < blob.size(); i += 32)
    level.push_back(blake3_hash(blob.subspan(i, 32)));
  while (level.size() > 1)
    level = fold(std::move(level));
  return level[0];
}
std::vector<std::uint8_t> make_reference_test_blob() {
  constexpr std::size_t tiles = 64, tail_chunks = 3;
  std::vector<std::uint8_t> blob(tiles * tile_bytes + tail_chunks * 32);
  std::uint64_t h = 0xDEADBEEFULL;
  for (auto &b : blob) {
    h = mix64(h);
    b = static_cast<std::uint8_t>(h);
  }
  return blob;
}
std::array<std::uint8_t, tile_bytes> v4_initial_state_bytes(std::uint64_t seed) {
  std::array<std::uint8_t, tile_bytes> s{};
  for (std::size_t r = 0; r < 32; r++) {
    std::uint64_t h = mix64(seed ^ (s0_salt + static_cast<std::uint64_t>(r)));
    for (std::size_t k = 0; k < 8; k++) {
      h = mix64(h);
      const auto v = static_cast<std::uint32_t>(h);
      for (unsigned i = 0; i < 4; i++)
        s[r * 32 + k * 4 + i] = static_cast<std::uint8_t>(v >> (i * 8));
    }
  }
  return s;
}
Hash v4_initial_state_hash(std::uint64_t seed) { return blake3_hash(v4_initial_state_bytes(seed)); }
void v4_transition_into(std::array<std::uint8_t, tile_bytes> &out,
                        const std::array<std::uint8_t, tile_bytes> &state,
                        const std::array<std::uint8_t, tile_bytes> &tile, std::uint32_t step) {
  if (&out == &state || &out == &tile || step == 0 || step > walk_steps)
    throw std::invalid_argument("invalid v4/v5 transition buffers or step");
  v4_step_dispatch()(state.data(), out.data(), tile.data(), step);
}
Walk walk_v4(std::uint64_t seed, std::span<const std::uint8_t> blob) {
  if (blob.size() % 32)
    throw std::invalid_argument("blob length is not chunk aligned");
  const std::uint64_t ntiles = blob.size() / 1024;
  if (ntiles == 0)
    throw std::invalid_argument("blob has no complete tile");
  Walk out;
  out.states.resize(257);
  out.offsets.reserve(256);
  out.states[0] = v4_initial_state_bytes(seed);
  const auto step_function = v4_step_scalar;
  std::uint64_t offset = mix64(seed ^ first_salt) % ntiles;
  for (std::uint32_t step = 1; step <= 256; step++) {
    out.offsets.push_back(offset);
    const auto *tile = blob.data() + offset * 1024;
    const auto &src = out.states[step - 1];
    auto &dst = out.states[step];
    step_function(src.data(), dst.data(), tile, step);
    if (step < 256) {
      std::span<const std::uint8_t, 32> snippet(tile, 32);
      offset = mix64(seed ^ ((std::uint64_t(step + 1) * step_salt)) ^
                     snippet_fold(snippet)) %
               ntiles;
    }
  }
  return out;
}
ModelProofBuild prove_v4_from_model(std::uint64_t seed,std::uint8_t tier,std::uint64_t ntiles,const ModelTileReader&read_tile,const ModelPathReader&read_path){
  if(tier>=5)throw std::out_of_range("mainnet PoM tier index outside 0..4");
  if(ntiles==0||!read_tile||!read_path)throw std::invalid_argument("invalid model proof source");
  ModelProofBuild built;auto &w=built.walk;w.states.resize(257);w.offsets.reserve(256);
  w.states[0]=v4_initial_state_bytes(seed);
  const auto step_function=v4_step_dispatch();
  std::uint64_t offset=mix64(seed^first_salt)%ntiles;std::vector<std::array<std::uint8_t,tile_bytes>> tiles;tiles.reserve(256);
  for(std::uint32_t step=1;step<=256;step++){
    auto tile=read_tile(offset);w.offsets.push_back(offset);
    const auto &src=w.states[step-1];auto &dst=w.states[step];
    step_function(src.data(),dst.data(),tile.data(),step);
    if(step<256){
      std::span<const std::uint8_t,32> snippet(tile.data(),32);
      offset=mix64(seed^(static_cast<std::uint64_t>(step+1)*step_salt)^snippet_fold(snippet))%ntiles;
    }
    tiles.push_back(tile);
  }
  built.proof.tier=tier;built.proof.tiles=std::move(tiles);built.proof.paths.reserve(256);
  for(const auto off:w.offsets)built.proof.paths.push_back(read_path(off));
  return built;
}
ModelWalkInput gather_v4_model_tiles(std::uint64_t seed, std::uint64_t ntiles,
                                     const ModelTileReader &read_tile) {
  if (ntiles == 0 || !read_tile)
    throw std::invalid_argument("invalid model tile source");
  ModelWalkInput input;
  input.offsets.reserve(walk_steps);
  input.tiles.reserve(walk_steps * tile_bytes);
  auto offset = mix64(seed ^ first_salt) % ntiles;
  for (std::uint32_t step = 1; step <= walk_steps; ++step) {
    auto tile = read_tile(offset);
    if (tile.size() != tile_bytes)
      throw std::runtime_error("model tile reader returned an invalid tile");
    input.offsets.push_back(offset);
    std::array<std::uint8_t, 32> snippet{};
    std::copy_n(tile.data(), snippet.size(), snippet.data());
    input.tiles.insert(input.tiles.end(), tile.begin(), tile.end());
    if (step < walk_steps)
      offset = mix64(seed ^ (std::uint64_t(step + 1) * step_salt) ^
                     snippet_fold(snippet)) % ntiles;
  }
  return input;
}

Walk walk_v4_from_tile_sequence(std::uint64_t seed, std::uint64_t ntiles,
                                std::span<const std::uint8_t> tiles) {
  if (ntiles == 0 || tiles.size() != walk_steps * tile_bytes)
    throw std::invalid_argument("invalid ordered PoM v4 tile sequence");
  Walk out;
  out.states.resize(walk_steps + 1);
  out.offsets.reserve(walk_steps);
  out.states[0] = v4_initial_state_bytes(seed);
  const auto step_function = v4_step_dispatch();
  auto offset = mix64(seed ^ first_salt) % ntiles;
  for (std::uint32_t step = 1; step <= walk_steps; ++step) {
    out.offsets.push_back(offset);
    const auto *tile = tiles.data() + (step - 1) * tile_bytes;
    const auto &src = out.states[step - 1];
    auto &dst = out.states[step];
    step_function(src.data(), dst.data(), tile, step);
    if (step < walk_steps) {
      std::span<const std::uint8_t, 32> snippet(tile, 32);
      offset = mix64(seed ^ (std::uint64_t(step + 1) * step_salt) ^
                     snippet_fold(snippet)) % ntiles;
    }
  }
  return out;
}

V4Proof prove_v4_from_walk(const Walk &walk, std::uint8_t tier,
                           std::span<const std::uint8_t> tiles,
                           const ModelPathReader &read_path) {
  if (tier >= 5 || walk.states.size() != walk_steps + 1 ||
      walk.offsets.size() != walk_steps ||
      tiles.size() != walk_steps * tile_bytes || !read_path)
    throw std::invalid_argument("invalid walk input for v4 proof construction");
  V4Proof proof;
  proof.tier = tier;
  proof.tiles.reserve(walk_steps);
  proof.paths.reserve(walk_steps);
  for (std::size_t i = 0; i < walk_steps; ++i) {
    std::array<std::uint8_t, tile_bytes> tile{};
    std::copy_n(tiles.data() + i * tile_bytes, tile_bytes, tile.data());
    proof.tiles.push_back(tile);
    proof.paths.push_back(read_path(walk.offsets[i]));
  }
  return proof;
}
std::uint64_t final_state_from_bytes(std::span<const std::uint8_t, 1024> bytes) {
  std::array<Hash, 32> level{};
  for (std::size_t r = 0; r < 32; r++)
    level[r] = blake3_hash(
        std::span<const std::uint8_t>(bytes.data() + r * 32, 32));
  for (std::size_t width = 32; width > 1; width >>= 1) {
    for (std::size_t i = 0; i < width; i += 2)
      level[i / 2] = hash_pair(level[i], level[i + 1]);
  }
  return load_le64(level[0].data());
}
std::uint64_t final_state(const Walk &w) {
  if (w.states.size() != 257)
    throw std::invalid_argument("walk must expose 257 states");
  return final_state_from_bytes(std::span<const std::uint8_t, 1024>(w.states.back().data(), 1024));
}
V4Proof prove_v4(std::uint64_t seed, std::uint8_t tier,
                 std::span<const std::uint8_t> blob) {
  if (tier >= 5)
    throw std::out_of_range("mainnet PoM tier index outside 0..4");
  if (blob.size() % 32 || blob.size() < 1024)
    throw std::invalid_argument("invalid canonical blob for v4 proof");
  auto walk = walk_v4(seed, blob);
  std::vector<Hash> all;
  for (std::size_t i = 0; i < blob.size(); i += 32)
    all.push_back(blake3_hash(blob.subspan(i, 32)));
  std::vector<std::vector<Hash>> levels;
  levels.push_back(all);
  while (levels.back().size() > 1)
    levels.push_back(fold(levels.back()));
  V4Proof p;
  p.tier = tier;
  p.tiles.reserve(256);
  p.paths.reserve(256);
  for (auto offset : walk.offsets) {
    std::array<std::uint8_t, 1024> tile{};
    std::copy_n(blob.data() + offset * 1024, 1024, tile.data());
    p.tiles.push_back(tile);
    std::uint64_t idx = offset * 32;
    std::vector<Hash> path;
    for (std::size_t level = 0; level + 1 < levels.size(); level++) {
      if (level >= 5) {
        const auto sibling = idx ^ 1;
        path.push_back(sibling < levels[level].size()
                           ? levels[level][static_cast<std::size_t>(sibling)]
                           : levels[level][static_cast<std::size_t>(idx)]);
      }
      idx >>= 1;
    }
    p.paths.push_back(std::move(path));
  }
  return p;
}
Hash pom_pow_value_h3(std::uint64_t state, const Hash &pph) {
  std::array<std::uint64_t, 4> w{};
  for (unsigned i = 0; i < 4; i++)
    w[i] = load_le64(pph.data() + i * 8) ^ h3_salt[i];
  std::array<std::uint64_t, 4> o{};
  o[0] = mix64(state ^ w[0] ^ 0x9E3779B97F4A7C15ULL);
  o[1] = mix64(o[0] ^ w[1] ^ 0xC2B2AE3D27D4EB4FULL);
  o[2] = mix64(o[1] ^ w[2] ^ 0x165667B19E3779F9ULL);
  o[3] = mix64(o[2] ^ w[3] ^ 0xD6E8FEB86659FD93ULL);
  Hash out{};
  for (unsigned i = 0; i < 4; i++)
    store_le64(out.data() + 8 * i, o[i]);
  return out;
}
std::vector<std::uint8_t>
borsh_v4_container(const V4Proof &p, std::uint64_t state, const Hash &pow) {
  if (p.tier >= 5)
    throw std::out_of_range("mainnet PoM tier index outside 0..4");
  if (p.tiles.size() != 256 || p.paths.size() != 256)
    throw std::invalid_argument(
        "v4 proof must contain exactly 256 tiles and paths");
  // Fixed Borsh header and vector prefixes, then each path's exact payload.
  // The official model's witness exceeds the old 400000-byte reservation:
  // calculate once to avoid reallocating/copying a partial winning proof.
  std::size_t wire_bytes = 97 + 256 * (4 + tile_bytes) + 256 * 4;
  for (const auto &path : p.paths) {
    if (path.size() > std::numeric_limits<std::uint32_t>::max())
      throw std::length_error("v4 Merkle path exceeds Borsh u32 length");
    if (path.size() > (std::numeric_limits<std::size_t>::max() - wire_bytes) / 32)
      throw std::length_error("v4 proof wire size overflow");
    wire_bytes += path.size() * 32;
  }
  std::vector<std::uint8_t> o;
  o.reserve(wire_bytes);
  o.push_back(p.tier);
  o.insert(o.end(), 32, 0);
  append_hash(o, pow);
  append_u64(o, state);
  append_u32(o, 0);
  append_u32(o, 0);
  append_u32(o, 0);
  o.push_back(0);
  o.push_back(0);
  o.push_back(1);
  o.push_back(p.tier);
  append_u32(o, 256);
  for (const auto &t : p.tiles) {
    append_u32(o, 1024);
    o.insert(o.end(), t.begin(), t.end());
  }
  append_u32(o, 256);
  for (const auto &path : p.paths) {
    append_u32(o, static_cast<std::uint32_t>(path.size()));
    for (const auto &h : path)
      append_hash(o, h);
  }
  return o;
}
Hash hash_header_override(const Header &h, std::uint64_t nonce,
                          std::uint64_t timestamp, bool h3, bool h6, bool task) {
  std::vector<std::uint8_t> data;
  data.reserve(6000);
  auto push16 = [&](std::uint16_t x) {
    data.push_back(static_cast<std::uint8_t>(x));
    data.push_back(static_cast<std::uint8_t>(x >> 8));
  };
  push16(h.version);
  append_u64(data, h.parents_by_level.size());
  for (const auto &level : h.parents_by_level) {
    append_u64(data, level.size());
    for (const auto &parent : level)
      append_hash(data, parent);
  }
  append_hash(data, h.hash_merkle_root);
  append_hash(data, h.accepted_id_merkle_root);
  append_hash(data, h.utxo_commitment);
  append_u64(data, timestamp);
  append_u32(data, h.bits);
  append_u64(data, nonce);
  append_u64(data, h.daa_score);
  append_u64(data, h.blue_score);
  auto first =
      std::find_if(h.blue_work_big_endian.begin(), h.blue_work_big_endian.end(),
                   [](auto b) { return b != 0; });
  auto n = static_cast<std::size_t>(h.blue_work_big_endian.end() - first);
  if (n > 24)
    throw std::invalid_argument("blue work exceeds Uint192 encoding");
  append_u64(data, n);
  data.insert(data.end(), first, h.blue_work_big_endian.end());
  append_hash(data, h.pruning_point);
  if (task)
    append_hash(data, h.task_root);
  if (h3)
    append_u64(data, h.pom_final_state);
  if (h6) {
    append_hash(data, h.service_state_hash);
    data.push_back(h.pom_tier);
  }
  return blake2_block(data);
}
Hash pre_pow_hash(const Header &h) {
  return hash_header_override(h, 0, 0, false, false,
                              h.daa_score >= block_task_activation_daa);
}
} // namespace keryx::nexus::consensus
