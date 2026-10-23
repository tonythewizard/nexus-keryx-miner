#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "keryx/nexus/trace_capture.hpp"

namespace keryx::nexus::opoi {

std::string coinbase_extra_data(std::string_view identity, std::uint64_t nonce);
std::string fresh_coinbase_extra_data(std::string_view identity);
bool valid_coinbase_extra_data(std::string_view value, std::string_view identity);

struct Challenge {
  std::array<unsigned char, 32> model_id{};
  std::string model_id_hex;
  std::string nonce_hex;
};

Challenge parse_challenge(const std::string &wire);
std::string official_qwen35_prompt(const std::string &nonce_hex);
// Exact v0.5.7 Qwen family ChatML wrapper for AiRequest prompts.
std::string official_qwen35_ai_prompt(std::string_view user_prompt);
std::string format_inference_result(const Challenge &challenge, const std::string &text);
std::vector<int> qwen35_context_candidates(int configured_context = -1);
bool challenge_enabled_for_network(const std::string &network, std::uint64_t daa_score) noexcept;

// Adapter to the stock miner's libkeryx-llama ABI 3 or 6. It refuses to load unless
// CUDA_VISIBLE_DEVICES is exactly the authorized RTX UUID and the GGUF SHA-256
// matches the installed tier-0 model. Loading/generation is intentionally lazy
// at the caller: mining must be paused and its CUDA buffers released first.
struct ResidentTensor {
  std::string name;
  std::uintptr_t address{};
  std::size_t bytes{};
  bool device{};
};

class StockLlamaRuntime final {
public:
  StockLlamaRuntime(std::string library_path, std::string gguf_path);
  ~StockLlamaRuntime();
  StockLlamaRuntime(StockLlamaRuntime &&) noexcept;
  StockLlamaRuntime &operator=(StockLlamaRuntime &&) noexcept;
  StockLlamaRuntime(const StockLlamaRuntime &) = delete;
  StockLlamaRuntime &operator=(const StockLlamaRuntime &) = delete;

  // Borrowed immutable tensor views; valid only for this runtime lifetime.
  std::vector<ResidentTensor> resident_tensors() const;
  std::string answer(const Challenge &challenge);
  // Stock llama ABI inference entrypoint for an assigned AiRequest. Prompt content is never
  // logged or persisted by this adapter. The caller must own CUDA and enforce request deadlines.
  std::string infer(std::string_view prompt, std::uint32_t max_tokens);
  TracedGeneration trace_tokens(std::span<const std::int32_t> tokens);
  std::vector<std::vector<std::uint8_t>> replay_trace_multi(const TracedGeneration &capture,
      std::span<const std::pair<std::uint32_t,std::uint32_t>> captures,std::uint32_t slot_bytes);
  int abi_version() const noexcept;
  // ABI8 only. Caller must release/synchronize any PoM pad before switching
  // context; acquisition never resets the shared CUDA primary context.
  void use_task_context();
  // ABI6 only. The caller must own exclusive GPU admission; no transaction is
  // signed or submitted by these methods. ABI3 always fails closed here.
  std::uint32_t configure_trace(std::span<const std::uint8_t> manifest, std::uint32_t positions, std::uint32_t expected_leaves = 0);
  TracedGeneration infer_traced(std::string_view prompt, const TracePrefix *prefix,
      std::uint32_t max_tokens, std::uint32_t min_prompt = 0, std::uint32_t answer_bytes_per_position = 0);
  std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> replay_trace(
      const TracedGeneration &, std::uint32_t position, std::uint32_t leaf_x, std::uint32_t leaf_y);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace keryx::nexus::opoi
