// Offline same-process engine lifetime test. No node or submission transport.
#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include "work_replay.hpp"
#include <fstream>
#include <iostream>
using namespace keryx::nexus::consensus;
using Bytes=std::vector<std::uint8_t>;
static Bytes read(const char* path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f || f.tellg()<0 || f.tellg()>4*1024*1024)throw std::runtime_error("Invalid bounded persistent fixture");f.seekg(0);return Bytes(std::istreambuf_iterator<char>(f),{});}
static std::uint64_t le(const Bytes& b,size_t pos,unsigned n){if(pos>b.size() || n>b.size()-pos)throw std::runtime_error("Truncated fixture");std::uint64_t x=0;for(unsigned i=0;i<n;++i)x|=std::uint64_t(b[pos+i])<<(i*8);return x;}
struct Input {std::uint64_t logical{};std::vector<std::uint64_t> seeds;Bytes tiles;std::vector<std::uint32_t> slots;};
static Input parse(const Bytes& raw){
 if(raw.size()<36 || std::memcmp(raw.data(),"NXWG1\0\0\0",8) || le(raw,8,4)!=1 || le(raw,12,4)!=1 || le(raw,24,4)!=256 || raw.size()!=28+8+256*1024+256*4)throw std::runtime_error("Expected single-nonce official-model fixture");
 Input x;x.logical=le(raw,16,8);x.seeds.push_back(le(raw,28,8));x.tiles.assign(raw.begin()+36,raw.begin()+36+256*1024);
 for(unsigned i=0;i<256;++i)x.slots.push_back(le(raw,36+256*1024+i*4,4));return x;
}
static void write(const std::filesystem::path& p,const VulkanWalkBatch& out){
 if(std::filesystem::exists(p))throw std::runtime_error("Refusing to overwrite GPU output");
 std::ofstream f(p,std::ios::binary);f.write("NXWO1\0\0\0",8);const std::array<char,16> guard=[](){std::array<char,16>x{};x.fill(char(0xcd));return x;}();
 for(unsigned i=0;i<3;++i){const char* data=i==0?reinterpret_cast<const char*>(out.offsets.data()):i==1?reinterpret_cast<const char*>(out.final_states.data()):reinterpret_cast<const char*>(out.diagnostic_states.data());const auto size=i==0?out.offsets.size()*8:i==1?out.final_states.size():out.diagnostic_states.size();f.write(data,size);f.write(guard.data(),guard.size());}
 if(!f)throw std::runtime_error("Cannot save persistent GPU output");
}
int main(int argc,char** argv){try{
 if(argc!=9)throw std::runtime_error("Usage: persistent-probe SPV SHA INPUT1 CONTEXT1 INPUT2 CONTEXT2 OUTDIR PCI");
 const auto first=parse(read(argv[3])),second=parse(read(argv[5]));const std::filesystem::path outdir=argv[7];
 std::filesystem::create_directories(outdir);VulkanWalkEngine engine(argv[8],argv[1],argv[2],4);auto allocated=engine.stats().allocated_bytes;
 VulkanWalkBatch out;
 auto execute=[&](const Input& in,const char* context,const char* event,const char* output){
  WorkReplay work(context,event,in.seeds[0]);work.before_dispatch();
  // WorkReplay's event is injected immediately after actual queue submission.
  bool event_cancelled=false;
  const bool completed=engine.run(in.logical,in.seeds,in.tiles,in.slots,true,out,[&]{return event_cancelled;},[&]{work.after_enqueue();event_cancelled=std::string(event)!="none";});
  const bool eligible=work.after_fence();
  if(completed!=eligible)throw std::runtime_error("Engine/controller cancellation disagreement");
  if(completed)write(outdir/output,out);
  else if(!out.offsets.empty() || !out.final_states.empty() || !out.diagnostic_states.empty())throw std::runtime_error("Cancelled engine leaked candidate output");
  if(engine.stats().allocated_bytes!=allocated)throw std::runtime_error("GPU buffers were reallocated between jobs");
 };
 execute(first,argv[4],"none","first.output");const auto first_result=out;
 execute(second,argv[6],"none","second.output");const auto second_result=out;
 // Exercise all four preallocated workgroups. Repeated seeds here are only
 // layout fixtures, never claimed as distinct mining attempts.
 auto batch=first;batch.seeds={first.seeds[0],second.seeds[0],first.seeds[0],second.seeds[0]};
 batch.tiles.insert(batch.tiles.end(),second.tiles.begin(),second.tiles.end());batch.slots.clear();
 for(unsigned i=0;i<4;++i)for(auto slot:(i%2?second.slots:first.slots))batch.slots.push_back(slot+(i%2?256:0));
 if(first.logical!=second.logical || !engine.run(batch.logical,batch.seeds,batch.tiles,batch.slots,true,out))throw std::runtime_error("Four-nonce batch failed");
 for(unsigned i=0;i<4;++i){const auto &expected=i%2?second_result:first_result;
  if(!std::equal(expected.offsets.begin(),expected.offsets.end(),out.offsets.begin()+i*256) ||
     !std::equal(expected.final_states.begin(),expected.final_states.end(),out.final_states.begin()+i*1024) ||
     !std::equal(expected.diagnostic_states.begin(),expected.diagnostic_states.end(),out.diagnostic_states.begin()+i*257*1024))throw std::runtime_error("Multi-workgroup output differs from verified single-nonce result");}
 // Diagnostics grow once to the largest requested batch, then remain bounded.
 allocated=engine.stats().allocated_bytes;
 if(!engine.run(first.logical,first.seeds,first.tiles,first.slots,false,out) || !out.diagnostic_states.empty() || out.offsets!=first_result.offsets || out.final_states!=first_result.final_states)throw std::runtime_error("Diagnostics-off result mismatch");
 const auto before=engine.stats();
 if(engine.run(first.logical,first.seeds,first.tiles,first.slots,true,out,[]{return true;}))throw std::runtime_error("Pre-cancelled work dispatched");
 if(engine.stats().submitted!=before.submitted)throw std::runtime_error("Pre-cancellation submitted a kernel");
 execute(first,argv[4],"reorg","must-not-exist-reorg.output");
 execute(second,argv[6],"ai","must-not-exist-ai.output");
 execute(first,argv[4],"none","resumed.output");
 const auto counts=engine.stats();
 if(counts.submitted!=7 || counts.completed!=7 || counts.cancelled_before_dispatch!=1 || counts.discarded_after_completion!=2)throw std::runtime_error("Incorrect native completion/cancellation counters");
 auto bad=first;bad.slots[0]=256;
 bool rejected=false;try{engine.run(bad.logical,bad.seeds,bad.tiles,bad.slots,true,out);}catch(const std::invalid_argument&){rejected=true;}
 if(!rejected || engine.stats().submitted!=7)throw std::runtime_error("Invalid slot reached GPU");
 std::cout<<"VULKAN_PERSISTENT_ENGINE=PASS device_instances=1 submitted="<<counts.submitted<<" completed="<<counts.completed<<" cancelled_before=1 discarded_after=2 allocated_bytes="<<allocated<<" steady_state_reallocation=NONE diagnostics_growth=BOUNDED batch4=PASS diagnostics_off=PASS resumed=PASS submission=NOT_LINKED\n";
}catch(const std::exception& e){std::cerr<<"VULKAN_PERSISTENT_ENGINE=FAIL "<<e.what()<<'\n';return 1;}}
