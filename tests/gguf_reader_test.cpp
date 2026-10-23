#include "keryx/nexus/gguf_reader.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <array>
#include <algorithm>

namespace fs=std::filesystem;
static void u32(std::ofstream&o,std::uint32_t v){for(int i=0;i<4;i++)o.put(static_cast<char>(v>>(8*i)));}
static void u64(std::ofstream&o,std::uint64_t v){for(int i=0;i<8;i++)o.put(static_cast<char>(v>>(8*i)));}
static void str(std::ofstream&o,const std::string&s){u64(o,s.size());o.write(s.data(),static_cast<std::streamsize>(s.size()));}
static void tensor(std::ofstream&o,const std::string&name,std::uint64_t off,std::uint64_t n){str(o,name);u32(o,1);u64(o,n);u32(o,0);u64(o,off);}
int main(int argc,char**argv){try{
  if(argc!=2)throw std::runtime_error("test directory argument missing");fs::path d=argv[1];fs::create_directories(d);fs::path p=d/"tiny.gguf";
  {std::ofstream o(p,std::ios::binary|std::ios::trunc);u32(o,0x46554747);u32(o,3);u64(o,2);u64(o,0);tensor(o,"z",1024,264);tensor(o,"a",0,256);while(static_cast<std::uint64_t>(o.tellp())<96)o.put('\0');
   // Canonical order is a then z despite reversed directory order.
   for(unsigned i=0;i<1024;i++)o.put(static_cast<char>(i));for(unsigned i=0;i<1056;i++)o.put(static_cast<char>(255-i));}
  keryx::nexus::model::ReadOnlyGguf g(p);if(g.chunk_count()!=65||g.canonical_tensors()[0].name!="a")throw std::runtime_error("canonical GGUF index mismatch");
  auto t=g.read_tile(0);if(t.size()!=1024||t[0]!=0||t[255]!=255||t[256]!=0)throw std::runtime_error("canonical tile bytes mismatch");
  std::array<std::uint8_t,2048> bulk{};g.read_tiles_into(0,bulk);
  for(unsigned i=0;i<2;++i){const auto single=g.read_tile(i);if(!std::equal(single.begin(),single.end(),bulk.begin()+i*1024))throw std::runtime_error("canonical bulk/single tile mismatch");}
  std::array<std::uint8_t,2048> streaming{};g.read_tiles_streaming(0,streaming);
  if(streaming!=bulk)throw std::runtime_error("Streaming upload changed canonical bytes across tensor boundary");
  bool streaming_range=false;try{g.read_tiles_streaming(1,streaming);}catch(const std::out_of_range&){streaming_range=true;}
  if(!streaming_range)throw std::runtime_error("Streaming tail overrun accepted");
  bool bulk_range=false;try{g.read_tiles_into(1,bulk);}catch(const std::out_of_range&){bulk_range=true;}
  if(!bulk_range)throw std::runtime_error("bulk tail overrun accepted");
  bool range=false;try{(void)g.read_tile(2);}catch(const std::out_of_range&){range=true;}if(!range)throw std::runtime_error("out-of-range tile accepted");
  auto root=g.recompute_root([](std::uint64_t,std::uint64_t){});std::vector<std::uint8_t> canonical(2080);for(unsigned i=0;i<1024;i++){canonical[i]=static_cast<std::uint8_t>(i);canonical[1024+i]=static_cast<std::uint8_t>(255-i);}for(unsigned i=1024;i<1056;i++)canonical[1024+i]=static_cast<std::uint8_t>(255-i);auto expected=keryx::nexus::consensus::merkle_root(canonical);if(root!=expected)throw std::runtime_error("streaming duplicate-last root differs from consensus implementation");
  std::cout<<"GGUF_PARSER=PASS canonical_order=PASS chunk_count=65 odd-tail_root=PASS read_only=PASS\n";fs::remove(p);fs::remove(d);return 0;
}catch(const std::exception&e){std::cerr<<"GGUF_TEST=FAIL "<<e.what()<<"\n";return 1;}}
