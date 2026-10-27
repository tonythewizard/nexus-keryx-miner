#include "keryx/nexus/consensus/pom_v5.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/gpu_preemption.hpp"
#ifdef NEXUS_TEST_HAS_CUDA
#include "keryx/nexus/consensus/cuda_resident.hpp"
#include "keryx/nexus/consensus/cuda_engine.hpp"
#include "keryx/nexus/developer_config.hpp"
#include <cuda.h>
#endif
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
namespace fs=std::filesystem;
namespace {
void require(bool b,const char*why){if(!b)throw std::runtime_error(why);}
template<class F>void rejected(F f,const char*why){bool caught=false;try{f();}catch(const std::exception&){caught=true;}require(caught,why);}
void put(std::ostream&o,std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)o.put(static_cast<char>(v>>(i*8)));}
void string(std::ostream&o,std::string_view s){put(o,s.size(),8);o.write(s.data(),static_cast<std::streamsize>(s.size()));}
std::string hex(std::span<const std::uint8_t> b){constexpr char d[]="0123456789abcdef";std::string s;for(auto v:b){s+=d[v>>4];s+=d[v&15];}return s;}
void artifact(const fs::path&path,std::uint8_t tier,const Hash&model,const Hash&root,std::uint64_t chunks,
              const V5PadSpec&pad,const Hash&pph,std::uint64_t timestamp,std::uint64_t nonce,
              const Walk&w,const V5Proof&proof) {
  Hash target{};target.fill(255);const auto seed=block_seed(pph,timestamp,nonce,pom_v5_activation_daa);
  const auto wire=borsh_v5_container(proof,final_state(w),pom_pow_value_h3(final_state(w),pph));
  std::ofstream out(path,std::ios::binary);require(bool(out),"cannot create v5 artifact");out.write("NXV5",4);put(out,tier,1);
  out.write(reinterpret_cast<const char*>(model.data()),32);out.write(reinterpret_cast<const char*>(root.data()),32);
  put(out,chunks,8);put(out,pad.tiles,8);put(out,seed,8);out.write(reinterpret_cast<const char*>(pph.data()),32);
  out.write(reinterpret_cast<const char*>(target.data()),32);put(out,timestamp,8);put(out,nonce,8);put(out,pom_v5_activation_daa,8);
  put(out,w.states.size(),4);put(out,w.offsets.size(),4);
  for(const auto&s:w.states)out.write(reinterpret_cast<const char*>(s.data()),tile_bytes);
  for(auto off:w.offsets)put(out,off,8);put(out,wire.size(),4);out.write(reinterpret_cast<const char*>(wire.data()),static_cast<std::streamsize>(wire.size()));
  require(bool(out),"v5 artifact write failed");
}
std::vector<std::vector<Hash>> levels(std::span<const std::uint8_t>blob) {
  std::vector<std::vector<Hash>>all(1);for(std::size_t i=0;i<blob.size();i+=32)all[0].push_back(blake3_hash(blob.subspan(i,32)));
  while(all.back().size()>1){const auto&old=all.back();std::vector<Hash>next;for(std::size_t i=0;i<old.size();i+=2)next.push_back(hash_pair(old[i],old[std::min(i+1,old.size()-1)]));all.push_back(std::move(next));}return all;
}
std::vector<Hash> path(const std::vector<std::vector<Hash>>&all,std::uint64_t tile) {
  std::vector<Hash>out;auto index=tile*32;
  for(std::size_t i=0;i+1<all.size();++i){if(i>=5)out.push_back(all[i][std::min(index^1,static_cast<std::uint64_t>(all[i].size()-1))]);index>>=1;}return out;
}
std::vector<std::uint8_t> blob() {
  std::vector<std::uint8_t>b(64*1024+3*32);std::uint64_t h=0xc0ffee;for(auto&v:b){h=mix64(h);v=static_cast<std::uint8_t>(h);}return b;
}
void write_gguf(const fs::path&p,const std::vector<std::uint8_t>&data) {
  std::ofstream o(p,std::ios::binary);put(o,0x46554747,4);put(o,3,4);put(o,2,8);put(o,0,8);
  auto tensor=[&](std::string_view name,std::uint64_t off,std::uint64_t bytes){string(o,name);put(o,1,4);put(o,bytes/4,8);put(o,0,4);put(o,off,8);};
  // Reversed tensor directory and a boundary inside a tile exercise descriptor lookup.
  tensor("z",63*32,data.size()-63*32);tensor("a",0,63*32);
  while(static_cast<std::uint64_t>(o.tellp())%32)o.put(0);
  o.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
}
void cpu(const fs::path&dir) {
  Hash m{},r{};m.fill(7);r.fill(9);auto pad=official_v5_pad(1,m,r);
  require(hex(pad.seed)=="cb678ed7a91fb53d40ad3b966d0219e92c75ead7761d6404bd533cc7ddf34f96","v5 pad seed vector");
  require(hex(v5_pad_entry(pad.seed,5))=="0601327f5974ca61e9a78cd447afdda4cd64050bac6ddf552ce1a165f04727e0","v5 pad entry vector");
  require(hex(blake3_hash(v5_pad_tile(pad.seed,3)))=="fa8d888e763b63542c23ad2a3e2ac715bf180f51f6b0c357a0399595593d55c2","v5 pad tile vector");
  auto data=blob();auto read=[&](std::uint64_t off){std::array<std::uint8_t,tile_bytes>t{};std::copy_n(data.data()+off*tile_bytes,tile_bytes,t.data());return t;};
  pad.tiles=61;require(final_state(walk_v5_from_model(0x55aa,64,pad,read))==5477356865520795736ULL,"official v5 walk vector");
  rejected([&]{(void)v5_pad_steps(0,1);},"empty weights accepted");rejected([&]{(void)v5_pad_steps(1,0);},"empty pad accepted");
  rejected([&]{(void)v5_pad_steps(1,UINT64_MAX);},"overflowing tile count accepted");
  rejected([&]{(void)v5_pad_tile(pad.seed,UINT64_MAX);},"overflowing pad index accepted");
  const auto all=levels(data);r=all.back()[0];Hash pph{};pph.fill(42);const auto timestamp=1700000123ULL;
  for(auto count:{1ULL,61ULL,10000ULL}) {
    pad=official_v5_pad(0,m,r);pad.tiles=count;const auto nonce=123+count;
    const auto built=prove_v5_from_model(block_seed(pph,timestamp,nonce,pom_v5_activation_daa),0,64,pad,read,[&](auto t){return path(all,t);});
    artifact(dir/("cpu-"+std::to_string(count)+".bin"),0,m,r,data.size()/32,pad,pph,timestamp,nonce,built.walk,built.proof);
    require(std::count_if(built.walk.offsets.begin(),built.walk.offsets.end(),[](auto off){return off>=64;})==v5_pad_steps(64,count),"v5 pad distribution");
    auto bad=built.proof;const auto it=std::find_if(bad.tiles.begin(),bad.tiles.end(),[](const auto&t){return t.empty();});
    bad.paths[static_cast<std::size_t>(it-bad.tiles.begin())].push_back({});
    rejected([&]{(void)borsh_v5_container(bad,0,pph);},"nonempty pad proof path accepted");
  }
  write_gguf(dir/"synthetic.gguf",data);
  std::cout<<"V5_CPU=PASS official_vectors=3 complete_walks=3 odd_tail=true invalid_inputs=rejected\n";
}
void cpu_real(const fs::path&dir,const fs::path&model_path,const fs::path&tree,const fs::path&metadata) {
  model::ReadOnlyGguf model(model_path);
  const Hash id{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
  const auto root=official_tier_anchor(h14_activation_daa,0).root;
  (void)model.verify_sidecar(tree,metadata,id,root,203469888);
  model::SparseMerkleIndex merkle(model,tree,false);const auto pad=official_v5_pad(0,id,root);
  Hash pph{};pph.fill(42);const auto timestamp=1700000123ULL;
  for(unsigned i=0;i<3;++i){const auto nonce=1234+i;
    const auto built=prove_v5_from_model(block_seed(pph,timestamp,nonce,pom_v5_activation_daa),0,model.chunk_count()/32,pad,
      [&](auto off){std::array<std::uint8_t,tile_bytes>b{};model.read_tile_into(off,b);return b;},[&](auto off){return merkle.tile_path(off);});
    artifact(dir/("real-cpu-"+std::to_string(i)+".bin"),0,id,root,model.chunk_count(),pad,pph,timestamp,nonce,built.walk,built.proof);
  }
  std::cout<<"V5_REAL_CPU=PASS complete_walks=3 official_pad_tiles="<<pad.tiles<<" submission=DISABLED\n";
}
#ifdef NEXUS_TEST_HAS_CUDA
void cu(CUresult r,const char*why){if(r!=CUDA_SUCCESS){const char*n=nullptr;cuGetErrorName(r,&n);throw std::runtime_error(std::string(why)+": "+(n?n:"CUDA error"));}}
struct Context { CUdevice d{};CUcontext c{};Context(){const auto devices=enumerate_cuda_devices();require(devices.size()==1&&devices[0].uuid==primary_gpu_uuid(),"exclusive GPU identity mismatch");cu(cuDeviceGet(&d,0),"device");cu(cuDevicePrimaryCtxRetain(&c,d),"retain test primary context");cu(cuCtxSetCurrent(c),"bind test context");}~Context(){cuDevicePrimaryCtxRelease(d);} };
struct DeviceBuffer {CUdeviceptr p{};DeviceBuffer(std::size_t n){cu(cuMemAlloc(&p,n),"test allocation");}~DeviceBuffer(){if(p)cuMemFree(p);}DeviceBuffer(const DeviceBuffer&)=delete;DeviceBuffer(DeviceBuffer&&o)noexcept:p(o.p){o.p=0;} };
std::size_t free_memory(){std::size_t f=0,t=0;cu(cuMemGetInfo(&f,&t),"test free VRAM");return f;}
void gpu(const fs::path&dir,const fs::path&model_path,const fs::path&tree_path,const fs::path&meta_path,bool real) {
  Context context;model::ReadOnlyGguf model(model_path);Hash id{};id.fill(7);Hash root{};
  std::unique_ptr<model::SparseMerkleIndex>merkle;auto synthetic=blob();auto all=levels(synthetic);
  if(real) {
    id={0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
    root=official_tier_anchor(h14_activation_daa,0).root;
    (void)model.verify_sidecar(tree_path,meta_path,id,root,203469888);
    merkle=std::make_unique<model::SparseMerkleIndex>(model,tree_path,false);
  } else root=all.back()[0];
  std::vector<DeviceBuffer>buffers;std::vector<opoi::ResidentTensor>tensors;
  const auto before=free_memory();
  {
    platform::ReadOnlyFile file(model_path);std::vector<std::uint8_t>staging(4*1024*1024);
    for(const auto&t:model.canonical_tensors()) {
      buffers.emplace_back(static_cast<std::size_t>(t.bytes));
      for(std::uint64_t off=0;off<t.bytes;off+=staging.size()) {
        const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(staging.size(),t.bytes-off));
        file.read_at_streaming(staging.data(),n,model.tensor_data_offset()+t.data_offset+off);cu(cuMemcpyHtoD(buffers.back().p+off,staging.data(),n),"bounded model upload");
      }
      tensors.push_back({t.name,static_cast<std::uintptr_t>(buffers.back().p),static_cast<std::size_t>(t.bytes),true});
    }
  }
  CudaResidentSearch engine(primary_gpu_uuid(),model,tensors);
  auto pad=official_v5_pad(0,id,root);if(!real)pad.tiles=61;
  require(engine.prepare_v5(pad),"v5 preparation failed");
  const auto bytes=engine.v5_pad_bytes();require(bytes==pad.tiles*1024,"v5 resident pad bytes wrong");
  for(auto off:std::array<std::uint64_t,3>{0,pad.tiles/2,pad.tiles-1})require(engine.read_v5_pad_tile(off)==v5_pad_tile(pad.seed,off),"GPU ARX fill differs from CPU");
  const auto read=[&](auto t){return [&]{std::array<std::uint8_t,tile_bytes>b{};model.read_tile_into(t,b);return b;}();};
  Hash pph{};pph.fill(42);Hash target{};target.fill(255);const auto timestamp=1700000123ULL;
  const unsigned cases=real?3U:12U;
  for(unsigned i=0;i<cases;++i) {
    const auto nonce=1234+i;const auto seed=block_seed(pph,timestamp,nonce,pom_v5_activation_daa);
    const auto cpu=prove_v5_from_model(seed,0,model.chunk_count()/32,pad,read,[&](auto t){return real?merkle->tile_path(t):path(all,t);});
    const auto gpu=engine.diagnose_v5(pph,timestamp,pom_v5_activation_daa,nonce);
    require(gpu.offsets==cpu.walk.offsets&&gpu.states==cpu.walk.states,"CUDA v5 full state/offset parity failure");
    artifact(dir/(std::string(real?"real-":"gpu-")+std::to_string(i)+".bin"),0,id,root,model.chunk_count(),pad,pph,timestamp,nonce,gpu,cpu.proof);
  }
  // Exercise target screening and partial final blocks against independently computed CPU walks.
  const auto count=real?5U:9U;Hash minimum{};minimum.fill(255);std::uint64_t expected=UINT64_MAX;
  for(unsigned i=0;i<count;++i){const auto nonce=2000+i;const auto w=walk_v5_from_model(block_seed(pph,timestamp,nonce,pom_v5_activation_daa),model.chunk_count()/32,pad,read);const auto pow=pom_pow_value_h3(final_state(w),pph);
    if(little_endian_leq(pow,minimum)){minimum=pow;expected=nonce;}}
  const auto found=engine.run(pph,timestamp,pom_v5_activation_daa,minimum,2000,count);
  require(found.winner&&*found.winner==expected&&found.completed==count,"v5 target-screened search disagrees with CPU");
  auto below=minimum;for(auto&v:below){if(v-- !=0)break;}
  require(!engine.run(pph,timestamp,pom_v5_activation_daa,below,2000,count).winner,"v5 below-minimum target accepted");
  require(engine.run(pph,timestamp,pom_v5_activation_daa,target,2000,count).winner==2000,"v5 lowest winning nonce not selected");
  rejected([&]{(void)engine.read_witness_tiles(1);},"fold-only v4 witness used in v5");
  rejected([&]{(void)engine.run(pph,timestamp,h14_activation_daa,target,1,1);},"v5 pad used in pre-v5 era");
  GpuPreemptionCoordinator ownership;unsigned cycles=0;
  for(bool fail_inference:{false,true}) {
    require(ownership.run_pom([&]{(void)engine.run(pph,timestamp,pom_v5_activation_daa,target,3000,1,[&](bool){ownership.announce_opoi();});}),"PoM admission failed");
    require(!ownership.run_pom([]{}),"AI priority did not close admission");
    const auto occupied=free_memory();bool restored=false;
    auto handoff=[&]{ownership.run_opoi([&]{engine.release_v5();require(!engine.v5_ready_for(pad)&&engine.v5_pad_bytes()==0,"pad remains ready during AI");
        const auto released=free_memory();require(released+4*1024*1024>=occupied+bytes,"pad VRAM was not released");},
      [&]{require(!engine.v5_ready_for(pad),"AI ran with a resident PoM pad");DeviceBuffer ai_scratch(8*1024*1024);cu(cuMemsetD8(ai_scratch.p,0x5a,8*1024*1024),"mock AI GPU work");if(fail_inference)throw std::runtime_error("injected AI fixture error");},
      [&]{require(engine.prepare_v5(pad),"v5 refill failed after AI");restored=true;});};
    if(fail_inference)rejected(handoff,"AI exception disappeared");else handoff();
    require(restored&&!ownership.opoi_pending()&&engine.v5_ready_for(pad),"v5 restore/ownership state invalid");
    require(engine.read_v5_pad_tile(pad.tiles-1)==v5_pad_tile(pad.seed,pad.tiles-1),"v5 refill changed pad bytes");
    require(ownership.run_pom([&]{require(engine.run(pph,timestamp,pom_v5_activation_daa,minimum,2000,count).winner==expected,"v5 search changed after AI resume");}),"PoM not resumed");++cycles;
  }
  engine.release_v5();rejected([&]{(void)engine.run(pph,timestamp,pom_v5_activation_daa,target,1,1);},"v5 search used a released pad");
  auto interrupted=pad;interrupted.tiles=4097;unsigned polls=0;
  require(!engine.prepare_v5(interrupted,[&]{return ++polls>=3;}),"v5 fill cancellation ignored");
  require(engine.v5_pad_bytes()==0&&!engine.v5_ready_for(interrupted),"partial pad published as ready");
  require(!engine.prepare_v5(pad,[]{return true;}),"initial AI cancellation ignored");
  auto oversized=pad;oversized.tiles=free_memory()/1024+1024*1024;
  rejected([&]{(void)engine.prepare_v5(oversized);},"v5 VRAM admission accepted oversized pad");require(engine.v5_pad_bytes()==0,"allocation failure left a usable pad");
  require(engine.prepare_v5(pad),"cannot restore after fill cancellation");
  engine.release_v5();
  // Regression of the untouched v4 kernel on the same model/stream after releasing v5.
  const auto v4_seed=block_seed(pph,timestamp,4000,h14_activation_daa);
  const auto gathered=gather_v4_model_tiles(v4_seed,model.chunk_count()/32,read);
  const auto v4_walk=walk_v4_from_tile_sequence(v4_seed,model.chunk_count()/32,gathered.tiles);
  const auto v4_target=pom_pow_value_h3(final_state(v4_walk),pph);
  require(engine.run(pph,timestamp,h14_activation_daa,v4_target,4000,1).winner==4000,"v4 target parity failed after v5 release");
  auto v4_below=v4_target;for(auto&v:v4_below){if(v-- !=0)break;}
  require(!engine.run(pph,timestamp,h14_activation_daa,v4_below,4000,1).winner,"v4 below-target regression after v5 release");
  const auto witness=engine.read_witness_tiles(v4_seed);
  require(std::equal(witness.offsets.begin(),witness.offsets.end(),gathered.offsets.begin(),gathered.offsets.end()),"v4 witness offsets changed");
  for(std::size_t step=0;step<walk_steps;++step)
    require(std::equal(witness.pairs[step][0].begin(),witness.pairs[step][0].end(),gathered.tiles.begin()+static_cast<std::ptrdiff_t>(step*tile_bytes)),"v4 witness bytes changed");
  std::cout<<"V5_GPU=PASS real_weights="<<real<<" complete_walks="<<cases<<" states_per_walk=257 offsets_per_walk=256 pad_bytes="<<bytes
    <<" ai_handoff_cycles="<<cycles<<" inference=GPU_MEMORY_MOCK cancellation=PASS vram_rejection=PASS target_screening=PASS v4_regression=PASS free_before_model="<<before<<" free_after_pad_release="<<free_memory()<<" submission=DISABLED\n";
}
#endif
}
int main(int argc,char**argv){try {
  require(argc>=3,"usage: v5-test --cpu|--gpu DIR | --gpu-real DIR MODEL TREE META");const fs::path dir=argv[2];fs::create_directories(dir);
  const std::string mode=argv[1];
  if(mode=="--cpu"){require(argc==3,"unexpected CPU arguments");cpu(dir);return 0;}
  if(mode=="--cpu-real"){require(argc==6,"missing real model/tree/meta");cpu_real(dir,argv[3],argv[4],argv[5]);return 0;}
#ifdef NEXUS_TEST_HAS_CUDA
  if(mode=="--gpu"){require(argc==3,"unexpected GPU arguments");gpu(dir,dir/"synthetic.gguf",{},{},false);return 0;}
  if(mode=="--gpu-real"){require(argc==6,"missing real model/tree/meta");gpu(dir,argv[3],argv[4],argv[5],true);return 0;}
#endif
  throw std::runtime_error("unsupported v5 test mode");
}catch(const std::exception&e){std::cerr<<"V5_TEST=FAIL "<<e.what()<<'\n';return 1;}}
