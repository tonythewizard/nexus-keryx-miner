#include "keryx/nexus/platform/read_only_file.hpp"
#include "keryx/nexus/bounded_file_sha256.hpp"
#include "keryx/nexus/platform/io.hpp"
#ifdef _WIN32
#include "keryx/nexus/platform/windows_process.hpp"
#endif
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using namespace keryx::nexus;
static void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
  require(argc==2,"fixture directory required");
  const auto root=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(root);
  const auto path=root/std::filesystem::u8path("offset-\xc3\xa8 fixture.dat");
  std::vector<unsigned char> data(1048576+37);
  for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<unsigned char>(i*13+i/255);
  {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(data.data()),data.size());require(bool(out),"write fixture");}
  {
    platform::ReadOnlyFile file(path);const auto identity=file.identity();
    require(identity.size==data.size(),"identity size");
    auto* mapping=file.map();require(mapping,"mapping unavailable");
    require(std::equal(data.begin(),data.end(),mapping),"mapped bytes changed");
    std::atomic_bool good{true};std::vector<std::thread> threads;
    for(unsigned worker=0;worker<8;++worker)threads.emplace_back([&,worker]{try{
      for(unsigned round=0;round<300;++round){std::array<unsigned char,1027> bytes{};
        const auto offset=(round*104729+worker*1009)%(data.size()-bytes.size());
        file.read_at(bytes.data(),bytes.size(),offset);
        if(!std::equal(bytes.begin(),bytes.end(),data.begin()+offset))good=false;
      }
    }catch(...){good=false;}});
    for(auto& thread:threads)thread.join();require(good,"concurrent positional read changed bytes");
    for(auto offset:{0U,13U,65535U,1048576U}){
      auto count=std::min<std::size_t>(37,data.size()-offset);std::array<unsigned char,37> bytes{};
      file.read_at_streaming(bytes.data(),count,offset);
      require(std::equal(bytes.begin(),bytes.begin()+count,data.begin()+offset),"streaming bytes changed");
    }
    bool short_read=false;try{std::array<char,2> bytes{};file.read_at(bytes.data(),2,data.size()-1);}catch(const std::runtime_error&){short_read=true;}
    require(short_read,"truncated read accepted");require(file.identity()==identity,"read mutated identity");
  }
  const auto abc=root/"abc";{std::ofstream out(abc);out<<"abc";}
  FileHashIoStats stats;
  require(bounded_file_sha256(abc.string(),&stats)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
  require(stats.bytes==3,"hash byte accounting");
  for(const auto& invalid:{root,root/"absent"}){
    bool rejected=false;try{platform::ReadOnlyFile f(invalid);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"nonregular or missing input accepted");
  }
#ifdef _WIN32
  require(platform::quote_windows_argument(L"a b\\")==L"\"a b\\\\\"","trailing-slash argument quoting");
  require(platform::quote_windows_argument(L"a\"b")==L"\"a\\\"b\"","quote escaping");
  const auto private_file=root/"private-file";
  const int fd=platform::io::open(private_file,O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW,0600);
  require(fd>=0,"private file creation");platform::io::Stat st{};
  require(platform::io::fstat(fd,&st)==0 && !(st.st_mode&077) && st.st_uid==platform::io::getuid(),"created ACL is not private");
  require(platform::io::write(fd,"key-fixture",11)==11 && platform::io::fsync(fd)==0,"durable private write");
  platform::io::close(fd);std::filesystem::remove(private_file);
#endif
  std::cout<<"PLATFORM_FILE=PASS concurrent_reads=2400 unicode=true mapped=true streaming=true bounds=true sha256=true\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
