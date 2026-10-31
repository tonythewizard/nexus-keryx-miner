#pragma once
#include "keryx/nexus/rpc_template_cache.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include <optional>

namespace keryx::nexus {
// Mining-thread-owned, one snapshot only. Complete wire changes invalidate
// derived header work, including same-generation transaction/envelope updates.
class PreparedRpcTemplateCache {
public:
  struct Work {
    consensus::Header header;
    consensus::Hash pre_pow;
    consensus::CompactTarget target;
  };
  const Work &get(const std::vector<std::uint8_t> &wire) {
    const auto &block=rpc_.block(wire);
    if (!work_ || revision_!=rpc_.revision()) {
      auto header=consensus::header_from_rpc(block.header());
      auto pre_pow=consensus::pre_pow_hash(header);
      auto target=consensus::decode_compact_target(header.bits);
      if(target.zero)throw std::runtime_error("live job has invalid compact target");
      work_=Work{std::move(header),pre_pow,target};
      revision_=rpc_.revision();
    }
    return *work_;
  }
  const protowire::RpcBlock &block(const std::vector<std::uint8_t> &wire) {
    return rpc_.block(wire);
  }
  const protowire::RpcBlock &cached_block() const {
    return rpc_.cached_block();
  }
  std::uint64_t revision() const noexcept { return revision_; }
private:
  RpcTemplateCache rpc_;
  std::optional<Work> work_;
  std::uint64_t revision_{};
};
} // namespace keryx::nexus
