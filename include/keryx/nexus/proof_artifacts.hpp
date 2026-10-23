#pragma once
#include <filesystem>
#include <span>
#include <stdexcept>
namespace keryx::nexus {
// The Rust CLI needs these scratch files only through verification. Keep every
// target winner and every unverified/failing candidate for forensic recovery.
inline void release_verified_nonwinner_artifacts(
    std::span<const std::filesystem::path> paths, bool rust_verified, bool target_met) {
  if (!rust_verified) throw std::logic_error("unverified proof artifacts must be preserved");
  if (target_met) return;
  for (const auto &path : paths) std::filesystem::remove(path);
}
}
