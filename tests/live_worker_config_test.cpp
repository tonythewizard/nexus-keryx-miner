#include "keryx/nexus/live_worker_config.hpp"
#include "keryx/nexus/ai_backend_config.hpp"
#include "keryx/nexus/amd_memory_handoff.hpp"
#include <iostream>
#include <vector>
using namespace keryx::nexus;
int main() {
 if(!primary_pom_selection(nullptr)||!primary_pom_selection("1")||primary_pom_selection("0"))
   throw std::runtime_error("primary PoM selection changed");
 for(const char *bad:{"", "true", "off", "01", "2", " 0"}) {
   bool rejected=false;try{primary_pom_selection(bad);}catch(const std::invalid_argument&){rejected=true;}
   if(!rejected)throw std::runtime_error("invalid primary PoM selection accepted");
 }
 auto rejected_ai=[](auto f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Unsafe AI configuration/release accepted");};
 if(opoi::ai_backend_config(nullptr,nullptr,nullptr,nullptr,nullptr,false))throw std::runtime_error("AI default changed");
 const std::string sha(64,'a');
 if(!opoi::ai_backend_config("vulkan","/helper",sha.c_str(),"/library",sha.c_str(),true))throw std::runtime_error("Pinned Vulkan backend rejected");
 rejected_ai([&]{opoi::ai_backend_config("auto",nullptr,nullptr,nullptr,nullptr,true);});
 rejected_ai([&]{opoi::ai_backend_config("cuda","/helper",sha.c_str(),"/library",sha.c_str(),true);});
 rejected_ai([&]{opoi::ai_backend_config("vulkan","/helper",sha.c_str(),"/library",sha.c_str(),false);});
 rejected_ai([&]{opoi::ai_backend_config("vulkan","relative",sha.c_str(),"/library",sha.c_str(),true);});
 rejected_ai([&]{opoi::ai_backend_config("vulkan","/helper","invalid","/library",sha.c_str(),true);});
 unsigned samples=0;
 AmdMemoryHandoff delayed([&]{++samples;return AmdMemoryHandoff::Sample{samples==2?128ULL*1024*1024:0,0};});
 delayed.wait_released({},std::chrono::milliseconds(200));
 if(samples<5)throw std::runtime_error("AMD release accepted before three stable samples");
 rejected_ai([&]{delayed.wait_released([]{return true;});});
 unsigned busy_sample=0;
 AmdMemoryHandoff busy([&]{return AmdMemoryHandoff::Sample{busy_sample++?256ULL*1024*1024:0,0};});
 rejected_ai([&]{busy.wait_released({},std::chrono::milliseconds(30));});
 const std::string uuid="GPU-00000000-0000-0000-0000-000000000000";
 std::vector<std::string> args={"worker","grpc://fixture:1","keryx:fixture",
  "NEXUS/escrow:"+std::string(64,'0')+"/esig:"+std::string(128,'0'),
  "model","tree","meta","oracle","out","0","4","live","--enable-submit","--enable-ai-response","--enable-escrow-claims"};
 auto parse=[&](const char *visible){std::vector<char*> raw;for(auto &s:args)raw.push_back(s.data());return parse_live_worker_config(raw.size(),raw.data(),visible,uuid);};
 auto config=parse(uuid.c_str());if(!config.policy.enabled()||!config.policy.ai_responses_enabled()||!config.escrow_claims||config.nonce_limit!=0||config.microbatch!=4)throw std::runtime_error("configuration policy changed");
 auto reject=[&](const char *visible){try{parse(visible);}catch(const std::exception&){return;}throw std::runtime_error("unsafe configuration accepted");};
 reject(nullptr);reject("GPU-unapproved");
 if(require_production_gpu_backend(nullptr,nullptr,uuid)!=GpuBackend::CudaDriver)
   throw std::runtime_error("default backend changed");
 for(auto [backend,device]:std::vector<std::pair<const char*,const char*>>{
   {"vulkan-jit","pci:0000:00:00.0"},{"unknown",nullptr},{"cuda-driver","pci:0000:00:00.0"}}) {
   bool blocked=false;try{require_production_gpu_backend(backend,device,uuid);}catch(const std::exception&){blocked=true;}
   if(!blocked)throw std::runtime_error("unvalidated production backend admitted");
 }
 require_production_gpu_backend("cuda-driver",uuid.c_str(),uuid);
 for(auto [index,value]:std::vector<std::pair<int,std::string>>{{9,"257"},{10,"0"},{10,"5"},{11,"shadow"},{13,"--no-ai-response"},{14,"--invalid"},{2,"invalid-payout"},{3,"invalid-delegation"}}){auto old=args[index];args[index]=value;reject(uuid.c_str());args[index]=old;}
 args.pop_back();args[11]="shadow";args[12]="--no-submit";args[13]="--no-ai-response";args[9]="1";
 config=parse(uuid.c_str());if(config.policy.enabled()||config.policy.ai_responses_enabled()||config.escrow_claims)throw std::runtime_error("shadow authorization changed");
 std::cout<<"LIVE_WORKER_CONFIG_TEST=PASS uuid=exclusive policies=fail_closed secrets=redacted\n";
}
