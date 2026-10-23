#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace keryx::nexus::consensus;
static void check(bool ok,const char *message) {
  if(!ok)throw std::runtime_error(message);
}
int main(int argc,char **argv) { try {
  if(argc!=2)throw std::invalid_argument("Expected explicitly authorized GPU UUID");
  constexpr std::uint64_t tiles=257;
  std::vector<std::uint8_t> model(tiles*1024);
  for(std::size_t i=0;i<model.size();++i)model[i]=mix64(i+9157)&255;
  VulkanWalkEngine engine(argv[1],"bda-native","bda-native",16,tiles);
  check(engine.screens_target_on_gpu(),"BDA screening capability absent");
  const auto upload=[&]{return engine.load_resident([&](auto first,auto bytes){
    std::copy_n(model.data()+first*1024,bytes.size(),bytes.data());
  });};
  check(upload(),"Synthetic resident upload failed");
  std::array<std::uint64_t,16> seeds{};
  std::array<Walk,16> walks;
  for(unsigned i=0;i<seeds.size();++i){seeds[i]=mix64(i+123);walks[i]=walk_v4(seeds[i],model);}
  for(unsigned count:{1,3,4,7,16}) {
    VulkanWalkBatch out;
    check(engine.run(tiles,std::span(seeds).first(count),{},{},true,out),"BDA diagnostic run failed");
    check(out.offsets.size()==count*256&&out.diagnostic_states.size()==count*257*1024,
          "BDA diagnostic dimensions mismatch");
    for(unsigned i=0;i<count;++i) {
      check(std::equal(walks[i].offsets.begin(),walks[i].offsets.end(),out.offsets.begin()+i*256),"Offset parity failed");
      check(std::equal(walks[i].states.back().begin(),walks[i].states.back().end(),out.final_states.begin()+i*1024),"Final state parity failed");
      check(std::memcmp(walks[i].states.data(),out.diagnostic_states.data()+i*257*1024,257*1024)==0,"Full walk parity failed");
    }
  }
  constexpr std::uint64_t salt[4]={0x7C99D381176D4EC4ULL,0xC2E28E3E28118C36ULL,0xD496CE1B129B76CAULL,0x47CF0979FA580BCEULL};
  for(unsigned trial=0;trial<16;++trial) {
    Hash pph{},target{};
    for(unsigned b=0;b<32;++b)pph[b]=mix64(trial*32+b)&255;
    target=pom_pow_value_h3(final_state(walks[trial]),pph);
    if(trial==0)target.fill(0);
    if(trial==1)target.fill(255);
    std::uint64_t words[4]{},targets[4]{};
    for(unsigned i=0;i<4;++i){
      for(unsigned b=0;b<8;++b){words[i]|=std::uint64_t(pph[8*i+b])<<(8*b);targets[i]|=std::uint64_t(target[8*i+b])<<(8*b);}
      words[i]^=salt[i];
    }
    std::optional<std::uint32_t> expected;
    for(unsigned i=0;i<16;++i)if(little_endian_leq(pom_pow_value_h3(final_state(walks[i]),pph),target)){expected=i;break;}
    VulkanWalkBatch out;
    check(engine.run(tiles,seeds,{},{},false,out,{}, {},false,words,targets),"BDA screening failed");
    check(out.final_states.empty()&&out.offsets.empty(),"Screening unexpectedly copied states");
    check(out.winner_index==expected,"GPU target screening disagreed with canonical CPU");
  }
  bool cancelled=false;VulkanWalkBatch out;
  check(!engine.run(tiles,seeds,{},{},false,out,[&]{return cancelled;},[&]{cancelled=true;},false),"Cancelled result escaped");
  check(out.final_states.empty()&&!out.winner_index,"Cancelled output retained");
  bool callback_error=false;
  try{engine.run(tiles,seeds,{},{},false,out,{},[]{throw std::runtime_error("injected enqueue failure");},false);}
  catch(const std::runtime_error&){callback_error=true;}
  check(callback_error,"Enqueue error was swallowed");
  check(engine.run(tiles,std::span(seeds).first(1),{},{},true,out),"Drained engine failed after callback exception");
  engine.release_resident();
  check(engine.stats().allocated_bytes==0,"Model VRAM accounting not released");
  check(upload(),"Resident reload failed");
  check(engine.run(tiles,std::span(seeds).first(1),{},{},true,out),"Reloaded BDA run failed");
  check(std::equal(walks[0].states.back().begin(),walks[0].states.back().end(),out.final_states.begin()),"Reload parity failed");
  std::cout<<"BDA_VALIDATION=PASS full_walks=31 target_cases=16 tail_batches=5 cancellation=PASS callback_drain=PASS release_reload=PASS\n";
  return 0;
}catch(const std::exception&e){std::cerr<<"BDA_VALIDATION=FAIL "<<e.what()<<'\n';return 1;}}
