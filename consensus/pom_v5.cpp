#include "keryx/nexus/consensus/pom_v5.hpp"
#include <algorithm>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <future>
#include <bit>
#include <limits>
#include <stdexcept>

namespace keryx::nexus::consensus {
namespace {
// Fixed capacity workers; callers retain their inputs until every task completes.
class ProofPool {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::packaged_task<void()>> tasks;
  bool closing=false;
  std::vector<std::thread> workers;
public:
  ProofPool() {
    try { for(unsigned i=0;i<8;++i) workers.emplace_back([this]{
      for(;;) {
        std::packaged_task<void()> task;
        { std::unique_lock lock(mutex);ready.wait(lock,[&]{return closing||!tasks.empty();});
          if(closing&&tasks.empty())return;
          task=std::move(tasks.front());tasks.pop_front(); }
        task();
      }
    }); } catch(...) { shutdown();throw; }
  }
  void shutdown() {
    {std::lock_guard lock(mutex);closing=true;}ready.notify_all();
    for(auto &worker:workers)if(worker.joinable())worker.join();
  }
  ~ProofPool(){shutdown();}
  std::future<void> submit(std::function<void()> work) {
    std::packaged_task<void()> task(std::move(work));auto result=task.get_future();
    {std::lock_guard lock(mutex);tasks.push_back(std::move(task));}ready.notify_one();return result;
  }
};
std::uint64_t load64(const std::uint8_t *p) {
  std::uint64_t v=0;for(unsigned j=0;j<8;++j)v|=std::uint64_t(p[j])<<(8*j);return v;
}
void store64(std::uint8_t *p,std::uint64_t v) {
  for(unsigned j=0;j<8;++j)p[j]=static_cast<std::uint8_t>(v>>(8*j));
}
std::uint64_t snippet_fold(const std::array<std::uint8_t,tile_bytes>&tile) {
  std::uint64_t h=0;
  for(unsigned i=0;i<8;++i){std::uint32_t w=0;for(unsigned j=0;j<4;++j)w|=std::uint32_t(tile[4*i+j])<<(8*j);h=mix64(h^w);}
  return h;
}
void put(std::vector<std::uint8_t>&out,std::uint64_t v,unsigned n) {
  for(unsigned j=0;j<n;++j)out.push_back(static_cast<std::uint8_t>(v>>(8*j)));
}
}
V5PadSpec official_v5_pad(std::uint8_t tier,const Hash&model,const Hash&root) {
  if(tier>=v5_pad_tiles_by_tier.size())throw std::out_of_range("v5 tier outside 0..4");
  constexpr std::string_view domain="KERYX-POM-V5-PAD";
  std::vector<std::uint8_t> bytes(domain.begin(),domain.end());bytes.push_back(tier);
  bytes.insert(bytes.end(),model.begin(),model.end());bytes.insert(bytes.end(),root.begin(),root.end());
  return {blake3_hash(bytes),v5_pad_tiles_by_tier[tier]};
}
std::array<std::uint64_t,4> v5_pad_seed_words(const Hash&seed) {
  constexpr std::array<std::uint64_t,4> iv{0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
  std::array<std::uint64_t,4>w{};for(unsigned i=0;i<4;++i)w[i]=load64(seed.data()+8*i)^iv[i];return w;
}
Hash v5_pad_entry(const Hash&seed,std::uint64_t index) {
  const auto w=v5_pad_seed_words(seed);auto a=w[0],b=w[1]^index,c=w[2],d=w[3];
  for(unsigned r=0;r<v5_pad_rounds;++r){a+=b;d=std::rotl(d^a,32);c+=d;b=std::rotl(b^c,24);a+=b;d=std::rotl(d^a,16);c+=d;b=std::rotl(b^c,63);}
  Hash out{};store64(out.data(),a);store64(out.data()+8,b);store64(out.data()+16,c);store64(out.data()+24,d);return out;
}
std::array<std::uint8_t,tile_bytes> v5_pad_tile(const Hash&seed,std::uint64_t tile) {
  if(tile>(UINT64_MAX-31)/32)throw std::out_of_range("v5 pad entry index overflow");
  std::array<std::uint8_t,tile_bytes> out{};
  for(unsigned i=0;i<32;++i){const auto entry=v5_pad_entry(seed,tile*32+i);std::copy(entry.begin(),entry.end(),out.begin()+32*i);}
  return out;
}
std::uint32_t v5_pad_steps(std::uint64_t weights,std::uint64_t pads) {
  constexpr auto max_tiles=UINT64_MAX/258;
  if(!weights||!pads||weights>max_tiles||pads>max_tiles)throw std::invalid_argument("invalid or overflowing v5 tile space");
  const auto total=weights+pads;
  return static_cast<std::uint32_t>(std::clamp((256*pads+total/2)/total,std::uint64_t(1),std::uint64_t(255)));
}
std::uint64_t v5_state_fold(const std::array<std::uint8_t,tile_bytes>&state) {
  std::uint64_t h=0;for(unsigned r=0;r<32;++r)h^=mix64(load64(state.data()+32*r)^r);return h;
}
Walk walk_v5_from_model(std::uint64_t seed,std::uint64_t weights,const V5PadSpec&pad,const ModelTileReader&read_weight) {
  const auto pad_steps=v5_pad_steps(weights,pad.tiles);
  if(!read_weight)throw std::invalid_argument("v5 weight reader missing");
  Walk out;out.states.resize(walk_steps+1);out.offsets.reserve(walk_steps);out.states[0]=v4_initial_state_bytes(seed);
  auto off=mix64(seed^0x6D1CCF96AC4D76F9ULL)%weights;
  for(unsigned step=1;step<=walk_steps;++step){
    out.offsets.push_back(off);
    const auto tile=off<weights?read_weight(off):v5_pad_tile(pad.seed,off-weights);
    if(step<walk_steps){
      const auto raw=mix64(seed^(std::uint64_t(step+1)*0x89050E78D34609EFULL)^snippet_fold(tile)^v5_state_fold(out.states[step-1]));
      const bool is_pad=((step+1)*pad_steps)/256!=(step*pad_steps)/256;
      off=is_pad?weights+raw%pad.tiles:raw%weights;
    }
    v4_transition_into(out.states[step],out.states[step-1],tile,step);
  }
  return out;
}
V5ModelProofBuild prove_v5_from_model(std::uint64_t seed,std::uint8_t tier,std::uint64_t weights,const V5PadSpec&pad,
    const ModelTileReader&read_weight,const ModelPathReader&read_path) {
  if(tier>=5||!read_path)throw std::invalid_argument("invalid v5 proof tier/path reader");
  V5ModelProofBuild built;built.walk=walk_v5_from_model(seed,weights,pad,read_weight);built.proof.tier=tier;
  for(const auto off:built.walk.offsets){
    if(off<weights){const auto t=read_weight(off);built.proof.tiles.emplace_back(t.begin(),t.end());built.proof.paths.push_back(read_path(off));}
    else {built.proof.tiles.emplace_back();built.proof.paths.emplace_back();}
  }
  return built;
}
V5Proof prove_v5_from_walk_parallel(const Walk &walk, std::uint8_t tier,
    std::uint64_t weights,
    const ModelTileReader &read_weight, const ModelPathReader &read_path,
    unsigned num_threads) {
  if (tier >= 5 || !read_path || !read_weight)
    throw std::invalid_argument("invalid v5 proof tier or readers");
  if (walk.offsets.size() != walk_steps)
    throw std::invalid_argument("invalid walk offsets size");

  V5Proof proof;
  proof.tier = tier;
  proof.tiles.resize(walk_steps);
  proof.paths.resize(walk_steps);

  if (num_threads == 0 || num_threads > 8) throw std::invalid_argument("v5 proof threads must be at most 8");

  static ProofPool pool;
  const std::size_t chunk_size = (walk_steps + num_threads - 1) / num_threads;
  std::vector<std::future<void>> futures;
  futures.reserve(num_threads);

  try {
  for (unsigned t = 0; t < num_threads; ++t) {
    const std::size_t start = t * chunk_size;
    if (start >= walk_steps) break;
    const std::size_t end = std::min(start + chunk_size, static_cast<std::size_t>(walk_steps));

    futures.push_back(pool.submit([&walk, weights, &read_weight, &read_path, &proof, start, end] {
      for (std::size_t i = start; i < end; ++i) {
        const auto off = walk.offsets[i];
        if (off < weights) {
          const auto t = read_weight(off);
          proof.tiles[i].assign(t.begin(), t.end());
          proof.paths[i] = read_path(off);
        }
      }
    }));
  }

  } catch(...) {
    for(auto &f:futures)if(f.valid())f.wait();
    throw;
  }
  std::exception_ptr failure;
  for(auto &f:futures) {
    try {f.get();} catch(...) {if(!failure)failure=std::current_exception();}
  }
  if(failure)std::rethrow_exception(failure);

  return proof;
}
std::vector<std::uint8_t> borsh_v5_container(const V5Proof&p,std::uint64_t state,const Hash&pow) {
  if(p.tier>=5||p.tiles.size()!=walk_steps||p.paths.size()!=walk_steps)throw std::invalid_argument("invalid v5 proof shape");
  std::vector<std::uint8_t>out;out.reserve(512*1024);
  out.push_back(p.tier);out.insert(out.end(),32,0);out.insert(out.end(),pow.begin(),pow.end());put(out,state,8);
  put(out,0,4);put(out,0,4);put(out,0,4);out.push_back(0);out.push_back(0);out.push_back(1);out.push_back(p.tier);
  put(out,walk_steps,4);
  for(std::size_t i=0;i<walk_steps;++i){const auto&t=p.tiles[i];if((t.empty()&&!p.paths[i].empty())||(!t.empty()&&t.size()!=tile_bytes))throw std::invalid_argument("invalid v5 weight/pad witness");put(out,t.size(),4);out.insert(out.end(),t.begin(),t.end());}
  put(out,walk_steps,4);
  for(const auto&path:p.paths){if(path.size()>64)throw std::invalid_argument("oversized v5 Merkle path");put(out,path.size(),4);for(const auto&h:path)out.insert(out.end(),h.begin(),h.end());}
  return out;
}
}
