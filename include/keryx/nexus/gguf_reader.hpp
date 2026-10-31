#pragma once
#include "keryx/nexus/platform/read_only_file.hpp"
#include <memory>
#include "keryx/nexus/consensus/pom_v4.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace keryx::nexus::model {
struct SidecarStatus {
  std::uint64_t chunk_count{};
  std::uint64_t gguf_size{};
  std::uint64_t tree_size{};
  consensus::Hash declared_root{};
  consensus::Hash tree_sha256{};
};
class ResourceLimit final : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
struct Tensor {
  std::string name;
  std::uint64_t data_offset{};
  std::uint64_t bytes{};
  std::uint32_t ggml_type{};
};

class ReadOnlyGguf {
public:
  explicit ReadOnlyGguf(const std::filesystem::path &path);
  ~ReadOnlyGguf();
  ReadOnlyGguf(const ReadOnlyGguf &) = delete;
  ReadOnlyGguf &operator=(const ReadOnlyGguf &) = delete;
  std::uint64_t file_size() const noexcept { return file_size_; }
  std::uint64_t tensor_data_offset() const noexcept { return tensor_data_offset_; }
  const std::vector<Tensor> &canonical_tensors() const noexcept { return tensors_; }
  std::uint64_t chunk_count() const noexcept { return chunk_count_; }
  std::vector<std::uint8_t> read_tile(std::uint64_t tile_index) const;
  void read_tile_into(std::uint64_t tile_index,
                      std::span<std::uint8_t, 1024> destination) const;
  void read_tiles_into(std::uint64_t first_tile, std::span<std::uint8_t> out) const;
  // Sequential VRAM upload: pread without faulting the persistent mmap, then
  // advise completed clean ranges away. Canonical tensor ordering is unchanged.
  void read_tiles_streaming(std::uint64_t first_tile, std::span<std::uint8_t> out) const;
  using ScanGuard = std::function<void(std::uint64_t, std::uint64_t)>;
  consensus::Hash recompute_root(const ScanGuard &guard,
                                 std::uint64_t progress_interval_chunks = 1U << 20) const;
  SidecarStatus verify_sidecar(const std::filesystem::path &tree,
                               const std::filesystem::path &metadata,
                               const consensus::Hash &official_model_id,
                               const consensus::Hash &official_root,
                               std::uint64_t official_chunks) const;
  const std::filesystem::path &path() const noexcept { return path_; }

private:
  void read_tiles_impl(std::uint64_t tile,std::span<std::uint8_t> out,bool streaming) const;
  struct TileSpan { std::uint64_t first_chunk{}; std::uint64_t chunk_count{}; std::uint64_t file_offset{}; };
  std::filesystem::path path_;
  std::unique_ptr<platform::ReadOnlyFile> file_;
  const std::uint8_t *mmap_base_{nullptr};
  std::uint64_t file_size_{};
  std::uint64_t tensor_data_offset_{};
  std::uint64_t chunk_count_{};
  std::vector<Tensor> tensors_;
  std::vector<TileSpan> tile_spans_;
};

class SparseMerkleIndex {
public:
  struct TileRootInput { std::uint64_t tile; std::array<std::uint8_t,1024> bytes; };
  struct PathCacheStats { std::uint64_t hits{}; std::uint64_t misses{}; std::size_t entries{}; };
  SparseMerkleIndex(const ReadOnlyGguf &gguf, std::filesystem::path tree);
  SparseMerkleIndex(const ReadOnlyGguf &gguf, std::filesystem::path tree, bool resident_upper);
  ~SparseMerkleIndex();
  SparseMerkleIndex(const SparseMerkleIndex &) = delete;
  SparseMerkleIndex &operator=(const SparseMerkleIndex &) = delete;
  std::vector<consensus::Hash> tile_path(std::uint64_t tile_index) const;
  void prefetch_tile_path(std::uint64_t tile_index) const;
  // Warm the existing bounded level-5 cache from an exact canonical tile
  // provider (for example the verified GPU-resident model).
  void cache_tile_root(std::uint64_t tile_index,
      const std::function<std::array<std::uint8_t,1024>(std::uint64_t)> &reader) const;
  // At most one winner's siblings, two CPU lanes, no file/GPU access.
  void cache_tile_roots(std::span<const TileRootInput> tiles) const;
  std::size_t resident_upper_bytes() const noexcept { return upper_bytes_; }
  std::uint32_t height() const noexcept { return total_levels_; }
  PathCacheStats path_cache_stats() const;
private:
  struct Checkpoint { std::uint32_t level; std::uint64_t offset; std::uint64_t count; };
  const ReadOnlyGguf &gguf_;
  std::filesystem::path tree_;
  std::unique_ptr<platform::ReadOnlyFile> tree_file_;
  std::uint32_t total_levels_{};
  std::vector<Checkpoint> checkpoints_;
  std::vector<std::vector<consensus::Hash>> upper_levels_;
  std::size_t upper_bytes_{};
  mutable std::mutex cache_mutex_;
  mutable std::unordered_map<std::uint64_t, consensus::Hash> path_cache_;
  mutable std::deque<std::uint64_t> path_cache_fifo_;
  mutable std::uint64_t path_cache_hits_{};
  mutable std::uint64_t path_cache_misses_{};
  struct SlabCacheEntry {
    std::uint64_t begin{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t count{};
    std::array<consensus::Hash, 64> nodes{};
  };
  // Keep the bounded cache off the caller stack (several indexes may coexist).
  mutable std::vector<SlabCacheEntry> slab_cache_{256};
  mutable std::size_t slab_cache_cursor_{};
  std::uint64_t count_at(std::uint32_t level) const;
  consensus::Hash read_node(const Checkpoint &checkpoint, std::uint64_t index) const;
  consensus::Hash cached_node(std::uint32_t level, std::uint64_t index) const;
  void cache_node(std::uint32_t level, std::uint64_t index,
                  const consensus::Hash &value) const;
  consensus::Hash tile_root(std::uint64_t tile_index) const;
  consensus::Hash subtree(std::uint64_t start, std::uint64_t span,
                          std::uint32_t source_level) const;
};
} // namespace keryx::nexus::model
