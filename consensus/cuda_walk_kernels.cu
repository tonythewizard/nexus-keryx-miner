#include <cstddef>
#include <cstdint>

namespace {
constexpr std::size_t tile_bytes_cuda = 1024;
constexpr std::size_t steps_cuda = 256;
constexpr std::size_t tile_cache_max_bytes = 32ULL * 1024 * 1024;
constexpr std::uint64_t s0_salt_cuda = 0x03421325594C3C51ULL;
constexpr std::uint64_t first_salt_cuda = 0x6D1CCF96AC4D76F9ULL;
constexpr std::uint64_t step_salt_cuda = 0x89050E78D34609EFULL;

__device__ __forceinline__ std::uint64_t mix64_d(std::uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

__device__ __forceinline__ std::uint8_t rho8_d(std::int32_t acc,
                                                std::uint32_t tweak) {
  std::uint32_t z = static_cast<std::uint32_t>(acc) ^ tweak;
  z *= 0x9E3779B9U;
  z ^= z >> 16;
  z *= 0x85EBCA6BU;
  z ^= z >> 13;
  return static_cast<std::uint8_t>(z);
}

__device__ __forceinline__ std::uint64_t snippet_fold_d(const std::uint8_t *p) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    std::uint32_t word = 0;
    for (unsigned j = 0; j < 4; ++j)
      word |= static_cast<std::uint32_t>(p[i * 4 + j]) << (j * 8);
    value = mix64_d(value ^ word);
  }
  return value;
}

} // namespace

extern "C" __global__ void pom_v4_walk_kernel(const std::uint64_t *seeds,
                                   const std::uint8_t *tiles,
                                   const std::uint32_t *tile_slots,
                                   std::uint64_t ntiles,
                                   std::uint64_t *offsets,
                                   std::uint8_t *final_states,
                                   std::uint8_t *trace,
                                   bool diagnostics) {
  const unsigned tid = threadIdx.x;
  const unsigned nonce = blockIdx.x;
  __shared__ std::uint8_t state_a[tile_bytes_cuda];
  __shared__ std::uint8_t state_b[tile_bytes_cuda];
  __shared__ std::uint8_t tile[tile_bytes_cuda];
  std::uint8_t *src = state_a;
  std::uint8_t *dst = state_b;
  const std::uint64_t seed = seeds[nonce];

  if (tid < 32) {
    std::uint64_t h = mix64_d(seed ^ (s0_salt_cuda + tid));
    for (unsigned group = 0; group < 8; ++group) {
      h = mix64_d(h);
      const std::uint32_t word = static_cast<std::uint32_t>(h);
      const unsigned base = tid * 32 + group * 4;
      state_a[base] = static_cast<std::uint8_t>(word);
      state_a[base + 1] = static_cast<std::uint8_t>(word >> 8);
      state_a[base + 2] = static_cast<std::uint8_t>(word >> 16);
      state_a[base + 3] = static_cast<std::uint8_t>(word >> 24);
    }
  }
  __syncthreads();
  if (diagnostics)
    for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
      trace[(static_cast<std::size_t>(nonce) * (steps_cuda + 1)) * tile_bytes_cuda + i] = state_a[i];

  std::uint64_t offset = mix64_d(seed ^ first_salt_cuda) % ntiles;
  for (unsigned step = 1; step <= steps_cuda; ++step) {
    const std::size_t tile_base =
        static_cast<std::size_t>(tile_slots[static_cast<std::size_t>(nonce) * steps_cuda + (step - 1)]) * tile_bytes_cuda;
    for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
      tile[i] = tiles[tile_base + i];
    __syncthreads();

    if (tid == 0)
      offsets[static_cast<std::size_t>(nonce) * steps_cuda + step - 1] = offset;
    for (unsigned cell = tid; cell < tile_bytes_cuda; cell += blockDim.x) {
      const unsigned row = cell / 32;
      const unsigned col = cell % 32;
      std::int32_t acc = 0;
      for (unsigned k = 0; k < 32; ++k) {
        const int lhs_byte = src[row * 32 + k];
        const int rhs_byte = tile[col * 32 + k];
        const int lhs = lhs_byte < 128 ? lhs_byte : lhs_byte - 256;
        const int rhs = rhs_byte < 128 ? rhs_byte : rhs_byte - 256;
        acc += lhs * rhs;
      }
      const std::uint32_t tweak = step * 0x9E3779B9U + row * 0xC2B2AE35U + col * 0x85EBCA6BU;
      dst[cell] = rho8_d(acc, tweak);
    }
    __syncthreads();
    if (diagnostics)
      for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
        trace[(static_cast<std::size_t>(nonce) * (steps_cuda + 1) + step) * tile_bytes_cuda + i] = dst[i];
    if (step < steps_cuda && tid == 0)
      offset = mix64_d(seed ^ (static_cast<std::uint64_t>(step + 1) * step_salt_cuda) ^
                       snippet_fold_d(tile)) % ntiles;
    __syncthreads();
    auto *swap = src;
    src = dst;
    dst = swap;
  }
  for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
    final_states[static_cast<std::size_t>(nonce) * tile_bytes_cuda + i] = src[i];
}

extern "C" __global__ void cache_scatter_kernel(std::uint8_t *cache,
                                     const std::uint8_t *packed_tiles,
                                     const std::uint32_t *destination_slots,
                                     std::uint32_t tile_count) {
  const std::uint32_t tile = blockIdx.x;
  if (tile >= tile_count) return;
  const std::uint32_t destination = destination_slots[tile];
  for (unsigned i = threadIdx.x; i < tile_bytes_cuda; i += blockDim.x)
    cache[static_cast<std::size_t>(destination) * tile_bytes_cuda + i] =
        packed_tiles[static_cast<std::size_t>(tile) * tile_bytes_cuda + i];
}
