#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "rpc.pb.h"
namespace keryx::nexus::consensus {
Header header_from_rpc(const protowire::RpcBlockHeader &rpc);
Hash block_hash_for_daa(const Header &header, std::uint64_t nonce,
                        std::uint64_t timestamp);
} // namespace keryx::nexus::consensus
