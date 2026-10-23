#include "keryx/nexus/gguf_reader.hpp"
#include <blake3.h>
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <openssl/evp.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace keryx::nexus::model {
namespace {
class Reader {
public:
  explicit Reader(std::ifstream &in) : in_(in) {}
  std::uint32_t u32() { std::array<std::uint8_t,4>b{}; bytes(b.data(),b.size()); return std::uint32_t(b[0]) | (std::uint32_t(b[1])<<8) | (std::uint32_t(b[2])<<16) | (std::uint32_t(b[3])<<24); }
  std::uint64_t u64() { std::array<std::uint8_t,8>b{}; bytes(b.data(),b.size()); std::uint64_t v=0; for(unsigned i=0;i<8;i++) v|=std::uint64_t(b[i])<<(8*i); return v; }
  void bytes(void *dst, std::size_t n) { if(n && !in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n))) throw std::runtime_error("GGUF truncated header"); }
  void skip(std::uint64_t n) { if(n > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) throw std::runtime_error("GGUF skip overflow"); in_.seekg(static_cast<std::streamoff>(n),std::ios::cur); if(!in_) throw std::runtime_error("GGUF seek outside file"); }
  std::string str() { auto n=u64(); if(n>64U*1024U*1024U) throw std::runtime_error("GGUF unreasonable string"); std::string s(static_cast<std::size_t>(n),'\0'); bytes(s.data(),s.size()); return s; }
private: std::ifstream &in_;
};
std::pair<std::uint64_t,std::uint64_t> type_layout(std::uint32_t t) {
  switch(t) {
  case 0:return {1,4}; case 1:return {1,2}; case 2:return {32,18}; case 3:return {32,20};
  case 6:return {32,22}; case 7:return {32,24}; case 8:return {32,34}; case 9:return {32,36};
  case 10:return {256,84}; case 11:return {256,110}; case 12:return {256,144}; case 13:return {256,176};
  case 14:return {256,210}; case 15:return {256,292}; case 20:return {32,18}; case 23:return {256,136};
  case 24:return {1,1}; case 25:return {1,2}; case 26:return {1,4}; case 27:return {1,8}; case 28:return {1,8}; case 30:return {1,2};
  default: throw std::runtime_error("unsupported GGML dtype " + std::to_string(t));
  }
}
void skip_value(Reader &r,std::uint32_t t) {
  switch(t) {
  case 0:case 1:case 7:r.skip(1);return; case 2:case 3:r.skip(2);return;
  case 4:case 5:case 6:r.skip(4);return; case 8:(void)r.str();return;
  case 10:case 11:case 12:r.skip(8);return;
  case 9: {
    const auto et=r.u32(); const auto n=r.u64(); std::uint64_t size=0;
    switch(et){case 0:case 1:case 7:size=1;break;case 2:case 3:size=2;break;case 4:case 5:case 6:size=4;break;case 10:case 11:case 12:size=8;break;default:break;}
    if(size){if(n>std::numeric_limits<std::uint64_t>::max()/size) throw std::runtime_error("GGUF array size overflow");r.skip(n*size);} else {if(n>1'000'000) throw std::runtime_error("GGUF unreasonable variable array");for(std::uint64_t i=0;i<n;i++)skip_value(r,et);} return;
  }
  default:throw std::runtime_error("unknown GGUF metadata type " + std::to_string(t));
  }
}
consensus::Hash hash_bytes(const void*p,std::size_t n){blake3_hasher h;blake3_hasher_init(&h);blake3_hasher_update(&h,p,n);consensus::Hash out{};blake3_hasher_finalize(&h,out.data(),out.size());return out;}
consensus::Hash pair_hash(const consensus::Hash&a,const consensus::Hash&b){std::array<std::uint8_t,64> in{};std::copy(a.begin(),a.end(),in.begin());std::copy(b.begin(),b.end(),in.begin()+32);return hash_bytes(in.data(),in.size());}
std::string field(const std::string &json,const std::string &key){std::smatch m;const auto re=std::regex("\\\""+key+"\\\"\\s*:\\s*([0-9]+)");if(!std::regex_search(json,m,re))throw std::runtime_error("sidecar missing numeric field: "+key);return m[1].str();}
consensus::Hash byte_array(const std::string&json,const std::string&key){std::smatch m;const auto re=std::regex("\\\""+key+"\\\"\\s*:\\s*\\[([^\\]]*)\\]");if(!std::regex_search(json,m,re))throw std::runtime_error("sidecar missing byte array: "+key);std::stringstream s(m[1].str());consensus::Hash h{};for(std::size_t i=0;i<h.size();i++){unsigned v;char comma;if(!(s>>v)||v>255)throw std::runtime_error("invalid sidecar byte array: "+key);h[i]=static_cast<std::uint8_t>(v);if(i+1<h.size()&&!(s>>comma&&comma==','))throw std::runtime_error("invalid sidecar byte array separator: "+key);}s>>std::ws;if(!s.eof())throw std::runtime_error("sidecar byte array has excess values: "+key);return h;}
consensus::Hash sha256_file(const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("cannot read sidecar tree");EVP_MD_CTX*ctx=EVP_MD_CTX_new();if(!ctx)throw std::runtime_error("SHA-256 context allocation failed");consensus::Hash out{};unsigned n=0;std::array<char,1024*1024>b{};bool ok=EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr)==1;while(ok&&f){f.read(b.data(),static_cast<std::streamsize>(b.size()));auto got=f.gcount();if(got>0)ok=EVP_DigestUpdate(ctx,b.data(),static_cast<std::size_t>(got))==1;}ok=ok&&EVP_DigestFinal_ex(ctx,out.data(),&n)==1;EVP_MD_CTX_free(ctx);if(!ok||n!=out.size())throw std::runtime_error("SHA-256 tree digest failed");return out;}
}

ReadOnlyGguf::ReadOnlyGguf(const std::filesystem::path &path):path_(path) {
  std::ifstream in(path,std::ios::binary); if(!in) throw std::runtime_error("cannot open GGUF read-only: "+path.string());
  in.seekg(0,std::ios::end); const auto end=in.tellg(); if(end<0)throw std::runtime_error("cannot stat GGUF"); file_size_=static_cast<std::uint64_t>(end); in.seekg(0);
  Reader r(in); if(r.u32()!=0x46554747U)throw std::runtime_error("GGUF magic mismatch"); const auto version=r.u32(); if(version<2||version>3)throw std::runtime_error("unsupported GGUF version");
  const auto nt=r.u64(), nkv=r.u64(); if(nt>1'000'000||nkv>1'000'000)throw std::runtime_error("GGUF unreasonable header counts");
  std::uint64_t alignment=32;
  for(std::uint64_t i=0;i<nkv;i++){auto key=r.str();auto type=r.u32(); if(key=="general.alignment"&&(type==4||type==10)){alignment=type==4?r.u32():r.u64();}else skip_value(r,type);}
  if(alignment==0||alignment>(1ULL<<20))throw std::runtime_error("invalid GGUF alignment");
  tensors_.reserve(static_cast<std::size_t>(nt));
  for(std::uint64_t i=0;i<nt;i++){
    Tensor t; t.name=r.str(); const auto dims=r.u32(); if(dims==0||dims>8)throw std::runtime_error("invalid GGUF tensor dimensions");
    std::uint64_t elements=1, first=1;
    for(std::uint32_t d=0;d<dims;d++){const auto n=r.u64();if(d==0)first=n;if(n&&elements>std::numeric_limits<std::uint64_t>::max()/n)throw std::runtime_error("GGUF tensor element overflow");elements*=n;}
    t.ggml_type=r.u32();t.data_offset=r.u64();const auto [block,bytes]=type_layout(t.ggml_type);if(first%block)throw std::runtime_error("tensor row incompatible with GGML block size: "+t.name);if(elements%block||elements/block>std::numeric_limits<std::uint64_t>::max()/bytes)throw std::runtime_error("GGUF tensor byte-size overflow");t.bytes=(elements/block)*bytes;tensors_.push_back(std::move(t));
  }
  const auto pos=static_cast<std::uint64_t>(in.tellg()); if(pos>std::numeric_limits<std::uint64_t>::max()-(alignment-1))throw std::runtime_error("GGUF alignment overflow");tensor_data_offset_=((pos+alignment-1)/alignment)*alignment;
  std::sort(tensors_.begin(),tensors_.end(),[](const Tensor&a,const Tensor&b){return a.name<b.name;});
  for(std::size_t i=1;i<tensors_.size();i++)if(tensors_[i-1].name==tensors_[i].name)throw std::runtime_error("duplicate GGUF tensor name");
  std::vector<std::pair<std::uint64_t,std::uint64_t>> ranges;ranges.reserve(tensors_.size());
  for(const auto&t:tensors_){if(t.data_offset%alignment)throw std::runtime_error("unaligned GGUF tensor offset: "+t.name);if(tensor_data_offset_>file_size_||t.data_offset>file_size_-tensor_data_offset_||t.bytes>file_size_-tensor_data_offset_-t.data_offset)throw std::runtime_error("GGUF tensor exceeds file bounds: "+t.name);ranges.emplace_back(t.data_offset,t.data_offset+t.bytes);const auto chunks=t.bytes/32;if(chunk_count_>std::numeric_limits<std::uint64_t>::max()-chunks)throw std::runtime_error("GGUF chunk count overflow");chunk_count_+=chunks;}
  std::sort(ranges.begin(),ranges.end());for(std::size_t i=1;i<ranges.size();i++)if(ranges[i].first<ranges[i-1].second)throw std::runtime_error("overlapping GGUF tensor data ranges");
  if(chunk_count_==0)throw std::runtime_error("GGUF contains no complete 32-byte chunks");
  std::uint64_t first_chunk=0;
  tile_spans_.reserve(tensors_.size());
  for(const auto&t:tensors_){const auto count=t.bytes/32;if(count){tile_spans_.push_back({first_chunk,count,tensor_data_offset_+t.data_offset});first_chunk+=count;}}
  if(first_chunk!=chunk_count_)throw std::runtime_error("canonical GGUF tile map count mismatch");
  file_fd_=::open(path_.c_str(),O_RDONLY|O_CLOEXEC);
  if(file_fd_<0)throw std::runtime_error("cannot open GGUF for persistent tile reads: "+std::string(std::strerror(errno)));
  void *mapped = ::mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, file_fd_, 0);
  if(mapped != MAP_FAILED) {
    mmap_base_ = static_cast<const std::uint8_t*>(mapped);
    // Demand paging only: do not prefetch the entire GGUF beside the node.
  }
}
ReadOnlyGguf::~ReadOnlyGguf(){
  if(mmap_base_) ::munmap(const_cast<std::uint8_t*>(mmap_base_), file_size_);
  if(file_fd_>=0)::close(file_fd_);
}

std::vector<std::uint8_t> ReadOnlyGguf::read_tile(std::uint64_t tile) const {
  std::vector<std::uint8_t> out(1024);read_tile_into(tile,std::span<std::uint8_t,1024>(out.data(),1024));return out;
}
void ReadOnlyGguf::read_tile_into(std::uint64_t tile,std::span<std::uint8_t,1024> out) const {
  read_tiles_into(tile,out);
}
void ReadOnlyGguf::read_tiles_into(std::uint64_t tile,std::span<std::uint8_t> out) const {
  read_tiles_impl(tile,out,false);
}
void ReadOnlyGguf::read_tiles_streaming(std::uint64_t tile,std::span<std::uint8_t> out) const {
  read_tiles_impl(tile,out,true);
}
void ReadOnlyGguf::read_tiles_impl(std::uint64_t tile,std::span<std::uint8_t> out,bool streaming) const {
  const auto tiles=out.size()/1024;
  if(out.empty()||out.size()%1024||tile>=chunk_count_/32||tiles>chunk_count_/32-tile)
    throw std::out_of_range("bulk tiles outside complete canonical range");
  const auto first=tile*32;std::uint64_t remaining=tiles*32,dest=0,chunk=first;
  auto it=std::upper_bound(tile_spans_.begin(),tile_spans_.end(),chunk,
      [](std::uint64_t value,const TileSpan&span){return value<span.first_chunk;});
  if(it!=tile_spans_.begin())--it;
  while(remaining&&it!=tile_spans_.end()){
    if(chunk<it->first_chunk||chunk>=it->first_chunk+it->chunk_count)throw std::runtime_error("canonical GGUF tile map has a gap");
    const auto local=chunk-it->first_chunk;const auto take=std::min(remaining,it->chunk_count-local);
    const auto fileoff=it->file_offset+local*32;
    if(fileoff>static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))throw std::runtime_error("GGUF tile offset exceeds off_t");
    const auto want=static_cast<std::size_t>(take*32);
    if(!streaming && mmap_base_ && fileoff + want <= file_size_) {
      std::memcpy(out.data()+dest, mmap_base_+fileoff, want);
    } else {
      std::size_t got=0;
      while(got<want){const auto n=::pread(file_fd_,out.data()+dest+got,want-got,static_cast<off_t>(fileoff+got));if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("short GGUF tile pread");got+=static_cast<std::size_t>(n);}
    }
    if(streaming&&::posix_fadvise(file_fd_,static_cast<off_t>(fileoff),static_cast<off_t>(want),POSIX_FADV_DONTNEED)!=0)
      throw std::runtime_error("Cannot release completed streaming GGUF cache range");
    remaining-=take;chunk+=take;dest+=take*32;++it;
  }
  if(remaining)throw std::runtime_error("canonical tile crosses invalid tensor map");
}

consensus::Hash ReadOnlyGguf::recompute_root(const ScanGuard&guard,std::uint64_t interval) const {
  if(interval==0)throw std::invalid_argument("progress interval must be positive");
  const auto fd=::open(path_.c_str(),O_RDONLY|O_CLOEXEC|O_NOATIME);if(fd<0)throw std::runtime_error("cannot open GGUF read-only without atime changes: "+std::string(std::strerror(errno)));
  struct FdGuard{int fd;~FdGuard(){if(fd>=0)::close(fd);}} close_fd{fd};struct stat before{};if(::fstat(fd,&before)!=0)throw std::runtime_error("cannot stat GGUF before root scan");
  std::array<consensus::Hash,64> stack{};std::array<bool,64> used{};std::array<std::uint8_t,2*1024*1024> slab{};std::uint64_t index=0;
  for(const auto&t:tensors_){const auto count=t.bytes/32;std::uint64_t done=0;while(done<count){const auto take=std::min<std::uint64_t>(slab.size()/32,count-done);const auto off=tensor_data_offset_+t.data_offset+done*32;const auto want=static_cast<std::size_t>(take*32);std::size_t got=0;while(got<want){const auto n=::pread(fd,slab.data()+got,want-got,static_cast<off_t>(off+got));if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("short read during GGUF root scan");got+=static_cast<std::size_t>(n);}for(std::uint64_t j=0;j<take;j++){auto h=hash_bytes(slab.data()+j*32,32);std::size_t level=0;while(used[level]){h=pair_hash(stack[level],h);used[level]=false;level++;}if(level>=stack.size())throw std::runtime_error("Merkle accumulator overflow");stack[level]=h;used[level]=true;index++;if(index%interval==0){if(guard)guard(index,chunk_count_);std::cerr<<"GGUF_ROOT_PROGRESS chunks="<<index<<" total="<<chunk_count_<<"\n";}}if(::posix_fadvise(fd,static_cast<off_t>(off),static_cast<off_t>(want),POSIX_FADV_DONTNEED)!=0)throw std::runtime_error("unable to advise kernel to release scanned GGUF pages");done+=take;}}
  struct stat after{};if(::fstat(fd,&after)!=0||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino||before.st_size!=after.st_size||before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec)throw std::runtime_error("GGUF identity or metadata changed during root scan");
  if(index!=chunk_count_)throw std::runtime_error("root scan chunk count changed");
  std::size_t low=0;while(low<used.size()&&!used[low])low++;if(low==used.size())throw std::runtime_error("empty Merkle tree");
  consensus::Hash current=stack[low];std::size_t level=low;
  for(std::size_t next=low+1;next<used.size();next++)if(used[next]){while(level<next){current=pair_hash(current,current);level++;}current=pair_hash(stack[next],current);level=next+1;}
  return current;
}
SidecarStatus ReadOnlyGguf::verify_sidecar(const std::filesystem::path&tree,const std::filesystem::path&metadata,const consensus::Hash&model,const consensus::Hash&root,std::uint64_t chunks)const{
  std::ifstream mf(metadata);if(!mf)throw std::runtime_error("sidecar metadata unavailable");std::string json((std::istreambuf_iterator<char>(mf)),{});if(field(json,"format_version")!="1"||field(json,"layout_version")!="1"||field(json,"chunk_size")!="32"||field(json,"checkpoint_interval")!="6")throw std::runtime_error("unsupported sidecar format/layout");
  SidecarStatus s;s.chunk_count=std::stoull(field(json,"n_chunks"));s.gguf_size=std::stoull(field(json,"gguf_size"));s.tree_size=std::stoull(field(json,"tree_size"));s.declared_root=byte_array(json,"root");s.tree_sha256=byte_array(json,"tree_sha256");if(byte_array(json,"model_id")!=model)throw std::runtime_error("sidecar model id mismatch");if(s.chunk_count!=chunks||chunks!=chunk_count_)throw std::runtime_error("sidecar chunk count mismatch");if(s.gguf_size!=file_size_)throw std::runtime_error("sidecar GGUF size mismatch");if(s.declared_root!=root)throw std::runtime_error("sidecar consensus root mismatch");std::uint64_t cnt=s.chunk_count,expected_size=0;std::uint32_t level=0;while(true){const bool stored=(level>0&&level%6==0)||cnt==1;if(stored){if(cnt>std::numeric_limits<std::uint64_t>::max()/32||expected_size>std::numeric_limits<std::uint64_t>::max()-cnt*32)throw std::runtime_error("sidecar tree size overflow");expected_size+=cnt*32;}if(cnt==1)break;cnt=cnt/2+cnt%2;level++;}if(s.tree_size!=expected_size)throw std::runtime_error("sidecar sparse checkpoint layout size mismatch");if(!std::filesystem::is_regular_file(tree)||std::filesystem::file_size(tree)!=s.tree_size)throw std::runtime_error("sidecar tree missing or size mismatch");if(sha256_file(tree)!=s.tree_sha256)throw std::runtime_error("sidecar tree SHA-256 mismatch");std::ifstream tf(tree,std::ios::binary);tf.seekg(-32,std::ios::end);consensus::Hash stored{};tf.read(reinterpret_cast<char*>(stored.data()),32);if(!tf||stored!=s.declared_root)throw std::runtime_error("sidecar terminal root mismatch");return s;
}

SparseMerkleIndex::SparseMerkleIndex(const ReadOnlyGguf&gguf,std::filesystem::path tree)
  : SparseMerkleIndex(gguf,std::move(tree),true) {}
SparseMerkleIndex::SparseMerkleIndex(const ReadOnlyGguf&gguf,std::filesystem::path tree,bool resident_upper):gguf_(gguf),tree_(std::move(tree)){
  if(gguf_.chunk_count()%32!=0)throw std::runtime_error("sparse tile proof currently requires a whole-tile H6 model");
  std::uint64_t count=gguf_.chunk_count(),offset=0;std::uint32_t level=0;
  while(true){const bool stored=(level>0&&level%6==0)||count==1;if(stored){checkpoints_.push_back({level,offset,count});if(count>std::numeric_limits<std::uint64_t>::max()/32||offset>std::numeric_limits<std::uint64_t>::max()-count*32)throw std::runtime_error("sparse checkpoint offset overflow");offset+=count*32;}if(count==1)break;count=count/2+count%2;level++;}
  total_levels_=level+1;if(!std::filesystem::is_regular_file(tree_)||std::filesystem::file_size(tree_)!=offset)throw std::runtime_error("sparse Merkle tree size does not match official layout");
  tree_fd_=::open(tree_.c_str(),O_RDONLY|O_CLOEXEC);
  if(tree_fd_<0)throw std::runtime_error("cannot open sparse Merkle tree: "+std::string(std::strerror(errno)));
  // Pin only the upper sparse checkpoint and its derived levels. Never load the
  // bottom checkpoint or GGUF into another host-sized cache (4 MiB hard cap).
  try {
    const auto cp=std::find_if(checkpoints_.begin(),checkpoints_.end(),[](const Checkpoint&x){return x.level==12;});
    if(resident_upper && cp!=checkpoints_.end() && cp->count<=65536) {
      upper_levels_.resize(total_levels_);
      auto &first=upper_levels_[12];first.resize(cp->count);
      const auto bytes=first.size()*sizeof(consensus::Hash);std::size_t got=0;
      while(got<bytes){const auto n=::pread(tree_fd_,reinterpret_cast<char*>(first.data())+got,bytes-got,static_cast<off_t>(cp->offset+got));if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("short upper Merkle checkpoint read");got+=n;}
      upper_bytes_=bytes;
      for(std::uint32_t l=13;l<total_levels_;++l){
        const auto &previous=upper_levels_[l-1];auto &next=upper_levels_[l];next.resize((previous.size()+1)/2);
        for(std::size_t i=0;i<previous.size();i+=2)next[i/2]=pair_hash(previous[i],i+1<previous.size()?previous[i+1]:previous[i]);
        upper_bytes_+=next.size()*sizeof(consensus::Hash);
      }
      if(upper_bytes_>4ULL*1024*1024)throw std::runtime_error("upper Merkle cache exceeds bound");
      const auto root_cp=std::find_if(checkpoints_.begin(),checkpoints_.end(),[](const Checkpoint&x){return x.count==1;});
      if(root_cp==checkpoints_.end() || upper_levels_.back().size()!=1 || upper_levels_.back()[0]!=read_node(*root_cp,0))
        throw std::runtime_error("upper Merkle checkpoint root mismatch");
    }
  }catch(...){::close(tree_fd_);tree_fd_=-1;throw;}

}
SparseMerkleIndex::~SparseMerkleIndex(){if(tree_fd_>=0)::close(tree_fd_);}
std::uint64_t SparseMerkleIndex::count_at(std::uint32_t level)const{std::uint64_t count=gguf_.chunk_count();for(std::uint32_t i=0;i<level;i++)count=count/2+count%2;return count;}
consensus::Hash SparseMerkleIndex::read_node(const Checkpoint&cp,std::uint64_t index)const{
  if(index>=cp.count)throw std::out_of_range("Merkle checkpoint node index");
  if(index>(std::numeric_limits<std::uint64_t>::max()-cp.offset)/32)
    throw std::runtime_error("Merkle checkpoint offset overflow");
  const auto off=cp.offset+index*32;
  if(off>static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    throw std::runtime_error("Merkle checkpoint offset exceeds off_t");
  consensus::Hash h{};std::size_t done=0;
  while(done<h.size()){
    const auto n=::pread(tree_fd_,h.data()+done,h.size()-done,static_cast<off_t>(off+done));
    if(n<0&&errno==EINTR)continue;
    if(n<=0)throw std::runtime_error("short sparse Merkle checkpoint pread");
    done+=static_cast<std::size_t>(n);
  }
  return h;
}
namespace {
constexpr std::size_t merkle_path_cache_capacity=16384;
std::uint64_t merkle_cache_key(std::uint32_t level,std::uint64_t index){
  if(level>255||index>0x00ffffffffffffffULL)throw std::overflow_error("Merkle cache key overflow");
  return (static_cast<std::uint64_t>(level)<<56)|index;
}
}
consensus::Hash SparseMerkleIndex::cached_node(std::uint32_t level,std::uint64_t index)const{
  const auto key=merkle_cache_key(level,index);
  {std::lock_guard lock(cache_mutex_);const auto it=path_cache_.find(key);if(it!=path_cache_.end()){path_cache_hits_++;return it->second;}path_cache_misses_++;}
  const auto cp=std::find_if(checkpoints_.begin(),checkpoints_.end(),[&](const Checkpoint&x){return x.level==level;});
  if(cp==checkpoints_.end())throw std::runtime_error("requested uncached Merkle node is not checkpointed");
  const auto value=read_node(*cp,index);cache_node(level,index,value);return value;
}
void SparseMerkleIndex::cache_node(std::uint32_t level,std::uint64_t index,const consensus::Hash&value)const{
  const auto key=merkle_cache_key(level,index);std::lock_guard lock(cache_mutex_);
  const auto found=path_cache_.find(key);if(found!=path_cache_.end()){found->second=value;return;}
  if(path_cache_.size()>=merkle_path_cache_capacity){const auto evict=path_cache_fifo_.front();path_cache_fifo_.pop_front();path_cache_.erase(evict);}
  path_cache_.emplace(key,value);path_cache_fifo_.push_back(key);
}
SparseMerkleIndex::PathCacheStats SparseMerkleIndex::path_cache_stats()const{
  std::lock_guard lock(cache_mutex_);return {path_cache_hits_,path_cache_misses_,path_cache_.size()};
}
namespace {
consensus::Hash canonical_tile_root(const std::array<std::uint8_t,1024> &bytes){
  std::array<consensus::Hash,32> level{};
  for(std::size_t i=0;i<level.size();i++)level[i]=consensus::blake3_hash(std::span<const std::uint8_t>(bytes.data()+i*32,32));
  std::size_t count=level.size();while(count>1){std::size_t next=0;for(std::size_t i=0;i<count;i+=2)level[next++]=consensus::hash_pair(level[i],i+1<count?level[i+1]:level[i]);count=next;}
  return level[0];
}
}
consensus::Hash SparseMerkleIndex::tile_root(std::uint64_t tile)const{
  std::array<std::uint8_t,1024> bytes{};gguf_.read_tile_into(tile,bytes);
  return canonical_tile_root(bytes);
}
void SparseMerkleIndex::cache_tile_root(std::uint64_t tile,
    const std::function<std::array<std::uint8_t,1024>(std::uint64_t)> &reader)const{
  if(tile>=gguf_.chunk_count()/32)throw std::out_of_range("cached Merkle tile outside canonical model");
  const auto key=merkle_cache_key(5,tile);
  {std::lock_guard lock(cache_mutex_);if(path_cache_.contains(key))return;}
  // No cache lock is held across GPU/file I/O; hash construction is identical
  // to the authoritative file-backed path. Rust still verifies the full proof.
  const auto bytes=reader(tile);
  cache_node(5,tile,canonical_tile_root(bytes));
}
void SparseMerkleIndex::cache_tile_roots(std::span<const TileRootInput> tiles)const{
  if(tiles.size()>consensus::walk_steps)throw std::invalid_argument("Merkle root batch exceeds one proof");
  // Validate the whole batch before publishing any cache entry.
  for(const auto &tile:tiles)if(tile.tile>=gguf_.chunk_count()/32)
    throw std::out_of_range("cached Merkle batch tile outside canonical model");
  std::unordered_map<std::uint64_t,std::size_t> unique;unique.reserve(tiles.size());
  std::vector<std::size_t> pending;pending.reserve(tiles.size());
  for(std::size_t i=0;i<tiles.size();++i){
    const auto [found,inserted]=unique.emplace(tiles[i].tile,i);
    if(!inserted&&tiles[found->second].bytes!=tiles[i].bytes)
      throw std::invalid_argument("conflicting canonical Merkle tile bytes");
  }
  {std::lock_guard lock(cache_mutex_);for(std::size_t i=0;i<tiles.size();++i)
    if(unique.at(tiles[i].tile)==i&&!path_cache_.contains(merkle_cache_key(5,tiles[i].tile)))pending.push_back(i);}
  std::vector<consensus::Hash> roots(pending.size());
  auto compute=[&](std::size_t begin,std::size_t end){for(auto i=begin;i<end;++i)
    roots[i]=canonical_tile_root(tiles[pending[i]].bytes);};
  // Sequential hashing for proof batches avoids the pthread_create / join overhead
  // of std::async on proof-sized batches (<=256 tiles), where blake3 SIMD hashing takes < 20us.
  if(pending.size()<=256)compute(0,pending.size());
  else{
    const auto split=pending.size()/2;
    auto lane=std::async(std::launch::async,[&]{compute(0,split);});
    compute(split,pending.size());lane.get();
  }
  // Deterministic insertion order preserves the existing bounded FIFO policy.
  for(std::size_t i=0;i<pending.size();++i)cache_node(5,tiles[pending[i]].tile,roots[i]);
}
consensus::Hash SparseMerkleIndex::subtree(std::uint64_t start,std::uint64_t span,std::uint32_t src_level)const{
  if(span==0||(span&(span-1))!=0)throw std::invalid_argument("Merkle subtree span must be a power of two");
  const auto rounds=static_cast<std::uint32_t>(std::countr_zero(span));
  const auto target_level=src_level+rounds;const auto target_index=start/span;
  {const auto key=merkle_cache_key(target_level,target_index);std::lock_guard lock(cache_mutex_);const auto it=path_cache_.find(key);if(it!=path_cache_.end()){path_cache_hits_++;return it->second;}path_cache_misses_++;}
  const auto cp_it=std::find_if(checkpoints_.begin(),checkpoints_.end(),[&](const Checkpoint&cp){return cp.level==src_level;});
  if(cp_it==checkpoints_.end()||start>=cp_it->count)throw std::runtime_error("missing sparse checkpoint source for Merkle path");
  if(span>32)throw std::runtime_error("sparse subtree exceeds bounded path workspace");
  const auto end=std::min<std::uint64_t>(cp_it->count,start+span);const auto node_count=static_cast<std::size_t>(end-start);
  if(node_count==0||node_count>span)throw std::runtime_error("invalid sparse subtree node range");
  std::array<consensus::Hash,32> level{};std::array<bool,32> present{};
  {
    std::lock_guard lock(cache_mutex_);
    for(std::size_t i=0;i<node_count;i++){const auto key=merkle_cache_key(src_level,start+i);const auto it=path_cache_.find(key);
      if(it==path_cache_.end())path_cache_misses_++;else{level[i]=it->second;present[i]=true;path_cache_hits_++;}}
  }
  std::size_t cursor=0;
  while(cursor<node_count){if(present[cursor]){cursor++;continue;}const auto begin=cursor;while(cursor<node_count&&!present[cursor])cursor++;
    const auto count=cursor-begin;const auto node_index=start+begin;
    if(node_index>(std::numeric_limits<std::uint64_t>::max()-cp_it->offset)/32)throw std::runtime_error("Merkle checkpoint range overflow");
    const auto file_offset=cp_it->offset+node_index*32;
    if(file_offset>static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))throw std::runtime_error("Merkle checkpoint range exceeds off_t");
    const auto bytes=count*sizeof(consensus::Hash);std::size_t got=0;
    while(got<bytes){const auto n=::pread(tree_fd_,level[begin].data()+got,bytes-got,static_cast<off_t>(file_offset+got));if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("short sparse Merkle checkpoint range pread");got+=static_cast<std::size_t>(n);}
    for(std::size_t i=begin;i<cursor;i++){present[i]=true;cache_node(src_level,start+i,level[i]);}
  }
  std::size_t count=node_count;
  for(std::uint32_t r=0;r<rounds;r++){std::size_t next=0;for(std::size_t i=0;i<count;i+=2)level[next++]=consensus::hash_pair(level[i],i+1<count?level[i+1]:level[i]);count=next;}
  if(count!=1)throw std::runtime_error("sparse Merkle subtree did not reduce to one node");
  cache_node(target_level,target_index,level[0]);return level[0];
}
std::vector<consensus::Hash> SparseMerkleIndex::tile_path(std::uint64_t tile)const{
  const auto tile_count=gguf_.chunk_count()/32;if(tile>=tile_count)throw std::out_of_range("tile proof index out of range");
  std::vector<consensus::Hash> path;path.reserve(total_levels_-6);std::uint64_t idx=tile,count=count_at(5);std::uint32_t level=5;
  // Levels 6..11 use disjoint siblings from the same <=64-node level-6
  // checkpoint slab. Read it once instead of six ranges and thousands of
  // per-leaf cache insertions. Higher levels and the GGUF sibling stay exact.
  std::array<consensus::Hash,64> slab{};
  std::uint64_t slab_begin=0,slab_count=0;
  const auto cp6=std::find_if(checkpoints_.begin(),checkpoints_.end(),[](const Checkpoint&cp){return cp.level==6;});
  if(cp6!=checkpoints_.end()) {
    slab_begin=((tile>>1)/64)*64;
    if(slab_begin>=cp6->count)throw std::runtime_error("Merkle slab index outside checkpoint");
    slab_count=std::min<std::uint64_t>(64,cp6->count-slab_begin);
    if(slab_begin>(std::numeric_limits<std::uint64_t>::max()-cp6->offset)/32)
      throw std::runtime_error("Merkle slab offset overflow");
    const auto off=cp6->offset+slab_begin*32;
    if(off>static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))throw std::runtime_error("Merkle slab offset exceeds off_t");
    const auto bytes=slab_count*sizeof(consensus::Hash);std::size_t got=0;
    while(got<bytes){const auto n=::pread(tree_fd_,reinterpret_cast<char*>(slab.data())+got,bytes-got,static_cast<off_t>(off+got));if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("short Merkle slab pread");got+=static_cast<std::size_t>(n);}
  }
  while(count>1){const auto sibling=(idx^1U)<count?(idx^1U):idx;consensus::Hash node{};
    if(level<upper_levels_.size() && !upper_levels_[level].empty()){
      node=upper_levels_[level].at(sibling);
    }else if(level==5){const auto key=merkle_cache_key(level,sibling);bool found=false;
      {std::lock_guard lock(cache_mutex_);const auto it=path_cache_.find(key);if(it!=path_cache_.end()){path_cache_hits_++;node=it->second;found=true;}else path_cache_misses_++;}
      if(!found){node=tile_root(sibling);cache_node(level,sibling,node);}
    }else if(slab_count && level>=6 && level<12) {
      const auto span=1ULL<<(level-6);const auto start=sibling*span;
      if(start<slab_begin||start>=slab_begin+slab_count)throw std::runtime_error("Merkle sibling outside bounded slab");
      const auto available=std::min<std::uint64_t>(span,slab_begin+slab_count-start);
      std::array<consensus::Hash,32> nodes{};
      std::copy_n(slab.begin()+(start-slab_begin),available,nodes.begin());
      auto n=available;
      for(std::uint32_t round=6;round<level;++round){std::uint64_t next=0;for(std::uint64_t i=0;i<n;i+=2)nodes[next++]=consensus::hash_pair(nodes[i],i+1<n?nodes[i+1]:nodes[i]);n=next;}
      if(n!=1)throw std::runtime_error("Merkle slab sibling reduction mismatch");
      node=nodes[0];
    }else{
      const auto exact=std::find_if(checkpoints_.begin(),checkpoints_.end(),[&](const Checkpoint&cp){return cp.level==level;});
      if(exact!=checkpoints_.end())node=cached_node(level,sibling);
      else{const auto src=(level/6)*6;const std::uint64_t span=1ULL<<(level-src);node=subtree(sibling*span,span,src);}
    }
    path.push_back(node);idx>>=1;count=count/2+count%2;level++;
  }
  if(level+1!=total_levels_)throw std::runtime_error("tile Merkle path height mismatch");
  return path;
}
void SparseMerkleIndex::prefetch_tile_path(std::uint64_t tile) const {
  if(tile>=gguf_.chunk_count()/32)throw std::out_of_range("Merkle prefetch tile out of range");
  const auto cp=std::find_if(checkpoints_.begin(),checkpoints_.end(),[](const Checkpoint&x){return x.level==6;});
  if(cp==checkpoints_.end())return;
  // All siblings at levels 6..11 lie in this one group of <=64 checkpoint nodes.
  const auto begin=((tile>>1)/64)*64;
  const auto count=std::min<std::uint64_t>(64,cp->count-begin);
  (void)::posix_fadvise(tree_fd_,static_cast<off_t>(cp->offset+begin*32),static_cast<off_t>(count*32),POSIX_FADV_WILLNEED);
  // Advice is optional; correctness always comes from exact pread + Rust.
  // Unsupported/failed advice falls back to the existing authoritative reads.
}
} // namespace keryx::nexus::model
