#include "keryx/nexus/gguf_reader.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <chrono>
using namespace keryx::nexus;
static void u32(std::ofstream&o,std::uint32_t v){for(int i=0;i<4;i++)o.put(char(v>>(8*i)));}
static void u64(std::ofstream&o,std::uint64_t v){for(int i=0;i<8;i++)o.put(char(v>>(8*i)));}
int main(int argc,char**argv){try{
 if(argc!=2&&argc!=3)throw std::runtime_error("persistent fixture directory [chunk count] required");
 const std::filesystem::path d=argv[1];if(!d.is_absolute()||d.string().starts_with("/tmp"))throw std::runtime_error("unsafe fixture path");std::filesystem::create_directories(d);
 const auto gfile=d/"fixture.gguf",tree=d/"fixture.tree";
 const std::uint64_t chunks=argc==3?std::stoull(argv[2]):12288;
 if(chunks<8192||chunks>32768||chunks%32)throw std::runtime_error("fixture chunk bound/alignment");
 std::vector<std::uint8_t> data(chunks*32);for(std::size_t i=0;i<data.size();++i)data[i]=std::uint8_t(i*13+(i/32)*7);
 {std::ofstream o(gfile,std::ios::binary);u32(o,0x46554747);u32(o,3);u64(o,1);u64(o,0);u64(o,1);o.put('a');u32(o,1);u64(o,data.size());u32(o,24);u64(o,0);while(std::uint64_t(o.tellp())<64)o.put(0);o.write(reinterpret_cast<char*>(data.data()),data.size());}
 std::vector<consensus::Hash> nodes;nodes.reserve(chunks);for(std::uint64_t i=0;i<chunks;++i)nodes.push_back(consensus::blake3_hash(std::span<const std::uint8_t>(data.data()+i*32,32)));
 {std::ofstream o(tree,std::ios::binary);unsigned level=0;while(true){if((level>0&&level%6==0)||nodes.size()==1)o.write(reinterpret_cast<const char*>(nodes.data()),nodes.size()*32);if(nodes.size()==1)break;std::vector<consensus::Hash> next;for(std::size_t i=0;i<nodes.size();i+=2)next.push_back(consensus::hash_pair(nodes[i],i+1<nodes.size()?nodes[i+1]:nodes[i]));nodes=std::move(next);++level;}}
 model::ReadOnlyGguf g(gfile);model::SparseMerkleIndex old(g,tree,false),candidate(g,tree,true),primed(g,tree,true);
 if((chunks==12288&&candidate.resident_upper_bytes()!=192)||candidate.resident_upper_bytes()>4*1024*1024)throw std::runtime_error("upper cache bound/count mismatch");
 unsigned reads=0;
 for(std::uint64_t i=0;i<chunks/32;++i){
  candidate.prefetch_tile_path(i);const auto expected=old.tile_path(i);
  if(candidate.tile_path(i)!=expected)throw std::runtime_error("prefetch/upper path bytes differ");
  auto h=consensus::merkle_root(g.read_tile(i));auto index=i;
  for(const auto &sibling:expected){h=(index&1)?consensus::hash_pair(sibling,h):consensus::hash_pair(h,sibling);index>>=1;}
  if(h!=nodes[0])throw std::runtime_error("independent duplicate-last Merkle path root mismatch");
  const auto sibling=(i^1U)<chunks/32?(i^1U):i;
  auto reader=[&](std::uint64_t index){++reads;std::array<std::uint8_t,1024> b{};g.read_tile_into(index,b);return b;};
  primed.cache_tile_root(sibling,reader);const auto before=reads;
  primed.cache_tile_root(sibling,reader);
  if(reads!=before)throw std::runtime_error("cached sibling was read twice");
  if(primed.tile_path(i)!=expected)throw std::runtime_error("primed Merkle sibling changed path bytes");
 }
 bool root_rejected=false;try{primed.cache_tile_root(chunks/32,[&](auto){return std::array<std::uint8_t,1024>{};});}catch(const std::out_of_range&){root_rejected=true;}
 if(!root_rejected)throw std::runtime_error("out-of-range sibling cache accepted");
 std::vector<model::SparseMerkleIndex::TileRootInput> batch;
 for(std::uint64_t i=0;i<256;++i){std::array<std::uint8_t,1024> b{};g.read_tile_into(i,b);batch.push_back({i,b});}
 for(unsigned pair=0;pair<6;++pair){
  model::SparseMerkleIndex serial(g,tree,true),parallel(g,tree,true);
  auto measure=[&](bool newer){const auto start=std::chrono::steady_clock::now();
   if(newer)parallel.cache_tile_roots(batch);
   else for(const auto &item:batch)serial.cache_tile_root(item.tile,[&](auto){return item.bytes;});
   const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   std::cout<<"MERKLE_ROOT_BATCH pair="<<pair<<" candidate="<<newer<<" roots=256 ms="<<ms<<'\n';};
  measure(pair%2);measure(!(pair%2));
  for(std::uint64_t i=0;i<256;++i)if(serial.tile_path(i^1U)!=parallel.tile_path(i^1U))throw std::runtime_error("parallel sibling roots changed proof paths");
  const auto before=parallel.path_cache_stats().entries;parallel.cache_tile_roots(batch);
  if(parallel.path_cache_stats().entries!=before)throw std::runtime_error("warm root batch grew cache");
 }
 model::SparseMerkleIndex batch_recovery(g,tree,true);
 batch.back().tile=chunks/32;bool bad_batch=false;
 try{batch_recovery.cache_tile_roots(batch);}catch(const std::out_of_range&){bad_batch=true;}
 if(!bad_batch||batch_recovery.path_cache_stats().entries)throw std::runtime_error("invalid batch partially published");
 batch.resize(257);bad_batch=false;
 try{batch_recovery.cache_tile_roots(batch);}catch(const std::invalid_argument&){bad_batch=true;}
 if(!bad_batch)throw std::runtime_error("oversized root batch accepted");
 batch_recovery.cache_tile_roots({});
 batch.resize(2);batch[0].tile=0;batch[1]=batch[0];batch[1].bytes[0]^=1;bad_batch=false;
 try{batch_recovery.cache_tile_roots(batch);}catch(const std::invalid_argument&){bad_batch=true;}
 if(!bad_batch||batch_recovery.path_cache_stats().entries)throw std::runtime_error("conflicting duplicate published cache entry");
 batch[1]=batch[0];batch_recovery.cache_tile_roots(batch);
 if(batch_recovery.path_cache_stats().entries!=1)throw std::runtime_error("duplicate root counted twice");
 model::SparseMerkleIndex recovery(g,tree,true);bool failed=false;
 try{recovery.cache_tile_root(1,[](auto)->std::array<std::uint8_t,1024>{throw std::runtime_error("reader failure");});}catch(const std::runtime_error&){failed=true;}
 if(!failed||recovery.tile_path(0)!=old.tile_path(0))throw std::runtime_error("reader failure poisoned Merkle cache");
 bool rejected=false;try{candidate.prefetch_tile_path(chunks/32);}catch(const std::out_of_range&){rejected=true;}if(!rejected)throw std::runtime_error("out-of-range prefetch accepted");
 auto corrupt=d/"corrupt.tree";std::filesystem::copy_file(tree,corrupt,std::filesystem::copy_options::overwrite_existing);{std::fstream f(corrupt,std::ios::binary|std::ios::in|std::ios::out);f.seekg(-1,std::ios::end);const auto last=f.get();f.seekp(-1,std::ios::end);f.put(char(last^1));}
 rejected=false;try{model::SparseMerkleIndex bad(g,corrupt,true);}catch(const std::runtime_error&){rejected=true;}if(!rejected)throw std::runtime_error("corrupt upper root accepted");
 std::cout<<"MERKLE_PREFETCH=PASS paths="<<chunks/32<<" byte_identical=true odd_upper_nodes=true bounds=true corrupt_root_rejected=true no_cuda=true\n";
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
