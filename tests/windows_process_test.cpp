#include "keryx/nexus/platform/windows_process.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <io.h>
#include <fcntl.h>
using namespace keryx::nexus::platform;
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
  _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
  if(argc==2 && std::string(argv[1])=="verify-real-stream"){
    unsigned char mode{};std::cin.read(reinterpret_cast<char*>(&mode),1);
    if(mode==1){Sleep(30000);return 0;}
    std::vector<char> payload(300000);std::cin.read(payload.data(),payload.size());
    if(std::cin.gcount()!=static_cast<std::streamsize>(payload.size()))return 2;
    std::cout.write(payload.data(),payload.size());std::cout.flush();return 0;
  }
  std::vector<wchar_t> path(32768);
  auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
  require(length && length<path.size(),"test executable path");
  std::filesystem::path executable(std::wstring(path.data(),length));
  {
    WindowsVerifierProcess child(executable);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    unsigned char mode{};child.transfer(&mode,1,true,deadline);
    std::vector<unsigned char> sent(300000),received(sent.size());
    for(std::size_t i=0;i<sent.size();++i)sent[i]=static_cast<unsigned char>(i*37);
    child.transfer(sent.data(),sent.size(),true,deadline);
    child.transfer(received.data(),received.size(),false,deadline);
    require(sent==received,"large pipe frame changed");
    bool closed=false;try{child.transfer(&mode,1,false,deadline);}catch(const std::runtime_error&){closed=true;}
    require(closed,"closed child accepted");
  }
  const auto started=std::chrono::steady_clock::now();
  {
    WindowsVerifierProcess child(executable);
    auto deadline=started+std::chrono::milliseconds(300);
    unsigned char mode=1;child.transfer(&mode,1,true,deadline);
    bool timed_out=false;try{child.transfer(&mode,1,false,deadline);}catch(const std::runtime_error&){timed_out=true;}
    require(timed_out,"stuck child did not time out");
  }
  require(std::chrono::steady_clock::now()-started<std::chrono::seconds(5),"owned child cleanup exceeded bound");
  std::cout<<"WINDOWS_PROCESS=PASS large_frame=300000 timeout=true closed_pipe=true owned_cleanup=true\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
