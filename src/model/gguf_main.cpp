#include <cstdlib>
#include "keryx/nexus/gguf_reader.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <array>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <sys/resource.h>
#include <chrono>
#include <algorithm>
#include <cctype>

static std::uint64_t mem_available_kib(){std::ifstream f("/proc/meminfo");std::string key,unit;std::uint64_t value;while(f>>key>>value>>unit)if(key=="MemAvailable:")return value;throw std::runtime_error("cannot verify MemAvailable");}
static double memory_full_avg10(){std::ifstream f("/proc/pressure/memory");std::string line;while(std::getline(f,line))if(line.rfind("full ",0)==0){const auto p=line.find("avg10=");if(p==std::string::npos)break;return std::stod(line.substr(p+6));}throw std::runtime_error("cannot verify memory PSI");}
static std::uint64_t swap_free_kib(){std::ifstream f("/proc/meminfo");std::string key,unit;std::uint64_t value;while(f>>key>>value>>unit)if(key=="SwapFree:")return value;throw std::runtime_error("cannot verify SwapFree");}
struct ProcIdentity{int pid;std::string start;};
static ProcIdentity find_proc(const std::string&expected,bool daemon){std::vector<ProcIdentity> found;for(const auto&entry:std::filesystem::directory_iterator("/proc")){const auto name=entry.path().filename().string();if(name.empty()||!std::all_of(name.begin(),name.end(),[](unsigned char c){return std::isdigit(c);}))continue;const auto pid=std::stoi(name);std::error_code ec;const auto exe=std::filesystem::canonical(entry.path()/"exe",ec);if(ec||exe!=expected)continue;if(daemon){std::ifstream c(entry.path()/"cmdline",std::ios::binary);std::string args((std::istreambuf_iterator<char>(c)),{});bool has_daemon=false;std::size_t start=0;while(start<args.size()){auto end=args.find('\0',start);if(end==std::string::npos)end=args.size();if(args.substr(start,end-start)=="daemon")has_daemon=true;start=end+1;}if(!has_daemon)continue;}std::ifstream st(entry.path()/"stat");std::string line;std::getline(st,line);const auto pos=line.rfind(')');if(pos==std::string::npos)continue;std::istringstream tail(line.substr(pos+2));std::string token;for(int i=0;i<=19;i++)if(!(tail>>token)){token.clear();break;}if(!token.empty())found.push_back({pid,token});}if(found.size()!=1)throw std::runtime_error("required service process is not uniquely observable: "+expected+" count="+std::to_string(found.size()));return found.front();}
static bool still_same(const ProcIdentity&i,const std::string&exe){std::error_code ec;if(std::filesystem::canonical("/proc/"+std::to_string(i.pid)+"/exe",ec)!=exe||ec)return false;std::ifstream st("/proc/"+std::to_string(i.pid)+"/stat");std::string line;std::getline(st,line);const auto pos=line.rfind(')');if(pos==std::string::npos)return false;std::istringstream tail(line.substr(pos+2));std::string token;for(int n=0;n<=19;n++)if(!(tail>>token))return false;return token==i.start;}

int main(int argc,char**argv){try{
  if(argc<3)throw std::runtime_error("usage: nexus-gguf-inspect inspect MODEL.gguf | export-index MODEL.gguf INDEX.bin | verify-sidecar MODEL.gguf TREE.bin TREE.json | scan-root MODEL.gguf --confirm-full-read");
  keryx::nexus::model::ReadOnlyGguf gguf(argv[2]);
  if(std::string(argv[1])=="inspect"&&argc==3){
    std::cout<<"READ_ONLY=PASS bytes="<<gguf.file_size()<<" tensor_data_offset="<<gguf.tensor_data_offset()<<" tensors="<<gguf.canonical_tensors().size()<<" chunks="<<gguf.chunk_count()<<" addressable_tiles="<<gguf.chunk_count()/32<<"\n";
  } else if(std::string(argv[1])=="verify-sidecar"&&argc==5){
    keryx::nexus::consensus::Hash model{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
    keryx::nexus::consensus::Hash root{0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};
    const auto status=gguf.verify_sidecar(argv[3],argv[4],model,root,203469888);
    std::cout<<"SIDECAR=PASS format=1 layout=1 chunks="<<status.chunk_count<<" tree_bytes="<<status.tree_size<<" SHA256=verified declared_root=official-H6-tier0\n";
  } else if(std::string(argv[1])=="export-index"&&argc==4){
    const std::filesystem::path out_path=argv[3];if(std::filesystem::exists(out_path))throw std::runtime_error("refusing to overwrite canonical index");std::ofstream out(out_path,std::ios::binary);if(!out)throw std::runtime_error("cannot create canonical index");out.write("NXIX",4);auto write_u32=[&](std::uint32_t x){for(unsigned i=0;i<4;i++)out.put(static_cast<char>(x>>(8*i)));};auto write_u64=[&](std::uint64_t x){for(unsigned i=0;i<8;i++)out.put(static_cast<char>(x>>(8*i)));};write_u64(gguf.file_size());write_u32(static_cast<std::uint32_t>(gguf.canonical_tensors().size()));for(const auto&t:gguf.canonical_tensors()){write_u32(static_cast<std::uint32_t>(t.name.size()));out.write(t.name.data(),static_cast<std::streamsize>(t.name.size()));write_u64(gguf.tensor_data_offset()+t.data_offset);write_u64(t.bytes);}out.flush();if(!out)throw std::runtime_error("canonical index write failed");std::cout<<"CANONICAL_INDEX=EXPORTED tensors="<<gguf.canonical_tensors().size()<<" chunks="<<gguf.chunk_count()<<" path="<<out_path<<"\n";
  } else if(std::string(argv[1])=="scan-root"&&argc==4&&std::string(argv[3])=="--confirm-full-read"){
    const auto avail=mem_available_kib();const auto psi=memory_full_avg10();const auto swap=swap_free_kib();
    std::cout<<"ROOT_SCAN_PREFLIGHT mode=bounded-stream mem_available_kib="<<avail<<" swap_free_kib="<<swap<<" memory_psi_full_avg10="<<psi<<" minimum_mem_available_kib=2097152 minimum_swap_free_kib=2097152 maximum_memory_psi_full_avg10=0.10 max_scan_buffer_bytes=2097152\n";
    if(avail<2ULL*1024*1024||swap<2ULL*1024*1024||psi>0.10){std::cout<<"ROOT_SCAN=BLOCKED resources_below_streaming_threshold; no model payload read\n";return 13;}
    const auto node=find_proc((std::getenv("NEXUS_NODE_EXECUTABLE") ? std::getenv("NEXUS_NODE_EXECUTABLE") : ""),false);const auto ipfs=find_proc("/usr/bin/ipfs",true);
    auto guard=[&](std::uint64_t done,std::uint64_t total){const auto a=mem_available_kib();const auto p=memory_full_avg10();if(a<1024ULL*1024||p>0.50)throw keryx::nexus::model::ResourceLimit("memory/PSI emergency threshold crossed");if(!still_same(node,(std::getenv("NEXUS_NODE_EXECUTABLE") ? std::getenv("NEXUS_NODE_EXECUTABLE") : "")))throw keryx::nexus::model::ResourceLimit("keryxd exited or identity changed during scan");if(!still_same(ipfs,"/usr/bin/ipfs"))throw keryx::nexus::model::ResourceLimit("IPFS exited or identity changed during scan");std::cerr<<"SCAN_HEALTH chunks="<<done<<" total="<<total<<" mem_available_kib="<<a<<" memory_psi_full_avg10="<<p<<" node_pid="<<node.pid<<" ipfs_pid="<<ipfs.pid<<"\n";};
    if(!still_same(node,(std::getenv("NEXUS_NODE_EXECUTABLE") ? std::getenv("NEXUS_NODE_EXECUTABLE") : ""))||!still_same(ipfs,"/usr/bin/ipfs"))throw keryx::nexus::model::ResourceLimit("node/IPFS identity changed before scan");
    const auto scan_started=std::chrono::steady_clock::now();keryx::nexus::consensus::Hash actual{};try{actual=gguf.recompute_root(guard);}catch(const keryx::nexus::model::ResourceLimit&e){std::cerr<<"ROOT_SCAN=BLOCKED "<<e.what()<<"\n";return 13;}const auto scan_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-scan_started).count();struct rusage usage{};if(::getrusage(RUSAGE_SELF,&usage)!=0)throw std::runtime_error("cannot measure root scan peak RSS");
    keryx::nexus::consensus::Hash official{0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};
    if(actual!=official){std::cout<<"ROOT_SCAN=FAIL computed_root_differs_from_H6_tier0\n";return 1;}
    std::cout<<"INDEPENDENT_GGUF_ROOT=PASS chunks="<<gguf.chunk_count()<<" root=official-H6-tier0 scan_elapsed_ms="<<scan_ms<<" peak_rss_kib="<<usage.ru_maxrss<<" buffer_bytes=2097152\n";
  } else throw std::runtime_error("invalid command or argument count");
  return 0;
}catch(const std::exception&e){std::cerr<<"GGUF_INSPECT=FAIL "<<e.what()<<"\n";return 2;}}
