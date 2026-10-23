"""CPU execution of the actual bounded CUDA offset and gather bodies. No GPU."""
import pathlib,subprocess,sys
s=pathlib.Path(__file__).resolve().parents[1]; out=pathlib.Path(sys.argv[1]).resolve()
if str(out)=="/tmp" or "/tmp/" in str(out):raise SystemExit("persistent output required")
out.mkdir(parents=True,exist_ok=True)
t=(s/"consensus/cuda_resident_kernels.cu").read_text()
def section(a,b):return t[t.index(a):t.index(b,t.index(a))]
body=section("__device__ __forceinline__ unsigned long long mix64", "__device__ __forceinline__ unsigned long long pom_seed_fold")
body+=section("#define V4_D", "// blake3 of")
body+=section("__device__ __forceinline__ unsigned long long v4_barrett_mod", "// Cache only")
body+=section('// Reconstruct one candidate', 'extern "C" __global__ void nexus_resident_search_preseeded(')
head=r'''#include <array>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <limits>
#define __device__
#define __global__
#define __forceinline__ inline
struct {unsigned x;} blockIdx,threadIdx;
struct alignas(16) ulonglong2 {unsigned long long x,y;};
static_assert(sizeof(ulonglong2)==16);
template<class T> T __ldg(const T* p){return *p;}
unsigned long long __umul64hi(unsigned long long a,unsigned long long b){return static_cast<unsigned __int128>(a)*b>>64;}
'''
main=r'''int main(){
 for(unsigned tiles:{2u,3u,17u,257u}){
  const unsigned chunks=tiles*32;
  // Non-aligned boundaries ensure paired tiles can straddle three tensors.
  std::vector<unsigned long long> prefix{0,1,7,33,34};
  while(prefix.back()>=chunks)prefix.pop_back();prefix.push_back(chunks);
  std::vector<std::vector<ulonglong2>> tensors;std::vector<unsigned long long> bases;
  std::vector<ulonglong2> canonical(chunks*2);
  for(unsigned i=0;i<canonical.size();++i)canonical[i]={mix64(i),mix64(i+179u)};
  for(unsigned i=0;i+1<prefix.size();++i){tensors.emplace_back(canonical.begin()+prefix[i]*2,canonical.begin()+prefix[i+1]*2);bases.push_back(reinterpret_cast<unsigned long long>(tensors.back().data()));}
  // Vector reallocation moves vector owners, not their allocations.
  std::vector<unsigned short> lut((chunks+7)/8);
  for(unsigned i=0;i<lut.size();++i){unsigned k=0;while(k+1<bases.size()&&prefix[k+1]<=i*8)++k;lut[i]=k;}
  std::vector<unsigned long long> folds(tiles);for(unsigned i=0;i<tiles;++i)folds[i]=mix64(i+19);
  for(unsigned trial=0;trial<64;++trial){
   const auto seed=trial==0?0ULL:trial==1?~0ULL:mix64(trial);
   std::array<unsigned long long,256> offsets{};blockIdx.x=0;threadIdx.x=0;
   nexus_witness_offsets(seed,tiles,~0ULL/tiles,folds.data(),offsets.data());
   auto ref=mix64(seed^V4_OFFSET_FIRST_SALT)%tiles;
   for(unsigned i=0;i<256;++i){if(offsets[i]!=ref)throw std::runtime_error("offset disagreement");if(i+1<256)ref=mix64(seed^static_cast<unsigned long long>(i+2)*V4_OFFSET_STEP_SALT^folds[ref])%tiles;}
   std::vector<ulonglong2> output(256*128);
   for(blockIdx.x=0;blockIdx.x<256;++blockIdx.x)for(threadIdx.x=0;threadIdx.x<64;++threadIdx.x)nexus_witness_pairs(bases.data(),prefix.data(),bases.size(),tiles,lut.data(),3,offsets.data(),output.data());
   for(unsigned step=0;step<256;++step)for(unsigned pair=0;pair<2;++pair){const auto off=offsets[step];const auto tile=pair&&(off^1ULL)<tiles?(off^1ULL):off;
    if(std::memcmp(output.data()+step*128+pair*64,canonical.data()+tile*64,1024))throw std::runtime_error("gather bytes disagreement");}
  }
 }
 std::cout<<"BULK_WITNESS_HOST=PASS seeds=256 steps=65536 tensor_boundaries=unaligned odd_tiles=covered\n";
}
'''
p=out/"bulk-witness-host.cpp";p.write_text(head+body+main)
subprocess.run(["/usr/bin/clang++","-std=c++20","-O2","-fsanitize=address,undefined",str(p),"-o",str(out/"bulk-witness-host")],check=True)
subprocess.run([str(out/"bulk-witness-host")],check=True)
