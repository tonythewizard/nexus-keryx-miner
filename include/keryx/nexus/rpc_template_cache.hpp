#pragma once

#include "messages.pb.h"
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace keryx::nexus {

// Single-owner, single-entry cache. Complete RPC bytes determine reuse, so an
// envelope update within the same canonical job cannot reuse stale data.
class RpcTemplateCache {
public:
  const protowire::RpcBlock &block(const std::vector<std::uint8_t> &wire) {
    if (wire_.empty() || wire_ != wire) {
      wire_.clear();
      if (wire.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
          !envelope_.ParseFromArray(wire.data(), static_cast<int>(wire.size())) ||
          !envelope_.has_getblocktemplateresponse() ||
          !envelope_.getblocktemplateresponse().has_block())
        throw std::runtime_error("queued live job protobuf snapshot is malformed");
      wire_ = wire;
      ++revision_;
    }
    return envelope_.getblocktemplateresponse().block();
  }
  std::uint64_t revision() const noexcept { return revision_; }
  const protowire::RpcBlock &cached_block() const {
    if (wire_.empty())
      throw std::logic_error("no cached RPC template block");
    return envelope_.getblocktemplateresponse().block();
  }

private:
  protowire::KaspadResponse envelope_;
  std::vector<std::uint8_t> wire_;
  std::uint64_t revision_{};
};

} // namespace keryx::nexus
