#include "keryx/nexus/developer_config.hpp"
#include "keryx/nexus/consensus/cuda_resident.hpp"
#include <cuda.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
namespace keryx::nexus::consensus {
namespace {

extern "C" const unsigned char _binary_nexus_resident_cubin_start[];
void check(CUresult r,const char *where) {
  if(r==CUDA_SUCCESS)return;
  const char *name=nullptr,*detail=nullptr;cuGetErrorName(r,&name);cuGetErrorString(r,&detail);
  throw std::runtime_error(std::string(where)+": "+(name?name:"UNKNOWN")+" "+(detail?detail:""));
}
std::string uuid_text(const CUuuid &u) {
  std::ostringstream s;s<<"GPU-"<<std::hex<<std::setfill('0');
  for(unsigned i=0;i<16;++i){if(i==4||i==6||i==8||i==10)s<<'-';s<<std::setw(2)<<unsigned(static_cast<unsigned char>(u.bytes[i]));}return s.str();
}
struct Context {CUdevice device{};CUcontext value{};bool retained{};
  ~Context(){if(retained)cuDevicePrimaryCtxRelease(device);}
};
struct Buffer {CUdeviceptr ptr{};CUcontext context{};
  Buffer()=default;Buffer(const Buffer&)=delete;Buffer&operator=(const Buffer&)=delete;
  Buffer(Buffer&&o)noexcept:ptr(o.ptr),context(o.context){o.ptr=0;}
  ~Buffer(){if(ptr){cuCtxSetCurrent(context);cuMemFree(ptr);}}
  void allocate(std::size_t n){check(cuCtxGetCurrent(&context),"allocation context");check(cuMemAlloc(&ptr,n),"resident cuMemAlloc");}
};
struct Resources {CUcontext context{};CUmodule module{};CUstream stream{};CUevent begin{},end{};
  ~Resources(){if(context)cuCtxSetCurrent(context);if(stream)cuStreamSynchronize(stream);if(begin)cuEventDestroy(begin);if(end)cuEventDestroy(end);if(stream)cuStreamDestroy(stream);if(module)cuModuleUnload(module);}
};
std::uint64_t le64(const std::uint8_t *p){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
}
struct CudaResidentSearch::Impl {
  Context ctx; // last released: model context is never reset
  std::vector<Buffer> uploads;
  Buffer bases,prefix,lut,winner,sponge,h10_seeds,tile_folds,witness_offsets,witness_pairs;
  ResidentWitnessTiles host_witness; // One bounded staging buffer; released with PoM on AI preemption.
  Resources resources;
  CUfunction kernel{},seed_kernel{},fold_kernel{},witness_offsets_kernel{},witness_pairs_kernel{};
  std::uint32_t tensor_count{},shift{12};
  std::uint64_t tiles{},reciprocal{};
  std::vector<std::uint64_t> canonical_bases,canonical_prefix;
  std::vector<std::uint16_t> host_lut;
  Impl(const std::string &uuid,const model::ReadOnlyGguf &gguf,const std::vector<opoi::ResidentTensor>&tensors){
    const char *visible=std::getenv("CUDA_VISIBLE_DEVICES");
    if(uuid!=primary_gpu_uuid()||!visible||uuid!=visible)throw std::runtime_error("resident solver requires exclusive authorized GPU UUID");
    check(cuInit(0),"cuInit");int count=0;check(cuDeviceGetCount(&count),"visible device count");
    if(count!=1)throw std::runtime_error("resident solver sees more than one device");
    check(cuDeviceGet(&ctx.device,0),"select visible device");CUuuid id{};check(cuDeviceGetUuid(&id,ctx.device),"verify UUID");
    if(uuid_text(id)!=uuid)throw std::runtime_error("resident solver UUID mismatch");
    int major=0,minor=0;check(cuDeviceGetAttribute(&major,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,ctx.device),"SM major");check(cuDeviceGetAttribute(&minor,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,ctx.device),"SM minor");
    if(major!=8||minor!=6)throw std::runtime_error("resident solver requires SM86");
    check(cuDevicePrimaryCtxRetain(&ctx.value,ctx.device),"retain shared primary context");ctx.retained=true;
    check(cuCtxSetCurrent(ctx.value),"bind shared primary context");resources.context=ctx.value;
    const auto &canonical=gguf.canonical_tensors();
    if(tensors.size()!=canonical.size()||tensors.empty()||tensors.size()>65535)throw std::runtime_error("canonical resident tensor count mismatch");
    std::vector<std::uint64_t> host_bases,host_prefix{0};
    std::size_t host_uploaded=0,borrowed=0;
    for(std::size_t i=0;i<tensors.size();++i){const auto &t=tensors[i];const auto &g=canonical[i];
      if(t.name!=g.name||t.bytes!=g.bytes||!t.address)throw std::runtime_error("resident tensor name/size mismatch");
      const auto chunks=t.bytes/32;if(!chunks)continue;
      CUdeviceptr ptr=t.address;
      if(t.device){CUcontext owner{};int ordinal=-1;
        check(cuPointerGetAttribute(&owner,CU_POINTER_ATTRIBUTE_CONTEXT,ptr),"tensor context");
        check(cuPointerGetAttribute(&ordinal,CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL,ptr),"tensor device");
        if(owner!=ctx.value||ordinal!=0)throw std::runtime_error("resident tensor belongs to an unauthorized context/device");++borrowed;
      }else{
        // Official Qwen model has exactly one CPU embedding tensor; no full GGUF duplication.
        if(host_uploaded||t.name!="token_embd.weight"||t.bytes!=699269120)
          throw std::runtime_error("unexpected host resident tensor: "+t.name+" bytes="+std::to_string(t.bytes));
        std::size_t free=0,total=0;check(cuMemGetInfo(&free,&total),"resident upload VRAM budget");
        if(free<t.bytes+64ULL*1024*1024)throw std::runtime_error("insufficient VRAM for canonical embedding upload and bounded PoM workspace");
        uploads.emplace_back();uploads.back().allocate(t.bytes);ptr=uploads.back().ptr;
        check(cuMemcpyHtoD(ptr,reinterpret_cast<const void*>(t.address),t.bytes),"upload CPU resident tensor");host_uploaded+=t.bytes;
      }
      host_bases.push_back(ptr);host_prefix.push_back(host_prefix.back()+chunks);
    }
    if(host_prefix.back()!=gguf.chunk_count()||host_prefix.back()%32)throw std::runtime_error("canonical resident chunk count mismatch");
    canonical_bases=host_bases;canonical_prefix=host_prefix;
    tensor_count=static_cast<std::uint32_t>(host_bases.size());tiles=host_prefix.back()/32;
    reciprocal=std::numeric_limits<std::uint64_t>::max()/tiles;
    // Verify canonical bytes at EVERY tensor boundary and 129 deterministic samples.
    auto compare_chunk=[&](std::uint64_t chunk){
      const auto j=static_cast<std::size_t>(std::upper_bound(host_prefix.begin(),host_prefix.end(),chunk)-host_prefix.begin()-1);
      std::array<std::uint8_t,32> gpu{};std::array<std::uint8_t,1024> tile{};
      check(cuMemcpyDtoH(gpu.data(),host_bases[j]+(chunk-host_prefix[j])*32,32),"resident byte gate");
      gguf.read_tile_into(chunk/32,tile);
      if(!std::equal(gpu.begin(),gpu.end(),tile.begin()+static_cast<std::ptrdiff_t>((chunk%32)*32)))throw std::runtime_error("resident canonical bytes do not match official GGUF");
    };
    for(std::size_t j=0;j<host_bases.size();++j){compare_chunk(host_prefix[j]);compare_chunk(host_prefix[j+1]-1);}
    for(std::uint64_t i=0;i<129;++i)compare_chunk((i*(host_prefix.back()-1))/128);
    const auto entries=static_cast<std::size_t>(((host_prefix.back()-1)>>shift)+1);
    std::vector<std::uint16_t> host_lut(entries);
    for(std::size_t i=0;i<entries;++i){const auto chunk=std::uint64_t(i)<<shift;const auto j=std::upper_bound(host_prefix.begin(),host_prefix.end(),chunk)-host_prefix.begin()-1;host_lut[i]=static_cast<std::uint16_t>(j);}
    this->host_lut=host_lut;
    bases.allocate(host_bases.size()*8);prefix.allocate(host_prefix.size()*8);lut.allocate(host_lut.size()*2);winner.allocate(8);sponge.allocate(25*8);h10_seeds.allocate(18432*8);
    check(cuMemcpyHtoD(bases.ptr,host_bases.data(),host_bases.size()*8),"copy tensor descriptors");check(cuMemcpyHtoD(prefix.ptr,host_prefix.data(),host_prefix.size()*8),"copy canonical prefix");check(cuMemcpyHtoD(lut.ptr,host_lut.data(),host_lut.size()*2),"copy chunk LUT");
    check(cuModuleLoadData(&resources.module,_binary_nexus_resident_cubin_start),"load resident SM86 cubin");check(cuModuleGetFunction(&kernel,resources.module,"nexus_resident_search_preseeded"),"resident kernel entry point");
    check(cuModuleGetFunction(&seed_kernel,resources.module,"nexus_h10_seeds"),"H10 seed entry point");
    check(cuModuleGetFunction(&fold_kernel,resources.module,"nexus_tile_folds"),"immutable tile fold entry point");
    check(cuModuleGetFunction(&witness_offsets_kernel,resources.module,"nexus_witness_offsets"),"witness offsets entry point");
    check(cuModuleGetFunction(&witness_pairs_kernel,resources.module,"nexus_witness_pairs"),"witness pairs entry point");
    witness_offsets.allocate(walk_steps*sizeof(std::uint64_t));
    witness_pairs.allocate(walk_steps*2*tile_bytes);
    const auto fold_bytes=((tiles+1)&~std::uint64_t(1))*8;
    std::size_t free=0,total=0;check(cuMemGetInfo(&free,&total),"tile fold cache VRAM budget");
    if(free<fold_bytes+64ULL*1024*1024)throw std::runtime_error("insufficient VRAM for bounded immutable tile fold cache");
    tile_folds.allocate(fold_bytes);
    if(fold_bytes>tiles*8)check(cuMemsetD8(tile_folds.ptr+tiles*8,0,fold_bytes-tiles*8),"initialize tile fold padding");
    check(cuStreamCreate(&resources.stream,CU_STREAM_NON_BLOCKING),"resident stream");check(cuEventCreate(&resources.begin,0),"resident begin event");check(cuEventCreate(&resources.end,0),"resident end event");
    void *fold_params[]={&bases.ptr,&prefix.ptr,&tensor_count,&tiles,&lut.ptr,&shift,&tile_folds.ptr};
    check(cuLaunchKernel(fold_kernel,static_cast<unsigned>((tiles+127)/128),1,1,128,1,1,0,resources.stream,fold_params,nullptr),"prepare immutable tile folds");
    check(cuStreamSynchronize(resources.stream),"complete immutable tile folds");
    // Check folds at every tensor boundary plus dispersed deterministic samples.
    auto verify_fold=[&](std::uint64_t tile){
      std::array<std::uint8_t,tile_bytes> bytes{};gguf.read_tile_into(tile,bytes);
      std::uint64_t expected=0,actual=0;
      for(unsigned wd=0;wd<8;++wd){std::uint64_t word=0;for(unsigned j=0;j<4;++j)word|=std::uint64_t(bytes[wd*4+j])<<(8*j);expected=mix64(expected^word);}
      check(cuMemcpyDtoH(&actual,tile_folds.ptr+tile*8,8),"immutable tile fold byte gate");
      if(actual!=expected)throw std::runtime_error("immutable tile fold disagrees with canonical GGUF bytes");
    };
    for(std::size_t j=0;j<canonical_bases.size();++j){verify_fold(canonical_prefix[j]/32);verify_fold((canonical_prefix[j+1]-1)/32);}
    for(std::uint64_t i=0;i<129;++i)verify_fold(i*(tiles-1)/128);
    std::cout<<"RESIDENT_MODEL=PASS tensors="<<tensor_count<<" borrowed_device_tensors="<<borrowed<<" host_uploaded_bytes="<<host_uploaded<<" byte_gate=all_tensor_boundaries_plus_129 primary_context=shared kernel=SM86_INT8_NCF seed=H10_PRECOMPUTED scratch_bytes=147456 tile_fold_bytes="<<fold_bytes<<" fold_gate=all_tensor_boundaries_plus_129\n"<<std::flush;
  }
};
CudaResidentSearch::CudaResidentSearch(const std::string &u,const model::ReadOnlyGguf &g,const std::vector<opoi::ResidentTensor>&t):impl_(std::make_unique<Impl>(u,g,t)){}
CudaResidentSearch::~CudaResidentSearch()=default;
std::array<std::uint8_t,tile_bytes> CudaResidentSearch::read_tile(std::uint64_t tile) {
  auto &p=*impl_;
  if(tile>=p.tiles)throw std::out_of_range("resident tile outside canonical model");
  CUcontext cur=nullptr;
  if(cuCtxGetCurrent(&cur)!=CUDA_SUCCESS||cur!=p.ctx.value)
    check(cuCtxSetCurrent(p.ctx.value),"bind resident tile read context");
  std::array<std::uint8_t,tile_bytes> bytes{};
  std::uint64_t chunk=tile*32;std::size_t copied=0;
  while(copied<32) {
    const auto hint=(!p.host_lut.empty()&&(chunk>>p.shift)<p.host_lut.size())?p.host_lut[chunk>>p.shift]:0;
    const auto tensor=static_cast<std::size_t>(std::upper_bound(p.canonical_prefix.begin()+hint,p.canonical_prefix.end(),chunk)-p.canonical_prefix.begin()-1);
    if(tensor>=p.canonical_bases.size())throw std::runtime_error("resident canonical tile descriptor mismatch");
    const auto chunks=std::min<std::uint64_t>(32-copied,p.canonical_prefix[tensor+1]-chunk);
    if(!chunks)throw std::runtime_error("resident canonical tile segment is empty");
    check(cuMemcpyDtoH(bytes.data()+copied*32,p.canonical_bases[tensor]+(chunk-p.canonical_prefix[tensor])*32,chunks*32),"read canonical resident tile");
    copied+=chunks;chunk+=chunks;
  }
  return bytes;
}
std::array<std::array<std::uint8_t,tile_bytes>,2> CudaResidentSearch::read_tile_pair(std::uint64_t tile) {
  auto &p=*impl_;
  if(tile>=p.tiles)throw std::out_of_range("resident tile pair outside canonical model");
  const auto first=tile&~std::uint64_t(1);
  if(first+1>=p.tiles)throw std::out_of_range("resident tile pair lacks canonical sibling");
  CUcontext cur=nullptr;
  if(cuCtxGetCurrent(&cur)!=CUDA_SUCCESS||cur!=p.ctx.value)
    check(cuCtxSetCurrent(p.ctx.value),"bind resident tile pair context");
  std::array<std::uint8_t,2*tile_bytes> bytes{};
  std::uint64_t chunk=first*32;std::size_t copied=0;
  while(copied<64) {
    const auto hint=(!p.host_lut.empty()&&(chunk>>p.shift)<p.host_lut.size())?p.host_lut[chunk>>p.shift]:0;
    const auto tensor=static_cast<std::size_t>(std::upper_bound(p.canonical_prefix.begin()+hint,p.canonical_prefix.end(),chunk)-p.canonical_prefix.begin()-1);
    if(tensor>=p.canonical_bases.size())throw std::runtime_error("resident tile pair descriptor mismatch");
    const auto chunks=std::min<std::uint64_t>(64-copied,p.canonical_prefix[tensor+1]-chunk);
    if(!chunks)throw std::runtime_error("resident tile pair segment is empty");
    check(cuMemcpyDtoH(bytes.data()+copied*32,p.canonical_bases[tensor]+(chunk-p.canonical_prefix[tensor])*32,chunks*32),"read canonical resident siblings");
    copied+=chunks;chunk+=chunks;
  }
  std::array<std::array<std::uint8_t,tile_bytes>,2> result{};
  const auto position=static_cast<std::size_t>(tile&1);
  std::copy_n(bytes.data()+position*tile_bytes,tile_bytes,result[0].data());
  std::copy_n(bytes.data()+(position^1)*tile_bytes,tile_bytes,result[1].data());
  return result;
}
ResidentWitnessTiles CudaResidentSearch::read_witness_tiles(std::uint64_t seed) {
  ResidentWitnessTiles result;
  read_witness_tiles_into(seed,result);
  return result;
}
void CudaResidentSearch::read_witness_tiles_into(std::uint64_t seed, ResidentWitnessTiles &result) {
  auto &p=*impl_;
  check(cuCtxSetCurrent(p.ctx.value),"bind witness gather context");
  result.pairs.resize(walk_steps);
  void *offset_params[]={&seed,&p.tiles,&p.reciprocal,&p.tile_folds.ptr,&p.witness_offsets.ptr};
  check(cuLaunchKernel(p.witness_offsets_kernel,1,1,1,1,1,1,0,p.resources.stream,offset_params,nullptr),"reconstruct witness offsets");
  void *pair_params[]={&p.bases.ptr,&p.prefix.ptr,&p.tensor_count,&p.tiles,&p.lut.ptr,&p.shift,&p.witness_offsets.ptr,&p.witness_pairs.ptr};
  check(cuLaunchKernel(p.witness_pairs_kernel,walk_steps,1,1,64,1,1,0,p.resources.stream,pair_params,nullptr),"gather witness tile siblings");
  check(cuStreamSynchronize(p.resources.stream),"complete bounded witness gather");
  check(cuMemcpyDtoH(result.offsets.data(),p.witness_offsets.ptr,sizeof(result.offsets)),"read witness offsets");
  check(cuMemcpyDtoH(result.pairs.data(),p.witness_pairs.ptr,walk_steps*2*tile_bytes),"read witness tile siblings");
  for(auto off:result.offsets)if(off>=p.tiles)throw std::runtime_error("witness offset outside canonical model");
}
const ResidentWitnessTiles &CudaResidentSearch::read_witness_tiles_cached(std::uint64_t seed) {
  read_witness_tiles_into(seed,impl_->host_witness);
  return impl_->host_witness;
}
bool CudaResidentSearch::diagnostic_headroom_available() const {
  check(cuCtxSetCurrent(impl_->ctx.value),"bind diagnostic budget context");
  std::size_t free=0,total=0;check(cuMemGetInfo(&free,&total),"query diagnostic coexistence budget");
  // Preserve the verifier's 256 MiB reserve plus 8 MiB for its one-proof
  // cache (256 KiB), bounded staging/trace and module overhead.
  return free>=264ULL*1024*1024;
}
ResidentSearchResult CudaResidentSearch::run(const Hash &pph,std::uint64_t timestamp,std::uint64_t daa,const Hash &target,std::uint64_t start,std::uint64_t count,const std::function<void(bool)> &after_enqueue){
  if(!count||count>18432||start==UINT64_MAX||count>UINT64_MAX-start)throw std::invalid_argument("resident nonce range outside bounded nonwrapping interval");
  auto &p=*impl_;check(cuCtxSetCurrent(p.ctx.value),"bind resident context on current thread");
  const auto state=seed_sponge_state(pph,timestamp,daa);check(cuMemcpyHtoD(p.sponge.ptr,state.data(),200),"copy seed sponge");
  const std::uint64_t none=UINT64_MAX;check(cuMemcpyHtoD(p.winner.ptr,&none,8),"reset winner");
  constexpr std::uint64_t salt[4]={0x7C99D381176D4EC4ULL,0xC2E28E3E28118C36ULL,0xD496CE1B129B76CAULL,0x47CF0979FA580BCEULL};
  std::uint64_t words[4],targets[4],unused[4]{};for(unsigned i=0;i<4;++i){words[i]=le64(pph.data()+8*i)^salt[i];targets[i]=le64(target.data()+8*i);}
  std::uint32_t k=256,seed_h10=1,precomputed=1;
  void *params[]={&p.bases.ptr,&p.prefix.ptr,&p.tensor_count,&p.tiles,&k,&words[0],&words[1],&words[2],&words[3],&unused[0],&unused[1],&unused[2],&unused[3],&timestamp,&targets[0],&targets[1],&targets[2],&targets[3],&start,&count,&p.lut.ptr,&p.shift,&p.reciprocal,&p.winner.ptr,&p.sponge.ptr,&seed_h10,&p.h10_seeds.ptr,&precomputed,&p.tile_folds.ptr};
  check(cuEventRecord(p.resources.begin,p.resources.stream),"record resident launch");
  // Include seed preparation in the measured batch time. Stream ordering
  // makes its output visible without a host or device-wide synchronization.
  void *seed_params[]={&p.sponge.ptr,&start,&count,&p.h10_seeds.ptr};
  check(cuLaunchKernel(p.seed_kernel,static_cast<unsigned>((count+127)/128),1,1,128,1,1,0,p.resources.stream,seed_params,nullptr),"prepare H10 seeds");
  check(cuLaunchKernel(p.kernel,static_cast<unsigned>((count+3)/4),1,1,128,1,1,12416,p.resources.stream,params,nullptr),"launch resident PoM");
  check(cuEventRecord(p.resources.end,p.resources.stream),"record resident completion");if(after_enqueue){const auto status=cuEventQuery(p.resources.end);if(status!=CUDA_SUCCESS&&status!=CUDA_ERROR_NOT_READY)check(status,"resident completion query");after_enqueue(status==CUDA_ERROR_NOT_READY);}
  check(cuEventSynchronize(p.resources.end),"complete resident PoM");
  float elapsed=0;check(cuEventElapsedTime(&elapsed,p.resources.begin,p.resources.end),"resident kernel elapsed");
  std::uint64_t found=none;check(cuMemcpyDtoH(&found,p.winner.ptr,8),"read winning nonce");
  if(found!=none&&(found<start||found-start>=count))throw std::runtime_error("resident GPU returned out-of-range nonce");
  return {found==none?std::nullopt:std::optional(found),count,elapsed};
}
}
