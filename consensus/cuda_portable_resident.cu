// Portable CUDA scalar resident walk, compiled JIT to Vulkan for AMD.
// Same canonical integer walk as cuda_walk_kernels.cu, with the full bounded
// tile pool resident. The GPU computes every data-dependent tile address.
// This is an offline development entry point, not production mining admission.
// No PTX/Tensor Core instructions and no handwritten GLSL algorithm.
#include <cstddef>
#include <cstdint>

namespace {
constexpr std::size_t tile_bytes_cuda = 1024;
constexpr std::size_t steps_cuda = 256;
// The Vulkan byte-storage lowering expands each LDS element to one dword.
// A 33-element row stride distributes column reads across the 32 LDS banks.
constexpr std::size_t tile_row_stride_cuda = 33;
constexpr std::size_t shared_offset_bytes_cuda = 8;
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

__device__ __forceinline__ std::uint32_t pack4_d(std::uint32_t b0,
                                                 std::uint32_t b1,
                                                 std::uint32_t b2,
                                                 std::uint32_t b3) {
  return (b0 & 255U) | ((b1 & 255U) << 8) | ((b2 & 255U) << 16) | (b3 << 24);
}

__device__ __forceinline__ int s8_d(int x) {
  return (x ^ 128) - 128;
}

__device__ __forceinline__ std::int32_t mac_d(std::int32_t acc,
                                              int lhs_byte,
                                              int rhs) {
  const int lhs = s8_d(lhs_byte);
  acc += lhs * rhs;
  return acc;
}

__device__ __forceinline__ std::int32_t dp4a_d(std::int32_t acc,
                                               std::uint32_t lhs_packed,
                                               std::uint32_t rhs_packed) {
  acc = mac_d(acc, static_cast<int>(lhs_packed & 255U),
              s8_d(static_cast<int>(rhs_packed & 255U)));
  acc = mac_d(acc, static_cast<int>((lhs_packed >> 8) & 255U),
              s8_d(static_cast<int>((rhs_packed >> 8) & 255U)));
  acc = mac_d(acc, static_cast<int>((lhs_packed >> 16) & 255U),
              s8_d(static_cast<int>((rhs_packed >> 16) & 255U)));
  acc = mac_d(acc, static_cast<int>((lhs_packed >> 24) & 255U),
              s8_d(static_cast<int>((rhs_packed >> 24) & 255U)));
  return acc;
}

__device__ __forceinline__ std::uint64_t mod_ntiles_d(std::uint64_t x,
                                                      std::uint64_t ntiles,
                                                      std::uint64_t inv_ntiles) {
  const std::uint64_t a_lo = x & 0xFFFFFFFFULL;
  const std::uint64_t a_hi = x >> 32;
  const std::uint64_t b_lo = inv_ntiles & 0xFFFFFFFFULL;
  const std::uint64_t b_hi = inv_ntiles >> 32;
  const std::uint64_t p0 = a_lo * b_lo;
  const std::uint64_t p1 = a_lo * b_hi;
  const std::uint64_t p2 = a_hi * b_lo;
  const std::uint64_t p3 = a_hi * b_hi;
  const std::uint64_t cy =
      ((p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL)) >> 32;
  const std::uint64_t q = p3 + (p1 >> 32) + (p2 >> 32) + cy;
  std::uint64_t r = x - q * ntiles;
  if (r >= ntiles)
    r -= ntiles;
  if (r >= ntiles)
    r -= ntiles;
  return r;
}

} // namespace

extern "C" __global__ void pom_v4_resident_walk_kernel(const std::uint64_t *seeds,
                                   const std::uint8_t *tiles,
                                   std::uint64_t ntiles,
                                   std::uint64_t *offsets,
                                   std::uint8_t *final_states,
                                   std::uint8_t *trace,
                                   bool diagnostics) {
  const unsigned tid = threadIdx.x;
  const unsigned nonce = blockIdx.x;
  __shared__ std::uint8_t state[2048];
  __shared__ std::uint8_t tile[1056];
  __shared__ std::uint8_t next_offset[shared_offset_bytes_cuda];
  unsigned src_base = 0;
  unsigned dst_base = 1024;
  const std::uint64_t seed = seeds[nonce];

  if (tid < 32) {
    std::uint64_t h = mix64_d(seed ^ (s0_salt_cuda + tid));
    for (unsigned group = 0; group < 8; ++group) {
      h = mix64_d(h);
      const std::uint32_t word = static_cast<std::uint32_t>(h);
      const unsigned base = tid * 32 + group * 4;
      state[base] = static_cast<std::uint8_t>(word);
      state[base + 1] = static_cast<std::uint8_t>(word >> 8);
      state[base + 2] = static_cast<std::uint8_t>(word >> 16);
      state[base + 3] = static_cast<std::uint8_t>(word >> 24);
    }
  }
  const std::uint64_t inv_ntiles =
      (tid == 0U) ? (0xFFFFFFFFFFFFFFFFULL / ntiles) : 0ULL;
  if (tid == 0) {
    const std::uint64_t init_off =
        mod_ntiles_d(mix64_d(seed ^ first_salt_cuda), ntiles, inv_ntiles);
    const std::uint32_t lo = static_cast<std::uint32_t>(init_off);
    const std::uint32_t hi = static_cast<std::uint32_t>(init_off >> 32);
    next_offset[0] = static_cast<std::uint8_t>(lo);
    next_offset[1] = static_cast<std::uint8_t>(lo >> 8);
    next_offset[2] = static_cast<std::uint8_t>(lo >> 16);
    next_offset[3] = static_cast<std::uint8_t>(lo >> 24);
    next_offset[4] = static_cast<std::uint8_t>(hi);
    next_offset[5] = static_cast<std::uint8_t>(hi >> 8);
    next_offset[6] = static_cast<std::uint8_t>(hi >> 16);
    next_offset[7] = static_cast<std::uint8_t>(hi >> 24);
  }
  __syncthreads();
  if (diagnostics)
    for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
      trace[(static_cast<std::size_t>(nonce) * (steps_cuda + 1)) * tile_bytes_cuda + i] = state[i];

  std::uint64_t offset =
      static_cast<std::uint64_t>(pack4_d(next_offset[0], next_offset[1], next_offset[2], next_offset[3])) |
      (static_cast<std::uint64_t>(pack4_d(next_offset[4], next_offset[5], next_offset[6], next_offset[7])) << 32);
  for (unsigned step = 1; step <= steps_cuda; ++step) {
    const std::size_t tile_base =
        static_cast<std::size_t>(offset) * tile_bytes_cuda;
    for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
      tile[(i / 32) * tile_row_stride_cuda + i % 32] = tiles[tile_base + i];
    __syncthreads();

    if (tid == 0)
      offsets[static_cast<std::size_t>(nonce) * steps_cuda + step - 1] = offset;
    // Tile addressing is uniform: compute once, then publish through the
    // matrix-completion barrier. snippet_fold uses the first 32 unpadded bytes.
    if (step < steps_cuda && tid == 0) {
      std::uint64_t fold = 0;
      for (unsigned i = 0; i < 32; i += 4) {
        const std::uint32_t word = pack4_d(tile[i], tile[i + 1], tile[i + 2], tile[i + 3]);
        fold = mix64_d(fold ^ word);
      }
      const std::uint64_t next_val = mod_ntiles_d(
          mix64_d(seed ^ (static_cast<std::uint64_t>(step + 1) * step_salt_cuda) ^ fold),
          ntiles, inv_ntiles);
      const std::uint32_t lo = static_cast<std::uint32_t>(next_val);
      const std::uint32_t hi = static_cast<std::uint32_t>(next_val >> 32);
      next_offset[0] = static_cast<std::uint8_t>(lo);
      next_offset[1] = static_cast<std::uint8_t>(lo >> 8);
      next_offset[2] = static_cast<std::uint8_t>(lo >> 16);
      next_offset[3] = static_cast<std::uint8_t>(lo >> 24);
      next_offset[4] = static_cast<std::uint8_t>(hi);
      next_offset[5] = static_cast<std::uint8_t>(hi >> 8);
      next_offset[6] = static_cast<std::uint8_t>(hi >> 16);
      next_offset[7] = static_cast<std::uint8_t>(hi >> 24);
    }

    for (unsigned cell = tid; cell < 256U; cell += blockDim.x) {
      const unsigned row = cell / 8U;
      const unsigned col = (cell % 8U) * 4U;
      const unsigned row_base = src_base + row * 32U;
      const unsigned col0_base = col * tile_row_stride_cuda;
      const unsigned col1_base = (col + 1U) * tile_row_stride_cuda;
      const unsigned col2_base = (col + 2U) * tile_row_stride_cuda;
      const unsigned col3_base = (col + 3U) * tile_row_stride_cuda;
      std::int32_t acc = 0;
      std::int32_t acc1 = 0;
      std::int32_t acc2 = 0;
      std::int32_t acc3 = 0;
      for (unsigned k = 0; k < 32U; k += 4U) {
        const std::uint32_t lhs =
            pack4_d(state[row_base + k], state[row_base + k + 1U],
                    state[row_base + k + 2U], state[row_base + k + 3U]);
        acc = dp4a_d(acc, lhs,
                     pack4_d(tile[col0_base + k], tile[col0_base + k + 1U],
                             tile[col0_base + k + 2U], tile[col0_base + k + 3U]));
        acc1 = dp4a_d(acc1, lhs,
                      pack4_d(tile[col1_base + k], tile[col1_base + k + 1U],
                              tile[col1_base + k + 2U], tile[col1_base + k + 3U]));
        acc2 = dp4a_d(acc2, lhs,
                      pack4_d(tile[col2_base + k], tile[col2_base + k + 1U],
                              tile[col2_base + k + 2U], tile[col2_base + k + 3U]));
        acc3 = dp4a_d(acc3, lhs,
                      pack4_d(tile[col3_base + k], tile[col3_base + k + 1U],
                              tile[col3_base + k + 2U], tile[col3_base + k + 3U]));
      }
      const std::uint32_t tweak =
          step * 0x9E3779B9U + row * 0xC2B2AE35U + col * 0x85EBCA6BU;
      const unsigned dst_idx = dst_base + cell * 4U;
      state[dst_idx] = rho8_d(acc, tweak);
      state[dst_idx + 1U] = rho8_d(acc1, tweak + 0x85EBCA6BU);
      state[dst_idx + 2U] = rho8_d(acc2, tweak + 2U * 0x85EBCA6BU);
      state[dst_idx + 3U] = rho8_d(acc3, tweak + 3U * 0x85EBCA6BU);
    }
    __syncthreads();
    if (diagnostics)
      for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
        trace[(static_cast<std::size_t>(nonce) * (steps_cuda + 1) + step) * tile_bytes_cuda + i] = state[dst_base + i];
    offset =
        static_cast<std::uint64_t>(pack4_d(next_offset[0], next_offset[1], next_offset[2], next_offset[3])) |
        (static_cast<std::uint64_t>(pack4_d(next_offset[4], next_offset[5], next_offset[6], next_offset[7])) << 32);
    const unsigned swap_base = src_base;
    src_base = dst_base;
    dst_base = swap_base;
  }
  for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
    final_states[static_cast<std::size_t>(nonce) * tile_bytes_cuda + i] = state[src_base + i];
}
