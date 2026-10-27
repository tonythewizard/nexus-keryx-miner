#pragma once
#include "keryx/nexus/ai_response_service.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/opoi.hpp"
#include <memory>
namespace keryx::nexus::ai {
// Only the native ABI9 route supports H15. Calls that can replay require the
// worker's exclusive inference ownership; RPC polling itself never owns a GPU.
class H15AuditClient {
public:
  H15AuditClient(opoi::StockLlamaRuntime &, const model::ReadOnlyGguf &,
                 const model::SparseMerkleIndex &, std::filesystem::path,
                 const std::array<std::uint8_t, 32> &, std::string endpoint,
                 const std::array<std::uint8_t, 32> &model_id,
                 bool offline_diagnostics = false);
  ~H15AuditClient();
  AiTracedAnswer infer(std::string_view, std::uint32_t,
                       const std::array<std::uint8_t, 32> &);
  void commit(std::string_view, std::span<const std::uint8_t>, std::uint64_t,
              std::uint64_t);
  std::optional<std::string>
  response_for_request(const std::array<std::uint8_t, 32> &) const;
  void response_intent(std::string_view);
  void response_result(std::string_view, bool rejected);
  void poll(); // selected-chain restore; bounded, read-only RPC
  std::uint64_t estimate_next_work(std::uint32_t prompt_bytes,
                                   std::uint32_t tokens) const;
  void advance(std::uint64_t,
               std::optional<std::uint64_t> next_request_work = {});
  void submit_due(std::uint64_t, bool authorized);
  std::array<std::uint32_t, 2> verify_capture(std::uint32_t seeds);
  std::array<std::uint32_t, 4> verify_lifecycle();
  bool ready() const noexcept;
  bool restoring() const;
  void set_gpu_owner(std::function<void(const std::function<void()> &)>);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace keryx::nexus::ai
