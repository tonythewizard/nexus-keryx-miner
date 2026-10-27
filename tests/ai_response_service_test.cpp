#include "keryx/nexus/protocol_rules.hpp"
#include "keryx/nexus/ai_response_service.hpp"
#include "keryx/nexus/consensus/cuda_engine.hpp"
#include "keryx/nexus/consensus/cuda_resident.hpp"
#include "keryx/nexus/consensus/resident_batch_policy.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/consensus/verified_walk_view.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/gpu_preemption.hpp"
#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/experimental_vulkan_inference.hpp"
#include "keryx/nexus/vulkan_inference_process.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/capture.hpp"
#ifdef NEXUS_TEST_HAS_VULKAN
#include "dual_gpu_ai_fixture.hpp"
#endif
#include "messages.grpc.pb.h"
#include <blake2.h>
#include <grpcpp/grpcpp.h>
#ifdef NEXUS_TEST_HAS_CUDA
#include <cuda.h>
#endif
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <cerrno>
#include <spawn.h>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace fs = std::filesystem;
using namespace keryx::nexus::ai;
using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
extern char **environ;
namespace {
#include "ai_response_fixture_support.hpp"

std::string read_text(const fs::path &path){std::ifstream in(path);return in?std::string(std::istreambuf_iterator<char>(in),{}):std::string{};}
std::string cgroup_v2_path(){std::istringstream in(read_text("/proc/self/cgroup"));std::string line;while(std::getline(in,line)){if(line.starts_with("0::"))return "/sys/fs/cgroup"+line.substr(3);}return {};}
void memory_profile(std::string_view phase){
 const auto cg=cgroup_v2_path();std::map<std::string,std::uint64_t> stat;
 if(!cg.empty()){std::istringstream in(read_text(fs::path(cg)/"memory.stat"));std::string key;std::uint64_t value;while(in>>key>>value)stat[key]=value;}
 std::uint64_t current=0,peak=0,swap=0;auto read_number=[](const fs::path &p)->std::uint64_t{try{return static_cast<std::uint64_t>(std::stoull(read_text(p)));}catch(...){return 0;}};
 if(!cg.empty()){current=read_number(fs::path(cg)/"memory.current");peak=read_number(fs::path(cg)/"memory.peak");swap=read_number(fs::path(cg)/"memory.swap.current");}
 std::string rss="unknown",anon="unknown",file="unknown",shmem="unknown";std::istringstream smaps(read_text("/proc/self/smaps_rollup"));std::string line;std::getline(smaps,line);std::string key;std::uint64_t kb;while(smaps>>key>>kb){if(key=="Rss:")rss=std::to_string(kb*1024);else if(key=="Pss_Anon:")anon=std::to_string(kb*1024);else if(key=="Pss_File:")file=std::to_string(kb*1024);else if(key=="Pss_Shmem:")shmem=std::to_string(kb*1024);std::string rest;std::getline(smaps,rest);}
 std::cout<<"CUDA_MEMORY phase="<<phase<<" current="<<current<<" peak="<<peak<<" swap="<<swap<<" anon="<<stat["anon"]<<" file="<<stat["file"]<<" shmem="<<stat["shmem"]<<" kernel="<<stat["kernel"]<<" rss="<<rss<<" pss_anon="<<anon<<" pss_file="<<file<<" pss_shmem="<<shmem<<"\n"<<std::flush;
}

// The stock loader hashes and then maps the official GGUF. The hash pass can
// leave several GiB of clean file cache charged to a constrained test cgroup;
// this test-only guard asks the kernel to discard clean cache while preserving
// the exact file bytes and real inference path.
class GgufCacheGuard final {
public:
 explicit GgufCacheGuard(const fs::path &path):fd_(::open(path.c_str(),O_RDONLY|O_CLOEXEC)){
  if(fd_<0)throw std::runtime_error("cannot open GGUF for test cache accounting");
  struct stat st{};if(::fstat(fd_,&st)!=0){::close(fd_);fd_=-1;throw std::runtime_error("cannot stat GGUF for test cache accounting");}
  length_=st.st_size;memory_profile("inference_before_runtime");
  thread_=std::thread([this]{
    auto next_profile=std::chrono::steady_clock::now();
    while(!stop_.load(std::memory_order_relaxed)) {
      // A fast GGUF hash pass can populate >1 GiB in the old 500 ms interval.
      // Discard only clean file cache promptly; never unmap model pages, alter
      // bytes, use drop_caches, or replace real CUDA inference.
      const int rc=::posix_fadvise(fd_,0,length_,POSIX_FADV_DONTNEED);
      if(rc!=0)std::cerr<<"CUDA_CACHE_ADVICE errno="<<rc<<"\n";
      const auto now=std::chrono::steady_clock::now();
      if(now>=next_profile){memory_profile("inference_runtime_active");next_profile=now+std::chrono::milliseconds(500);}
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });
 }
 ~GgufCacheGuard(){stop_.store(true,std::memory_order_relaxed);if(thread_.joinable())thread_.join();if(fd_>=0)::close(fd_);memory_profile("inference_runtime_released");}
 GgufCacheGuard(const GgufCacheGuard&)=delete;GgufCacheGuard&operator=(const GgufCacheGuard&)=delete;
private:int fd_{-1};off_t length_{};std::atomic_bool stop_{};std::thread thread_;
};

struct RealPomContext {
 std::uint64_t seed{},chunks{},timestamp{},nonce{},daa{},generation{},final{};
 std::uint8_t tier{};
 Hash root{},target{},pph{},capture_digest{},pow{};
};
std::uint32_t read32(std::span<const std::uint8_t> b,std::size_t &p){if(p+4>b.size())throw std::runtime_error("truncated CUDA fixture u32");std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(b[p++])<<(i*8U);return v;}
std::uint64_t read64(std::span<const std::uint8_t> b,std::size_t &p){if(p+8>b.size())throw std::runtime_error("truncated CUDA fixture u64");std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(b[p++])<<(i*8U);return v;}
template<class T> T read_array(std::span<const std::uint8_t> b,std::size_t &p){if(p+sizeof(T)>b.size())throw std::runtime_error("truncated CUDA fixture array");T v{};std::copy_n(b.data()+p,sizeof(T),v.data());p+=sizeof(T);return v;}
#ifdef NEXUS_TEST_HAS_CUDA
void assert_primary_cuda_context(CUcontext &baseline){
 CUcontext current{};if(cuCtxGetCurrent(&current)!=CUDA_SUCCESS||!current)throw std::runtime_error("CUDA operation has no current context");
 CUdevice device{};if(cuCtxGetDevice(&device)!=CUDA_SUCCESS)throw std::runtime_error("cannot identify current CUDA context device");
 CUcontext primary{};if(cuDevicePrimaryCtxRetain(&primary,device)!=CUDA_SUCCESS)throw std::runtime_error("cannot inspect CUDA primary context");
 const auto release=cuDevicePrimaryCtxRelease(device);if(release!=CUDA_SUCCESS)throw std::runtime_error("cannot release CUDA context inspection reference");
 if(current!=primary)throw std::runtime_error("PoM/OPoI operation used a non-primary CUDA context");
 if(!baseline)baseline=current;else if(baseline!=current)throw std::runtime_error("CUDA primary context changed across PoM/OPoI transition");
}
// An out-of-process Vulkan inference must preserve the caller's CUDA binding,
// which may legitimately be null after releasing Driver API PoM resources.
// Validate the retained primary context explicitly, then restore that binding.
void assert_retained_primary_cuda_context(CUcontext &baseline){
 if(!baseline||cuCtxPushCurrent(baseline)!=CUDA_SUCCESS)throw std::runtime_error("Retained primary CUDA context unavailable after AMD inference");
 try {assert_primary_cuda_context(baseline);}catch(...){CUcontext popped{};cuCtxPopCurrent(&popped);throw;}
 CUcontext popped{};if(cuCtxPopCurrent(&popped)!=CUDA_SUCCESS||popped!=baseline)
   throw std::runtime_error("Primary CUDA context inspection did not restore the caller binding");
}
#else
void assert_primary_cuda_context(void*){}
#endif
void put(std::ostream &o,const void *p,std::size_t n){o.write(static_cast<const char*>(p),static_cast<std::streamsize>(n));if(!o)throw std::runtime_error("CUDA proof artifact write failed");}
void put32(std::ostream &o,std::uint32_t v){std::array<std::uint8_t,4>b{};for(unsigned i=0;i<4;++i)b[i]=static_cast<std::uint8_t>(v>>(i*8U));put(o,b.data(),b.size());}
void put64(std::ostream &o,std::uint64_t v){std::array<std::uint8_t,8>b{};for(unsigned i=0;i<8;++i)b[i]=static_cast<std::uint8_t>(v>>(i*8U));put(o,b.data(),b.size());}

#ifdef NEXUS_TEST_HAS_CUDA
class PomAiPreemption final {
public:
 PomAiPreemption(fs::path model_path,fs::path tree,fs::path meta,fs::path source_trace,
                 fs::path source_context,fs::path oracle,fs::path output,
                 std::string llama_path,std::string vulkan_module={},std::string vulkan_sha={})
   : model_path_(std::move(model_path)),tree_path_(std::move(tree)),meta_path_(std::move(meta)),
     oracle_(std::move(oracle)),output_(std::move(output)),llama_path_(std::move(llama_path)),
     gguf_(model_path_),merkle_(gguf_,tree_path_) {
  constexpr std::string_view uuid="GPU-00000000-0000-0000-0000-000000000000";
  const auto *visible=std::getenv("CUDA_VISIBLE_DEVICES");
  require(visible&&std::string(visible)==uuid,"CUDA_VISIBLE_DEVICES must select only authorized primary NVIDIA GPU UUID");
  static constexpr Hash model{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
  static constexpr Hash root{0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};
  model_id_=model; root_=root;
  (void)gguf_.verify_sidecar(tree_path_,meta_path_,model_id_,root_,203469888);
  const auto ctx=read_file(source_context);std::size_t p=0;
  require(ctx.size()>=225&&std::string(reinterpret_cast<const char*>(ctx.data()),4)=="NXRF","verified PoM context fixture is malformed");p=4;
  context_.seed=read64(ctx,p);context_.chunks=read64(ctx,p);context_.tier=ctx.at(p++);
  context_.root=read_array<Hash>(ctx,p);context_.target=read_array<Hash>(ctx,p);context_.pph=read_array<Hash>(ctx,p);
  context_.timestamp=read64(ctx,p);context_.nonce=read64(ctx,p);context_.daa=read64(ctx,p);context_.generation=read64(ctx,p);
  context_.capture_digest=read_array<Hash>(ctx,p);context_.final=read64(ctx,p);context_.pow=read_array<Hash>(ctx,p);
  next_nonce_=context_.nonce;
  const auto wire_len=read32(ctx,p);if(p+wire_len!=ctx.size())throw std::runtime_error("PoM context proof bytes are malformed");
  require(context_.root==root_&&context_.chunks==gguf_.chunk_count(),"PoM context does not bind official GGUF root/chunks");
  const auto trace=read_file(source_trace);p=0;
  require(trace.size()>=28&&std::string(reinterpret_cast<const char*>(trace.data()),4)=="NXST","verified PoM trace fixture is malformed");p=4;
  require(read64(trace,p)==context_.seed&&read64(trace,p)==context_.nonce&&read32(trace,p)==257&&read32(trace,p)==256,
          "PoM trace does not match immutable proof context");
  original_states_.assign(trace.begin()+static_cast<std::ptrdiff_t>(p),trace.begin()+static_cast<std::ptrdiff_t>(p+257*tile_bytes));p+=257*tile_bytes;
  original_offsets_.resize(256);for(auto &off:original_offsets_)off=read64(trace,p);
  require(p==trace.size(),"trailing bytes in verified PoM trace");
  runtime_=std::make_unique<opoi::StockLlamaRuntime>(llama_path_,model_path_.string());
  resident_=std::make_unique<CudaResidentSearch>(std::string(uuid),gguf_,runtime_->resident_tensors());
  engine_=std::make_unique<CudaWalkEngine>(std::string(uuid),1,true);
  tile_count_=gguf_.chunk_count()/chunks_per_tile;
  fs::create_directories(output_);
  memory_profile("pom_initialized");
  validate_resident_gather();
  validate_resident_pairs();
  validate_resident_bulk_witness();
  validate_resident_sibling_paths();
  validate_resident_math();
  measure_search_gain();
#ifdef NEXUS_TEST_HAS_VULKAN
  if(!vulkan_module.empty()) {
    ProofArtifactContext c;c.seed=context_.seed;c.chunks=context_.chunks;c.timestamp=context_.timestamp;
    c.nonce=context_.nonce;c.daa=context_.daa;c.generation=context_.generation;c.tier=context_.tier;
    c.root=context_.root;c.target=context_.target;c.pph=context_.pph;c.capture=context_.capture_digest;
    dual_=std::make_unique<keryx::nexus::testing::DualGpuAiFixture>(c,gguf_,tree_path_,oracle_,output_/"amd",vulkan_module,vulkan_sha);
  }
#endif
 }
 ~PomAiPreemption(){try{stop(false);}catch(const std::exception &e){std::cerr<<"POM_FIXTURE_CLEANUP_ERROR "<<e.what()<<'\n';}}

 void start_active(std::uint64_t nonce) {
#ifdef NEXUS_TEST_HAS_VULKAN
  if(dual_){nonce=dual_->begin_primary(live_resident_batch_size);dual_->publish();}
#endif
  {
   std::lock_guard lock(mutex_);if(active_)throw std::runtime_error("PoM fixture already active");
   opoi_done_=false;kernel_in_flight_=false;active_=true;error_=nullptr;
   active_batch_nonce_=nonce;
  }
  thread_=std::thread([this,nonce]{try{
    const auto seed=block_seed(context_.pph,context_.timestamp,nonce,context_.daa);
    (void)coordinator_.run_pom([&]{
      if(!resident_)resident_=std::make_unique<CudaResidentSearch>("GPU-00000000-0000-0000-0000-000000000000",gguf_,runtime_->resident_tensors());
      Hash zero{};
      resident_->run(context_.pph,context_.timestamp,context_.daa,zero,nonce,live_resident_batch_size,[&](bool in_flight){
      if(in_flight)assert_primary_cuda_context(primary_context_);
      std::unique_lock l(mutex_);kernel_in_flight_=in_flight;opoi_done_=true;changed_.notify_all();
      if(!in_flight)throw std::runtime_error("test reached PoM hook after kernel completed; cannot attest active preemption");
      changed_.wait_for(l,std::chrono::seconds(15),[&]{return coordinator_.opoi_pending();});
      if(!coordinator_.opoi_pending())throw std::runtime_error("AiRequest did not preempt active PoM kernel before timeout");
    });run_cuda(seed);});
   }catch(...){std::lock_guard l(mutex_);error_=std::current_exception();opoi_done_=true;changed_.notify_all();}});
  std::unique_lock lock2(mutex_);if(!changed_.wait_for(lock2,std::chrono::seconds(15),[&]{return opoi_done_;}))throw std::runtime_error("PoM CUDA kernel did not reach active enqueue hook");
  if(error_)std::rethrow_exception(error_);
  require(kernel_in_flight_,"PoM kernel was not in flight at AiRequest admission");
#ifdef NEXUS_TEST_HAS_VULKAN
  lock2.unlock();if(dual_)dual_->wait_active();
#endif
 }

 void use_vulkan_inference(std::function<std::string(std::string_view,std::uint32_t)> infer){
#ifdef NEXUS_TEST_HAS_VULKAN
  require(bool(dual_),"Native AMD AI preemption requires the dual GPU fixture");
  alternate_inference_=std::move(infer);
#else
  throw std::runtime_error("Native AMD inference support absent");
#endif
 }
 std::string infer_during_pom(std::string_view prompt,std::uint32_t max_tokens) {
#ifdef NEXUS_TEST_HAS_VULKAN
  SecondaryLanePause amd_pause(dual_?dual_->lane():nullptr);
#endif
  require(active_&&kernel_in_flight_,"AiRequest arrived without active PoM kernel");
  std::string answer;
  memory_profile("pom_drained_before_inference");
  (void)coordinator_.run_opoi(
    [&]{resident_.reset();engine_.reset();last_batch_.reset();last_input_=ModelWalkInput{};std::cout<<"AI_REQUEST_POM_PREEMPT=KERNEL_DRAINED pid="<<::getpid()<<" cuda_buffers_released=true host_proof_buffers_released=true\n"<<std::flush;},
    [&]{
#ifdef NEXUS_TEST_HAS_VULKAN
        if(alternate_inference_){
          dual_->release_for_inference();
#ifdef NEXUS_TEST_HAS_CUDA
          CUcontext before{},after{};
          require(cuCtxGetCurrent(&before)==CUDA_SUCCESS,"Cannot snapshot caller CUDA context before AMD inference");
#endif
          answer=alternate_inference_(prompt,max_tokens);
#ifdef NEXUS_TEST_HAS_CUDA
          require(cuCtxGetCurrent(&after)==CUDA_SUCCESS&&after==before,"AMD inference changed the calling thread CUDA context");
          assert_retained_primary_cuda_context(primary_context_);
          std::cout<<"AMD_CUDA_CONTEXT_PRESERVED=PASS caller_bound="<<(before?"true":"false")<<" retained_primary=true\n"<<std::flush;
#endif
        }
        else
#endif
        answer=runtime_->infer(keryx::nexus::opoi::official_qwen35_ai_prompt(prompt),max_tokens);
        memory_profile("inference_complete_before_runtime_release");
#ifdef NEXUS_TEST_HAS_CUDA
        if(alternate_inference_)assert_retained_primary_cuda_context(primary_context_);else
#endif
        assert_primary_cuda_context(primary_context_);
        if(answer.empty())throw std::runtime_error("real Qwen CUDA inference returned empty output");return answer;},
    [&]{require(!resident_&&!engine_,"PoM allocations must remain released until admission");
#ifdef NEXUS_TEST_HAS_CUDA
        if(alternate_inference_)assert_retained_primary_cuda_context(primary_context_);else
#endif
        assert_primary_cuda_context(primary_context_);
        std::cout<<"AI_REQUEST_POM_RESTORE=LAZY allocations_released=true\n"<<std::flush;});
  return answer;
 }

 void announce_ai_request() {
  coordinator_.announce_opoi();
  changed_.notify_all();
 }

 void finish_and_resume(std::uint64_t resumed_nonce) {
  if(thread_.joinable())thread_.join();
  if(error_)std::rethrow_exception(error_);
  require(active_batch_nonce_!=UINT64_MAX,"active PoM nonce was not recorded");
  // Verify the kernel which was already in flight when the AiRequest entered, then run
  // and verify a distinct, incremented nonce after the AI response has been reconciled.
  const auto active=make_proof(active_batch_nonce_);
  verify_rust(active,"pom-before-ai");
#ifdef NEXUS_TEST_HAS_VULKAN
  if(dual_){dual_->end_primary();resumed_nonce=dual_->begin_primary(1);}
#endif
  const auto resumed=make_proof(resumed_nonce);
  verify_rust(resumed,"pom-resumed-after-ai");
#ifdef NEXUS_TEST_HAS_VULKAN
  if(dual_){dual_->end_primary();dual_->verified_resume();}
#endif
  next_nonce_=resumed_nonce+1;
  active_=false;
  std::cout<<"AI_REQUEST_POM_RESUME=PASS active_nonce="<<active_batch_nonce_
           <<" resumed_nonce="<<resumed_nonce<<" rust=PASS engine_instances="<<engine_instances_
           <<" same_process=true cuda_primary_context_reused=true submit=MOCK_ONLY\n"<<std::flush;
 }

 void stop(bool verify=true) {coordinator_.request_stop();changed_.notify_all();if(thread_.joinable())thread_.join();
#ifdef NEXUS_TEST_HAS_VULKAN
  if(dual_)dual_->stop(verify);
#endif
  resident_.reset();engine_.reset();runtime_.reset();}
 bool active() const noexcept{return active_;}
 std::uint64_t next_nonce() const noexcept{return next_nonce_;}

private:
 void validate_resident_bulk_witness(){
  double old_ms=0,new_ms=0;constexpr unsigned cases=8;
  const void *staging_address=nullptr;
  for(unsigned i=0;i<cases;++i){
   const auto seed=block_seed(context_.pph,context_.timestamp,context_.nonce+3600000+i,context_.daa);
   ModelWalkInput before,after;ResidentWitnessTiles witness;
   auto baseline=[&]{const auto t=std::chrono::steady_clock::now();before=gather_v4_model_tiles(seed,tile_count_,[&](auto off){return resident_->read_tile_pair(off)[0];});old_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();};
   auto candidate=[&]{const auto t=std::chrono::steady_clock::now();const auto &cached=resident_->read_witness_tiles_cached(seed);
    require(!staging_address||staging_address==cached.pairs.data(),"witness staging reallocated");staging_address=cached.pairs.data();
    witness=cached;std::size_t step=0;after=gather_v4_model_tiles(seed,tile_count_,[&](auto off){require(step<walk_steps&&witness.offsets[step]==off,"bulk offset disagrees with CPU gather");return witness.pairs[step++][0];});require(step==walk_steps,"bulk witness step count");new_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();};
   if(i&1U){candidate();baseline();}else{baseline();candidate();}
   require(before.offsets==after.offsets&&before.tiles==after.tiles,"bulk witness changed canonical bytes");
   for(unsigned step=0;step<walk_steps;++step){std::array<std::uint8_t,tile_bytes> tile{},sibling{};const auto off=witness.offsets[step];gguf_.read_tile_into(off,tile);gguf_.read_tile_into((off^1U)<tile_count_?(off^1U):off,sibling);require(witness.pairs[step][0]==tile&&witness.pairs[step][1]==sibling,"bulk witness differs from canonical GGUF");}
  }
  std::cout<<"RESIDENT_BULK_WITNESS_PARITY=PASS cases="<<cases<<" steps="<<cases*walk_steps<<" old_mean_ms="<<old_ms/cases<<" bulk_mean_ms="<<new_ms/cases<<" staging_bytes="<<walk_steps*(2*tile_bytes+8)<<" order=alternating\n"<<std::flush;
 }
 void validate_resident_pairs(){
  double old_ms=0,new_ms=0;constexpr unsigned cases=8;
  for(unsigned i=0;i<cases;++i){
   const auto seed=block_seed(context_.pph,context_.timestamp,context_.nonce+2700000+i,context_.daa);
   model::SparseMerkleIndex before(gguf_,tree_path_),after(gguf_,tree_path_);
   ModelWalkInput old_input,new_input;
   auto baseline=[&]{auto t=std::chrono::steady_clock::now();old_input=gather_v4_model_tiles(seed,tile_count_,[&](auto off){auto tile=resident_->read_tile(off);before.cache_tile_root(off^1U,[&](auto index){return resident_->read_tile(index);});return tile;});old_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();};
   auto candidate=[&]{auto t=std::chrono::steady_clock::now();new_input=gather_v4_model_tiles(seed,tile_count_,[&](auto off){auto pair=resident_->read_tile_pair(off);after.cache_tile_root(off^1U,[&](auto index){require(index==(off^1U),"pair sibling mismatch");return pair[1];});return pair[0];});new_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();};
   if(i&1U){candidate();baseline();}else{baseline();candidate();}
   require(old_input.tiles==new_input.tiles&&old_input.offsets==new_input.offsets,"combined sibling read changed PoM bytes");
   for(auto off:old_input.offsets)require(before.tile_path(off)==after.tile_path(off),"combined read changed Merkle path");
  }
  std::uint64_t chunks=0;unsigned boundaries=0;
  for(const auto &tensor:runtime_->resident_tensors()){
   const auto count=tensor.bytes/32;if(!count)continue;
   for(auto tile:{chunks/32,(chunks+count-1)/32}){
    auto pair=resident_->read_tile_pair(tile);std::array<std::uint8_t,tile_bytes> a{},b{};
    gguf_.read_tile_into(tile,a);gguf_.read_tile_into(tile^1U,b);
    require(pair[0]==a&&pair[1]==b,"combined read crosses canonical tensor boundary incorrectly");++boundaries;
   }chunks+=count;
  }
  bool rejected=false;try{resident_->read_tile_pair(tile_count_);}catch(const std::out_of_range &){rejected=true;}
  require(rejected,"combined read accepted invalid tile");
  std::cout<<"RESIDENT_TILE_PAIR_PARITY=PASS cases="<<cases<<" boundary_pairs="<<boundaries<<" paths="<<cases*256
           <<" old_mean_ms="<<old_ms/cases<<" paired_mean_ms="<<new_ms/cases<<" order=alternating\n"<<std::flush;
 }
 void validate_resident_sibling_paths(){
  double baseline_ms=0,candidate_ms=0;constexpr unsigned cases=8;
  for(unsigned i=0;i<cases;++i){
   const auto seed=block_seed(context_.pph,context_.timestamp,context_.nonce+1800000+i,context_.daa);
   const auto input=gather_v4_model_tiles(seed,tile_count_,[&](auto off){return resident_->read_tile(off);});
   model::SparseMerkleIndex baseline(gguf_,tree_path_),candidate(gguf_,tree_path_);
   std::vector<std::vector<Hash>> before,after;
   auto old=[&]{const auto t=std::chrono::steady_clock::now();for(auto off:input.offsets)before.push_back(baseline.tile_path(off));baseline_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();};
   auto improved=[&]{const auto t=std::chrono::steady_clock::now();for(auto off:input.offsets){const auto sibling=(off^1U)<tile_count_?(off^1U):off;candidate.cache_tile_root(sibling,[&](auto index){return resident_->read_tile(index);});}for(auto off:input.offsets)after.push_back(candidate.tile_path(off));candidate_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();};
   if(i&1U){improved();old();}else{old();improved();}
   require(before==after,"resident sibling cache changed Merkle path bytes");
  }
  std::cout<<"RESIDENT_MERKLE_SIBLING_PARITY=PASS cases="<<cases<<" paths="<<cases*256
           <<" baseline_mean_ms="<<baseline_ms/cases<<" candidate_including_reads_mean_ms="<<candidate_ms/cases
           <<" order=alternating cache_capacity_unchanged=true bytes_identical=true\n"<<std::flush;
 }
 void validate_resident_gather(){
  double disk_ms=0,resident_ms=0;constexpr unsigned cases=8;
  for(unsigned i=0;i<cases;++i){
   const auto seed=block_seed(context_.pph,context_.timestamp,context_.nonce+900000+i,context_.daa);
   auto begin=std::chrono::steady_clock::now();
   const auto reference=gather_v4_model_tiles(seed,tile_count_,[&](std::uint64_t off){
    std::array<std::uint8_t,tile_bytes> t{};gguf_.read_tile_into(off,t);merkle_.prefetch_tile_path(off);return t;});
   disk_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
   begin=std::chrono::steady_clock::now();
   const auto actual=gather_v4_model_tiles(seed,tile_count_,[&](std::uint64_t off){
    auto t=resident_->read_tile(off);merkle_.prefetch_tile_path(off);return t;});
   resident_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
   require(reference.offsets==actual.offsets&&reference.tiles==actual.tiles,"resident gather differs from canonical GGUF bytes or offsets");
  }
  std::uint64_t chunks=0;unsigned boundaries=0;
  for(const auto &tensor:runtime_->resident_tensors()){
   const auto count=tensor.bytes/32;if(!count)continue;
   for(const auto tile:{chunks/32,(chunks+count-1)/32}){
    std::array<std::uint8_t,tile_bytes> expected{};gguf_.read_tile_into(tile,expected);
    require(resident_->read_tile(tile)==expected,"resident tile crossing a tensor boundary differs from canonical GGUF");++boundaries;
   }
   chunks+=count;
  }
  bool rejected=false;try{resident_->read_tile(tile_count_);}catch(const std::out_of_range &){rejected=true;}
  require(rejected,"resident gather accepted out-of-range tile");
  std::cout<<"RESIDENT_GATHER_PARITY=PASS cases="<<cases<<" boundary_tiles="<<boundaries
           <<" old_mean_ms="<<disk_ms/cases<<" resident_mean_ms="<<resident_ms/cases
           <<" bytes_identical=true offsets_identical=true gpu_allocations_added=0 kernel_changed=false\n"<<std::flush;
 }
 void measure_search_gain(){
  const auto begin=std::chrono::steady_clock::now();std::optional<std::uint64_t> expected;
  for(unsigned i=0;i<16;++i){const auto nonce=context_.nonce+i;const auto proof=make_proof(nonce);
    Hash value{};std::copy_n(proof.context.begin()+189,32,value.begin());
    if(!expected&&little_endian_leq(value,context_.target))expected=nonce;
    verify_rust(proof,"replay-baseline-"+std::to_string(i));
  }
  const auto baseline_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
  if(!resident_)resident_=std::make_unique<CudaResidentSearch>("GPU-00000000-0000-0000-0000-000000000000",gguf_,runtime_->resident_tensors());
  const auto batch_begin=std::chrono::steady_clock::now();
  const auto equivalent=resident_->run(context_.pph,context_.timestamp,context_.daa,context_.target,context_.nonce,16);
  const auto candidate_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-batch_begin).count();
  require(equivalent.winner==expected&&equivalent.completed==16,"same-input candidate replay target result mismatch");
  const auto throughput_begin=std::chrono::steady_clock::now();double kernel_ms=0;
  for(unsigned i=0;i<8;++i){const auto result=resident_->run(context_.pph,context_.timestamp,context_.daa,context_.target,context_.nonce+1000000+i*18432,18432);kernel_ms+=result.kernel_ms;}
  const auto throughput_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-throughput_begin).count();
  std::cout<<"RESIDENT_REPLAY=PASS same_template_same_nonces=16 old_complete_proof_Rust_attempts_s="<<16/baseline_seconds
           <<" new_target_tested_attempts_s="<<16/candidate_seconds<<" baseline_s="<<baseline_seconds
           <<" candidate_s="<<candidate_seconds<<" production_batch=18432 batch_attempts_s="<<147456/throughput_seconds
           <<" mean_kernel_ms="<<kernel_ms/8<<" submits=NONE\n"<<std::flush;
 }
 void validate_resident_math(){
  tile_count_=gguf_.chunk_count()/chunks_per_tile;
  for(unsigned i=0;i<24;++i){
    const auto daa=i<16?h14_activation_daa+100:h10_activation_daa+1;
    Hash pph=context_.pph;pph[i%32]^=static_cast<std::uint8_t>(i*17);
    const auto nonce=context_.nonce+i*997;
    const auto seed=block_seed(pph,context_.timestamp,nonce,daa);
    const auto input=gather_v4_model_tiles(seed,tile_count_,[&](std::uint64_t off){std::array<std::uint8_t,1024> t{};gguf_.read_tile_into(off,t);return t;});
    const auto walk=walk_v4_from_tile_sequence(seed,tile_count_,input.tiles);
    const auto pow=pom_pow_value_h3(final_state(walk),pph);
    const auto equal=resident_->run(pph,context_.timestamp,daa,pow,nonce,1);
    require(equal.winner==nonce&&equal.completed==1,"resident full PoM pow disagrees with CPU at exact target");
    auto below=pow;bool borrowed=true;for(auto &b:below){if(b!=0){--b;borrowed=false;break;}b=255;}
    require(!borrowed,"fixture pow unexpectedly zero");
    require(!resident_->run(pph,context_.timestamp,daa,below,nonce,1).winner,"resident target predecessor incorrectly accepted");
  }
  // Partial warps/blocks and multiple eligible nonces must return the lowest winning nonce.
  for(const auto count:{3ULL,4ULL,5ULL,64ULL}){
    std::vector<Hash> values;
    for(std::uint64_t i=0;i<count;++i){const auto nonce=context_.nonce+i;
      const auto seed=block_seed(context_.pph,context_.timestamp,nonce,context_.daa);
      const auto input=gather_v4_model_tiles(seed,tile_count_,[&](std::uint64_t off){std::array<std::uint8_t,1024> t{};gguf_.read_tile_into(off,t);return t;});
      values.push_back(pom_pow_value_h3(final_state(walk_v4_from_tile_sequence(seed,tile_count_,input.tiles)),context_.pph));
    }
    const auto target=values[count/2];std::optional<std::uint64_t> expected;
    for(std::uint64_t i=0;i<count;++i)if(little_endian_leq(values[i],target)){expected=context_.nonce+i;break;}
    const auto result=resident_->run(context_.pph,context_.timestamp,context_.daa,target,context_.nonce,count);
    require(result.winner==expected&&result.completed==count,"resident batch nonce/target reduction mismatch");
  }
  std::cout<<"RESIDENT_POW_PARITY=PASS cases=24 eras=H10,H14 exact_target_and_predecessor=true source=independent_CPU_reference\n"<<std::flush;
 }
 static std::vector<std::uint8_t> read_file(const fs::path &p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("cannot open persistent proof fixture: "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
 struct ProofArtifact {std::vector<std::uint8_t> wire;std::vector<std::uint8_t> trace;std::vector<std::uint8_t> context;};
 void run_cuda(std::uint64_t seed,const std::function<void(bool)> &hook={}){
  if(!resident_)resident_=std::make_unique<CudaResidentSearch>("GPU-00000000-0000-0000-0000-000000000000",gguf_,runtime_->resident_tensors());
  // Use the candidate winner path for every complete proof and post-AI resume.
  const auto witness=resident_->read_witness_tiles(seed);
  std::size_t witness_step=0;
  auto input=gather_v4_model_tiles(seed,tile_count_,[&](std::uint64_t offset){
   if(resident_){
    require(witness_step<walk_steps&&witness.offsets[witness_step]==offset,"bulk proof offset differs from CPU gather");
    const auto &pair=witness.pairs[witness_step++];const auto &tile=pair[0];const auto sibling=(offset^1U)<tile_count_?(offset^1U):offset;
    merkle_.cache_tile_root(sibling,[&](auto index){require(index==sibling,"proof sibling index mismatch");return index==offset?tile:pair[1];});
    return tile;
   }
   const auto bytes=gguf_.read_tile(offset);if(bytes.size()!=tile_bytes)throw std::runtime_error("GGUF tile has noncanonical size");
   std::array<std::uint8_t,tile_bytes> tile{};std::copy(bytes.begin(),bytes.end(),tile.begin());return tile;
  });
  if(resident_&&!resident_->diagnostic_headroom_available())resident_.reset();
  if(!engine_){engine_=std::make_unique<CudaWalkEngine>("GPU-00000000-0000-0000-0000-000000000000",1,true);++engine_instances_;}

  std::vector<std::uint64_t> ids;std::vector<std::uint32_t> slots;std::vector<std::uint8_t> packed;
  std::unordered_map<std::uint64_t,std::uint32_t> seen;ids.reserve(walk_steps);slots.reserve(walk_steps);packed.reserve(walk_steps*tile_bytes);
  for(std::size_t i=0;i<input.offsets.size();++i){const auto off=input.offsets[i];auto it=seen.find(off);if(it==seen.end()){const auto slot=static_cast<std::uint32_t>(ids.size());seen.emplace(off,slot);ids.push_back(off);slots.push_back(slot);const auto *b=input.tiles.data()+i*tile_bytes;packed.insert(packed.end(),b,b+tile_bytes);}else slots.push_back(it->second);}
  CudaWalkBatch gpu;const auto identity=hex(model_id_)+hex(root_);engine_->run(tile_count_,identity,{seed},packed,ids,slots,true,gpu,[&](bool in_flight){
    if(in_flight)assert_primary_cuda_context(primary_context_);
    if(hook)hook(in_flight);
  });
  auto cpu=walk_v4_from_tile_sequence(seed,tile_count_,input.tiles);
  (void)verified_walk_view(cpu,gpu.offsets,gpu.diagnostic_states);
  if(seed==context_.seed) {
    std::vector<std::uint8_t> states;states.reserve(257*tile_bytes);
    for(std::size_t i=0;i<257;++i)states.insert(states.end(),cpu.states[i].begin(),cpu.states[i].end());
    if(states!=original_states_||gpu.offsets!=original_offsets_)
      throw std::runtime_error("initial active PoM fixture differs from previously Rust-verified trace");
  }
  last_batch_=std::move(gpu);last_input_=std::move(input);last_seed_=seed;
 }
 ProofArtifact make_proof(std::uint64_t nonce){
  const auto seed=block_seed(context_.pph,context_.timestamp,nonce,context_.daa);
  if(!last_batch_||last_seed_!=seed) {
   const bool admitted=coordinator_.run_pom([&]{run_cuda(seed);});
   if(!admitted)throw std::runtime_error("resumed PoM proof was not admitted by GPU ownership coordinator");
  }
  Walk walk;walk.offsets=last_batch_->offsets;walk.states.resize(257);
  for(std::size_t i=0;i<257;++i)std::copy_n(last_batch_->diagnostic_states.data()+i*tile_bytes,tile_bytes,walk.states[i].data());
  const auto proof=prove_v4_from_walk(walk,context_.tier,last_input_.tiles,[&](std::uint64_t off){return merkle_.tile_path(off);});
  const auto final=final_state(walk);const auto pow=pom_pow_value_h3(final,context_.pph);const auto wire=borsh_v4_container(proof,final,pow);
  ProofArtifact a;a.wire=wire;a.trace.insert(a.trace.end(),{'N','X','S','T'});
  auto append64=[&](std::uint64_t v){for(unsigned i=0;i<8;++i)a.trace.push_back(static_cast<std::uint8_t>(v>>(i*8U)));};
  auto append32=[&](std::uint32_t v){for(unsigned i=0;i<4;++i)a.trace.push_back(static_cast<std::uint8_t>(v>>(i*8U)));};
  append64(seed);append64(nonce);append32(257);append32(256);
  for(const auto &s:walk.states)a.trace.insert(a.trace.end(),s.begin(),s.end());for(auto off:walk.offsets)append64(off);
  a.context.insert(a.context.end(),{'N','X','R','F'});append_context64(a.context,seed);append_context64(a.context,context_.chunks);a.context.push_back(context_.tier);
  a.context.insert(a.context.end(),context_.root.begin(),context_.root.end());a.context.insert(a.context.end(),context_.target.begin(),context_.target.end());a.context.insert(a.context.end(),context_.pph.begin(),context_.pph.end());
  append_context64(a.context,context_.timestamp);append_context64(a.context,nonce);append_context64(a.context,context_.daa);append_context64(a.context,context_.generation+nonce-context_.nonce);
  a.context.insert(a.context.end(),context_.capture_digest.begin(),context_.capture_digest.end());append_context64(a.context,final);a.context.insert(a.context.end(),pow.begin(),pow.end());append_context32(a.context,static_cast<std::uint32_t>(wire.size()));a.context.insert(a.context.end(),wire.begin(),wire.end());
  return a;
 }
 static void append_context32(std::vector<std::uint8_t>&v,std::uint32_t n){for(unsigned i=0;i<4;++i)v.push_back(static_cast<std::uint8_t>(n>>(8U*i)));}
 static void append_context64(std::vector<std::uint8_t>&v,std::uint64_t n){for(unsigned i=0;i<8;++i)v.push_back(static_cast<std::uint8_t>(n>>(8U*i)));}
 void verify_rust(const ProofArtifact &a,std::string_view label){
  const auto base=std::string(label)+"-"+std::to_string(active_batch_nonce_==UINT64_MAX?context_.nonce:active_batch_nonce_);
  const auto trace=output_/(base+".trace"),ctx=output_/(base+".context"),proof=output_/(base+".proof");
  for(const auto &p:{trace,ctx,proof})if(fs::exists(p))throw std::runtime_error("refusing to overwrite integration proof artifacts");
  {std::ofstream f(trace,std::ios::binary);put(f,a.trace.data(),a.trace.size());}
  {std::ofstream f(ctx,std::ios::binary);put(f,a.context.data(),a.context.size());}
  {std::ofstream f(proof,std::ios::binary);put(f,a.wire.data(),a.wire.size());}
  auto exe=oracle_.string(),mode=std::string("verify-real"),c=ctx.string(),t=trace.string(),p=proof.string();std::array<char*,6> argv{exe.data(),mode.data(),c.data(),t.data(),p.data(),nullptr};pid_t pid{};
  const int rc=posix_spawn(&pid,oracle_.c_str(),nullptr,nullptr,argv.data(),environ);if(rc)throw std::runtime_error("failed to start Rust PoM verifier");int status{};while(::waitpid(pid,&status,0)<0){if(errno==EINTR)continue;throw std::runtime_error("waitpid Rust PoM verifier failed");}
  if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("official Rust verifier rejected CUDA proof after AiRequest");
 }
 fs::path model_path_,tree_path_,meta_path_,oracle_,output_;std::string llama_path_;model::ReadOnlyGguf gguf_;model::SparseMerkleIndex merkle_;RealPomContext context_{};Hash model_id_{},root_{};
 std::uint64_t tile_count_{},last_seed_{},active_batch_nonce_{UINT64_MAX},next_nonce_{};std::vector<std::uint8_t> original_states_;std::vector<std::uint64_t> original_offsets_;
#ifdef NEXUS_TEST_HAS_CUDA
 CUcontext primary_context_{};
#else
 void *primary_context_{};
#endif
#ifdef NEXUS_TEST_HAS_VULKAN
 std::unique_ptr<keryx::nexus::testing::DualGpuAiFixture> dual_;
#endif
 std::function<std::string(std::string_view,std::uint32_t)> alternate_inference_;
 std::unique_ptr<opoi::StockLlamaRuntime> runtime_;std::unique_ptr<CudaResidentSearch> resident_;
 std::unique_ptr<CudaWalkEngine> engine_;GpuPreemptionCoordinator coordinator_;std::optional<CudaWalkBatch> last_batch_;ModelWalkInput last_input_;std::thread thread_;std::mutex mutex_;std::condition_variable changed_;bool active_{},opoi_done_{},kernel_in_flight_{};std::exception_ptr error_;std::uint64_t engine_instances_{1};
};
#else
// The protocol-only build must not link or emulate a CUDA PoM harness.
// A CUDA request is an explicit error; only the CUDA build has the real path.
class PomAiPreemption final {
 [[noreturn]] static void unavailable(){throw std::runtime_error("CUDA PoM integration is unavailable in this CPU-only test binary");}
public:
 template<class... Args> explicit PomAiPreemption(Args&&...){unavailable();}
 void use_vulkan_inference(std::function<std::string(std::string_view,std::uint32_t)>){unavailable();}
 std::string infer_during_pom(std::string_view,std::uint32_t){unavailable();}
 void announce_ai_request(){unavailable();}
 void start_active(std::uint64_t){unavailable();}
 std::uint64_t next_nonce()const{unavailable();}
 void finish_and_resume(std::uint64_t){unavailable();}
 void stop(){unavailable();}
};
#endif

void journal_recovery_regressions(const fs::path &dir) {
 const auto file=dir/"atomic-recovery.journal";
 fs::remove(file);
 ResponseJournal journal(file);
 const std::string id(64,'b'),txid(64,'d');
 require(journal.begin(id),"journal fixture did not begin");
 for(auto state:{ResponseState::Assigned,ResponseState::RequestValidated,ResponseState::Inferencing,
                 ResponseState::ResponsePrepared,ResponseState::ResponsePublished,ResponseState::TransactionPrepared})
  require(journal.transition(id,state),"journal fixture transition failed");
 require(journal.transition(id,ResponseState::Submitting,txid),"submitting fixture failed");
 require(journal.transition(id,ResponseState::UnknownAfterTimeout),"ambiguous fixture failed");
 const auto read=[](const fs::path &path){std::ifstream in(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
 const auto original=read(file);
 const auto corrupted=original.substr(0,original.find('\n')+1)+"broken journal tail\n";
 {std::ofstream out(file,std::ios::binary|std::ios::trunc);out<<corrupted;}
 bool failed=false;try{journal.recover();}catch(const std::exception&){failed=true;}
 require(failed,"corrupt recovery must fail");
 auto saved=journal.get(id);
 require(saved&&saved->state==ResponseState::UnknownAfterTimeout&&saved->transaction_id==txid,
         "failed recovery discarded the previous ambiguous receipt");
 require(read(file)==corrupted,"failed recovery rewrote the original journal");
 {std::ofstream out(file,std::ios::binary|std::ios::trunc);out<<original;}
 journal.recover();
 const auto backup=dir/"atomic-recovery.saved";fs::rename(file,backup);fs::create_directory(file);
 failed=false;try{journal.recover();}catch(const std::exception&){failed=true;}
 require(failed,"existing unreadable journal must not look like an empty journal");
 saved=journal.get(id);require(saved&&saved->state==ResponseState::UnknownAfterTimeout&&saved->transaction_id==txid,
                            "failed journal read altered the last valid receipt");
 fs::remove(file);fs::rename(backup,file);
 const auto before_rebind=read(file);
 failed=false;try{journal.transition(id,ResponseState::ChainObserved,std::string(64,'e'));}catch(const std::exception&){failed=true;}
 require(failed,"an existing response transaction identity was replaced");
 require(read(file)==before_rebind&&journal.get(id)->transaction_id==txid,"rejected identity replacement mutated the journal");
 require(journal.transition(id,ResponseState::ChainObserved,txid),"matching canonical transaction must remain admissible");
 require(journal.begin(std::string(64,'a')),"second lookup fixture did not begin");
 const std::string padded="xx"+id+"suffix";
 require(journal.get(std::string_view(padded).substr(2,id.size())).has_value(),"non-NUL-terminated ID lookup failed");
 require(!journal.get(std::string_view(padded).substr(2,id.size()-1)),"ID prefix matched a full hash");
 std::string embedded=id;embedded[31]='\0';require(!journal.get(embedded),"embedded NUL matched a hash");
 const auto invalid_file=dir/"invalid-request-id.journal";fs::remove(invalid_file);ResponseJournal invalid(invalid_file);
 for(const std::string bad:{std::string{},std::string("short"),std::string(64,'A'),std::string(63,'b')}){
  failed=false;try{invalid.begin(bad);}catch(const std::invalid_argument&){failed=true;}
  require(failed&&!fs::exists(invalid_file),"invalid request ID created an unrecoverable journal record");
 }
 ResponseJournal absent(dir/"missing-journal");absent.recover();require(absent.snapshot().empty(),"missing fresh journal is not empty");
 std::cout<<"RESPONSE_JOURNAL_RECOVERY=PASS atomic_cache=true corrupt_file_preserved=true read_error_fail_closed=true immutable_txid=true exact_view_lookup=true invalid_id_no_write=true\n";
}

}

int main(int argc,char **argv){
 if(argc<4){std::cerr<<"usage: nexus-ai-response-service-test PUBLIC_FIXTURE PRIVATE_FIXTURE JOURNAL_DIR [--real-inference GGUF LLAMA_LIBRARY] [--ipfs-api URL] [--pom-cuda TREE META TRACE CONTEXT ORACLE OUTPUT_DIR]\n";return 2;}
 try{
  fs::path dir=argv[3];fs::create_directories(dir);journal_recovery_regressions(dir);SubmitMock service;grpc::ServerBuilder builder;int port=0;builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);builder.RegisterService(&service);auto server=builder.BuildAndStart();if(!server||!port)throw std::runtime_error("mock SubmitTransaction server failed");
  const std::string endpoint="grpc://127.0.0.1:"+std::to_string(port);
  const auto public_f=fixture(argv[1]);const auto private_f=fixture(argv[2]);
  require(stock_verify_escrow_delegation(payout,public_f.escrow,public_f.cert),"official escrow delegation rejected");
  require(public_f.escrow==private_f.escrow&&public_f.cert==private_f.cert,"fixture escrow identity differs");
  std::unique_ptr<keryx::nexus::opoi::StockLlamaRuntime> runtime;std::string ipfs_api;
  std::unique_ptr<keryx::nexus::opoi::ExperimentalVulkanInference> vulkan_inference;
  std::string vulkan_ai_library,vulkan_ai_sha,helper_path,helper_sha;fs::path vulkan_ai_model;
  std::array<std::string,6> amd_cycle{};bool amd_cycle_requested=false;
  struct Lease {int fd{-1};~Lease(){if(fd>=0)close(fd);}} amd_lease;
  std::unique_ptr<opoi::VulkanInferenceProcess> vulkan_process;
#ifdef NEXUS_TEST_HAS_VULKAN
  std::unique_ptr<model::ReadOnlyGguf> amd_model;
  std::unique_ptr<testing::DualGpuAiFixture> amd_pom;
#endif
  fs::path real_model,grpc_capture;std::string llama_path,vulkan_module,vulkan_sha;std::array<fs::path,6> pom_args{};bool pom_requested=false,real_inference_requested=false,vulkan_requested=false;
  for(int i=4;i<argc;){const std::string_view option(argv[i]);
    if(option=="--real-inference"&&i+2<argc){real_inference_requested=true;real_model=argv[i+1];llama_path=argv[i+2];i+=3;}
    else if(option=="--real-vulkan-inference"&&i+3<argc){vulkan_ai_model=argv[i+1];vulkan_ai_library=argv[i+2];vulkan_ai_sha=argv[i+3];i+=4;}
    else if(option=="--vulkan-inference-helper"&&i+2<argc){helper_path=argv[i+1];helper_sha=argv[i+2];i+=3;}
    else if(option=="--vulkan-ai-pom"&&i+6<argc){amd_cycle_requested=true;for(unsigned j=0;j<6;++j)amd_cycle[j]=argv[i+1+j];i+=7;}
    else if(option=="--ipfs-api"&&i+1<argc){ipfs_api=argv[i+1];i+=2;}
    else if(option=="--grpc-capture"&&i+1<argc){grpc_capture=argv[i+1];i+=2;}
    else if(option=="--pom-cuda"&&i+6<argc){pom_requested=true;for(std::size_t j=0;j<6;++j)pom_args[j]=argv[i+1+static_cast<int>(j)];i+=7;}
    else if(option=="--pom-vulkan"&&i+2<argc){vulkan_requested=true;vulkan_module=argv[i+1];vulkan_sha=argv[i+2];i+=3;}
    else throw std::runtime_error("unknown or incomplete test option");
  }
  if(!vulkan_ai_library.empty()) {
    if((real_inference_requested||pom_requested||vulkan_requested)&&
        !(real_inference_requested&&pom_requested&&vulkan_requested&&!helper_path.empty()))
      throw std::runtime_error("Combined native AI requires real CUDA PoM, dual GPU fixture and isolated helper");
    const fs::path lease="/opt/nexus-data/state/gpu-devices/lease-pci_0000_00_00_0.lock";
    if(!helper_path.empty()){
      amd_lease.fd=open(lease.c_str(),O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);struct stat st{};
      require(amd_lease.fd>=0&&!fstat(amd_lease.fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==getuid()&&!(st.st_mode&0077)&&!flock(amd_lease.fd,LOCK_EX|LOCK_NB),"AMD child fixture requires an exclusive private device lease");
      vulkan_process=std::make_unique<opoi::VulkanInferenceProcess>(helper_path,helper_sha,vulkan_ai_library,vulkan_ai_sha,vulkan_ai_model.string(),amd_lease.fd);
    } else vulkan_inference=std::make_unique<keryx::nexus::opoi::ExperimentalVulkanInference>(vulkan_ai_library,vulkan_ai_sha,vulkan_ai_model.string(),lease.string());
  }
  if(amd_cycle_requested){
    require(bool(vulkan_process),"AMD PoM/AI lifecycle requires the isolated native inference helper");
#ifdef NEXUS_TEST_HAS_VULKAN
    std::ifstream input(amd_cycle[1],std::ios::binary|std::ios::ate);
    require(input&&input.tellg()>=225&&input.tellg()<2*1024*1024,"AMD replay context size invalid");
    std::vector<std::uint8_t> ctx(static_cast<std::size_t>(input.tellg()));input.seekg(0);
    input.read(reinterpret_cast<char*>(ctx.data()),ctx.size());require(bool(input),"AMD replay context read failed");
    std::size_t offset=4;ProofArtifactContext c;
    require(ctx.size()>=225&&std::string(ctx.begin(),ctx.begin()+4)=="NXRF","AMD replay context malformed");
    c.seed=read64(ctx,offset);c.chunks=read64(ctx,offset);c.tier=ctx.at(offset++);
    c.root=read_array<Hash>(ctx,offset);c.target=read_array<Hash>(ctx,offset);c.pph=read_array<Hash>(ctx,offset);
    c.timestamp=read64(ctx,offset);c.nonce=read64(ctx,offset);c.daa=read64(ctx,offset);c.generation=read64(ctx,offset);
    c.capture=read_array<Hash>(ctx,offset);c.final=read64(ctx,offset);c.pow=read_array<Hash>(ctx,offset);
    const auto wire_size=read32(ctx,offset);require(offset+wire_size==ctx.size(),"AMD replay proof length invalid");
    const char *resident=std::getenv("NEXUS_TEST_AMD_RESIDENT");
    require(resident&&std::string_view(resident)=="1","AMD AI lifecycle must use resident model banks");
    amd_model=std::make_unique<model::ReadOnlyGguf>(vulkan_ai_model);
    require(c.chunks==amd_model->chunk_count(),"AMD replay model dimensions mismatch");
    amd_pom=std::make_unique<testing::DualGpuAiFixture>(c,*amd_model,amd_cycle[0],amd_cycle[2],amd_cycle[5],amd_cycle[3],amd_cycle[4],5);
#else
    throw std::runtime_error("AMD replay support absent in this binary");
#endif
  }
  if(!helper_path.empty()&&!vulkan_process)throw std::runtime_error("Inference helper requires native Vulkan model configuration");
  if(vulkan_requested) {
    if(vulkan_module.empty()||vulkan_sha.size()!=64)throw std::runtime_error("Dual GPU test requires a module and SHA-256");
    if(!pom_requested||!real_inference_requested)throw std::runtime_error("Dual GPU test requires real CUDA PoM and Qwen inference");
#ifndef NEXUS_TEST_HAS_VULKAN
    throw std::runtime_error("This test executable has no Vulkan fixture support");
#endif
  }
  // Install the existing test-only clean-cache guard before checksumming/loading
  // the GGUF, not after initialization has already displaced the node working set.
  std::unique_ptr<GgufCacheGuard> cache_guard;
  if(real_inference_requested)cache_guard=std::make_unique<GgufCacheGuard>(real_model);
  std::unique_ptr<PomAiPreemption> pom;
  if(pom_requested){
    if(!real_inference_requested||real_model.empty()||llama_path.empty())throw std::runtime_error("PoM/AiRequest integration requires real Qwen CUDA inference");
    pom=std::make_unique<PomAiPreemption>(real_model,pom_args[0],pom_args[1],pom_args[2],pom_args[3],pom_args[4],pom_args[5],llama_path,vulkan_module,vulkan_sha);
    if(grpc_capture.empty())throw std::runtime_error("same-process preemption test requires an authentic mainnet template capture");
    service.set_template_capture(grpc_capture);
    if(vulkan_process)pom->use_vulkan_inference([&](std::string_view prompt,std::uint32_t tokens){
      return vulkan_process->infer(opoi::official_qwen35_ai_prompt(prompt),tokens);
    });
  } else if(real_inference_requested) runtime=std::make_unique<keryx::nexus::opoi::StockLlamaRuntime>(llama_path,real_model.string());
  std::string last_answer;
  bool preempt_next=false;
  auto infer=[&](std::string_view p,std::uint32_t n){require(n==16,"wrong token limit");
    try {
    if(preempt_next){preempt_next=false;last_answer=pom->infer_during_pom(p,n);}
    else if(runtime)last_answer=runtime->infer(keryx::nexus::opoi::official_qwen35_ai_prompt(p),n);
    else if(vulkan_process&&!pom){
#ifdef NEXUS_TEST_HAS_VULKAN
      if(amd_pom){
        amd_pom->publish();amd_pom->wait_active();
        {SecondaryLanePause pause(amd_pom->lane());amd_pom->release_for_inference();
         last_answer=vulkan_process->infer(keryx::nexus::opoi::official_qwen35_ai_prompt(p),n);}
        amd_pom->verified_resume();
      }else
#endif
      last_answer=vulkan_process->infer(keryx::nexus::opoi::official_qwen35_ai_prompt(p),n);
    }
    else if(vulkan_inference)last_answer=vulkan_inference->infer(keryx::nexus::opoi::official_qwen35_ai_prompt(p),n);
    else last_answer="A concise verified answer.";
    } catch(const std::exception &e){std::cerr<<"REAL_INFERENCE_ERROR type=exception detail="<<e.what()<<"\n"<<std::flush;throw;}
    return last_answer;};
  {
   // Exercise the Rust network matrix through the actual C++ ABI without a GPU.
   using keryx::nexus::protocol::flags;
   using keryx::nexus::protocol::require_legacy_worker_era;
   require(flags("mainnet",140'114'999)==0 && flags("mainnet",140'115'000)==3,
           "mainnet H15 boundary differs from pinned official rules");
   require(flags("testnet",3499)==0 && flags("testnet",3500)==3,
           "testnet attention gate must be distinct from disabled block tasks");
   require(flags("mainnet",UINT64_MAX)==3 && flags("testnet",UINT64_MAX)==3,
           "H15 attention must stay active and block tasks disabled at sentinel DAA");
   bool refused=false;try{require_legacy_worker_era("mainnet",140'115'000);}catch(const std::runtime_error&){refused=true;}
   require(refused,"legacy worker admitted the H15 era");
   refused=false;try{(void)flags("unknown",0);}catch(const std::invalid_argument&){refused=true;}
   require(refused,"unknown network was silently treated as mainnet");
   for(bool crossing:{false,true}) {
    auto c=config_for(private_f,dir/(crossing?"h15-crossing.journal":"h15-refusal.journal"));
    AiResponseService svc(c);svc.recover();
    unsigned inference_calls=0,publish_calls=0,submit_calls=0;bool crossed=false;
    auto effects=effects_for(endpoint,[](auto,auto){return std::string("unused");},service,ipfs_api);
    effects.infer=[&](auto,auto){++inference_calls;crossed=true;return std::string("answer");};
    effects.current_daa=[&]{return crossing&&!crossed?140'114'999ULL:140'115'000ULL;};
    effects.publish_and_readback=[&](auto){++publish_calls;return std::array<std::uint8_t,34>{};};
    effects.submit=[&](auto,auto){++submit_calls;return AiResponseEffects::SubmitResult{};};
    const auto daa=crossing?140'114'999ULL:140'115'000ULL;
    require(svc.serve(private_f.tx,daa,{},effects)==ServeResult::Failed,"unsupported H15 request was not refused");
    require(inference_calls==(crossing?1U:0U)&&publish_calls==0&&submit_calls==0,
            "H15 admission allowed inference or publication beyond its boundary");
   }
  }
  {
   for(bool retry_crossing:{false,true}) {
    auto c=config_for(private_f,dir/(retry_crossing?"h15-retry-crossing.journal":"h15-sign-crossing.journal"));
    AiResponseService svc(c);svc.recover();bool crossed=false;unsigned submits=0;
    auto effects=effects_for(endpoint,[](auto,auto){return std::string("answer");},service,ipfs_api);
    effects.current_daa=[&]{return crossed?140'115'000ULL:140'114'999ULL;};
    effects.phase_timing=[&](std::string_view phase,auto){if(!retry_crossing&&phase=="transaction_build")crossed=true;};
    auto req=parse_request(private_f.tx,140'114'999,nullptr,"mainnet");require(req.has_value(),"H15 retry fixture invalid");
    const auto pending="the AiResponse for request "+hex(req->request_hash)+
      " carries an inline body, but that is not a pending private request naming this responder";
    effects.submit=[&](auto,auto){++submits;return AiResponseEffects::SubmitResult{AiResponseEffects::SubmitResult::State::Rejected,pending};};
    effects.wait_before_retry=[&](auto){crossed=true;return true;};
    require(svc.serve(private_f.tx,140'114'999,{},effects)==ServeResult::Failed,"H15 crossing during signing/retry was admitted");
    require(submits==(retry_crossing?1U:0U),"H15 crossing sent or retried an incompatible response");
    const auto stored=svc.journal().get(req->stable_id);
    require(stored&&stored->state==ResponseState::Failed,"H15 crossing lost its failed journal state");
    if(retry_crossing)require(!stored->transaction_id.empty(),"H15 retry crossing lost its original transaction identity");
   }
  }
  {
   AiResponseService svc(config_for(public_f,dir/"public.journal"));svc.recover();auto effects=effects_for(endpoint,infer,service,ipfs_api);
   if(pom)discover_via_live_grpc(service,endpoint,svc,public_f.tx,mainnet_private_inference_gate_daa-1);
   else require(svc.discover(public_f.tx,mainnet_private_inference_gate_daa-1),"public request was not durably discovered");
   require(!svc.discover(public_f.tx,mainnet_private_inference_gate_daa-1),"duplicate public discovery was not deduplicated");
   if(pom){pom->start_active(pom->next_nonce());preempt_next=true;pom->announce_ai_request();std::cout<<"AI_REQUEST_INGRESS=PUBLIC assigned=true source=isolated_protocol_fixture during_active_cuda_kernel=true\n"<<std::flush;}
   std::unordered_map<std::string,std::uint64_t> phase_times;
   effects.phase_timing=[&](std::string_view phase,std::uint64_t ns) {
    phase_times[std::string(phase)]=ns;
    if(phase=="request_open")throw std::runtime_error("injected telemetry sink failure");
   };
   const auto result=svc.serve(public_f.tx,mainnet_private_inference_gate_daa-1,providers_for(public_f),effects);
   for(const auto phase:{"request_open","inference","ipfs_publish_readback","response_sign","response_verify","transaction_build","submit_rpc"})
    require(phase_times.contains(phase),"AI phase duration missing; telemetry failure changed serving");
   if(result!=ServeResult::Submitted){std::string diagnostic;const auto request=parse_request(public_f.tx,mainnet_private_inference_gate_daa-1,&diagnostic,"mainnet");
    const auto record=request?svc.journal().get(request->stable_id):std::nullopt;
    std::cerr<<"PUBLIC_RESPONSE_FAILURE result="<<static_cast<int>(result)<<" journal="
             <<(record?response_state_name(record->state):"missing")<<" mock_submits="<<service.count()
             <<" parse="<<diagnostic<<"\n"<<std::flush;}
   require(result==ServeResult::Submitted,"public response lifecycle failed");
   require(service.wait_count(1),"SubmitTransaction mock did not receive public AiResponse");
   const auto public_snapshot=svc.journal().snapshot();
   require(public_snapshot.size()==1&&public_snapshot.front().state==ResponseState::RpcAcknowledged,
           "read-only response journal snapshot did not preserve the acknowledged request state");
   std::string public_why;auto public_request=parse_request(public_f.tx,mainnet_private_inference_gate_daa-1,&public_why,"mainnet");
   require(public_request&&stock_verify_response(unhex(service.last().payload()),public_request->request_hash,public_f.escrow,std::nullopt),"independent Rust public response signature verification failed");
   protowire::RpcBlock observed_block;observed_block.mutable_header()->set_daascore(mainnet_private_inference_gate_daa);
   observed_block.add_transactions()->CopyFrom(service.last());
   require(svc.observe_block(observed_block)==1,"valid stock AiResponse was not reconciled from block observation");
   require(svc.journal().get(public_request->stable_id)->state==ResponseState::ChainObserved,
           "RPC acknowledgement was not advanced to distinct DAG-observed state");
   require(svc.serve(public_f.tx,mainnet_private_inference_gate_daa-1,providers_for(public_f),effects)==ServeResult::Duplicate,"public request was not deduplicated");
   require(service.count()==1,"duplicate public AiResponse was transmitted");
   if(pom)pom->finish_and_resume(pom->next_nonce()+1);
  }
  {
   std::string why;auto parsed=parse_request(public_f.tx,mainnet_private_inference_gate_daa-1,&why,"mainnet");require(parsed.has_value(),"public request parse failed for crash fixture");
   ResponseJournal before_crash(dir/"before-submit.journal");before_crash.recover();require(before_crash.begin(parsed->stable_id),"could not seed pre-submit journal");
   require(before_crash.transition(parsed->stable_id,ResponseState::Assigned)&&before_crash.transition(parsed->stable_id,ResponseState::RequestValidated)&&
           before_crash.transition(parsed->stable_id,ResponseState::Inferencing)&&before_crash.transition(parsed->stable_id,ResponseState::ResponsePrepared),"could not seed crash state");
   AiResponseService restarted(config_for(public_f,dir/"before-submit.journal"));restarted.recover();auto effects=effects_for(endpoint,infer,service,ipfs_api);
   require(restarted.serve(public_f.tx,mainnet_private_inference_gate_daa-1,providers_for(public_f),effects)==ServeResult::Submitted,"pre-submit crash recovery did not resume request");
   require(service.wait_count(2),"recovered request did not reach SubmitTransaction mock");
  }
  {
   AiResponseService svc(config_for(private_f,dir/"private.journal"));svc.recover();auto effects=effects_for(endpoint,infer,service,ipfs_api);
   if(pom)discover_via_live_grpc(service,endpoint,svc,private_f.tx,mainnet_private_inference_gate_daa+100);
   else require(svc.discover(private_f.tx,mainnet_private_inference_gate_daa+100),"private request was not durably discovered");
   if(pom){pom->start_active(pom->next_nonce());preempt_next=true;pom->announce_ai_request();std::cout<<"AI_REQUEST_INGRESS=PRIVATE assigned=true source=isolated_protocol_fixture during_active_cuda_kernel=true\n"<<std::flush;}
   const auto private_id=parse_request(private_f.tx,mainnet_private_inference_gate_daa+100,nullptr)->stable_id;
   {ResponseJournal legacy(dir/"private.journal");legacy.recover();require(legacy.transition(private_id,ResponseState::NotAssigned),"could not seed legacy false negative assignment");}
   svc.recover();
   const auto result=svc.serve(private_f.tx,mainnet_private_inference_gate_daa+100,{},effects);
   require(result==ServeResult::Submitted,"private response lifecycle failed");
   require(service.wait_count(3),"SubmitTransaction mock did not receive private AiResponse");
   auto request_bytes=unhex(private_f.tx.payload());std::string why;auto parsed=parse_request(private_f.tx,mainnet_private_inference_gate_daa+100,&why,"mainnet");require(parsed.has_value(),"private request parse failed");
   auto response=unhex(service.last().payload());std::vector<std::uint8_t> opened;
   require(stock_verify_response(response,parsed->request_hash,private_f.escrow,private_f.requester_secret,&opened),"independent Rust signature/private response verification failed");
   require(std::string(opened.begin(),opened.end())==last_answer,"private answer does not open to inference result");
   require(!stock_verify_response(response,parsed->request_hash,private_f.escrow,std::nullopt),
           "full private response verification must still require the requester root");
   require(stock_verify_response_signature(response,parsed->request_hash,private_f.escrow),
           "private response signature must be observable without decrypting the answer");
   auto wrong_request=parsed->request_hash;wrong_request[0]^=1;
   auto wrong_responder=private_f.escrow;wrong_responder[0]^=1;
   auto tampered_response=response;tampered_response.back()^=1;
   require(!stock_verify_response_signature(response,wrong_request,private_f.escrow)&&
           !stock_verify_response_signature(response,parsed->request_hash,wrong_responder)&&
           !stock_verify_response_signature(tampered_response,parsed->request_hash,private_f.escrow),
           "signature-only observation accepted a wrong binding or modified response");
   protowire::RpcBlock private_block;
   private_block.mutable_header()->set_daascore(mainnet_private_inference_gate_daa+101);
   private_block.add_transactions()->CopyFrom(service.last());
   require(svc.observe_block(private_block)==1&&svc.observe_block(private_block)==0&&
           svc.journal().get(parsed->stable_id)->state==ResponseState::ChainObserved,
           "private RPC ACK did not reconcile exactly once from block observation");
   std::cout<<"PRIVATE_RESPONSE_OBSERVATION=PASS signature=true decryption_not_required=true "
                "wrong_binding_rejected=true tamper_rejected=true duplicate_observation=0\n";
   require(service.last().payload().size()>0,"private wire payload absent");
   AiResponseService restarted(config_for(private_f,dir/"private.journal"));restarted.recover();
   require(restarted.serve(private_f.tx,mainnet_private_inference_gate_daa+100,providers_for(private_f),effects)==ServeResult::Duplicate,"restart duplicated a submitted private response");
   require(service.count()==3,"restart emitted a duplicate SubmitTransaction");
   if(pom)pom->finish_and_resume(pom->next_nonce()+1);
  }
  {
   auto effects=effects_for(endpoint,infer,service,ipfs_api);
   auto actual_submit=effects.submit;
   effects.submit=[actual_submit](const protowire::RpcTransaction &tx,std::string_view id){(void)actual_submit(tx,id);return AiResponseEffects::SubmitResult{AiResponseEffects::SubmitResult::State::Unknown,"injected lost RPC response"};};
   AiResponseService first(config_for(public_f,dir/"after-send-timeout.journal"));first.recover();
   // Reuse a distinct valid request ID by changing the prompt payload while retaining the official format.
   auto tx=public_f.tx;auto payload=unhex(tx.payload());payload.back()^=0x01;tx.set_payload(hex(payload));
   require(first.serve(tx,mainnet_private_inference_gate_daa-1,providers_for(public_f),effects)==ServeResult::WaitingReconciliation,"uncertain submit was not retained for reconciliation");
   require(service.wait_count(4),"timeout fixture was not actually sent to mock");
   AiResponseService recovered(config_for(public_f,dir/"after-send-timeout.journal"));recovered.recover();auto reconciled=effects_for(endpoint,infer,service,ipfs_api);reconciled.reconcile=[](std::string_view){return true;};
   require(recovered.serve(tx,mainnet_private_inference_gate_daa-1,providers_for(public_f),reconciled)==ServeResult::Duplicate,"known in-mempool response was not reconciled");
   require(service.count()==4,"reconciliation retried an already submitted response");
  }
  {
   auto effects=effects_for(endpoint,infer,service,ipfs_api);
   AiResponseService not_assigned(config_for(public_f,dir/"not-assigned.journal"));not_assigned.recover();
   require(not_assigned.serve(public_f.tx,mainnet_private_inference_gate_daa-1,{},effects)==ServeResult::NotAssigned,
           "request without active service assignment was not rejected");
   AiResponseService expired(config_for(public_f,dir/"expired.journal"));expired.recover();
   require(expired.serve(public_f.tx,mainnet_private_inference_gate_daa-1,providers_for(public_f),effects,1)==ServeResult::Expired,
           "expired request deadline was not enforced");
   AiResponseService backfill_expired(config_for(public_f,dir/"backfill-expired.journal"));backfill_expired.recover();
   auto late=effects;late.current_daa=[] {return mainnet_private_inference_gate_daa+1000;};
   require(backfill_expired.serve(public_f.tx,mainnet_private_inference_gate_daa-1,providers_for(public_f),late)==ServeResult::Expired,
           "old backfill request was evaluated against its containing DAA instead of current DAA");
   auto failing=effects;failing.infer=[](std::string_view,std::uint32_t)->std::string{throw std::runtime_error("fixture inference failure");};
   auto unique=public_f.tx;auto payload=unhex(unique.payload());payload.back()^=0x02;unique.set_payload(hex(payload));
   AiResponseService inference_failure(config_for(public_f,dir/"infer-failure.journal"));inference_failure.recover();
   const int before=service.count();
   require(inference_failure.serve(unique,mainnet_private_inference_gate_daa-1,providers_for(public_f),failing)==ServeResult::Failed,
           "inference failure did not fail closed");
   require(service.count()==before,"inference failure reached SubmitTransaction");
   auto no_ipfs=effects;no_ipfs.publish_and_readback=[](std::span<const std::uint8_t>)->std::array<std::uint8_t,34>{throw std::runtime_error("mock IPFS unavailable");};
   auto unique_ipfs=public_f.tx;payload=unhex(unique_ipfs.payload());payload.back()^=0x03;unique_ipfs.set_payload(hex(payload));
   AiResponseService ipfs_failure(config_for(public_f,dir/"ipfs-failure.journal"));ipfs_failure.recover();
   require(ipfs_failure.serve(unique_ipfs,mainnet_private_inference_gate_daa-1,providers_for(public_f),no_ipfs)==ServeResult::Failed,
           "unavailable IPFS did not fail closed");
   require(service.count()==before,"IPFS failure reached SubmitTransaction");
   auto unique_reject=public_f.tx;payload=unhex(unique_reject.payload());payload.back()^=0x04;unique_reject.set_payload(hex(payload));
   auto reject_effects=effects;reject_effects.infer=[](std::string_view,std::uint32_t){return std::string("isolated rejection fixture");};
   service.reject_next();AiResponseService rejected(config_for(public_f,dir/"rpc-rejected.journal"));rejected.recover();
   require(rejected.serve(unique_reject,mainnet_private_inference_gate_daa-1,providers_for(public_f),reject_effects)==ServeResult::Failed,
           "RPC rejection was not surfaced as failure");
   require(service.wait_count(before+1),"RPC rejection fixture did not reach isolated mock");
  }
  {
   auto request_tx=public_f.tx;auto request_payload=unhex(request_tx.payload());request_payload.back()^=0x10;request_tx.set_payload(hex(request_payload));
   const auto daa=mainnet_private_inference_gate_daa-1;std::string parse_error;
   const auto parsed=parse_request(request_tx,daa,&parse_error,"mainnet");require(parsed.has_value(),"reorg AiRequest fixture failed official Rust parsing");
   auto effects=effects_for(endpoint,infer,service,ipfs_api);effects.infer=[](std::string_view,std::uint32_t){return std::string("reorg fixture answer");};
   AiResponseService svc(config_for(public_f,dir/"reorg-e2e.journal"));svc.recover();
   require(svc.discover(request_tx,daa),"selected-chain AiRequest was not discovered");
   std::unordered_map<std::string,std::string> selected_request_blocks;
   selected_request_blocks[std::string(64,'c')]=parsed->stable_id;
   require(selected_request_blocks.erase(std::string(64,'c'))==1&&
           svc.journal().get(parsed->stable_id).has_value(),"request block removal discarded its logical request identity");
   require(!svc.discover(request_tx,daa),"request reinclusion created a second canonical request");
   selected_request_blocks[std::string(64,'d')]=parsed->stable_id;
   require(selected_request_blocks.size()==1&&selected_request_blocks.begin()->second==parsed->stable_id,
           "request reinclusion changed its canonical identity");
   const int baseline=service.count();
   require(svc.serve(request_tx,daa,providers_for(public_f),effects)==ServeResult::Submitted,"selected-chain request response was not acknowledged");
   require(service.wait_count(baseline+1),"reorg AiResponse fixture did not reach SubmitTransaction mock");
   const auto first_response=service.last();
   const auto response_payload=unhex(first_response.payload());
   require(stock_verify_response(response_payload,parsed->request_hash,public_f.escrow,std::nullopt),
           "reorg response fixture failed official Rust signature verification");
   const auto record=svc.journal().get(parsed->stable_id);
   require(record&&record->state==ResponseState::RpcAcknowledged,"RPC ACK was incorrectly represented as selected-chain confirmation");
   require(svc.serve(request_tx,daa,providers_for(public_f),effects)==ServeResult::Duplicate&&service.count()==baseline+1,
           "RPC ACK without selected-chain observation caused a duplicate response");
   protowire::RpcBlock response_block;response_block.mutable_header()->set_daascore(daa+1);
   response_block.mutable_verbosedata()->set_hash(std::string(64,'a'));response_block.add_transactions()->CopyFrom(first_response);
   require(svc.observe_block(response_block)==1&&svc.journal().get(parsed->stable_id)->state==ResponseState::ChainObserved,
           "confirmed response block was not reconciled");
   require(svc.observe_removed_chain_blocks({std::string(64,'a')})==1&&
           svc.journal().get(parsed->stable_id)->state==ResponseState::ResponseReorged,
           "response block removal did not reopen selected-chain reconciliation");
   AiResponseService after_reorg_crash(config_for(public_f,dir/"reorg-e2e.journal"));after_reorg_crash.recover();
   require(after_reorg_crash.journal().get(parsed->stable_id)->state==ResponseState::ResponseReorged,
           "journal recovery lost the response reorg state");
   auto known=effects;known.reconcile=[](std::string_view){return true;};
   require(after_reorg_crash.serve(request_tx,daa,providers_for(public_f),known)==ServeResult::WaitingReconciliation&&
           service.count()==baseline+1,"recovered in-DAG response triggered a duplicate submit");
   response_block.mutable_verbosedata()->set_hash(std::string(64,'b'));
   require(after_reorg_crash.observe_block(response_block)==1&&
           after_reorg_crash.journal().get(parsed->stable_id)->state==ResponseState::ChainObserved,
           "same response reinclusion did not restore its chain-observed state");
   require(after_reorg_crash.serve(request_tx,daa,providers_for(public_f),effects)==ServeResult::Duplicate&&
           service.count()==baseline+1,"reincluded request/response was served twice");
   require(after_reorg_crash.observe_removed_chain_blocks({std::string(64,'b')})==1,
           "second response removal was not tracked");
   auto absent=effects;absent.reconcile=[](std::string_view){return false;};
   require(after_reorg_crash.serve(request_tx,daa,providers_for(public_f),absent)==ServeResult::Submitted,
           "request was not re-eligible after response removal and authoritative absence");
   require(service.wait_count(baseline+2),"eligible reorg retry was not submitted exactly once");
   require(after_reorg_crash.serve(request_tx,daa,providers_for(public_f),absent)==ServeResult::Duplicate&&
           service.count()==baseline+2,"reorg retry produced a duplicate response");
   const auto retry_record=after_reorg_crash.journal().get(parsed->stable_id);
   require(retry_record&&retry_record->request_id==parsed->stable_id&&retry_record->state==ResponseState::RpcAcknowledged,
           "reorg retry changed canonical request identity or journal state");
   protowire::RpcBlock retry_block;retry_block.mutable_header()->set_daascore(daa+2);
   retry_block.mutable_verbosedata()->set_hash(std::string(64,'e'));retry_block.add_transactions()->CopyFrom(service.last());
   require(after_reorg_crash.observe_block(retry_block)==1&&
           after_reorg_crash.observe_removed_chain_blocks({std::string(64,'e')})==1,
           "deadline fixture did not reconcile its response reorg");
   auto expired_retry=absent;expired_retry.current_daa=[daa]{return daa+stock_response_window_daa+2;};
   require(after_reorg_crash.serve(request_tx,daa,providers_for(public_f),expired_retry)==ServeResult::Expired&&
           service.count()==baseline+2&&after_reorg_crash.journal().get(parsed->stable_id)->state==ResponseState::Expired,
           "response reorg retry ignored the original protocol deadline or submitted after expiry");
   std::cout<<"AI_REQUEST_RESPONSE_REORG_E2E=PASS selected_request=true response_removed=true response_reincluded=true "
               "response_absent_retry=true rpc_ack_not_confirmation=true request_reinclusion_deduplicated=true "
               "journal_crash_recovery=true original_deadline_enforced=true deadline_daa="<<daa<<" duplicate_submit=false\n"<<std::flush;
  }
  // The official 4ebf4bc byte cap must precede CID generation/signing/sealing.
  if(!pom && !runtime) {
   for(const bool private_request : {false,true}) {
    const auto &fixture = private_request ? private_f : public_f;
    const auto daa = private_request ? mainnet_private_inference_gate_daa+100 : mainnet_private_inference_gate_daa-1;
    const auto parsed = parse_request(fixture.tx,daa,nullptr,"mainnet");
    require(parsed.has_value(),"UTF-8 cap request fixture did not parse");
    const auto cap = static_cast<std::size_t>(parsed->max_tokens)*32;
    const std::string expected(cap-1,'x');
    const std::string oversized = expected + "\xe2\x82\xac" + "tail";
    std::vector<std::uint8_t> published;
    auto effects=effects_for(endpoint,[oversized](std::string_view,std::uint32_t){return oversized;},service,ipfs_api);
    const auto publish=effects.publish_and_readback;
    effects.publish_and_readback=[&](std::span<const std::uint8_t> body){published.assign(body.begin(),body.end());try{return publish(body);}catch(const std::exception &e){std::cerr<<"UTF8_FIXTURE_PUBLISH_ERROR "<<e.what()<<"\n";throw;}};
    AiResponseService svc(config_for(fixture,dir/(private_request?"utf8-private.journal":"utf8-public.journal")));svc.recover();
    require(svc.serve(fixture.tx,daa,providers_for(fixture),effects)==ServeResult::Submitted,"bounded UTF-8 answer was not submitted to mock");
    std::vector<std::uint8_t> opened;
    require(stock_verify_response(unhex(service.last().payload()),parsed->request_hash,fixture.escrow,
      private_request?std::optional(fixture.requester_secret):std::nullopt,private_request?&opened:nullptr),"bounded answer signature or private sealing invalid");
    const auto &body = private_request ? opened : published;
    require(std::string(body.begin(),body.end())==expected,"answer cap split UTF-8 or published/signed a different body");
    std::cout<<"OFFICIAL_ANSWER_BYTE_CAP=PASS private="<<private_request<<" utf8_boundary=true signature=true mainnet_tx=false\n";
   }
  }
  // Admission can lag DAG discovery. Never re-infer, re-sign or retry an
  // ambiguous send. These fixtures exercise the real Rust signing adapter.
  for (const std::string mode : {"raw", "wrapped", "exhausted", "expired", "cancelled",
                                  "unknown", "wrong-request", "permanent", "public"}) {
   const bool public_case=mode=="public";
   const auto &f=public_case?public_f:private_f;
   const auto daa=public_case?mainnet_private_inference_gate_daa-1:mainnet_private_inference_gate_daa+100;
   std::string why;const auto parsed=parse_request(f.tx,daa,&why,"mainnet");
   require(parsed.has_value(),"retry request parse failed");
   AiResponseService svc(config_for(f,dir/("retry-"+mode+".journal")));svc.recover();
   auto effects=effects_for(endpoint,{},service);unsigned calls=0,inferences=0,waits=0,diagnostics=0;
   std::uint64_t current=daa;std::string first_wire,first_id;
   effects.infer=[&](std::string_view,std::uint32_t){++inferences;return std::string("retry fixture answer");};
   effects.current_daa=[&]{return current;};
   effects.wait_before_retry=[&](std::uint32_t ms){require(ms==3000,"retry interval differs from stock");++waits;
    if(mode=="expired")current=daa+11;return mode!="cancelled";};
   effects.submission_diagnostic=[&](std::string_view,std::uint32_t attempt,std::string_view reason,std::string_view digest){
    ++diagnostics;require(attempt==diagnostics&&digest.size()==64,"submission diagnostic missing correlation");
    require(reason.find("answer")==std::string_view::npos,"diagnostic leaked response data");};
   effects.submit=[&](const protowire::RpcTransaction &tx,std::string_view id){
    ++calls;if(calls==1){first_wire=tx.SerializeAsString();first_id=id;}
    require(tx.SerializeAsString()==first_wire&&id==first_id,"retry changed signed bytes or txid");
    using State=AiResponseEffects::SubmitResult::State;
    if(mode=="unknown")return AiResponseEffects::SubmitResult{State::Unknown,"transport timeout"};
    if(mode=="permanent")return AiResponseEffects::SubmitResult{State::Rejected,"invalid signature"};
    if((mode=="raw"||mode=="wrapped")&&calls==3)return AiResponseEffects::SubmitResult{State::Acknowledged,"accepted"};
    auto request_hash=hex(parsed->request_hash);
    if(mode=="wrong-request")request_hash=std::string(64,'0');
    auto detail="the AiResponse for request "+request_hash+
       " carries an inline body, but that is not a pending private request naming this responder";
    if(mode=="wrapped")detail="Rejected transaction "+std::string(id)+": "+detail;
    return AiResponseEffects::SubmitResult{State::Rejected,detail};
   };
   const auto result=svc.serve(f.tx,daa,providers_for(f),effects,daa+10,daa);
   require(inferences==1&&calls==diagnostics,"retry re-ran inference or lost diagnostics");
   const auto state=svc.journal().get(parsed->stable_id)->state;
   if(mode=="raw"||mode=="wrapped") {
    require(result==ServeResult::Submitted&&calls==3&&waits==2&&state==ResponseState::RpcAcknowledged,"transient admission retry failed");
    require(svc.serve(f.tx,daa,providers_for(f),effects,daa+10,daa)==ServeResult::Duplicate&&calls==3,"ACK caused a duplicate retry");
   } else if(mode=="exhausted")require(result==ServeResult::Failed&&calls==100&&waits==99&&state==ResponseState::RpcRejected,"retry bound not enforced");
   else if(mode=="expired")require(result==ServeResult::Expired&&calls==1&&state==ResponseState::Expired,"expired response was retried");
   else if(mode=="cancelled"||mode=="unknown") {
    require(result==ServeResult::WaitingReconciliation&&calls==1,"ambiguous/cancelled send was retried");
    AiResponseService recovered(config_for(f,dir/("retry-"+mode+".journal")));recovered.recover();
    require(recovered.serve(f.tx,daa,providers_for(f),effects,daa+10,daa)==ServeResult::WaitingReconciliation&&calls==1,
            "restart retried an unresolved transaction");
   } else {
    require(result==ServeResult::Failed&&calls==1&&waits==0&&state==ResponseState::RpcRejected,"unsafe RPC rejection retried");
    require(svc.serve(f.tx,daa,providers_for(f),effects,daa+10,daa)==ServeResult::Failed&&calls==1&&inferences==1,
            "known rejection was misreported as a successfully served duplicate");
   }
   std::cout<<"PRIVATE_ADMISSION_RETRY="<<mode<<" PASS calls="<<calls<<" inference="<<inferences<<"\n";
  }
  {
   AiResponseService svc(config_for(public_f,dir/"ipfs-failure.journal"));svc.recover();
   auto effects=effects_for(endpoint,infer,service,ipfs_api);const auto before=service.count();
   effects.publish_and_readback=[](std::span<const std::uint8_t>)->std::array<std::uint8_t,34>{throw std::runtime_error("injected IPFS integrity failure");};
   require(svc.serve(public_f.tx,mainnet_private_inference_gate_daa-1,providers_for(public_f),effects)==ServeResult::Failed,"IPFS integrity error did not fail closed");
   require(service.count()==before,"IPFS integrity error sent an AiResponse");
   std::cout<<"IPFS_FAILURE_BEFORE_SUBMIT=PASS mainnet_tx=false\n";
  }
  {
   AiResponseTransport transport(endpoint);
   const auto tx=service.last();const auto id=txid(tx);const auto before=service.count();
   require(transport.submit(tx,id).state==AiResponseEffects::SubmitResult::State::Acknowledged,"pooled transport ACK failed");
   const auto peers=service.peers();service.reject_next();
   require(transport.submit(tx,id).state==AiResponseEffects::SubmitResult::State::Rejected,"pooled transport lost rejection");
   require(transport.submit(tx,id).state==AiResponseEffects::SubmitResult::State::Acknowledged,"pooled transport did not recover from rejection");
   require(service.count()==before+3&&service.peers()==peers,"pooled transport reconnected or retried a request");
   std::cout<<"AI_POOLED_TRANSPORT=PASS ack_reject_ack=true same_peer=true automatic_retry=false mainnet_tx=false\n";
  }
  {
   constexpr std::uint64_t daa=140'115'000;
   for(const bool persist_ok:{true,false}){
    auto config=config_for(private_f,dir/(persist_ok?"h15-native.journal":"h15-persist-error.journal"));config.native_h15=true;
    AiResponseService svc(config);svc.recover();auto effects=effects_for(endpoint,infer,service,ipfs_api);
    unsigned traced_calls=0,persist_calls=0;bool stored=false;const auto before=service.count();
    const std::vector<std::uint8_t> packed{'t','o','k','e','n','s',0,'a','n','s','w','e','r'};
    effects.infer_traced=[&](std::string_view,std::uint32_t,const std::array<std::uint8_t,32>&){
      ++traced_calls;AiTracedAnswer result;result.answer="native answer";result.sealed_plaintext=packed;result.commitment.fill(6);
      for(unsigned i=0;i<4;++i){result.commitment[32+i]=std::uint8_t(8U>>(8*i));result.commitment[36+i]=std::uint8_t(12U>>(8*i));}result.commitment[40]=2;return result;
    };
    effects.persist_trace=[&](std::string_view id,std::span<const std::uint8_t>body,std::uint64_t at,std::uint64_t expiry){
      ++persist_calls;require(at==daa&&expiry==daa+500&&service.count()==before,"H15 response sent before trace persistence");
      require(stock_response_trace_v2(body).has_value(),"H15 trace extension missing before persistence");
      require(id.size()==64,"H15 persisted response identity missing");if(!persist_ok)throw std::runtime_error("injected trace fsync error");stored=true;
    };
    const auto submit=effects.submit;effects.submit=[&](const protowire::RpcTransaction&tx,std::string_view id){require(stored,"H15 network effect precedes durable trace");return submit(tx,id);};
    const auto result=svc.serve(private_f.tx,daa,providers_for(private_f),effects,daa+500,daa);
    require(traced_calls==1&&persist_calls==1,"H15 native path was not wired");
    if(persist_ok){
      require(result==ServeResult::Submitted&&service.count()==before+1,"H15 response not submitted after persistence");
      auto parsed=parse_request(private_f.tx,daa);std::vector<std::uint8_t> opened;
      require(parsed&&stock_verify_response(unhex(service.last().payload()),parsed->request_hash,private_f.escrow,private_f.requester_secret,&opened)&&opened==packed,
        "H15 private response failed token-plaintext/signature binding");
      require(svc.serve(private_f.tx,daa,providers_for(private_f),effects,daa+500,daa)==ServeResult::Duplicate&&traced_calls==1,"H15 reconnect regenerated an acknowledged answer");
      const auto saved_id=svc.journal().get(parsed->stable_id)->transaction_id;
      for(const bool known:{false,true}){
       auto recovered_config=config_for(private_f,dir/(known?"h15-recovered-known.journal":"h15-recovered-unknown.journal"));recovered_config.native_h15=true;
       AiResponseService recovered(recovered_config);recovered.recover();auto recovered_effects=effects;
       recovered_effects.recover_traced=[&](const std::array<std::uint8_t,32>&request)->std::optional<std::string>{require(request==parsed->request_hash,"H15 recovery request binding changed");return saved_id;};
       recovered_effects.reconcile=[&](std::string_view id){require(id==saved_id,"H15 recovery changed signed txid");return known;};
       require(recovered.serve(private_f.tx,daa,providers_for(private_f),recovered_effects,daa+500,daa)==(known?ServeResult::Duplicate:ServeResult::WaitingReconciliation),"H15 durable response recovery failed");
       require(traced_calls==1&&persist_calls==1&&service.count()==before+1,"H15 recovery regenerated or resent outside durable scheduler");
       require(recovered.journal().get(parsed->stable_id)->transaction_id==saved_id,"H15 recovery lost response identity");
      }

    }else require(result==ServeResult::Failed&&service.count()==before,"H15 persistence failure reached SubmitTransaction");
   }
   std::cout<<"H15_NATIVE_SERVICE=PASS durable_before_submit=true private_tokens=SEALED fsync_failure=NO_SEND duplicate=NO_REGENERATION signed_response_recovery=PASS\n";
  }
  server->Shutdown();
  if(pom)pom->stop();
#ifdef NEXUS_TEST_HAS_VULKAN
  if(amd_pom)amd_pom->stop();
#endif
  if(vulkan_inference||vulkan_process)std::cout<<"AMD_AI_RESPONSE_MOCK_E2E=PASS backend=Vulkan public=true private=true signatures=official mainnet_transactions=0 production_admitted=false\n";
  std::cout<<"AI_RESPONSE_MOCK_E2E=PASS public=1 private=1 inference_callback=verified_payload_signature=PASS submit_mock=2 dedup=PASS restart_recovery=PASS ipfs="<<(ipfs_api.empty()?"callback":"production-adapter-mock")<<" mainnet_tx=false\n";
  return 0;
 }catch(const std::exception &e){std::cerr<<"AI_RESPONSE_MOCK_E2E=FAIL detail="<<e.what()<<"\n";return 1;}
}
