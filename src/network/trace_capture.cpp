#include "keryx/nexus/trace_capture.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace keryx::nexus::opoi::detail {
namespace {
constexpr std::size_t trace_cap = 64U << 20;
constexpr std::size_t trace_host_cap = 512U << 20;
constexpr std::size_t replay_cap = 4U << 20;
void require(bool valid, const char *message) {
  if (!valid) throw std::invalid_argument(message);
}
void text_bound(std::string_view text) {
  require(text.size() <= 60U * 1024 && text.find('\0') == text.npos, "Invalid traced prompt bounds");
}
}
TraceCapture::TraceCapture(TraceApi api, void *model, std::uint32_t context, std::uint32_t context_floor)
    : api_(api), model_(model), context_(context), allocated_context_(context), context_floor_(context_floor) {
  require(model && context && context <= 32768 && api.configure && api.leaf_count &&
          api.generate && api.read && api.replay && context_floor<=context, "Incomplete ABI6 trace runtime");
}
void TraceCapture::use_task_context() {
  require((api_.abi==8 || api_.abi==9) && api_.context_acquire,"Task context requires ABI8/9 context acquisition");
  if (!context_full_ && allocated_context_) return;
  const int got=api_.context_acquire(model_,64);
  allocated_context_=got>0 ? static_cast<std::uint32_t>(got) : 0;
  context_full_=false;
  if(got<=0) fail("task context acquisition",got);
}
void TraceCapture::ensure_full_context() {
  if(context_full_ && allocated_context_) return;
  require((api_.abi==8 || api_.abi==9) && api_.context_acquire,"Full context requires ABI8/9 context acquisition");
  int got=api_.context_acquire(model_,static_cast<int>(context_));
  if(got<0 && context_floor_ && context_floor_<context_)
    got=api_.context_acquire(model_,static_cast<int>(context_floor_));
  allocated_context_=got>0 ? static_cast<std::uint32_t>(got) : 0;
  context_full_=got>0;
  if(got<=0) fail("full context acquisition",got);
}
void TraceCapture::ensure_context_for(std::size_t positions) {
  require(positions && positions<=context_,"Invalid replay context demand");
  if(allocated_context_>=positions) return;
  if(positions<=64 && (api_.abi==8 || api_.abi==9) && api_.context_acquire) {
    const int got=api_.context_acquire(model_,64);
    allocated_context_=got>0 ? static_cast<std::uint32_t>(got) : 0;
    context_full_=false;
    if(allocated_context_>=positions) return;
  }
  ensure_full_context();
  require(allocated_context_>=positions,"Allocated context is smaller than replay demand");
}
[[noreturn]] void TraceCapture::fail(const char *operation, int code) const {
  // Native error text is intentionally not included: it can contain prompt data.
  throw std::runtime_error(std::string("ABI6 ") + operation + " failed (" + std::to_string(code) + ")");
}
std::uint32_t TraceCapture::configure(std::span<const std::uint8_t> manifest, std::uint32_t positions, std::uint32_t expected_leaves) {
  require(!manifest.empty() && manifest.size() <= (1U << 20) && positions && positions <= context_,
          "Invalid trace manifest or position limit");
  const bool chain=manifest.size()>=16 && std::memcmp(manifest.data(),"KXMANIF2",8)==0;
  require(manifest.size()>=16 && (std::memcmp(manifest.data(),"KXMANIF1",8)==0 ||
          (chain && (api_.abi==8 || api_.abi==9) && expected_leaves)), "Unsupported trace manifest/runtime contract");
  std::uint32_t products = 0;
  for (unsigned i = 0; i < 4; ++i) products |= std::uint32_t(manifest[12 + i]) << (8 * i);
  require(products && products <= 4096 && (std::size_t(products) * 2 + 1) <= trace_host_cap / 32 / positions,
          "Trace manifest exceeds capture memory budget");
  if (!chain) { require(!expected_leaves || expected_leaves==2*products+1,"Stage-1 leaf contract mismatch");expected_leaves=2*products+1; }
  require(expected_leaves>=2*products+1 && expected_leaves<=trace_host_cap/32/positions,"Stage-2 capture exceeds bounded memory");
  leaves_ = positions_ = 0; // Any failed reconfiguration invalidates the prior session.
  const auto rc = api_.configure(model_, manifest.data(), manifest.size(), static_cast<int>(positions));
  if (rc) fail("configure", rc);
  const auto leaves = api_.leaf_count(model_);
  require(leaves > 0 && static_cast<std::size_t>(leaves) <= trace_host_cap / 32 / positions,
          "Trace capture exceeds bounded host memory");
  require(static_cast<std::uint32_t>(leaves) == expected_leaves, "ABI6 leaf count does not match manifest");
  leaves_ = static_cast<std::uint32_t>(leaves);
  positions_ = positions;
  return leaves_;
}
TracedGeneration TraceCapture::generate(std::string_view prompt, const TracePrefix *prefix,
    std::uint32_t max_tokens, std::uint32_t min_prompt, std::uint32_t answer_bytes_per_position) {
  require(leaves_ && positions_ && sequence_ready_, "Trace capture not configured");
  text_bound(prompt);
  require(!prompt.empty() && max_tokens && max_tokens <= 4096 && min_prompt <= positions_ &&
          answer_bytes_per_position <= (64U << 10), "Invalid traced generation bounds");
  if (prefix) {
    text_bound(prefix->rest);
    require(!prefix->tokens.empty() && prefix->tokens.size() <= context_ && prefix->head_tokens <= context_ &&
            std::all_of(prefix->tokens.begin(), prefix->tokens.end(), [](auto n) { return n >= 0; }),
            "Invalid request-bound prefix");
  }
  const std::string owned(prompt), rest(prefix ? prefix->rest : std::string_view{});
  ensure_full_context();
  std::vector<char> output(64U << 10);
  TracedGeneration result;
  result.tokens.resize(context_);
  int n_prompt = 0, n_fed = 0;
  const int n = api_.generate(model_, owned.c_str(), prefix ? prefix->tokens.data() : nullptr,
      prefix ? static_cast<int>(prefix->tokens.size()) : 0, prefix ? static_cast<int>(prefix->head_tokens) : 0,
      rest.c_str(), static_cast<int>(max_tokens), output.data(), static_cast<int>(output.size()),
      result.tokens.data(), static_cast<int>(result.tokens.size()), &n_prompt, &n_fed,
      static_cast<int>(min_prompt), static_cast<int>(answer_bytes_per_position));
  if (n < 0) fail("generate", n);
  require(n > 0 && static_cast<std::size_t>(n) < output.size() && n_prompt > 0 && n_fed >= n_prompt &&
          static_cast<std::uint32_t>(n_fed) <= positions_ && static_cast<std::uint32_t>(n_fed) <= allocated_context_,
          "Invalid ABI6 generation dimensions");
  result.text.assign(output.data(), static_cast<std::size_t>(n));
  require(result.text.find('\0') == result.text.npos, "Invalid ABI6 generated text");
  result.tokens.resize(static_cast<std::size_t>(n_fed));
  require(std::all_of(result.tokens.begin(), result.tokens.end(), [](auto n) { return n >= 0; }),
          "Invalid captured token");
  result.prompt_positions = static_cast<std::uint32_t>(n_prompt);
  result.leaves.resize(static_cast<std::size_t>(n_fed) * leaves_);
  result.ubatch.resize(static_cast<std::size_t>(n_fed));
  std::uint32_t n_pos = 0;
  const auto rc = api_.read(model_, reinterpret_cast<std::uint8_t *>(result.leaves.data()),
      result.leaves.size() * 32, result.ubatch.data(), result.ubatch.size(), &n_pos);
  if (rc) fail("read", rc);
  require(n_pos == static_cast<std::uint32_t>(n_fed) &&
          std::all_of(result.ubatch.begin(), result.ubatch.end(), [&](auto n) { return n > 0 && n <= allocated_context_; }),
          "Incomplete ABI6 trace: token and position coverage differ");
  read_sequence(result);
  return result;
}
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> TraceCapture::replay(
    const TracedGeneration &capture, std::uint32_t position, std::uint32_t leaf_x, std::uint32_t leaf_y) {
  require(leaves_ && capture.prompt_positions && capture.prompt_positions <= capture.tokens.size() &&
          capture.tokens.size() <= positions_ && position < capture.tokens.size() &&
          capture.ubatch.size() == capture.tokens.size() && capture.leaves.size() == capture.tokens.size() * leaves_ &&
          leaf_x < leaves_ && leaf_y < leaves_ &&
          std::all_of(capture.tokens.begin(), capture.tokens.end(), [](auto n) { return n >= 0; }),
          "Invalid ABI6 replay bounds");
  std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> result;
  ensure_context_for(capture.tokens.size());
  result.first.resize(replay_cap); result.second.resize(replay_cap);
  std::uint32_t nx = 0, ny = 0;
  const auto rc = api_.replay(model_, capture.tokens.data(), static_cast<int>(capture.tokens.size()),
      static_cast<int>(capture.prompt_positions), position, leaf_x, leaf_y, result.first.data(), result.first.size(),
      result.second.data(), result.second.size(), &nx, &ny);
  if (rc) fail("replay", rc);
  require(nx && ny && nx <= replay_cap && ny <= replay_cap, "Invalid ABI6 replay sizes");
  result.first.resize(nx); result.second.resize(ny);
  return result;
}
TracedGeneration TraceCapture::trace_tokens(std::span<const std::int32_t> tokens) {
  require((api_.abi==8 || api_.abi==9) && api_.trace_tokens && leaves_ && positions_ && sequence_ready_,"Token trace requires ABI8/9");
  require(!tokens.empty() && tokens.size()<=positions_ &&
          std::all_of(tokens.begin(),tokens.end(),[](auto n){return n>=0;}),"Invalid token trace bounds");
  ensure_context_for(tokens.size());
  const auto rc=api_.trace_tokens(model_,tokens.data(),static_cast<int>(tokens.size()));
  if(rc)fail("trace_tokens",rc);
  TracedGeneration result;result.tokens.assign(tokens.begin(),tokens.end());result.prompt_positions=tokens.size();
  result.leaves.resize(tokens.size()*leaves_);result.ubatch.resize(tokens.size());std::uint32_t n=0;
  const auto read=api_.read(model_,reinterpret_cast<std::uint8_t*>(result.leaves.data()),result.leaves.size()*32,
                            result.ubatch.data(),result.ubatch.size(),&n);
  if(read)fail("trace_tokens_read",read);
  require(n==tokens.size() && std::all_of(result.ubatch.begin(),result.ubatch.end(),[&](auto x){return x>0 && x<=allocated_context_;}),"Incomplete token trace");
  read_sequence(result);
  return result;
}
std::vector<std::vector<std::uint8_t>> TraceCapture::replay_multi(const TracedGeneration &capture,
    std::span<const std::pair<std::uint32_t,std::uint32_t>> caps,std::uint32_t slot) {
  require((api_.abi==8 || api_.abi==9) && api_.replay_multi && leaves_ && capture.prompt_positions &&
          capture.prompt_positions<=capture.tokens.size() && capture.tokens.size()<=positions_ &&
          capture.ubatch.size()==capture.tokens.size() && capture.leaves.size()==capture.tokens.size()*leaves_ &&
          std::all_of(capture.tokens.begin(),capture.tokens.end(),[](auto x){return x>=0;}),"Invalid ABI8 replay capture");
  require(!caps.empty() && caps.size()<=32 && slot && slot<=replay_cap && slot%4==0 &&
          caps.size()<=trace_cap/slot,"Invalid ABI8 capture budget");
  std::vector<std::uint32_t> positions,leaves,lens(caps.size());
  for(auto [pos,leaf]:caps){require(pos<capture.tokens.size() && leaf<leaves_,"Invalid ABI8 replay point");positions.push_back(pos);leaves.push_back(leaf);}
  std::vector<std::uint8_t> bytes(caps.size()*slot);
  ensure_context_for(capture.tokens.size());
  const auto rc=api_.replay_multi(model_,capture.tokens.data(),static_cast<int>(capture.tokens.size()),
      capture.prompt_positions,positions.data(),leaves.data(),caps.size(),slot,bytes.data(),bytes.size(),lens.data());
  if(rc)fail("replay_multi",rc);
  std::vector<std::vector<std::uint8_t>> result;result.reserve(caps.size());
  for(std::size_t i=0;i<caps.size();++i){require(lens[i] && lens[i]<=slot && lens[i]%4==0,"Invalid ABI8 replay size");result.emplace_back(bytes.begin()+i*slot,bytes.begin()+i*slot+lens[i]);}
  return result;
}
std::uint32_t TraceCapture::configure_sequence(std::span<const SequenceLayer> layers,
    std::uint32_t n_attn,std::uint32_t n_recur,std::uint32_t heads,std::uint32_t interval) {
  require(api_.abi==9 && api_.seq_configure && api_.seq_roots && api_.seq_replay,
          "Sequence capture requires complete ABI9");
  if(layers.empty()) {
    require(!n_attn && !n_recur && !heads && !interval,"Invalid sequence disable contract");
  } else {
    require(n_attn<=512 && n_recur<=512 && layers.size()==std::size_t(n_attn)+n_recur &&
            layers.size()<=512 && heads && heads<=256 && interval && interval<=context_,
            "Invalid sequence layout dimensions");
    std::vector<bool> seen_layer(512),seen_attn(n_attn),seen_recur(n_recur);
    for(const auto &row:layers) {
      const auto [layer,kind,slot,h,dim]=row;
      require(layer<512 && !seen_layer[layer] && kind>=1 && kind<=4 && h && h<=heads &&
              dim && dim<=4096 && dim%32==0,"Invalid sequence layer");
      seen_layer[layer]=true;
      auto &slots=kind<=2 ? seen_attn : seen_recur;
      require(slot<slots.size() && !slots[slot],"Duplicate or invalid sequence slot");slots[slot]=true;
    }
  }
  sequence_leaves_=0;sequence_ready_=false;
  // Configuration is independent of the positional trace and survives reconfiguration.
  if(!layers.empty()) ensure_full_context();
  std::vector<std::uint32_t> flat;flat.reserve(layers.size()*5);
  for(const auto &row:layers)flat.insert(flat.end(),row.begin(),row.end());
  const int rc=api_.seq_configure(model_,flat.data(),static_cast<std::uint32_t>(layers.size()),n_attn,n_recur,heads,interval);
  if(rc)fail("sequence configure",rc);
  sequence_leaves_=layers.empty() ? 0 : 2*heads*(n_attn+n_recur)+heads*n_attn;
  sequence_ready_=true;
  return sequence_leaves_;
}
void TraceCapture::read_sequence(TracedGeneration &capture) {
  if(!sequence_leaves_)return;
  std::vector<std::array<std::uint8_t,32>> roots(sequence_leaves_);std::uint32_t n=0;
  const int rc=api_.seq_roots(model_,reinterpret_cast<std::uint8_t*>(roots.data()),roots.size()*32,&n);
  if(rc)fail("sequence roots",rc);
  require(n==sequence_leaves_,"Incomplete sequence root coverage");
  capture.sequence_roots=std::move(roots);
}
std::vector<SequenceCapture> TraceCapture::replay_sequence(const TracedGeneration &capture,
    std::span<const SequenceSpan> spans,std::uint32_t max_chunks) {
  require(api_.abi==9 && api_.seq_replay && sequence_leaves_ && leaves_ &&
          capture.prompt_positions && capture.prompt_positions<=capture.tokens.size() &&
          capture.tokens.size()<=positions_ && capture.ubatch.size()==capture.tokens.size() &&
          capture.leaves.size()==capture.tokens.size()*leaves_ && capture.sequence_roots &&
          capture.sequence_roots->size()==sequence_leaves_ &&
          std::all_of(capture.tokens.begin(),capture.tokens.end(),[](auto t){return t>=0;}),
          "Invalid sequenced replay capture");
  require(!spans.empty() && spans.size()<=32 && max_chunks && max_chunks<=65536,
          "Invalid sequence replay budget");
  const std::size_t cv_count=spans.size()*max_chunks;
  // Reserve half the budget for the returned vectors while raw buffers are alive.
  require(cv_count<=trace_cap/64,"Sequence CV budget exceeded");
  std::vector<std::uint32_t> streams,n_cvs(spans.size());
  std::vector<std::uint64_t> lo,hi,lens(spans.size());std::size_t total=0;
  for(const auto &span:spans) {
    require(span.leaf<sequence_leaves_ && span.lo<span.hi && span.hi<=std::uint64_t(max_chunks)*1024 &&
            span.hi-span.lo<=trace_cap-total,"Invalid sequence replay span");
    total+=span.hi-span.lo;streams.push_back(span.leaf);lo.push_back(span.lo);hi.push_back(span.hi);
  }
  require(total<=trace_cap/2-cv_count*32,"Combined sequence replay budget exceeded");
  std::vector<std::uint8_t> bytes(total);
  std::vector<std::array<std::uint8_t,32>> cvs(cv_count);
  ensure_full_context();
  require(allocated_context_>=capture.tokens.size(),"Allocated context is smaller than sequence replay demand");
  const int rc=api_.seq_replay(model_,capture.tokens.data(),static_cast<int>(capture.tokens.size()),static_cast<int>(capture.prompt_positions),
      streams.data(),lo.data(),hi.data(),static_cast<std::uint32_t>(spans.size()),max_chunks,bytes.data(),bytes.size(),lens.data(),
      reinterpret_cast<std::uint8_t*>(cvs.data()),cvs.size()*32,n_cvs.data());
  if(rc)fail("sequence replay",rc);
  std::vector<SequenceCapture> result;result.reserve(spans.size());std::size_t bo=0,co=0;
  for(std::size_t i=0;i<spans.size();++i) {
    require(lens[i]==hi[i]-lo[i] && lens[i]<=bytes.size()-bo && n_cvs[i] &&
            n_cvs[i]<=max_chunks && n_cvs[i]<=cvs.size()-co && n_cvs[i]>=(hi[i]+1023)/1024,
            "Invalid sequence replay output dimensions");
    result.push_back({{bytes.data()+bo,bytes.data()+bo+lens[i]}, {cvs.data()+co,cvs.data()+co+n_cvs[i]}});
    bo+=lens[i];co+=n_cvs[i];
  }
  require(bo==bytes.size(),"Incomplete sequence replay bytes");
  return result;
}
} // namespace keryx::nexus::opoi::detail
