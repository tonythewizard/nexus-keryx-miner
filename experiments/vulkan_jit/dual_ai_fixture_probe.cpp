// AMD-only smoke test of the optional combined CUDA/Qwen test extension.
// Primary reservations and AI scopes are CPU fixtures here, not NVIDIA/Qwen.
#include "dual_gpu_ai_fixture.hpp"
#include <iostream>
using namespace keryx::nexus;
int main(int argc,char **argv){try{
  if(argc!=9)throw std::runtime_error("Usage: probe MODEL TREE META CONTEXT ORACLE OUT SPV SHA");
  std::ifstream f(argv[4],std::ios::binary);std::vector<std::uint8_t> b{std::istreambuf_iterator<char>(f),{}};
  if(b.size()<225||b.size()>2*1024*1024||std::string(b.begin(),b.begin()+4)!="NXRF")throw std::runtime_error("Invalid fixture context");
  const auto le=[&](std::size_t pos){std::uint64_t value=0;for(unsigned i=0;i<8;++i)value|=std::uint64_t(b.at(pos+i))<<(8*i);return value;};
  ProofArtifactContext c;c.seed=le(4);c.chunks=le(12);c.tier=b[20];
  std::copy_n(b.begin()+21,32,c.root.begin());std::copy_n(b.begin()+53,32,c.target.begin());std::copy_n(b.begin()+85,32,c.pph.begin());
  c.timestamp=le(117);c.nonce=le(125);c.daa=le(133);c.generation=le(141);std::copy_n(b.begin()+149,32,c.capture.begin());
  const auto anchor=consensus::official_tier_anchor(c.daa,c.tier);
  if(anchor.root!=c.root||anchor.chunks!=c.chunks)throw std::runtime_error("Unofficial model anchor");
  const consensus::Hash model_id{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
  model::ReadOnlyGguf model(argv[1]);model.verify_sidecar(argv[2],argv[3],model_id,c.root,c.chunks);
  if(std::filesystem::exists(argv[6]))throw std::runtime_error("Evidence directory already exists");
  testing::DualGpuAiFixture fixture(c,model,argv[2],argv[5],argv[6],argv[7],argv[8]);
  for(unsigned round=0;round<2;++round){
    fixture.begin_primary(16);fixture.publish();fixture.wait_active();
    {SecondaryLanePause pause(fixture.lane());}
    fixture.end_primary();fixture.begin_primary(1);fixture.end_primary();fixture.verified_resume();
  }
  fixture.stop();std::cout<<"AMD_AI_FIXTURE_PROBE=PASS actual_vulkan=true rust=PASS primary_cuda=NOT_RUN qwen=NOT_RUN submissions=0\n";
}catch(const std::exception &e){std::cerr<<"AMD_AI_FIXTURE_PROBE=FAIL "<<e.what()<<'\n';return 1;}}
