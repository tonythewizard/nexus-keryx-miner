#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace keryx::nexus::opoi {
struct TracePrefix {
  std::span<const std::int32_t> tokens;
  std::uint32_t head_tokens{};
  std::string_view rest;
};
struct TracedGeneration {
  std::string text;
  std::vector<std::int32_t> tokens;
  std::uint32_t prompt_positions{};
  std::vector<std::uint32_t> ubatch;
  std::vector<std::array<std::uint8_t, 32>> leaves;
  std::optional<std::vector<std::array<std::uint8_t,32>>> sequence_roots;
};

using SequenceLayer = std::array<std::uint32_t, 5>; // layer, kind, slot, heads, head_dim
struct SequenceSpan { std::uint32_t leaf; std::uint64_t lo, hi; };
struct SequenceCapture {
  std::vector<std::uint8_t> bytes;
  std::vector<std::array<std::uint8_t,32>> chunk_cvs;
};

namespace detail {
// Exact official ABI6/8/9 function types. Non-owning; the runtime owns the model
// and serializes all calls, including regular generation, under its mutex.
struct TraceApi {
  int (*configure)(void *, const void *, std::size_t, int){};
  int (*leaf_count)(void *){};
  int (*generate)(void *, const char *, const std::int32_t *, int, int, const char *, int,
                  char *, int, std::int32_t *, int, int *, int *, int, int){};
  int (*read)(void *, std::uint8_t *, std::size_t, std::uint32_t *, std::size_t, std::uint32_t *){};
  int (*replay)(void *, const std::int32_t *, int, int, std::uint32_t, std::uint32_t, std::uint32_t,
                std::uint8_t *, std::size_t, std::uint8_t *, std::size_t, std::uint32_t *, std::uint32_t *){};
  const char *(*error)(){};
  int abi{6};
  int (*trace_tokens)(void *,const std::int32_t *,int){};
  int (*replay_multi)(void *,const std::int32_t *,int,int,const std::uint32_t *,const std::uint32_t *,
                      std::uint32_t,std::uint32_t,std::uint8_t *,std::size_t,std::uint32_t *){};
  int (*context_acquire)(void *, int){};
  int (*seq_configure)(void *,const std::uint32_t *,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t){};
  int (*seq_roots)(void *,std::uint8_t *,std::size_t,std::uint32_t *){};
  int (*seq_replay)(void *,const std::int32_t *,int,int,const std::uint32_t *,const std::uint64_t *,const std::uint64_t *,
                    std::uint32_t,std::uint32_t,std::uint8_t *,std::size_t,std::uint64_t *,std::uint8_t *,std::size_t,std::uint32_t *){};
};
class TraceCapture final {
public:
  TraceCapture(TraceApi api, void *model, std::uint32_t context, std::uint32_t context_floor = 0);
  std::uint32_t configure(std::span<const std::uint8_t> manifest, std::uint32_t positions, std::uint32_t expected_leaves = 0);
  TracedGeneration generate(std::string_view prompt, const TracePrefix *prefix,
                            std::uint32_t max_tokens, std::uint32_t min_prompt,
                            std::uint32_t answer_bytes_per_position);
  std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> replay(
      const TracedGeneration &capture, std::uint32_t position, std::uint32_t leaf_x, std::uint32_t leaf_y);
  TracedGeneration trace_tokens(std::span<const std::int32_t> tokens);
  std::vector<std::vector<std::uint8_t>> replay_multi(const TracedGeneration &capture,
      std::span<const std::pair<std::uint32_t,std::uint32_t>> captures,std::uint32_t slot_bytes);
  std::uint32_t configure_sequence(std::span<const SequenceLayer> layers,std::uint32_t n_attn,
      std::uint32_t n_recur,std::uint32_t heads,std::uint32_t interval);
  std::vector<SequenceCapture> replay_sequence(const TracedGeneration &,std::span<const SequenceSpan>,std::uint32_t max_chunks);
  void use_task_context();
  void ensure_full_context();
private:
  TraceApi api_;
  void *model_;
  std::uint32_t context_, positions_{}, leaves_{};
  std::uint32_t allocated_context_{};
  std::uint32_t context_floor_{};
  bool context_full_{true};
  std::uint32_t sequence_leaves_{};
  bool sequence_ready_{true};
  void read_sequence(TracedGeneration &);
  void ensure_context_for(std::size_t positions);
  [[noreturn]] void fail(const char *operation, int code) const;
};
} // namespace detail
} // namespace keryx::nexus::opoi
