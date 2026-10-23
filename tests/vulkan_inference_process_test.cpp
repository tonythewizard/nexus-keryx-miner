#include "keryx/nexus/vulkan_inference_process.hpp"
#include <filesystem>
#include <iostream>
#include <thread>
using namespace keryx::nexus;
int main(int argc,char **argv){try{
  if(argc==7){
    if(std::string_view(argv[4])=="99")return 13;
    struct stat st{};if(fstat(3,&st)||!S_ISREG(st.st_mode))return 12;
    std::string prompt;char c;while(std::cin.get(c))prompt+=c;
    if(prompt=="fail")return 9;
    if(prompt=="wait"){std::this_thread::sleep_for(std::chrono::seconds(5));return 0;}
    if(prompt=="oversize"){std::cout<<std::string(65536,'x');return 0;}
    if(prompt=="env"){
      if(std::string_view(std::getenv("VK_DRIVER_FILES"))!="/usr/share/vulkan/icd.d/nvidia_icd.json"||
         std::string_view(std::getenv("VK_ICD_FILENAMES"))!="/usr/share/vulkan/icd.d/nvidia_icd.json")return 14;
    }
    std::cout<<"verified:"<<prompt;return 0;
  }
  if(argc!=2)throw std::runtime_error("Persistent fixture directory required");
  const std::filesystem::path dir=argv[1];std::filesystem::create_directories(dir);
  const auto path=dir/"lease";
  const int lease=open(path.c_str(),O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
  if(lease<0||flock(lease,LOCK_EX|LOCK_NB))throw std::runtime_error("Test lease unavailable");
  const auto exe=std::filesystem::canonical("/proc/self/exe").string();
  setenv("VK_DRIVER_FILES","parent-mixed-loader",1);
  setenv("VK_ICD_FILENAMES","parent-mixed-loader",1);
  setenv("NEXUS_VULKAN_AI_ICD","/usr/share/vulkan/icd.d/nvidia_icd.json",1);
  unsigned checks=0;
  auto reject=[&](auto f){try{f();}catch(const std::exception&){++checks;return;}throw std::runtime_error("Expected transport rejection");};
  reject([&]{opoi::VulkanInferenceProcess wrong(exe,std::string(64,'0'),"library","sha","model",lease);});
  opoi::VulkanInferenceProcess child(exe,bounded_file_sha256(exe),"library","sha","model",lease);
  if(child.infer("env",16)!="verified:env"||std::string_view(std::getenv("VK_DRIVER_FILES"))!="parent-mixed-loader")
    throw std::runtime_error("Child loader isolation failed");++checks;
  if(child.infer("hello",16)!="verified:hello")throw std::runtime_error("Prompt/output pipe mismatch");++checks;
  if(child.infer(std::string(60*1024,'p'),16).size()!=60*1024+9)throw std::runtime_error("Large prompt stalled");++checks;
  reject([&]{child.infer(std::string(60*1024,'p'),99);});
  reject([&]{child.infer("fail",16);});reject([&]{child.infer("oversize",16);});
  reject([&]{child.infer(std::string(60*1024+1,'p'),16);});
  reject([&]{child.infer("wait",16,[]{return true;});});
  const auto cancellation_start=std::chrono::steady_clock::now();
  reject([&]{child.infer("wait",16,[&]{return std::chrono::steady_clock::now()-cancellation_start>std::chrono::milliseconds(100);});});
  const auto begin=std::chrono::steady_clock::now();
  reject([&]{child.infer("wait",16,{},std::chrono::seconds(1));});
  if(std::chrono::steady_clock::now()-begin>std::chrono::seconds(3))throw std::runtime_error("Cooperative child cancellation was not bounded");
  int other=open(path.c_str(),O_RDWR|O_NOFOLLOW|O_CLOEXEC);
  if(other<0||flock(other,LOCK_EX|LOCK_NB)==0)throw std::runtime_error("Child cleanup released parent lease");
  close(other);close(lease);std::filesystem::remove(path);
  int status{};if(waitpid(-1,&status,WNOHANG)!=-1||errno!=ECHILD)throw std::runtime_error("Unreaped inference child");
  std::cout<<"VULKAN_INFERENCE_PROCESS=PASS checks="<<checks<<" inherited_lease=true cancellation=true children=0 gpu_initialized=false\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
