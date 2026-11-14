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
template <typename T> T symbol_optional(void *library, const char *name) {
  auto *value = platform::library_symbol(library, name);
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

std::string sanitize_nul_bytes(std::string_view text) {
  if (text.find('\0') == std::string_view::npos) {
    return std::string(text);
  }
  std::string clean;
  clean.reserve(text.size());
  for (char c : text) {
    if (c != '\0') clean.push_back(c);
  }
  return clean;
}

std::size_t prompt_budget(std::size_t ctx_floor, std::size_t max_tokens) noexcept {
  if (ctx_floor <= max_tokens + 512) return 0;
  return ctx_floor - max_tokens - 512;
}

std::optional<std::string> head_tail_cut(std::string_view prompt, std::size_t cap) {
  constexpr std::string_view sep = "\n…\n";
  if (prompt.size() <= cap) return std::nullopt;
  if (cap <= sep.size()) return std::string{};
  const std::size_t body = cap - sep.size();
  const std::size_t head_target = body / 2;
  const std::size_t tail_target = body - head_target;

  // Find valid UTF-8 boundaries for head
  std::size_t head_end = 0;
  for (std::size_t i = 0; i <= head_target && i < prompt.size(); ) {
    unsigned char c = static_cast<unsigned char>(prompt[i]);
    std::size_t len = 1;
    if ((c & 0x80) == 0x00) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (i + len <= head_target) {
      i += len;
      head_end = i;
    } else {
      break;
    }
  }

  // Tail: reverse find UTF-8 boundary
  std::size_t tail_start = prompt.size();
  for (std::size_t i = prompt.size(); i > 0; ) {
    std::size_t prev = i - 1;
    while (prev > 0 && (static_cast<unsigned char>(prompt[prev]) & 0xC0) == 0x80) {
      --prev;
    }
    if (prompt.size() - prev <= tail_target) {
      tail_start = prev;
      i = prev;
    } else {
      break;
    }
  }

  std::string out;
  out.reserve(head_end + sep.size() + (prompt.size() - tail_start));
  out.append(prompt.data(), head_end);
  out.append(sep);
  out.append(prompt.data() + tail_start, prompt.size() - tail_start);
  return out;
}

std::string official_qwen35_prompt(const std::string &nonce_hex) {
  if (nonce_hex.empty()) throw std::invalid_argument("OPoI nonce is empty");
  const std::string clean_nonce = sanitize_nul_bytes(nonce_hex);
  const std::string prompt = "Keryx inference challenge " + clean_nonce + ": briefly describe what you are.";
  return official_qwen35_ai_prompt(prompt);
}

std::string official_qwen35_ai_prompt(std::string_view user_prompt) {
  const std::string clean(user_prompt);
  if (clean.empty())
    throw std::invalid_argument("AiRequest prompt is empty");
  return std::string("<|im_start|>system\n") + std::string(stock_system_prompt) + "<|im_end|>\n" +
      "<|im_start|>user\n" + clean + "<|im_end|>\n" +
      "<|im_start|>assistant\n<think>\n\n</think>\n\n";
}

std::vector<int> qwen35_context_candidates(int configured_context) {
  constexpr int floor = 8192;
  constexpr int cap = 32768;
  if (configured_context >= 0) return {std::clamp(configured_context, 1, 131072)};
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
  using GenerateNFn=int(*)(void *,const char *,std::size_t,int,char *,int);
  GenerateNFn generate_n{};
  FreeFn free_model{};
  using PomReadyFn = bool (*)(void *);
  using PomChunksFn = std::uint64_t (*)(void *);
  using PomMineFn = std::int64_t (*)(void *, const std::uint64_t[4], const std::uint64_t[4], const std::uint64_t[4],
                                     std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t);
  using PomMineV5Fn = std::int64_t (*)(void *, const std::uint64_t[4], const std::uint64_t[4], const std::uint64_t[4],
                                       std::uint64_t, std::uint64_t, std::uint32_t,
                                       const std::uint64_t[4], std::uint64_t);
  using PomReleasePadFn = void (*)(void *);
  using PomV5BatchFn = std::uint32_t (*)(void *);
  PomReadyFn pom_ready{};
  PomChunksFn pom_n_chunks{};
  PomMineFn pom_mine{};
  PomMineV5Fn pom_mine_v5{};
  PomReleasePadFn pom_release_pad{};
  PomV5BatchFn pom_v5_batch{};
  void (*q8c_release)(int){};
  void (*q8s_release)(int){};
  bool scratch_active{true};
  std::string gguf;
  int abi{};
  std::uint32_t context_capacity{};
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
  const char *ai_backend = std::getenv("NEXUS_AI_BACKEND");
  const bool is_vulkan = (ai_backend && std::string_view(ai_backend) == "vulkan") ||
                         library_path.find("-vk") != std::string::npos ||
                         library_path.find("vulkan") != std::string::npos;
  if (!is_vulkan) {
    const char *visible = std::getenv("CUDA_VISIBLE_DEVICES");
    if (!visible || std::string(visible) != primary_gpu_uuid())
      throw std::runtime_error("OPoI CUDA runtime requires CUDA_VISIBLE_DEVICES set only to authorized primary NVIDIA GPU UUID");
  }
  if (sha256_file(gguf_path) != official_model_sha256)
    throw std::runtime_error("OPoI GGUF SHA-256 does not match the configured official model");
  impl_->gguf = std::move(gguf_path);
  static std::once_flag canonical_environment;
  std::call_once(canonical_environment, [] {
#ifdef _WIN32
    SetEnvironmentVariableA("GGML_CUDA_DISABLE_GRAPHS", "1");
    SetEnvironmentVariableA("LLAMA_ATTN_ROT_DISABLE", "1");
    if (_putenv_s("GGML_CUDA_DISABLE_GRAPHS","1") || _putenv_s("LLAMA_ATTN_ROT_DISABLE","1"))
#else
    if (setenv("GGML_CUDA_DISABLE_GRAPHS","1",1) || setenv("LLAMA_ATTN_ROT_DISABLE","1",1))
#endif
      throw std::runtime_error("Cannot set canonical inference environment");
  });
  impl_->library = platform::open_library(std::filesystem::u8path(library_path));
  if (!impl_->library) throw std::runtime_error(std::string("cannot load stock libkeryx-llama: ") + "native loader error");
  const auto abi = symbol<AbiFn>(impl_->library, "keryx_llama_abi");
  const auto vk_abi = symbol_optional<AbiFn>(impl_->library, "keryx_llama_vk_abi");
  const auto load = symbol<LoadFn>(impl_->library, "keryx_llama_load");
  impl_->generate = symbol<GenerateFn>(impl_->library, "keryx_llama_generate");
  impl_->generate_n=symbol_optional<Impl::GenerateNFn>(impl_->library,"keryx_llama_generate_n");
  impl_->free_model = symbol<FreeFn>(impl_->library, "keryx_llama_free");
  impl_->last_error = symbol<ErrorFn>(impl_->library, "keryx_llama_last_error");
  impl_->pom_ready = symbol_optional<Impl::PomReadyFn>(impl_->library, "keryx_llama_pom_ready");
  impl_->pom_n_chunks = symbol_optional<Impl::PomChunksFn>(impl_->library, "keryx_llama_pom_n_chunks");
  impl_->pom_mine = symbol_optional<Impl::PomMineFn>(impl_->library, "keryx_llama_pom_mine");
  impl_->pom_mine_v5 = symbol_optional<Impl::PomMineV5Fn>(impl_->library, "keryx_llama_pom_mine_v5");
  impl_->pom_v5_batch = symbol_optional<Impl::PomV5BatchFn>(impl_->library, "keryx_llama_pom_v5_batch");
  impl_->pom_release_pad = symbol_optional<Impl::PomReleasePadFn>(impl_->library, "keryx_llama_pom_release_pad");
  impl_->abi = (is_vulkan && vk_abi) ? vk_abi() : abi();
  if (impl_->abi != 3 && impl_->abi != 6 && impl_->abi != 8 && impl_->abi != 9)
    throw std::runtime_error("stock libkeryx-llama ABI mismatch (expected 3, 6, 8 or 9)");
  if (impl_->abi == 6 || impl_->abi == 8 || impl_->abi == 9) {
    auto &api = impl_->trace_api;
    api.configure = symbol<decltype(api.configure)>(impl_->library, "keryx_llama_trace_configure");
    api.leaf_count = symbol<decltype(api.leaf_count)>(impl_->library, "keryx_llama_trace_leaf_count");
    api.generate = symbol<decltype(api.generate)>(impl_->library, "keryx_llama_generate_traced");
    api.generate_n=symbol_optional<decltype(api.generate_n)>(impl_->library,"keryx_llama_generate_traced_n");
    api.read = symbol<decltype(api.read)>(impl_->library, "keryx_llama_trace_read");
    api.replay = symbol_optional<decltype(api.replay)>(impl_->library, "keryx_llama_replay");
    api.replay_multi = symbol_optional<decltype(api.replay_multi)>(impl_->library, "keryx_llama_replay_multi");
    api.error = impl_->last_error;
    api.abi = impl_->abi;
    if (impl_->abi == 9 || is_vulkan) {
      api.seq_configure = symbol_optional<decltype(api.seq_configure)>(impl_->library,"keryx_llama_seq_configure");
      api.seq_roots = symbol_optional<decltype(api.seq_roots)>(impl_->library,"keryx_llama_seq_roots");
      api.seq_replay = symbol_optional<decltype(api.seq_replay)>(impl_->library,"keryx_llama_seq_replay");
      if (api.seq_configure && api.seq_roots && api.seq_replay) {
        api.abi = 9;
        impl_->abi = 9;
      }
    }
    if (impl_->abi == 8 || impl_->abi == 9) {
      api.trace_tokens = symbol<decltype(api.trace_tokens)>(impl_->library,"keryx_llama_trace_tokens");
      api.replay_multi = symbol<decltype(api.replay_multi)>(impl_->library,"keryx_llama_replay_multi");
      api.context_acquire = symbol<decltype(api.context_acquire)>(impl_->library,"keryx_llama_context_acquire");
      impl_->q8c_release = reinterpret_cast<void (*)(int)>(platform::library_symbol(impl_->library, "ggml_cuda_q8c_release"));
      impl_->q8s_release = reinterpret_cast<void (*)(int)>(platform::library_symbol(impl_->library, "ggml_cuda_q8s_release"));
    }
  }
  // Match the official Qwen tier-0 cap/floor and KERYX_LLAMA_CTX override.
  // Retry only when the model loaded but context allocation failed.
  int configured_context = -1;
  if (const char *raw = std::getenv("KERYX_LLAMA_CTX")) {
    char *parse_end = nullptr;
    const long parsed = std::strtol(raw, &parse_end, 10);
    if (parse_end != raw && *parse_end == '\0' && parsed >= std::numeric_limits<int>::min() && parsed <= std::numeric_limits<int>::max())
      configured_context = std::clamp(static_cast<int>(parsed), 1, 131072);
  }
  std::string last_message;
  for (const int context : qwen35_context_candidates(configured_context)) {
    if ((impl_->abi == 6 || impl_->abi == 8 || impl_->abi == 9) && context > 32768)
      throw std::runtime_error("ABI6 context exceeds bounded capture adapter limit");
    impl_->model = load(impl_->gguf.c_str(), 0, context);
    if (impl_->model) {
      impl_->context_capacity=context>0?static_cast<std::uint32_t>(context):4096U;
      if (impl_->abi == 6 || impl_->abi == 8 || impl_->abi == 9)
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

std::optional<CanonicalWeightBlob> StockLlamaRuntime::canonical_weights() const {
  if(!impl_ || !impl_->model)throw std::logic_error("resident model is not loaded");
  const std::lock_guard lock(impl_->mutex);
  // Optional ABI9 extension from official miner 4167e409; older runtimes keep
  // their existing segmented allocation. The caller validates order and bytes.
  using Blob=std::size_t (*)(void*,void**);
  const auto blob=symbol_optional<Blob>(impl_->library,"keryx_llama_canonical_blob");
  if(!blob)return std::nullopt;
  void *base=nullptr;const auto bytes=blob(impl_->model,&base);
  if(!bytes || !base)return std::nullopt;
  return CanonicalWeightBlob{reinterpret_cast<std::uintptr_t>(base),bytes};
}

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
  if (impl_->context_capacity <= 256) {
    const std::string prompt = "<|im_start|>system\nYou are a Keryx AI.<|im_end|>\n"
                               "<|im_start|>user\nChallenge " + challenge.nonce_hex + ": describe yourself.<|im_end|>\n"
                               "<|im_start|>assistant\n<think>\n\n</think>\n\n";
    return infer(prompt, 32);
  }
  const auto prompt = official_qwen35_prompt(challenge.nonce_hex);
  return infer(prompt, 64);
}

std::string StockLlamaRuntime::infer(std::string_view prompt, std::uint32_t max_tokens) {
  if (!impl_ || !impl_->model || !impl_->generate) throw std::logic_error("inference runtime is not loaded");
  const std::lock_guard lock(impl_->mutex);
  std::string sanitized(prompt);
  if(!impl_->generate_n&&prompt.find('\0')!=std::string_view::npos)
    throw std::invalid_argument("NUL prompt requires a length-aware inference runtime");
  if (sanitized.empty() || max_tokens == 0 || max_tokens > 4096)
    throw std::invalid_argument("AiRequest prompt or max_tokens outside stock protocol bounds");

  // Context budget check and head_tail_cut fallback
  const std::size_t floor = impl_->context_capacity ? impl_->context_capacity : 8192;
  const std::size_t budget = prompt_budget(floor, max_tokens);
  if (sanitized.size() > budget && budget > 0) {
    if (auto cut = head_tail_cut(sanitized, budget)) {
      sanitized = std::move(*cut);
    }
  }

  impl_->scratch_active = true;
  if (impl_->trace) impl_->trace->ensure_full_context();
  std::vector<char> output(64 * 1024, '\0');
  const auto generate=[&]{return impl_->generate_n ?
      impl_->generate_n(impl_->model,sanitized.data(),sanitized.size(),static_cast<int>(max_tokens),output.data(),static_cast<int>(output.size())) :
      impl_->generate(impl_->model,sanitized.c_str(),static_cast<int>(max_tokens),output.data(),static_cast<int>(output.size()));};
  int length=generate();
  if (length <= 0) {
    // Retry once with aggressive head_tail_cut if length indicates prompt too long
    if (auto cut = head_tail_cut(sanitized, budget > 256 ? budget / 2 : 256)) {
      sanitized = std::move(*cut);
      length=generate();
    }
  }
  if (length <= 0 || static_cast<std::size_t>(length) > output.size())
    throw std::runtime_error("stock llama AiRequest inference failed or returned invalid length");
  return std::string(output.data(), static_cast<std::size_t>(length));
}

std::uint32_t StockLlamaRuntime::configure_sequence(std::span<const SequenceLayer> layers,
    std::uint32_t n_attn,std::uint32_t n_recur,std::uint32_t heads,std::uint32_t interval) {
  if(!impl_ || !impl_->trace || impl_->abi!=9)throw std::runtime_error("Sequence trace requires ABI9");
  const std::lock_guard lock(impl_->mutex);
  if (!layers.empty()) impl_->scratch_active = true;
  return impl_->trace->configure_sequence(layers,n_attn,n_recur,heads,interval);
}
std::vector<SequenceCapture> StockLlamaRuntime::replay_sequence(const TracedGeneration &capture,
    std::span<const SequenceSpan> spans,std::uint32_t max_chunks) {
  if(!impl_ || !impl_->trace || impl_->abi!=9)throw std::runtime_error("Sequence replay requires ABI9");
  const std::lock_guard lock(impl_->mutex);
  impl_->scratch_active = true;
  return impl_->trace->replay_sequence(capture,spans,max_chunks);
}
int StockLlamaRuntime::abi_version() const noexcept { return impl_ ? impl_->abi : 0; }

void StockLlamaRuntime::use_task_context() {
  if(!impl_ || !impl_->trace || (impl_->abi!=8 && impl_->abi!=9))
    throw std::runtime_error("Task context requires stock llama ABI8/9");
  const std::lock_guard lock(impl_->mutex);
  if (impl_->scratch_active) {
    if (impl_->abi == 9) {
      (void)impl_->trace->configure_sequence({}, 0, 0, 0, 0);
    }
    if (impl_->q8s_release) {
      impl_->q8s_release(0);
    }
    if (impl_->q8c_release) {
      impl_->q8c_release(0);
    }
    impl_->trace->use_task_context();
    if (impl_->q8c_release) {
      impl_->q8c_release(0);
    }
    impl_->scratch_active = false;
    return;
  }
  impl_->trace->use_task_context();
}

std::uint32_t StockLlamaRuntime::configure_trace(std::span<const std::uint8_t> manifest, std::uint32_t positions, std::uint32_t expected_leaves) {
  if (!impl_ || !impl_->trace) throw std::runtime_error("Trace capture requires stock llama ABI6");
  const std::lock_guard lock(impl_->mutex);
  impl_->scratch_active = true;
  return impl_->trace->configure(manifest, positions, expected_leaves);
}
TracedGeneration StockLlamaRuntime::infer_traced(std::string_view prompt, const TracePrefix *prefix,
    std::uint32_t max_tokens, std::uint32_t min_prompt, std::uint32_t answer_bytes_per_position) {
  if (!impl_ || !impl_->trace) throw std::runtime_error("Traced inference requires stock llama ABI6");
  const std::lock_guard lock(impl_->mutex);
  impl_->scratch_active = true;
  return impl_->trace->generate(prompt, prefix, max_tokens, min_prompt, answer_bytes_per_position);
}
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> StockLlamaRuntime::replay_trace(
    const TracedGeneration &capture, std::uint32_t position, std::uint32_t leaf_x, std::uint32_t leaf_y) {
  if (!impl_ || !impl_->trace) throw std::runtime_error("Trace replay requires stock llama ABI6");
  const std::lock_guard lock(impl_->mutex);
  impl_->scratch_active = true;
  return impl_->trace->replay(capture, position, leaf_x, leaf_y);
}

TracedGeneration StockLlamaRuntime::trace_tokens(std::span<const std::int32_t> tokens) {
  if(!impl_ || !impl_->trace || (impl_->abi!=8 && impl_->abi!=9))throw std::runtime_error("Token trace requires stock llama ABI8/9");
  const std::lock_guard lock(impl_->mutex);
  impl_->scratch_active = true;
  return impl_->trace->trace_tokens(tokens);
}
std::vector<std::vector<std::uint8_t>> StockLlamaRuntime::replay_trace_multi(const TracedGeneration &capture,
    std::span<const std::pair<std::uint32_t,std::uint32_t>> caps,std::uint32_t slot) {
  if(!impl_ || !impl_->trace || (impl_->abi!=8 && impl_->abi!=9))throw std::runtime_error("Multi replay requires stock llama ABI8/9");
  const std::lock_guard lock(impl_->mutex);
  impl_->scratch_active = true;
  return impl_->trace->replay_multi(capture,caps,slot);
}
} // namespace keryx::nexus::opoi

namespace keryx::nexus::opoi {
std::uint32_t StockLlamaRuntime::trace_context_capacity() const noexcept{return impl_?impl_->context_capacity:0;}

bool StockLlamaRuntime::pom_ready() const {
  if (!impl_ || !impl_->model || !impl_->pom_ready) return false;
  const std::lock_guard lock(impl_->mutex);
  return impl_->pom_ready(impl_->model);
}

std::uint64_t StockLlamaRuntime::pom_n_chunks() const {
  if (!impl_ || !impl_->model || !impl_->pom_n_chunks) return 0;
  const std::lock_guard lock(impl_->mutex);
  return impl_->pom_n_chunks(impl_->model);
}

std::int64_t StockLlamaRuntime::pom_mine(const std::uint64_t p[4], const std::uint64_t s[4], const std::uint64_t t[4],
                                         std::uint64_t timestamp, std::uint64_t start_nonce, std::uint32_t batch,
                                         std::uint32_t k, std::uint32_t h10) {
  if (!impl_ || !impl_->model || !impl_->pom_mine) return -2;
  const std::lock_guard lock(impl_->mutex);
  return impl_->pom_mine(impl_->model, p, s, t, timestamp, start_nonce, batch, k, h10);
}

bool StockLlamaRuntime::pom_v5_ready() const {
  if (!impl_ || !impl_->model) return false;
  return impl_->pom_mine_v5 != nullptr;
}

std::uint32_t StockLlamaRuntime::pom_v5_batch() const {
  if (!impl_ || !impl_->model || !impl_->pom_v5_batch) return 0;
  const std::lock_guard lock(impl_->mutex);
  return impl_->pom_v5_batch(impl_->model);
}

std::int64_t StockLlamaRuntime::pom_mine_v5(const std::uint64_t p[4], const std::uint64_t s[4], const std::uint64_t t[4],
                                            std::uint64_t timestamp, std::uint64_t start_nonce, std::uint32_t batch,
                                            const std::uint64_t pad_seed[4], std::uint64_t pad_tiles) {
  if (!impl_ || !impl_->model || !impl_->pom_mine_v5) return -2;
  const std::lock_guard lock(impl_->mutex);
  return impl_->pom_mine_v5(impl_->model, p, s, t, timestamp, start_nonce, batch, pad_seed, pad_tiles);
}

void StockLlamaRuntime::pom_release_pad() {
  if (!impl_ || !impl_->model || !impl_->pom_release_pad) return;
  const std::lock_guard lock(impl_->mutex);
  impl_->pom_release_pad(impl_->model);
}

std::pair<std::string,std::string> official_qwen35_trace_prompt(std::string_view user){
  const std::string clean(user);
  if(clean.empty()) throw std::invalid_argument("Invalid trace prompt");
  return {"<|im_start|>system\nRequest id:","\n"+std::string(stock_system_prompt)+"<|im_end|>\n<|im_start|>user\n"+clean+"<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"};
}
}

namespace keryx::nexus::opoi {
bool StockLlamaRuntime::supports_length_aware_tracing() const noexcept {
  return impl_&&impl_->trace_api.generate_n;
}
}
