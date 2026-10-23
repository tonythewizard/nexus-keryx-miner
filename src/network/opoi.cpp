#include "keryx/nexus/developer_config.hpp"
#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/bounded_file_sha256.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <charconv>
#include "keryx/nexus/platform/shared_library.hpp"

#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace keryx::nexus::opoi {
namespace {
constexpr char authorized_uuid[] = "" NEXUS_PRIMARY_GPU_UUID "";
constexpr char official_model_sha256[] = "946072b16f5d672e60357410f900888f6522b3ae49f241b5bf2142cb89637fb6";
constexpr std::string_view stock_system_prompt =
    "You are a Keryx Network AI, a decentralized assistant served by the GPU miners of the Keryx network. "
    "Answer in the language of the user's message. "
    "Do not introduce yourself or describe Keryx unless the user asks about it. If asked: Keryx is a proof-of-work BlockDAG "
    "derived from Kaspa (about 10 blocks per second) where each mining GPU proves on every block that it holds a model in VRAM "
    "(Proof-of-Model), so mining and inference are the same job; requests are on-chain transactions paid in KRX. "
    "Web search results, facts or earlier messages included in the request are your sources for recent or specific information: "
    "use them. Without them, answer from your training knowledge and say when it may be out of date; "
    "never claim to have searched the web or cite sources that are not included in the request. "
    "Never mention your underlying model name or the company that trained it: if asked, you are a Keryx Network AI. "
    "Be thorough but concise.";
constexpr std::array<unsigned char, 32> qwen35_model_id{
    0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,
    0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
using AbiFn = int (*)();
using ErrorFn = const char *(*)();
using LoadFn = void *(*)(const char *, int, int);
using GenerateFn = int (*)(void *, const char *, int, char *, int);
using FreeFn = void (*)(void *);

std::string sha256_file(const std::string &path) {
  return bounded_file_sha256(path);
}
template <typename T> T symbol(void *library, const char *name) {
  auto *value = platform::library_symbol(library, name);
  if (!value) throw std::runtime_error(std::string("stock llama missing ") + name);
  return reinterpret_cast<T>(value);
}
std::string native_error(ErrorFn fn) {
  if (!fn) return "native model load failed";
  const char *message = fn();
  return message && *message ? std::string(message) : "native model load failed";
}
}

extern "C" int nexus_opoi_coinbase_tag(std::uint64_t, unsigned char *);
std::string coinbase_extra_data(std::string_view identity, std::uint64_t nonce) {
  constexpr char digits[]="0123456789abcdef";
  std::string n(16,'0');
  for(unsigned i=0;i<16;++i)n[15-i]=digits[(nonce>>(4*i))&15];
  std::array<unsigned char,16> tag{};
  if(nexus_opoi_coinbase_tag(nonce,tag.data())!=1)
    throw std::runtime_error("official coinbase OPoI tag computation failed");
  return std::string(identity)+"/"+n+"/ai:v1:"+std::string(tag.begin(),tag.end());
}
std::string fresh_coinbase_extra_data(std::string_view identity) {
  std::uint64_t nonce=0;
  if(RAND_bytes(reinterpret_cast<unsigned char*>(&nonce),sizeof(nonce))!=1)
    throw std::runtime_error("coinbase nonce entropy unavailable");
  return coinbase_extra_data(identity,nonce);
}
bool valid_coinbase_extra_data(std::string_view value,std::string_view identity) {
  if(value.size()!=identity.size()+40 || !value.starts_with(identity) || value[identity.size()]!='/')return false;
  const auto n=value.substr(identity.size()+1,16);
  std::uint64_t nonce=0;const auto r=std::from_chars(n.data(),n.data()+n.size(),nonce,16);
  return r.ec==std::errc{} && r.ptr==n.data()+n.size() && value==coinbase_extra_data(identity,nonce);
}

Challenge parse_challenge(const std::string &wire) {
  const auto sep = wire.find(':');
  if (sep == std::string::npos || sep == 0 || sep + 1 == wire.size() ||
      wire.find(':', sep + 1) != std::string::npos)
    throw std::invalid_argument("OPoI challenge must be model_id_hex:nonce_hex");
  Challenge out;
  out.model_id_hex = wire.substr(0, sep);
  out.nonce_hex = wire.substr(sep + 1);
  if (out.model_id_hex.size() != 64 || out.nonce_hex.empty() || (out.nonce_hex.size() & 1U) != 0)
    throw std::invalid_argument("OPoI model id/nonce hex length invalid");
  auto nibble = [](char c) -> unsigned char {
    if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
    throw std::invalid_argument("OPoI challenge contains non-hex characters");
  };
  for (std::size_t i = 0; i < 32; ++i)
    out.model_id[i] = static_cast<unsigned char>((nibble(out.model_id_hex[2*i]) << 4) |
                                                  nibble(out.model_id_hex[2*i+1]));
  for (char c : out.nonce_hex) (void)nibble(c);
  if (out.model_id != qwen35_model_id)
    throw std::invalid_argument("OPoI challenge requests a model other than the configured Qwen3.5 tier-0 model");
  return out;
}

std::string official_qwen35_prompt(const std::string &nonce_hex) {
  if (nonce_hex.empty()) throw std::invalid_argument("OPoI nonce is empty");
  const std::string prompt = "Keryx inference challenge " + nonce_hex + ": briefly describe what you are.";
  return official_qwen35_ai_prompt(prompt);
}

std::string official_qwen35_ai_prompt(std::string_view user_prompt) {
  if (user_prompt.empty() || user_prompt.find('\0') != std::string_view::npos)
    throw std::invalid_argument("AiRequest prompt is empty or contains NUL");
  return std::string("<|im_start|>system\n") + std::string(stock_system_prompt) + "<|im_end|>\n" +
      "<|im_start|>user\n" + std::string(user_prompt) + "<|im_end|>\n" +
      "<|im_start|>assistant\n<think>\n\n</think>\n\n";
}

std::vector<int> qwen35_context_candidates(int configured_context) {
  constexpr int floor = 8192;
  constexpr int cap = 32768;
  if (configured_context >= 0) return {configured_context};
  std::vector<int> candidates;
  for (int context = cap; context > floor; context /= 2) candidates.push_back(context);
  candidates.push_back(floor);
  return candidates;
}

bool challenge_enabled_for_network(const std::string &network, std::uint64_t daa_score) noexcept {
  // Mirrors v0.5.7 pom_activation_daa(): mainnet activates PoM at 37,780,000;
  // testnet activates at DAA 1. After activation, block PoM is the capability gate.
  if (network == "mainnet") return daa_score < 37'780'000;
  if (network == "testnet") return daa_score < 1;
  return false;
}

namespace {
bool protocol_text_valid(std::string_view text) noexcept {
  for (std::size_t i = 0; i < text.size();) {
    const auto first = static_cast<unsigned char>(text[i]);
    if (first < 0x80U) {
      if (first == 0 || (first < 0x20U && first != '\n' && first != '\r' && first != '\t')) return false;
      ++i;
      continue;
    }
    std::size_t count = 0;
    unsigned char second_min = 0x80U, second_max = 0xbfU;
    if (first >= 0xc2U && first <= 0xdfU) count = 1;
    else if (first == 0xe0U) { count = 2; second_min = 0xa0U; }
    else if (first >= 0xe1U && first <= 0xecU) count = 2;
    else if (first == 0xedU) { count = 2; second_max = 0x9fU; }
    else if (first >= 0xeeU && first <= 0xefU) count = 2;
    else if (first == 0xf0U) { count = 3; second_min = 0x90U; }
    else if (first >= 0xf1U && first <= 0xf3U) count = 3;
    else if (first == 0xf4U) { count = 3; second_max = 0x8fU; }
    else return false;
    if (i + count >= text.size()) return false;
    const auto second = static_cast<unsigned char>(text[i + 1]);
    if (second < second_min || second > second_max) return false;
    for (std::size_t j = 2; j <= count; ++j) {
      const auto continuation = static_cast<unsigned char>(text[i + j]);
      if (continuation < 0x80U || continuation > 0xbfU) return false;
    }
    i += count + 1;
  }
  return true;
}
} // namespace

std::string format_inference_result(const Challenge &challenge, const std::string &text) {
  if (text.empty() || !protocol_text_valid(text))
    throw std::invalid_argument("OPoI runtime returned empty or invalid UTF-8/control-containing text");
  // Stock v0.5.7 GetBlockTemplateRequest wire format is
  // model_id_hex:nonce_hex:result_text (see the local official grpc.rs).
  return challenge.model_id_hex + ":" + challenge.nonce_hex + ":" + text;
}

struct StockLlamaRuntime::Impl {
  void *library{};
  void *model{};
  ErrorFn last_error{};
  GenerateFn generate{};
  FreeFn free_model{};
  std::string gguf;
  int abi{};
  detail::TraceApi trace_api{};
  std::unique_ptr<detail::TraceCapture> trace;
  mutable std::mutex mutex;
  ~Impl() {
    if (model && free_model) free_model(model);
    if (library) platform::close_library(library);
  }
};

StockLlamaRuntime::StockLlamaRuntime(std::string library_path, std::string gguf_path)
    : impl_(std::make_unique<Impl>()) {
  const char *visible = std::getenv("CUDA_VISIBLE_DEVICES");
  if (!visible || std::string(visible) != authorized_uuid)
    throw std::runtime_error("OPoI CUDA runtime requires CUDA_VISIBLE_DEVICES set only to authorized primary NVIDIA GPU UUID");
  if (sha256_file(gguf_path) != official_model_sha256)
    throw std::runtime_error("OPoI GGUF SHA-256 does not match the configured official model");
  impl_->gguf = std::move(gguf_path);
  impl_->library = platform::open_library(std::filesystem::u8path(library_path));
  if (!impl_->library) throw std::runtime_error(std::string("cannot load stock libkeryx-llama: ") + "native loader error");
  const auto abi = symbol<AbiFn>(impl_->library, "keryx_llama_abi");
  const auto load = symbol<LoadFn>(impl_->library, "keryx_llama_load");
  impl_->generate = symbol<GenerateFn>(impl_->library, "keryx_llama_generate");
  impl_->free_model = symbol<FreeFn>(impl_->library, "keryx_llama_free");
  impl_->last_error = symbol<ErrorFn>(impl_->library, "keryx_llama_last_error");
  impl_->abi = abi();
  if (impl_->abi != 3 && impl_->abi != 6 && impl_->abi != 8)
    throw std::runtime_error("stock libkeryx-llama ABI mismatch (expected 3, 6 or 8)");
  if (impl_->abi == 6 || impl_->abi == 8) {
    auto &api = impl_->trace_api;
    api.configure = symbol<decltype(api.configure)>(impl_->library, "keryx_llama_trace_configure");
    api.leaf_count = symbol<decltype(api.leaf_count)>(impl_->library, "keryx_llama_trace_leaf_count");
    api.generate = symbol<decltype(api.generate)>(impl_->library, "keryx_llama_generate_traced");
    api.read = symbol<decltype(api.read)>(impl_->library, "keryx_llama_trace_read");
    api.replay = symbol<decltype(api.replay)>(impl_->library, "keryx_llama_replay");
    api.error = impl_->last_error;
    api.abi = impl_->abi;
    if (impl_->abi == 8) {
      api.trace_tokens = symbol<decltype(api.trace_tokens)>(impl_->library,"keryx_llama_trace_tokens");
      api.replay_multi = symbol<decltype(api.replay_multi)>(impl_->library,"keryx_llama_replay_multi");
      api.context_acquire = symbol<decltype(api.context_acquire)>(impl_->library,"keryx_llama_context_acquire");
    }
  }
  // Match the official Qwen tier-0 cap/floor and KERYX_LLAMA_CTX override.
  // Retry only when the model loaded but context allocation failed.
  int configured_context = -1;
  if (const char *raw = std::getenv("KERYX_LLAMA_CTX")) {
    char *parse_end = nullptr;
    const long parsed = std::strtol(raw, &parse_end, 10);
    if (parse_end != raw && *parse_end == '\0' && parsed >= 0 && parsed <= std::numeric_limits<int>::max())
      configured_context = static_cast<int>(parsed);
  }
  std::string last_message;
  for (const int context : qwen35_context_candidates(configured_context)) {
    if ((impl_->abi == 6 || impl_->abi == 8) && context > 32768)
      throw std::runtime_error("ABI6 context exceeds bounded capture adapter limit");
    impl_->model = load(impl_->gguf.c_str(), 0, context);
    if (impl_->model) {
      if (impl_->abi == 6 || impl_->abi == 8)
        impl_->trace = std::make_unique<detail::TraceCapture>(impl_->trace_api, impl_->model,
            context > 0 ? static_cast<std::uint32_t>(context) : 4096U,
            context > 8192 ? 8192U : 0U);
      break;
    }
    last_message = native_error(impl_->last_error);
    if (last_message.find("context: llama_init_from_model failed") == std::string::npos)
      throw std::runtime_error("stock llama model load failed: " + last_message);
  }
  if (!impl_->model) throw std::runtime_error("stock llama context ladder exhausted: " + last_message);
}

StockLlamaRuntime::~StockLlamaRuntime() = default;
StockLlamaRuntime::StockLlamaRuntime(StockLlamaRuntime &&) noexcept = default;
StockLlamaRuntime &StockLlamaRuntime::operator=(StockLlamaRuntime &&) noexcept = default;

std::vector<ResidentTensor> StockLlamaRuntime::resident_tensors() const {
  if (!impl_ || !impl_->model) throw std::logic_error("resident model is not loaded");
  const std::lock_guard lock(impl_->mutex);
  using Count = std::size_t (*)(void *);
  using Info = bool (*)(void *, std::size_t, const char **, void **, std::size_t *, int *);
  const auto count = symbol<Count>(impl_->library, "keryx_llama_tensor_count")(impl_->model);
  const auto info = symbol<Info>(impl_->library, "keryx_llama_tensor_info");
  if (!count || count > 65535) throw std::runtime_error("invalid resident tensor count");
  std::vector<ResidentTensor> result; result.reserve(count);
  for (std::size_t i=0; i<count; ++i) {
    const char *name=nullptr; void *ptr=nullptr; std::size_t bytes=0; int device=0;
    if (!info(impl_->model,i,&name,&ptr,&bytes,&device) || !name || !ptr || !bytes)
      throw std::runtime_error("stock ABI returned invalid resident tensor metadata");
    result.push_back({name,reinterpret_cast<std::uintptr_t>(ptr),bytes,device!=0});
  }
  return result;
}

std::string StockLlamaRuntime::answer(const Challenge &challenge) {
  if (!impl_ || !impl_->model || !impl_->generate) throw std::logic_error("OPoI runtime is not loaded");
  if (challenge.model_id != qwen35_model_id) throw std::invalid_argument("OPoI challenge/model mismatch");
  const auto prompt = official_qwen35_prompt(challenge.nonce_hex);
  return infer(prompt, 64);
}

std::string StockLlamaRuntime::infer(std::string_view prompt, std::uint32_t max_tokens) {
  if (!impl_ || !impl_->model || !impl_->generate) throw std::logic_error("inference runtime is not loaded");
  const std::lock_guard lock(impl_->mutex);
  if (prompt.empty() || prompt.size() > 60U * 1024U || max_tokens == 0 || max_tokens > 4096)
    throw std::invalid_argument("AiRequest prompt or max_tokens outside stock protocol bounds");
  if (prompt.find('\0') != std::string_view::npos)
    throw std::invalid_argument("AiRequest prompt contains an embedded NUL byte");
  const std::string owned_prompt(prompt);
  if (impl_->trace) impl_->trace->ensure_full_context();
  std::vector<char> output(64 * 1024, '\0');
  const int length = impl_->generate(impl_->model, owned_prompt.c_str(), static_cast<int>(max_tokens), output.data(),
                                     static_cast<int>(output.size()));
  if (length <= 0 || static_cast<std::size_t>(length) > output.size())
    throw std::runtime_error("stock llama AiRequest inference failed or returned invalid length");
  return std::string(output.data(), static_cast<std::size_t>(length));
}

int StockLlamaRuntime::abi_version() const noexcept { return impl_ ? impl_->abi : 0; }

void StockLlamaRuntime::use_task_context() {
  if(!impl_ || !impl_->trace || impl_->abi!=8)
    throw std::runtime_error("Task context requires stock llama ABI8");
  const std::lock_guard lock(impl_->mutex);
  impl_->trace->use_task_context();
}

std::uint32_t StockLlamaRuntime::configure_trace(std::span<const std::uint8_t> manifest, std::uint32_t positions, std::uint32_t expected_leaves) {
  if (!impl_ || !impl_->trace) throw std::runtime_error("Trace capture requires stock llama ABI6");
  const std::lock_guard lock(impl_->mutex);
  return impl_->trace->configure(manifest, positions, expected_leaves);
}
TracedGeneration StockLlamaRuntime::infer_traced(std::string_view prompt, const TracePrefix *prefix,
    std::uint32_t max_tokens, std::uint32_t min_prompt, std::uint32_t answer_bytes_per_position) {
  if (!impl_ || !impl_->trace) throw std::runtime_error("Traced inference requires stock llama ABI6");
  const std::lock_guard lock(impl_->mutex);
  return impl_->trace->generate(prompt, prefix, max_tokens, min_prompt, answer_bytes_per_position);
}
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> StockLlamaRuntime::replay_trace(
    const TracedGeneration &capture, std::uint32_t position, std::uint32_t leaf_x, std::uint32_t leaf_y) {
  if (!impl_ || !impl_->trace) throw std::runtime_error("Trace replay requires stock llama ABI6");
  const std::lock_guard lock(impl_->mutex);
  return impl_->trace->replay(capture, position, leaf_x, leaf_y);
}

TracedGeneration StockLlamaRuntime::trace_tokens(std::span<const std::int32_t> tokens) {
  if(!impl_ || !impl_->trace || impl_->abi!=8)throw std::runtime_error("Token trace requires stock llama ABI8");
  const std::lock_guard lock(impl_->mutex);return impl_->trace->trace_tokens(tokens);
}
std::vector<std::vector<std::uint8_t>> StockLlamaRuntime::replay_trace_multi(const TracedGeneration &capture,
    std::span<const std::pair<std::uint32_t,std::uint32_t>> caps,std::uint32_t slot) {
  if(!impl_ || !impl_->trace || impl_->abi!=8)throw std::runtime_error("Multi replay requires stock llama ABI8");
  const std::lock_guard lock(impl_->mutex);return impl_->trace->replay_multi(capture,caps,slot);
}
} // namespace keryx::nexus::opoi
