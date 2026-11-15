// Adapted from local keryx-miner-official cuda/pom_mine.cu, Apache-2.0 OR MIT.
// Source SHA256 ae1201c2b7bab390a89bc0ca75a6b6c5404a800acc50558b3f84f26bbf3a5659.
// Independent Rust parity is required; this is not provenance of the packaged stock binary.
#include <cstdint>
#include "resident_keccak.cuh"
__device__ __forceinline__ unsigned long long mix64(unsigned long long x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

__device__ __forceinline__ unsigned long long pom_seed_fold(
    unsigned long long nonce, unsigned long long time_,
    unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3) {
    unsigned long long s = mix64(nonce ^ 0x4B65727978531ULL);
    s = mix64(s ^ time_);
    s = mix64(s ^ p0); s = mix64(s ^ p1); s = mix64(s ^ p2); s = mix64(s ^ p3);
    return s;
}

__device__ __forceinline__ void pom_pow_fold(
    unsigned long long fin, unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3,
    unsigned long long out[4]) {
    out[0] = mix64(fin ^ p0 ^ 0x9E3779B97F4A7C15ULL);
    out[1] = mix64(out[0] ^ p1 ^ 0xC2B2AE3D27D4EB4FULL);
    out[2] = mix64(out[1] ^ p2 ^ 0x165667B19E3779F9ULL);
    out[3] = mix64(out[2] ^ p3 ^ 0xD6E8FEB86659FD93ULL);
}

__device__ __forceinline__ bool pom_le_leq(const unsigned long long a[4],
                                           unsigned long long b0, unsigned long long b1,
                                           unsigned long long b2, unsigned long long b3) {
    if (a[3] != b3) return a[3] < b3;
    if (a[2] != b2) return a[2] < b2;
    if (a[1] != b1) return a[1] < b1;
    return a[0] <= b0;
}
// --- blake3 (single-chunk path: inputs <= 1024 B, counter 0) ---

__device__ static const unsigned int B3_IV[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

__device__ static const unsigned char B3_SCHED[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};

#define B3_CHUNK_START 1u
#define B3_CHUNK_END 2u
#define B3_ROOT 8u

__device__ __forceinline__ unsigned int b3_rotr(unsigned int x, int n) {
    return __funnelshift_r(x, x, n);
}

__device__ __forceinline__ void b3_g(unsigned int* v, int a, int b, int c, int d,
                                     unsigned int mx, unsigned int my) {
    v[a] += v[b] + mx; v[d] = b3_rotr(v[d] ^ v[a], 16);
    v[c] += v[d];      v[b] = b3_rotr(v[b] ^ v[c], 12);
    v[a] += v[b] + my; v[d] = b3_rotr(v[d] ^ v[a], 8);
    v[c] += v[d];      v[b] = b3_rotr(v[b] ^ v[c], 7);
}

__device__ __forceinline__ void b3_compress(unsigned int cv[8], const unsigned int m[16],
                                            unsigned int block_len, unsigned int flags) {
    unsigned int v[16];
    #pragma unroll
    for (int i = 0; i < 8; i++) v[i] = cv[i];
    v[8] = B3_IV[0]; v[9] = B3_IV[1]; v[10] = B3_IV[2]; v[11] = B3_IV[3];
    v[12] = 0; v[13] = 0; v[14] = block_len; v[15] = flags;
    #pragma unroll
    for (int r = 0; r < 7; r++) {
        const unsigned char* s = B3_SCHED[r];
        b3_g(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        b3_g(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        b3_g(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        b3_g(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        b3_g(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        b3_g(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        b3_g(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        b3_g(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    #pragma unroll
    for (int i = 0; i < 8; i++) cv[i] = v[i] ^ v[i + 8];
}

// blake3 of a 64 B concat (two child hashes): single block.
__device__ __forceinline__ void b3_hash_pair(const unsigned int m[16], unsigned int out[8]) {
    unsigned int cv[8];
    #pragma unroll
    for (int i = 0; i < 8; i++) cv[i] = B3_IV[i];
    b3_compress(cv, m, 64, B3_CHUNK_START | B3_CHUNK_END | B3_ROOT);
    #pragma unroll
    for (int i = 0; i < 8; i++) out[i] = cv[i];
}

// --- v3 walk primitives ---

__device__ __forceinline__ unsigned int v3_rho8(int acc, unsigned int tweak) {
    unsigned int z = (unsigned int)acc ^ tweak;
    z *= 0x9E3779B9u; z ^= z >> 16;
    z *= 0x85EBCA6Bu; z ^= z >> 13;
    return z & 0xffu;
}

#define V4_D 32
#define V4_D4 (V4_D / 4)          // 8 uints per row/column
#define V4_TILE_WORDS ((V4_D * V4_D) / 4) // Tile is 1024 bytes, stored as uint32 words.
#define V4_TILE_BYTES 1024
#define V4_TILE_CHUNKS 32
static_assert(V4_D == 32, "PoM v4 requires D=32");

#define V4_S0_ROW_SALT       0x03421325594C3C51ULL
#define V4_OFFSET_FIRST_SALT 0x6D1CCF96AC4D76F9ULL
#define V4_OFFSET_STEP_SALT  0x89050E78D34609EFULL
// blake3 of a 32-byte state row (single partial block).
__device__ __forceinline__ void b3_hash_row32(const unsigned int row[8], unsigned int out[8]) {
    unsigned int cv[8];
    #pragma unroll
    for (int i = 0; i < 8; i++) cv[i] = B3_IV[i];
    unsigned int m[16];
    #pragma unroll
    for (int i = 0; i < 8; i++) m[i] = row[i];
    #pragma unroll
    for (int i = 8; i < 16; i++) m[i] = 0u;
    b3_compress(cv, m, 32, B3_CHUNK_START | B3_CHUNK_END | B3_ROOT);
    #pragma unroll
    for (int i = 0; i < 8; i++) out[i] = cv[i];
}

__device__ __forceinline__ void v4_cp_async16(void* smem_dst, const void* gmem_src) {
    unsigned long long sdst = __cvta_generic_to_shared(smem_dst);
    asm volatile("cp.async.cg.shared.global [%0], [%1], 16;" :: "l"(sdst), "l"(gmem_src));
}

// One v4 step on tensor cores. s_state = 256-word shared state (row r = words r*8..r*8+7,
// same packed layout as the host); s_tile = the 1 KB column-major tile.
__device__ __forceinline__ void v4_imma_step(
    unsigned int* s_state, const unsigned int* s_tile, unsigned int step, unsigned int x) {
    const unsigned int gid = x >> 2, tig = x & 3u;
    unsigned int a[2][4];
    #pragma unroll
    for (unsigned int g = 0; g < 2; g++) {
        const unsigned int r0 = g * 16u + gid, r1 = r0 + 8u;
        a[g][0] = s_state[r0 * 8u + tig];
        a[g][1] = s_state[r1 * 8u + tig];
        a[g][2] = s_state[r0 * 8u + tig + 4u];
        a[g][3] = s_state[r1 * 8u + tig + 4u];
    }
    __syncwarp();   // every lane read the old state before anyone overwrites it
    unsigned short* s_state16 = (unsigned short*)s_state;
    const unsigned int step_base = step * 0x9E3779B9u;
    #pragma unroll
    for (unsigned int cg = 0; cg < 4; cg++) {
        const unsigned int cb = (cg * 8u + gid) * 8u;
        const unsigned int b0 = s_tile[cb + tig];
        const unsigned int b1 = s_tile[cb + tig + 4u];
        #pragma unroll
        for (unsigned int g = 0; g < 2; g++) {
            int c0 = 0, c1 = 0, c2 = 0, c3 = 0;
            asm volatile(
                "mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 "
                "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
                : "+r"(c0), "+r"(c1), "+r"(c2), "+r"(c3)
                : "r"(a[g][0]), "r"(a[g][1]), "r"(a[g][2]), "r"(a[g][3]), "r"(b0), "r"(b1));
            const unsigned int r0 = g * 16u + gid, r1 = r0 + 8u;
            const unsigned int j0 = cg * 8u + tig * 2u;
            const unsigned int tw0 = step_base + r0 * 0xC2B2AE35u + j0 * 0x85EBCA6Bu;
            const unsigned int tw1 = step_base + r1 * 0xC2B2AE35u + j0 * 0x85EBCA6Bu;
            s_state16[r0 * 16u + (j0 >> 1)] =
                (unsigned short)(v3_rho8(c0, tw0) | (v3_rho8(c1, tw0 + 0x85EBCA6Bu) << 8));
            s_state16[r1 * 16u + (j0 >> 1)] =
                (unsigned short)(v3_rho8(c2, tw1) | (v3_rho8(c3, tw1 + 0x85EBCA6Bu) << 8));
        }
    }
    __syncwarp();
}

#define V4_NCF_WARPS 4    // nonces (warps) per block


__device__ __forceinline__ unsigned long long v4_barrett_mod(
    unsigned long long x, unsigned long long n, unsigned long long inv_n) {
    unsigned long long q = __umul64hi(x, inv_n);
    unsigned long long r = x - q * n;
    while (r >= n) r -= n;
    return r;
}

__device__ __forceinline__ const ulonglong2* v4_chunk_addr_lut(
    const unsigned long long* bases, const unsigned long long* prefix, unsigned int T,
    const unsigned short* lut, unsigned int lut_sh,
    unsigned long long tile_index, unsigned int lane) {
    const unsigned long long idx = tile_index * (unsigned long long)V4_TILE_CHUNKS + lane;
    unsigned int lo = __ldg(&lut[idx >> lut_sh]);
    while (lo + 1 < T && __ldg(&prefix[lo + 1]) <= idx) lo++;
    const ulonglong2* q = (const ulonglong2*)bases[lo];
    return q + (idx - prefix[lo]) * 2ULL;
}

// Cache only the immutable fold of each canonical tile's first 32 bytes.
extern "C" __global__ void nexus_tile_folds(
    const unsigned long long* bases,const unsigned long long* prefix,unsigned int T,
    unsigned long long n_tiles,const unsigned short* lut,unsigned int lut_sh,
    unsigned long long* folds) {
    const auto tile=static_cast<unsigned long long>(blockIdx.x)*blockDim.x+threadIdx.x;
    if(tile>=n_tiles)return;
    const auto* words=reinterpret_cast<const unsigned int*>(v4_chunk_addr_lut(bases,prefix,T,lut,lut_sh,tile,0));
    unsigned long long sf=0;
    #pragma unroll
    for(unsigned wd=0;wd<8;++wd)sf=mix64(sf^static_cast<unsigned long long>(words[wd]));
    folds[tile]=sf;
}

// Warp-cooperative tile address resolution: lane 0 checks whether all 32 chunks
// of `tile_index` lie within the same canonical tensor. When true (>99.99% of
// tiles), lane 0 broadcasts the contiguous tile base pointer to all 32 lanes,
// avoiding 31 redundant LUT/prefix/base global memory lookups per step. Tiles
// straddling a tensor boundary fall back to exact per-lane resolution.
__device__ __forceinline__ const ulonglong2* v4_chunk_addr_warp(
    const unsigned long long* bases, const unsigned long long* prefix, unsigned int T,
    const unsigned short* lut, unsigned int lut_sh,
    unsigned long long tile_index, unsigned int lane) {
    unsigned long long contiguous_tile_base = 0ULL;
    if (lane == 0u) {
        const unsigned long long idx0 = tile_index * (unsigned long long)V4_TILE_CHUNKS;
        unsigned int lo = __ldg(&lut[idx0 >> lut_sh]);
        while (lo + 1 < T && __ldg(&prefix[lo + 1]) <= idx0) lo++;
        const unsigned long long p_lo = __ldg(&prefix[lo]);
        const unsigned long long p_hi = (lo + 1 < T) ? __ldg(&prefix[lo + 1]) : (idx0 + V4_TILE_CHUNKS);
        if (idx0 + V4_TILE_CHUNKS <= p_hi) {
            contiguous_tile_base = __ldg(&bases[lo]) + (idx0 - p_lo) * 32ULL;
        }
    }
    contiguous_tile_base = __shfl_sync(0xFFFFFFFFu, contiguous_tile_base, 0);
    if (contiguous_tile_base != 0ULL) {
        return reinterpret_cast<const ulonglong2*>(contiguous_tile_base) + lane * 2ULL;
    }
    return v4_chunk_addr_lut(bases, prefix, T, lut, lut_sh, tile_index, lane);
}

// Reconstruct one candidate's canonical offset chain from immutable folds.
// This does not decide target validity or replace CPU/Rust verification.
extern "C" __global__ void nexus_witness_offsets(
    unsigned long long seed, unsigned long long n_tiles, unsigned long long inv_n,
    const unsigned long long* folds, unsigned long long* offsets) {
    if(blockIdx.x || threadIdx.x)return;
    auto off=v4_barrett_mod(mix64(seed ^ V4_OFFSET_FIRST_SALT),n_tiles,inv_n);
    for(unsigned step=0;step<256;++step) {
        offsets[step]=off;
        if(step+1<256)off=v4_barrett_mod(
            mix64(seed ^ static_cast<unsigned long long>(step+2)*V4_OFFSET_STEP_SALT ^ folds[off]),n_tiles,inv_n);
    }
}
// One block per step; each thread copies one canonical 32-byte chunk. Descriptor
// lookup is performed per chunk, including tiles crossing tensor boundaries.
extern "C" __global__ void nexus_witness_pairs(
    const unsigned long long* bases,const unsigned long long* prefix,unsigned int T,
    unsigned long long n_tiles,const unsigned short* lut,unsigned int lut_sh,
    const unsigned long long* offsets,ulonglong2* output) {
    const unsigned step=blockIdx.x,lane=threadIdx.x;
    if(step>=256 || lane>=64)return;
    auto tile=offsets[step];
    if(lane>=32 && (tile^1ULL)<n_tiles)tile^=1ULL;
    const auto* src=v4_chunk_addr_lut(bases,prefix,T,lut,lut_sh,tile,lane&31u);
    const unsigned dst=step*128u+lane*2u;
    output[dst]=src[0];output[dst+1]=src[1];
}

extern "C" __global__ void nexus_resident_search_preseeded(
    const unsigned long long* bases, const unsigned long long* prefix, unsigned int T,
    unsigned long long n_tiles, unsigned int K,
    unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3,
    unsigned long long s0, unsigned long long s1, unsigned long long s2, unsigned long long s3,
    unsigned long long time_,
    unsigned long long t0, unsigned long long t1, unsigned long long t2, unsigned long long t3,
    unsigned long long nonce_base, unsigned long long n_nonces,
    const unsigned short* lut, unsigned int lut_sh, unsigned long long inv_n,
    unsigned long long* winner,
    const unsigned long long* h10_state, unsigned int seed_h10,
    const unsigned long long* h10_seeds, unsigned int seed_precomputed,
    const unsigned long long* tile_folds) {
    (void)h10_state;
    (void)seed_h10;
    extern __shared__ unsigned int s_shared[];
    const unsigned int w = threadIdx.x >> 5;
    const unsigned int x = threadIdx.x & 31u;
    const unsigned long long i = (unsigned long long)blockIdx.x * V4_NCF_WARPS + w;
    if (i >= n_nonces) return;
    // Per warp: 1 KiB state + two (1 KiB tile + 16-byte fold pair) buffers.
    unsigned int* s_warp = s_shared + w * (256u + 2u * 260u);
    unsigned int* s_state = s_warp;
    unsigned int* s_tiles = s_warp + 256u;

    const unsigned long long nonce = nonce_base + i;
    // Every lane needs the identical seed; H10 seeds are precomputed by nexus_h10_seeds.
    unsigned long long seed = (x == 0u)
        ? (seed_precomputed ? __ldg(&h10_seeds[i]) : pom_seed_fold(nonce, time_, s0, s1, s2, s3))
        : 0ULL;
    seed = __shfl_sync(0xFFFFFFFFu, seed, 0);
    { unsigned long long h = mix64(seed ^ (V4_S0_ROW_SALT + (unsigned long long)x));
      #pragma unroll
      for (int k4 = 0; k4 < V4_D4; k4++) { h = mix64(h); s_state[x * 8u + k4] = (unsigned int)h; } }
    __syncwarp();

    unsigned long long off = v4_barrett_mod(mix64(seed ^ V4_OFFSET_FIRST_SALT), n_tiles, inv_n);
    unsigned long long cur_fold = (x == 0u) ? __ldg(&tile_folds[off]) : 0ULL;
    {
        const ulonglong2* q = v4_chunk_addr_warp(bases, prefix, T, lut, lut_sh, off, x);
        ulonglong2* dst = (ulonglong2*)(s_tiles + 260u + x * 8);   // tile 1 -> buffer (1&1)=1
        v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
        asm volatile("cp.async.commit_group;");
    }
    for (unsigned int step = 1; step <= K; step++) {
        unsigned int* cur = s_tiles + (step & 1u) * 260u;
        if (step < K) {
            // The next tile offset is identical for every lane of this nonce.
            // Issue the next tile's cp.async BEFORE waiting on the current tile's shared memory.
            unsigned long long next_off = 0;
            if (x == 0u) {
                next_off = v4_barrett_mod(
                    mix64(seed ^ (unsigned long long)(step + 1) * V4_OFFSET_STEP_SALT ^ cur_fold),
                    n_tiles, inv_n);
                cur_fold = __ldg(&tile_folds[next_off]);
            }
            off = __shfl_sync(0xFFFFFFFFu, next_off, 0);
            const ulonglong2* q = v4_chunk_addr_warp(bases, prefix, T, lut, lut_sh, off, x);
            ulonglong2* dst = (ulonglong2*)(s_tiles + ((step + 1u) & 1u) * 260u + x * 8);
            v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
        }
        asm volatile("cp.async.commit_group;");
        if (step < K) {
            asm volatile("cp.async.wait_group 1;");
        } else {
            asm volatile("cp.async.wait_group 0;");
        }
        __syncwarp();
        v4_imma_step(s_state, cur, step, x);
    }
    __syncwarp();

    // root_K + fold, verbatim reference tail.
    unsigned int row4[V4_D4];
    #pragma unroll
    for (int k4 = 0; k4 < V4_D4; k4++) row4[k4] = s_state[x * 8u + k4];
    unsigned int* s_tile = s_tiles;
    b3_hash_row32(row4, s_tile + x * 8);
    __syncwarp();
    unsigned int* src = s_tile; unsigned int* dst = s_tile + V4_D * 8;
    for (unsigned int n = V4_D; n > 1; n >>= 1) {
        if (x < n / 2) b3_hash_pair(src + x * 16, dst + x * 8);
        __syncwarp();
        unsigned int* tmp = src; src = dst; dst = tmp;
    }
    if (x == 0) {
        const unsigned long long fin = (unsigned long long)src[0] | ((unsigned long long)src[1] << 32);
        unsigned long long pv[4];
        pom_pow_fold(fin, p0, p1, p2, p3, pv);
        if (pom_le_leq(pv, t0, t1, t2, t3)) atomicMin(winner, nonce);
    }
}

extern "C" __global__ void nexus_h10_seeds(const unsigned long long *state,
    unsigned long long first,unsigned long long count,unsigned long long *seeds) {
  const auto i=static_cast<unsigned long long>(blockIdx.x)*blockDim.x+threadIdx.x;
  if(i<count)seeds[i]=pom_seed_h10(first+i,state);
}

// v5 search shares the byte-exact v4 transition and H14 seed kernels above.
// PoM v5 ARX context pad. No host pad image or readback cache is retained.
extern "C" __global__ void nexus_v5_pad_fill(unsigned long long* pad,
    unsigned long long first,unsigned long long count,
    unsigned long long w0,unsigned long long w1,unsigned long long w2,unsigned long long w3) {
  const auto t=static_cast<unsigned long long>(blockIdx.x)*blockDim.x+threadIdx.x;
  if(t>=count)return;
  const auto index=first+t;auto a=w0,b=w1^index,c=w2,d=w3;
  for(unsigned r=0;r<1920;++r) {
    a+=b;d^=a;d=(d<<32)|(d>>32);
    c+=d;b^=c;b=(b<<24)|(b>>40);
    a+=b;d^=a;d=(d<<16)|(d>>48);
    c+=d;b^=c;b=(b<<63)|(b>>1);
  }
  pad[index*4]=a;pad[index*4+1]=b;pad[index*4+2]=c;pad[index*4+3]=d;
}

extern "C" __global__ void nexus_resident_search_v5(
    const unsigned long long* bases, const unsigned long long* prefix, unsigned int T,
    unsigned long long n_tiles, unsigned int K,
    unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3,
    unsigned long long s0, unsigned long long s1, unsigned long long s2, unsigned long long s3,
    unsigned long long time_,
    unsigned long long t0, unsigned long long t1, unsigned long long t2, unsigned long long t3,
    unsigned long long nonce_base, unsigned long long n_nonces,
    const unsigned short* lut, unsigned int lut_sh, unsigned long long inv_n,
    unsigned long long* winner,
    const unsigned long long* h10_state, unsigned int seed_h10,
    const unsigned long long* h10_seeds, unsigned int seed_precomputed,
    const unsigned long long* tile_folds,
    unsigned long long n_pad, unsigned long long inv_pad, unsigned int pad_steps,
    unsigned long long* diagnostic_offsets, unsigned int* diagnostic_states) {
    (void)h10_state;
    (void)seed_h10;
    extern __shared__ unsigned int s_shared[];
    const unsigned int w = threadIdx.x >> 5;
    const unsigned int x = threadIdx.x & 31u;
    const unsigned long long i = (unsigned long long)blockIdx.x * V4_NCF_WARPS + w;
    if (i >= n_nonces) return;
    // Per warp: 1 KiB state + two (1 KiB tile + 16-byte fold pair) buffers.
    unsigned int* s_warp = s_shared + w * (256u + 2u * 260u);
    unsigned int* s_state = s_warp;
    unsigned int* s_tiles = s_warp + 256u;

    const unsigned long long nonce = nonce_base + i;
    // Every lane needs the identical seed; H10 seeds are precomputed by nexus_h10_seeds.
    unsigned long long seed = (x == 0u)
        ? (seed_precomputed ? __ldg(&h10_seeds[i]) : pom_seed_fold(nonce, time_, s0, s1, s2, s3))
        : 0ULL;
    seed = __shfl_sync(0xFFFFFFFFu, seed, 0);
    { unsigned long long h = mix64(seed ^ (V4_S0_ROW_SALT + (unsigned long long)x));
      #pragma unroll
      for (int k4 = 0; k4 < V4_D4; k4++) { h = mix64(h); s_state[x * 8u + k4] = (unsigned int)h; } }
    __syncwarp();

    if(diagnostic_states) {
      for(unsigned k=0;k<8;++k)diagnostic_states[(i*257ULL)*256ULL+x*8u+k]=s_state[x*8u+k];
    }
    unsigned long long off = v4_barrett_mod(mix64(seed ^ V4_OFFSET_FIRST_SALT), n_tiles, inv_n);
    {
        const ulonglong2* q = v4_chunk_addr_warp(bases, prefix, T, lut, lut_sh, off, x);
        ulonglong2* dst = (ulonglong2*)(s_tiles + 260u + x * 8);   // tile 1 -> buffer (1&1)=1
        v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
        if(x==0u && off<n_tiles)v4_cp_async16(s_tiles+260u+256u,tile_folds+(off&~1ULL));
        asm volatile("cp.async.commit_group;");
    }
    for (unsigned int step = 1; step <= K; step++) {
        unsigned int* cur = s_tiles + (step & 1u) * 260u;
        asm volatile("cp.async.wait_group 0;");
        __syncwarp();
        if(diagnostic_offsets && x==0u)diagnostic_offsets[i*256ULL+step-1u]=off;
        if (step < K) {
            // v5 depends on the pre-transition state. Every lane contributes its
            // row's first eight bytes before any lane overwrites that state.
            unsigned long long hs=mix64((static_cast<unsigned long long>(s_state[x*8u+1u])<<32 | s_state[x*8u]) ^ x);
            // SM80+ native warp reductions replace ten 32-bit shuffle/XOR pairs.
            const unsigned int hs_lo=__reduce_xor_sync(0xffffffffu,static_cast<unsigned int>(hs));
            const unsigned int hs_hi=__reduce_xor_sync(0xffffffffu,static_cast<unsigned int>(hs>>32));
            hs=(static_cast<unsigned long long>(hs_hi)<<32)|hs_lo;
            unsigned long long next_off=0;
            if(x==0u) {
                unsigned long long sf=0;
                if(off<n_tiles)sf=reinterpret_cast<const unsigned long long*>(cur+256u)[off&1u];
                else for(unsigned wd=0;wd<8;++wd)sf=mix64(sf^static_cast<unsigned long long>(cur[wd]));
                const auto raw=mix64(seed^static_cast<unsigned long long>(step+1u)*V4_OFFSET_STEP_SALT^sf^hs);
                const bool is_pad=((step+1u)*pad_steps)/256u!=(step*pad_steps)/256u;
                next_off=is_pad?n_tiles+v4_barrett_mod(raw,n_pad,inv_pad):v4_barrett_mod(raw,n_tiles,inv_n);
            }
            off = __shfl_sync(0xFFFFFFFFu, next_off, 0);
            const ulonglong2* q = v4_chunk_addr_warp(bases, prefix, T, lut, lut_sh, off, x);
            ulonglong2* dst = (ulonglong2*)(s_tiles + ((step + 1u) & 1u) * 260u + x * 8);
            v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
            if(x==0u && off<n_tiles)v4_cp_async16(s_tiles+((step+1u)&1u)*260u+256u,tile_folds+(off&~1ULL));
        }
        asm volatile("cp.async.commit_group;");
        v4_imma_step(s_state, cur, step, x);
        if(diagnostic_states) {
          for(unsigned k=0;k<8;++k)diagnostic_states[(i*257ULL+step)*256ULL+x*8u+k]=s_state[x*8u+k];
        }
    }
    asm volatile("cp.async.wait_group 0;");
    __syncwarp();

    // root_K + fold, verbatim reference tail.
    unsigned int row4[V4_D4];
    #pragma unroll
    for (int k4 = 0; k4 < V4_D4; k4++) row4[k4] = s_state[x * 8u + k4];
    unsigned int* s_tile = s_tiles;
    b3_hash_row32(row4, s_tile + x * 8);
    __syncwarp();
    unsigned int* src = s_tile; unsigned int* dst = s_tile + V4_D * 8;
    for (unsigned int n = V4_D; n > 1; n >>= 1) {
        if (x < n / 2) b3_hash_pair(src + x * 16, dst + x * 8);
        __syncwarp();
        unsigned int* tmp = src; src = dst; dst = tmp;
    }
    if (x == 0) {
        const unsigned long long fin = (unsigned long long)src[0] | ((unsigned long long)src[1] << 32);
        unsigned long long pv[4];
        pom_pow_fold(fin, p0, p1, p2, p3, pv);
        if (pom_le_leq(pv, t0, t1, t2, t3)) atomicMin(winner, nonce);
    }
}

extern "C" __global__ void nexus_resident_search_v5_2x(
    const unsigned long long* bases, const unsigned long long* prefix, unsigned int T,
    unsigned long long n_tiles, unsigned int K,
    unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3,
    unsigned long long s0, unsigned long long s1, unsigned long long s2, unsigned long long s3,
    unsigned long long time_,
    unsigned long long t0, unsigned long long t1, unsigned long long t2, unsigned long long t3,
    unsigned long long nonce_base, unsigned long long n_nonces,
    const unsigned short* lut, unsigned int lut_sh, unsigned long long inv_n,
    unsigned long long* winner,
    const unsigned long long* h10_state, unsigned int seed_h10,
    const unsigned long long* h10_seeds, unsigned int seed_precomputed,
    const unsigned long long* tile_folds,
    unsigned long long n_pad, unsigned long long inv_pad, unsigned int pad_steps,
    unsigned long long* diagnostic_offsets, unsigned int* diagnostic_states) {
    (void)h10_state; (void)seed_h10; (void)diagnostic_offsets; (void)diagnostic_states;
    extern __shared__ unsigned int s_shared[];
    const unsigned int w = threadIdx.x >> 5;
    const unsigned int x = threadIdx.x & 31u;
    const unsigned long long i0 = ((unsigned long long)blockIdx.x * V4_NCF_WARPS + w) * 2ULL;
    if (i0 >= n_nonces) return;
    const unsigned long long i1 = i0 + 1ULL;
    const bool has1 = (i1 < n_nonces);

    // Per warp: 2x (1 KiB state + 2x (1 KiB tile + 16-byte fold pair)) = 1552 uints = 6208 bytes.
    unsigned int* s_warp = s_shared + w * (2u * (256u + 2u * 260u));
    unsigned int* s_state0 = s_warp;
    unsigned int* s_tiles0 = s_warp + 256u;
    unsigned int* s_state1 = s_warp + 256u + 2u * 260u;
    unsigned int* s_tiles1 = s_state1 + 256u;

    const unsigned long long nonce0 = nonce_base + i0;
    const unsigned long long nonce1 = nonce_base + i1;
    unsigned long long seed0 = (x == 0u) ? (seed_precomputed ? __ldg(&h10_seeds[i0]) : pom_seed_fold(nonce0, time_, s0, s1, s2, s3)) : 0ULL;
    seed0 = __shfl_sync(0xFFFFFFFFu, seed0, 0);
    { unsigned long long h = mix64(seed0 ^ (V4_S0_ROW_SALT + (unsigned long long)x));
      #pragma unroll
      for (int k4 = 0; k4 < V4_D4; k4++) { h = mix64(h); s_state0[x * 8u + k4] = (unsigned int)h; } }

    unsigned long long seed1 = 0ULL;
    if (has1) {
        seed1 = (x == 0u) ? (seed_precomputed ? __ldg(&h10_seeds[i1]) : pom_seed_fold(nonce1, time_, s0, s1, s2, s3)) : 0ULL;
        seed1 = __shfl_sync(0xFFFFFFFFu, seed1, 0);
        unsigned long long h = mix64(seed1 ^ (V4_S0_ROW_SALT + (unsigned long long)x));
        #pragma unroll
        for (int k4 = 0; k4 < V4_D4; k4++) { h = mix64(h); s_state1[x * 8u + k4] = (unsigned int)h; }
    }
    __syncwarp();

    unsigned long long off0 = v4_barrett_mod(mix64(seed0 ^ V4_OFFSET_FIRST_SALT), n_tiles, inv_n);
    {
        const ulonglong2* q = v4_chunk_addr_warp(bases, prefix, T, lut, lut_sh, off0, x);
        ulonglong2* dst = (ulonglong2*)(s_tiles0 + 260u + x * 8);
        v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
        if (x == 0u && off0 < n_tiles) v4_cp_async16(s_tiles0 + 260u + 256u, tile_folds + (off0 & ~1ULL));
        asm volatile("cp.async.commit_group;");
    }
    unsigned long long off1 = 0ULL;
    if (has1) {
        off1 = v4_barrett_mod(mix64(seed1 ^ V4_OFFSET_FIRST_SALT), n_tiles, inv_n);
        const ulonglong2* q = v4_chunk_addr_warp(bases, prefix, T, lut, lut_sh, off1, x);
        ulonglong2* dst = (ulonglong2*)(s_tiles1 + 260u + x * 8);
        v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
        if (x == 0u && off1 < n_tiles) v4_cp_async16(s_tiles1 + 260u + 256u, tile_folds + (off1 & ~1ULL));
        asm volatile("cp.async.commit_group;");
    }

    for (unsigned int step = 1; step <= K; step++) {
        if (has1) { asm volatile("cp.async.wait_group 1;"); }
        else { asm volatile("cp.async.wait_group 0;"); }
        __syncwarp();
        unsigned int* cur0 = s_tiles0 + (step & 1u) * 260u;
        if (step < K) {
            unsigned long long hs = mix64((static_cast<unsigned long long>(s_state0[x*8u+1u])<<32 | s_state0[x*8u]) ^ x);
            const unsigned int hs_lo = __reduce_xor_sync(0xffffffffu, static_cast<unsigned int>(hs));
            const unsigned int hs_hi = __reduce_xor_sync(0xffffffffu, static_cast<unsigned int>(hs>>32));
            hs = (static_cast<unsigned long long>(hs_hi)<<32) | hs_lo;
            unsigned long long next_off = 0;
            if (x == 0u) {
                unsigned long long sf = 0;
                if (off0 < n_tiles) sf = reinterpret_cast<const unsigned long long*>(cur0 + 256u)[off0 & 1u];
                else for (unsigned wd = 0; wd < 8; ++wd) sf = mix64(sf ^ static_cast<unsigned long long>(cur0[wd]));
                const auto raw = mix64(seed0 ^ static_cast<unsigned long long>(step + 1u) * V4_OFFSET_STEP_SALT ^ sf ^ hs);
                const bool is_pad = ((step + 1u) * pad_steps) / 256u != (step * pad_steps) / 256u;
                next_off = is_pad ? n_tiles + v4_barrett_mod(raw, n_pad, inv_pad) : v4_barrett_mod(raw, n_tiles, inv_n);
            }
            off0 = __shfl_sync(0xFFFFFFFFu, next_off, 0);
            const ulonglong2* q = v4_chunk_addr_warp(bases, prefix, T, lut, lut_sh, off0, x);
            ulonglong2* dst = (ulonglong2*)(s_tiles0 + ((step + 1u) & 1u) * 260u + x * 8);
            v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
            if (x == 0u && off0 < n_tiles) v4_cp_async16(s_tiles0 + ((step + 1u) & 1u) * 260u + 256u, tile_folds + (off0 & ~1ULL));
            asm volatile("cp.async.commit_group;");
        }
        v4_imma_step(s_state0, cur0, step, x);

        if (has1) {
            if (step < K) { asm volatile("cp.async.wait_group 1;"); }
            else { asm volatile("cp.async.wait_group 0;"); }
            __syncwarp();
            unsigned int* cur1 = s_tiles1 + (step & 1u) * 260u;
            if (step < K) {
                unsigned long long hs = mix64((static_cast<unsigned long long>(s_state1[x*8u+1u])<<32 | s_state1[x*8u]) ^ x);
                const unsigned int hs_lo = __reduce_xor_sync(0xffffffffu, static_cast<unsigned int>(hs));
                const unsigned int hs_hi = __reduce_xor_sync(0xffffffffu, static_cast<unsigned int>(hs>>32));
                hs = (static_cast<unsigned long long>(hs_hi)<<32) | hs_lo;
                unsigned long long next_off = 0;
                if (x == 0u) {
                    unsigned long long sf = 0;
                    if (off1 < n_tiles) sf = reinterpret_cast<const unsigned long long*>(cur1 + 256u)[off1 & 1u];
                    else for (unsigned wd = 0; wd < 8; ++wd) sf = mix64(sf ^ static_cast<unsigned long long>(cur1[wd]));
                    const auto raw = mix64(seed1 ^ static_cast<unsigned long long>(step + 1u) * V4_OFFSET_STEP_SALT ^ sf ^ hs);
                    const bool is_pad = ((step + 1u) * pad_steps) / 256u != (step * pad_steps) / 256u;
                    next_off = is_pad ? n_tiles + v4_barrett_mod(raw, n_pad, inv_pad) : v4_barrett_mod(raw, n_tiles, inv_n);
                }
                off1 = __shfl_sync(0xFFFFFFFFu, next_off, 0);
                const ulonglong2* q = v4_chunk_addr_warp(bases, prefix, T, lut, lut_sh, off1, x);
                ulonglong2* dst = (ulonglong2*)(s_tiles1 + ((step + 1u) & 1u) * 260u + x * 8);
                v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
                if (x == 0u && off1 < n_tiles) v4_cp_async16(s_tiles1 + ((step + 1u) & 1u) * 260u + 256u, tile_folds + (off1 & ~1ULL));
                asm volatile("cp.async.commit_group;");
            }
            v4_imma_step(s_state1, cur1, step, x);
        }
    }
    asm volatile("cp.async.wait_group 0;");
    __syncwarp();

    auto finalize_nonce = [&](unsigned int* s_st, unsigned int* s_tl, unsigned long long n_val) {
        unsigned int row4[V4_D4];
        #pragma unroll
        for (int k4 = 0; k4 < V4_D4; k4++) row4[k4] = s_st[x * 8u + k4];
        b3_hash_row32(row4, s_tl + x * 8);
        __syncwarp();
        unsigned int* src = s_tl; unsigned int* dst = s_tl + V4_D * 8;
        for (unsigned int n = V4_D; n > 1; n >>= 1) {
            if (x < n / 2) b3_hash_pair(src + x * 16, dst + x * 8);
            __syncwarp();
            unsigned int* tmp = src; src = dst; dst = tmp;
        }
        if (x == 0) {
            const unsigned long long fin = (unsigned long long)src[0] | ((unsigned long long)src[1] << 32);
            unsigned long long pv[4];
            pom_pow_fold(fin, p0, p1, p2, p3, pv);
            if (pom_le_leq(pv, t0, t1, t2, t3)) atomicMin(winner, n_val);
        }
    };
    finalize_nonce(s_state0, s_tiles0, nonce0);
    if (has1) finalize_nonce(s_state1, s_tiles1, nonce1);
}

// --- PoM v5 lock-step walk over contiguous canonical weights (ported from keryx-miner-supr / ocminer) ---
// Several nonces share a warp; tile t is read directly at wbase + t KiB, pad tile p at pbase + p KiB.
// Designed for sm_80+ tensor cores with async cp.async shared-memory double-buffering.
__device__ __forceinline__ unsigned int v5ls_sw(unsigned int r, unsigned int w) {
    return r * 8u + (w ^ (((r >> 2) & 1u) * 4u));
}

__device__ __forceinline__ void v5ls_load(unsigned int* tile_dst, const unsigned char* tile_src, unsigned int x) {
    #pragma unroll
    for (int k = 0; k < 2; k++) {
        const unsigned int p = x + 32u * k, col = p >> 1, half = p & 1u;
        const unsigned int word = col * 8u + ((half ^ ((col >> 2) & 1u)) * 4u);
        const unsigned long long sd = __cvta_generic_to_shared(tile_dst + word);
        asm volatile("cp.async.cg.shared.global [%0], [%1], 16;" :: "l"(sd), "l"(tile_src + p * 16u));
    }
}

__device__ __forceinline__ unsigned short v3_rho16(int c0, int c1, unsigned int tw, unsigned int tw_b) {
    unsigned int z0 = (unsigned int)c0 ^ tw;
    z0 *= 0x9E3779B9u; z0 ^= z0 >> 16;
    z0 *= 0x85EBCA6Bu; z0 ^= z0 >> 13;

    unsigned int z1 = (unsigned int)c1 ^ tw_b;
    z1 *= 0x9E3779B9u; z1 ^= z1 >> 16;
    z1 *= 0x85EBCA6Bu; z1 ^= z1 >> 13;

    unsigned int res;
    asm("prmt.b32 %0, %1, %2, 0x0040;" : "=r"(res) : "r"(z0), "r"(z1));
    return (unsigned short)res;
}

__device__ __forceinline__ void v5ls_step(
    unsigned int* s_state, const unsigned int* s_tile, unsigned int step_base, unsigned int x,
    const unsigned int tw_base[4][2]) {
    const unsigned int gid = x >> 2, tig = x & 3u;
    unsigned int a[2][4];
    #pragma unroll
    for (unsigned int g = 0; g < 2; g++) {
        const unsigned int r0 = g * 16u + gid, r1 = r0 + 8u;
        a[g][0] = s_state[v5ls_sw(r0, tig)];      a[g][1] = s_state[v5ls_sw(r1, tig)];
        a[g][2] = s_state[v5ls_sw(r0, tig + 4u)]; a[g][3] = s_state[v5ls_sw(r1, tig + 4u)];
    }
    unsigned int b[4][2];
    #pragma unroll
    for (unsigned int cg = 0; cg < 4; cg++) {
        const unsigned int col = cg * 8u + gid;
        b[cg][0] = s_tile[v5ls_sw(col, tig)];
        b[cg][1] = s_tile[v5ls_sw(col, tig + 4u)];
    }
    unsigned short* s16 = (unsigned short*)s_state;
    #pragma unroll
    for (unsigned int cg = 0; cg < 4; cg++) {
        const unsigned int b0 = b[cg][0];
        const unsigned int b1 = b[cg][1];

        int c0_0 = 0, c1_0 = 0, c2_0 = 0, c3_0 = 0;
        asm volatile(
            "mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 "
            "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
            : "+r"(c0_0), "+r"(c1_0), "+r"(c2_0), "+r"(c3_0)
            : "r"(a[0][0]), "r"(a[0][1]), "r"(a[0][2]), "r"(a[0][3]), "r"(b0), "r"(b1));

        int c0_1 = 0, c1_1 = 0, c2_1 = 0, c3_1 = 0;
        asm volatile(
            "mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 "
            "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
            : "+r"(c0_1), "+r"(c1_1), "+r"(c2_1), "+r"(c3_1)
            : "r"(a[1][0]), "r"(a[1][1]), "r"(a[1][2]), "r"(a[1][3]), "r"(b0), "r"(b1));

        const unsigned int col_idx = (cg * 8u + tig * 2u) >> 2;
        const unsigned int s_idx0 = 2u * v5ls_sw(gid, col_idx) + (tig & 1u);

        const unsigned int tw0_0 = step_base + tw_base[cg][0];
        const unsigned int tw1_0 = tw0_0 + 8u * 0xC2B2AE35u;
        s16[s_idx0] = v3_rho16(c0_0, c1_0, tw0_0, tw0_0 + 0x85EBCA6Bu);
        s16[s_idx0 + 128u] = v3_rho16(c2_0, c3_0, tw1_0, tw1_0 + 0x85EBCA6Bu);

        const unsigned int tw0_1 = step_base + tw_base[cg][1];
        const unsigned int tw1_1 = tw0_1 + 8u * 0xC2B2AE35u;
        s16[s_idx0 + 256u] = v3_rho16(c0_1, c1_1, tw0_1, tw0_1 + 0x85EBCA6Bu);
        s16[s_idx0 + 384u] = v3_rho16(c2_1, c3_1, tw1_1, tw1_1 + 0x85EBCA6Bu);
    }
}

template <int NL, int WARPS_PER_BLOCK = 1>
__device__ __forceinline__ void v5ls_walk(
    const unsigned char* wbase, unsigned long long n_tiles, const unsigned char* pbase, unsigned long long n_pad,
    unsigned int K, unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3,
    unsigned long long t0, unsigned long long t1, unsigned long long t2, unsigned long long t3,
    unsigned long long nonce_base, unsigned long long n_nonces,
    unsigned long long inv_n, unsigned long long inv_p, unsigned int k_pad,
    const unsigned long long* h10_seeds, unsigned long long* winner) {
    extern __shared__ unsigned int s_ls_base[];
    const unsigned int warp_id = threadIdx.x >> 5;
    const unsigned int x = threadIdx.x & 31u;
    unsigned int* s_ls = s_ls_base + warp_id * (NL * 768u);
    constexpr unsigned int GS = 32u / NL;
    const unsigned int g = x / GS;
    const unsigned long long i0 = ((unsigned long long)blockIdx.x * WARPS_PER_BLOCK + warp_id) * NL;
    if (i0 >= n_nonces) return;
    const unsigned long long my_i = min(i0 + g, n_nonces - 1);
    const unsigned long long my_seed = h10_seeds[my_i];

    const unsigned int gid = x >> 2, tig = x & 3u;
    unsigned int tw_base[4][2];
    #pragma unroll
    for (unsigned int cg = 0; cg < 4; cg++) {
        const unsigned int j0 = cg * 8u + tig * 2u;
        #pragma unroll
        for (unsigned int gr = 0; gr < 2; gr++) {
            const unsigned int r0 = gr * 16u + gid;
            tw_base[cg][gr] = r0 * 0xC2B2AE35u + j0 * 0x85EBCA6Bu;
        }
    }

    const unsigned int col0 = x >> 1, half0 = x & 1u;
    const unsigned int word0 = col0 * 8u + ((half0 ^ ((col0 >> 2) & 1u)) * 4u);
    const unsigned int col1 = (x + 32u) >> 1, half1 = x & 1u;
    const unsigned int word1 = col1 * 8u + ((half1 ^ ((col1 >> 2) & 1u)) * 4u);

    const unsigned int base_smem = static_cast<unsigned int>(__cvta_generic_to_shared(s_ls));
    unsigned int smem_dst0[NL], smem_dst1[NL];
    #pragma unroll
    for (int l = 0; l < NL; l++) {
        smem_dst0[l] = base_smem + (l * 768u + 256u + word0) * 4u;
        smem_dst1[l] = base_smem + (l * 768u + 256u + word1) * 4u;
    }

    unsigned long long my_off = v4_barrett_mod(mix64(my_seed ^ V4_OFFSET_FIRST_SALT), n_tiles, inv_n);
    #pragma unroll
    for (int l = 0; l < NL; l++) {
        const unsigned long long off = __shfl_sync(0xffffffffu, my_off, l * GS);
        const unsigned char* src = wbase + off * 1024ULL;
        asm volatile("cp.async.ca.shared::cta.global [%0], [%1], 16;" :: "r"(smem_dst0[l] + 1024u), "l"(src + x * 16u));
        asm volatile("cp.async.ca.shared::cta.global [%0], [%1], 16;" :: "r"(smem_dst1[l] + 1024u), "l"(src + x * 16u + 512u));
    }
    asm volatile("cp.async.commit_group;");
    #pragma unroll
    for (int l = 0; l < NL; l++) {
        unsigned int* st = s_ls + l * 768u;
        const unsigned long long sd = __shfl_sync(0xffffffffu, my_seed, l * GS);
        unsigned long long h = mix64(sd ^ (V4_S0_ROW_SALT + (unsigned long long)x));
        #pragma unroll
        for (int k4 = 0; k4 < V4_D4; k4++) { h = mix64(h); st[v5ls_sw(x, k4)] = (unsigned int)h; }
    }
    __syncwarp();
    const unsigned int f0 = v5ls_sw(x, 0);
    const unsigned long long* st64[NL];
    #pragma unroll
    for (int l = 0; l < NL; l++) {
        st64[l] = reinterpret_cast<const unsigned long long*>(s_ls + l * 768u + f0);
    }
    unsigned long long step_salt = 2ULL * V4_OFFSET_STEP_SALT;
    for (unsigned int step = 1; step <= K; step++) {
        const unsigned int cb = (step & 1u) * 256u;                     // 0: tile buffer 0, 256: buffer 1
        asm volatile("cp.async.wait_group 0;");
        if (step < K) {
            unsigned long long hs_mine = 0;
            #pragma unroll
            for (int l = 0; l < NL; l++) {
                const unsigned long long r0 = *st64[l];
                unsigned long long hs = mix64(r0 ^ (unsigned long long)x);
                const unsigned int lo = __reduce_xor_sync(0xffffffffu, (unsigned int)hs);
                const unsigned int hi = __reduce_xor_sync(0xffffffffu, (unsigned int)(hs >> 32));
                hs = ((unsigned long long)hi << 32) | lo;
                if (l == g) hs_mine = hs;
            }
            unsigned long long sf = 0;
            if ((x & (GS - 1u)) == 0u) {
                const unsigned int* cur = s_ls + g * 768u + 256u + cb;
                #pragma unroll
                for (int wd = 0; wd < 8; wd++) sf = mix64(sf ^ (unsigned long long)cur[wd]);
            }
            sf = __shfl_sync(0xffffffffu, sf, g * GS);
            const unsigned long long raw = mix64(my_seed ^ step_salt ^ sf ^ hs_mine);
            step_salt += V4_OFFSET_STEP_SALT;
            const bool pad = ((step + 1u) * k_pad) / 256u != (step * k_pad) / 256u;
            my_off = pad ? n_tiles + v4_barrett_mod(raw, n_pad, inv_p) : v4_barrett_mod(raw, n_tiles, inv_n);
            const unsigned int toggle_offset = ((step & 1u) ^ 1u) * 1024u;
            #pragma unroll
            for (int l = 0; l < NL; l++) {
                const unsigned long long off = __shfl_sync(0xffffffffu, my_off, l * GS);
                const unsigned char* src = off < n_tiles ? wbase + off * 1024ULL : pbase + (off - n_tiles) * 1024ULL;
                asm volatile("cp.async.ca.shared::cta.global [%0], [%1], 16;" :: "r"(smem_dst0[l] + toggle_offset), "l"(src + x * 16u));
                asm volatile("cp.async.ca.shared::cta.global [%0], [%1], 16;" :: "r"(smem_dst1[l] + toggle_offset), "l"(src + x * 16u + 512u));
            }
        }
        asm volatile("cp.async.commit_group;");
        const unsigned int step_base = step * 0x9E3779B9u;
        #pragma unroll
        for (int l = 0; l < NL; l++) v5ls_step(s_ls + l * 768u, s_ls + l * 768u + 256u + cb, step_base, x, tw_base);
        __syncwarp();
    }
    asm volatile("cp.async.wait_group 0;");
    __syncwarp();
    #pragma unroll
    for (int l = 0; l < NL; l++) {
        unsigned int* st = s_ls + l * 768u;
        unsigned int row4[V4_D4];
        #pragma unroll
        for (int k4 = 0; k4 < V4_D4; k4++) row4[k4] = st[v5ls_sw(x, k4)];
        unsigned int* s_tile = st + 256u;               // both tile buffers are free now
        b3_hash_row32(row4, s_tile + x * 8);
        __syncwarp();
        unsigned int* src = s_tile; unsigned int* dst = s_tile + V4_D * 8;
        for (unsigned int n = V4_D; n > 1; n >>= 1) {
            if (x < n / 2) b3_hash_pair(src + x * 16, dst + x * 8);
            __syncwarp();
            unsigned int* tmp = src; src = dst; dst = tmp;
        }
        if (x == 0 && i0 + l < n_nonces) {
            const unsigned long long fin = (unsigned long long)src[0] | ((unsigned long long)src[1] << 32);
            unsigned long long pv[4];
            pom_pow_fold(fin, p0, p1, p2, p3, pv);
            if (pom_le_leq(pv, t0, t1, t2, t3)) atomicMin(winner, nonce_base + i0 + l);
        }
        __syncwarp();
    }
}

// Four nonces per 32-thread block (RTX 4090 / RTX 5090; 12 KiB shared memory)
extern "C" __global__ void __launch_bounds__(32) pom_mine_v5_lockstep_seeded(
    const unsigned char* wbase, unsigned long long n_tiles, const unsigned char* pbase, unsigned long long n_pad,
    unsigned int K, unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3,
    unsigned long long t0, unsigned long long t1, unsigned long long t2, unsigned long long t3,
    unsigned long long nonce_base, unsigned long long n_nonces,
    unsigned long long inv_n, unsigned long long inv_p, unsigned int k_pad,
    const unsigned long long* h10_seeds, unsigned long long* winner) {
    v5ls_walk<4>(wbase, n_tiles, pbase, n_pad, K, p0, p1, p2, p3, t0, t1, t2, t3, nonce_base, n_nonces, inv_n, inv_p, k_pad, h10_seeds, winner);
}

// Two warps per block, 4 nonces per warp = 8 nonces/block (RTX 3070 Ti high occupancy; 24 KiB shared memory)
extern "C" __global__ void __launch_bounds__(64) pom_mine_v5_lockstep_w2_seeded(
    const unsigned char* wbase, unsigned long long n_tiles, const unsigned char* pbase, unsigned long long n_pad,
    unsigned int K, unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3,
    unsigned long long t0, unsigned long long t1, unsigned long long t2, unsigned long long t3,
    unsigned long long nonce_base, unsigned long long n_nonces,
    unsigned long long inv_n, unsigned long long inv_p, unsigned int k_pad,
    const unsigned long long* h10_seeds, unsigned long long* winner) {
    v5ls_walk<4, 2>(wbase, n_tiles, pbase, n_pad, K, p0, p1, p2, p3, t0, t1, t2, t3, nonce_base, n_nonces, inv_n, inv_p, k_pad, h10_seeds, winner);
}

// Two nonces per 32-thread block (RTX 3070 Ti, 6 KiB shared memory)
extern "C" __global__ void __launch_bounds__(32) pom_mine_v5_lockstep2_seeded(
    const unsigned char* wbase, unsigned long long n_tiles, const unsigned char* pbase, unsigned long long n_pad,
    unsigned int K, unsigned long long p0, unsigned long long p1, unsigned long long p2, unsigned long long p3,
    unsigned long long t0, unsigned long long t1, unsigned long long t2, unsigned long long t3,
    unsigned long long nonce_base, unsigned long long n_nonces,
    unsigned long long inv_n, unsigned long long inv_p, unsigned int k_pad,
    const unsigned long long* h10_seeds, unsigned long long* winner) {
    v5ls_walk<2>(wbase, n_tiles, pbase, n_pad, K, p0, p1, p2, p3, t0, t1, t2, t3, nonce_base, n_nonces, inv_n, inv_p, k_pad, h10_seeds, winner);
}

#include "resident_path_pipeline.cuh"
