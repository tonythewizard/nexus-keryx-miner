// PoM v4 immutable-model address pipeline. K is fixed at 256 by the host.

extern "C" __global__ void build_metadata(const unsigned long long* bases,const unsigned long long* prefix,unsigned T,unsigned long long tiles,const unsigned short* lut,unsigned shift,const unsigned long long* folds,ulonglong2* meta) {
 auto i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=tiles)return;
 auto chunk=i*32;unsigned lo=lut[chunk>>shift];while(lo+1<T&&prefix[lo+1]<=chunk)++lo;
 meta[i]=make_ulonglong2(chunk+32<=prefix[lo+1]?bases[lo]+(chunk-prefix[lo])*32:0,folds[i]);
}
__device__ const ulonglong2* metadata_address(const unsigned long long* bases,const unsigned long long* prefix,unsigned T,const unsigned short* lut,unsigned shift,unsigned long long tile,unsigned lane,const unsigned long long* meta) {
 unsigned long long ptr=lane==0?__ldg(&meta[tile*2]):0;ptr=__shfl_sync(0xffffffffu,ptr,0);
 if(ptr)return reinterpret_cast<const ulonglong2*>(ptr)+lane*2;
 return v4_chunk_addr_lut(bases,prefix,T,lut,shift,tile,lane);
}
extern "C" __global__ void prepare_paths(const unsigned long long* seeds,unsigned long long tiles,unsigned long long recip,const ulonglong2* meta,unsigned long long count,unsigned long long* paths) {
 auto i=(unsigned long long)blockIdx.x*blockDim.x+threadIdx.x;if(i>=count)return;auto seed=seeds[i];auto off=v4_barrett_mod(mix64(seed^V4_OFFSET_FIRST_SALT),tiles,recip);
 for(unsigned step=0;step<256;++step){const auto m=meta[off];paths[step*count+i]=m.x?m.x:((off<<1)|1);if(step<255)off=v4_barrett_mod(mix64(seed^(unsigned long long)(step+2)*V4_OFFSET_STEP_SALT^m.y),tiles,recip);}
}
__device__ const ulonglong2* path_address(const unsigned long long* bases,const unsigned long long* prefix,unsigned T,const unsigned short* lut,unsigned shift,const unsigned long long* paths,unsigned long long index,unsigned lane) {
 auto ptr=lane==0?__ldg(paths+index):0;ptr=__shfl_sync(0xffffffffu,ptr,0);
 if(ptr&1)return v4_chunk_addr_lut(bases,prefix,T,lut,shift,ptr>>1,lane);
 return reinterpret_cast<const ulonglong2*>(ptr)+lane*2;
}
__device__ __forceinline__ void register_step(
    unsigned int (&a)[2][4], const unsigned int* s_tile, unsigned int step, unsigned int x) {
    const unsigned int gid = x >> 2, tig = x & 3u;
    unsigned int next[2][4];
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
            const unsigned int lo = v3_rho8(c0, tw0) | (v3_rho8(c1, tw0 + 0x85EBCA6Bu) << 8);
            const unsigned int hi = v3_rho8(c2, tw1) | (v3_rho8(c3, tw1 + 0x85EBCA6Bu) << 8);
            if ((cg & 1u)==0) {next[g][cg]=lo;next[g][cg+1]=hi;}
            else {next[g][cg-1]|=lo<<16;next[g][cg]|=hi<<16;}
        }
    }
    #pragma unroll
    for(unsigned g=0;g<2;++g) {
        #pragma unroll
        for(unsigned j=0;j<4;++j) {
            const unsigned p=next[g][j];
            const unsigned q=__shfl_xor_sync(0xffffffffu,p,1);
            const unsigned packed=(tig&1u)?((q>>16)|(p&0xffff0000u)):((p&0xffffu)|(q<<16));
            a[g][j]=__shfl_sync(0xffffffffu,packed,gid*4u+(tig&1u)*2u+(tig>>1u));
        }
    }
}

__device__ void load_registers(unsigned int (&a)[2][4], const unsigned int* state,unsigned x) {
 unsigned gid=x>>2,t=x&3;
 #pragma unroll
 for(unsigned g=0;g<2;++g){unsigned r=g*16+gid;a[g][0]=state[r*8+t];a[g][1]=state[(r+8)*8+t];a[g][2]=state[r*8+t+4];a[g][3]=state[(r+8)*8+t+4];}
}
__device__ void store_registers(const unsigned int (&a)[2][4], unsigned int* state,unsigned x) {
 unsigned gid=x>>2,t=x&3;
 #pragma unroll
 for(unsigned g=0;g<2;++g){unsigned r=g*16+gid;state[r*8+t]=a[g][0];state[(r+8)*8+t]=a[g][1];state[r*8+t+4]=a[g][2];state[(r+8)*8+t+4]=a[g][3];}
}
extern "C" __global__ void path_register_search(
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
    const unsigned long long* tile_folds, const unsigned long long* paths) {
    (void)h10_state;
    (void)seed_h10;
    extern __shared__ unsigned int s_shared[];
    const unsigned int w = threadIdx.x >> 5;
    const unsigned int x = threadIdx.x & 31u;
    const unsigned long long i = (unsigned long long)blockIdx.x * V4_NCF_WARPS + w;
    if (i >= n_nonces) return;
    // Per warp: 1 KiB state + two (1 KiB tile + 16-byte fold pair) buffers.
    unsigned int* s_warp = s_shared + w * (2u * 260u);
    unsigned int* s_state = s_warp;
    unsigned int* s_tiles = s_warp;

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
    unsigned int a[2][4];load_registers(a,s_state,x);
    __syncwarp();

    {
        const ulonglong2* q = path_address(bases, prefix, T, lut, lut_sh, paths, i, x);
        ulonglong2* dst = (ulonglong2*)(s_tiles + 260u + x * 8);   // tile 1 -> buffer (1&1)=1
        v4_cp_async16(dst, q); v4_cp_async16(dst + 1, q + 1);
        asm volatile("cp.async.commit_group;");
    }
    for (unsigned int step = 1; step <= K; step++) {
        unsigned int* cur = s_tiles + (step & 1u) * 260u;
        if (step < K) {
            // The next tile offset is identical for every lane of this nonce.
            // Issue the next tile's cp.async BEFORE waiting on the current tile's shared memory.
            const ulonglong2* q = path_address(bases,prefix,T,lut,lut_sh,paths,(unsigned long long)step*n_nonces+i,x);
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
        register_step(a, cur, step, x);
    }
    __syncwarp();

    store_registers(a,s_state,x);
    __syncwarp();
    // root_K + fold, verbatim reference tail.
    unsigned int row4[V4_D4];
    #pragma unroll
    for (int k4 = 0; k4 < V4_D4; k4++) row4[k4] = s_state[x * 8u + k4];
    __syncwarp();
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
