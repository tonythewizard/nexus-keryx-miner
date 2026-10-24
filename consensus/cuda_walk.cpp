#include "keryx/nexus/developer_config.hpp"
#include "keryx/nexus/consensus/cuda_engine.hpp"
#include <cuda.h>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <list>
#include <unordered_map>
#include <unordered_set>

namespace keryx::nexus::consensus {
namespace {
constexpr std::size_t tile_bytes_cuda = 1024;
constexpr std::size_t steps_cuda = 256;
constexpr std::size_t tile_cache_max_bytes = 32ULL * 1024 * 1024;

extern "C" const unsigned char _binary_nexus_pom_cubin_start[];

void check(CUresult status, const char *what) {
  if (status != CUDA_SUCCESS) {
    const char *name = nullptr, *detail = nullptr;
    cuGetErrorName(status, &name);
    cuGetErrorString(status, &detail);
    throw std::runtime_error(std::string(what) + ": " +
        (name ? name : "CUDA driver error") + " (" +
        (detail ? detail : "unknown") + ")");
  }
}

std::string format_uuid(const CUuuid &uuid) {
  std::ostringstream out;
  out << "GPU-" << std::hex << std::setfill('0');
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(
        static_cast<unsigned char>(uuid.bytes[i]));
  }
  return out.str();
}

struct PrimaryContext {
  CUdevice device{};
  CUcontext value{};
  bool retained{};
  void retain(CUdevice selected) {
    device = selected;
    check(cuDevicePrimaryCtxRetain(&value, device), "retain authorized GPU primary context");
    retained = true;
    check(cuCtxSetCurrent(value), "set authorized GPU primary context");
  }
  ~PrimaryContext() {
    if (retained) {
      cuCtxSetCurrent(nullptr);
      cuDevicePrimaryCtxRelease(device);
    }
  }
};

void ensure_context(CUcontext context) {
  check(cuCtxSetCurrent(context), "set PoM primary context on calling thread");
}

struct DeviceBuffer {
  CUdeviceptr ptr{};
  CUcontext context{};
  DeviceBuffer() = default;
  explicit DeviceBuffer(std::size_t bytes) {
    check(cuCtxGetCurrent(&context), "get current context for allocation");
    if (bytes) check(cuMemAlloc(&ptr, bytes), "cuMemAlloc");
  }
  ~DeviceBuffer() {
    if (ptr) { cuCtxSetCurrent(context); cuMemFree(ptr); }
  }
  DeviceBuffer(const DeviceBuffer &) = delete;
  DeviceBuffer &operator=(const DeviceBuffer &) = delete;
  DeviceBuffer(DeviceBuffer &&other) noexcept : ptr(other.ptr), context(other.context) { other.ptr = 0; }
  DeviceBuffer &operator=(DeviceBuffer &&other) noexcept {
    if (this != &other) {
      if (ptr) { cuCtxSetCurrent(context); cuMemFree(ptr); }
      ptr = other.ptr; context = other.context; other.ptr = 0;
    }
    return *this;
  }
};
struct Stream {
  CUstream value{};
  CUcontext context{};
  void create() {
    check(cuCtxGetCurrent(&context), "get current context for stream");
    check(cuStreamCreate(&value, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
  }
  ~Stream() { if (value) { cuCtxSetCurrent(context); cuStreamDestroy(value); } }
};
struct Event {
  CUevent value{};
  CUcontext context{};
  void create() {
    check(cuCtxGetCurrent(&context), "get current context for event");
    check(cuEventCreate(&value, CU_EVENT_DEFAULT), "cuEventCreate");
  }
  ~Event() { if (value) { cuCtxSetCurrent(context); cuEventDestroy(value); } }
};
struct PinnedBuffer {
  std::uint8_t *ptr{};
  std::size_t bytes{};
  CUcontext context{};
  PinnedBuffer() = default;
  void allocate(std::size_t size) {
    if (ptr || bytes) throw std::logic_error("pinned staging buffer already allocated");
    bytes = size;
    check(cuCtxGetCurrent(&context), "get current context for pinned staging");
    if (bytes) check(cuMemHostAlloc(reinterpret_cast<void **>(&ptr), bytes,
                                    CU_MEMHOSTALLOC_PORTABLE), "cuMemHostAlloc staging");
  }
  ~PinnedBuffer() {
    if (ptr) { cuCtxSetCurrent(context); cuMemFreeHost(ptr); }
  }
  PinnedBuffer(const PinnedBuffer &) = delete;
  PinnedBuffer &operator=(const PinnedBuffer &) = delete;
};
} // namespace

std::vector<CudaDeviceInfo> enumerate_cuda_devices() {
  check(cuInit(0), "cuInit");
  int count = 0;
  check(cuDeviceGetCount(&count), "cuDeviceGetCount");
  std::vector<CudaDeviceInfo> devices;
  for (int ordinal = 0; ordinal < count; ++ordinal) {
    CUdevice device{};
    char name[256]{};
    CUuuid uuid{};
    int major = 0, minor = 0;
    std::size_t total = 0;
    check(cuDeviceGet(&device, ordinal), "cuDeviceGet");
    check(cuDeviceGetName(name, sizeof(name), device), "cuDeviceGetName");
    check(cuDeviceGetUuid(&uuid, device), "cuDeviceGetUuid");
    check(cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device), "query compute capability major");
    check(cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device), "query compute capability minor");
    check(cuDeviceTotalMem(&total, device), "cuDeviceTotalMem");
    devices.push_back({ordinal, name, format_uuid(uuid), major, minor, total, 0});
  }
  return devices;
}

struct CudaWalkEngine::Impl {
  struct CacheEntry {
    std::uint64_t tile_id{};
    bool occupied{};
    std::list<std::uint32_t>::iterator lru_position;
  };
  PrimaryContext context;
  CUmodule module{};
  CUfunction pom_kernel{};
  CUfunction scatter_kernel{};
  CudaDeviceInfo device;
  std::size_t max_nonces{};
  std::size_t cache_capacity_tiles{};
  std::size_t owned_device_bytes{};
  std::string active_model_identity;
  DeviceBuffer seeds, tiles, upload_tiles, upload_slots, slots, offsets, final_states, trace;
  PinnedBuffer pinned_upload;
  PinnedBuffer pinned_inputs;
  PinnedBuffer pinned_outputs;
  std::vector<CacheEntry> cache_entries;
  std::unordered_map<std::uint64_t, std::uint32_t> cache_by_tile;
  std::list<std::uint32_t> cache_lru;
  bool cache_committed{true};
  Stream stream;
  Event transfer_start, copy_end, scatter_start, scatter_end;
  Event kernel_start, kernel_end, transfer_end;

  Impl(const std::string &required_uuid, std::size_t max_batch, bool proof_only)
      : max_nonces(max_batch) {
    const char *visible = std::getenv("CUDA_VISIBLE_DEVICES");
    if (required_uuid != primary_gpu_uuid() || visible == nullptr ||
        std::string_view(visible) != primary_gpu_uuid())
      throw std::runtime_error(
          "CUDA fail-closed: CUDA_VISIBLE_DEVICES and requested UUID must identify only the authorized primary NVIDIA GPU");
    if (max_nonces == 0 || max_nonces > 8)
      throw std::invalid_argument("persistent CUDA engine capacity must be in [1,8]");
    auto devices = enumerate_cuda_devices();
    if (devices.size() != 1)
      throw std::runtime_error("CUDA fail-closed: runtime must expose exactly one authorized device");
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto &d) {
      return d.uuid == required_uuid;
    });
    if (found == devices.end())
      throw std::runtime_error("requested CUDA GPU UUID is not visible: " + required_uuid);
    if (found->name != "NVIDIA GeForce primary NVIDIA GPU" ||
        found->major != 8 || found->minor != 6)
      throw std::runtime_error("CUDA PoM baseline requires SM 8.6; selected device is " +
          found->name + " SM " + std::to_string(found->major) + "." + std::to_string(found->minor));
    CUdevice selected{};
    check(cuDeviceGet(&selected, found->ordinal), "cuDeviceGet selected GPU");
    context.retain(selected);
    check(cuModuleLoadData(&module, _binary_nexus_pom_cubin_start), "cuModuleLoadData embedded SM 8.6 PoM module");
    check(cuModuleGetFunction(&pom_kernel, module, "pom_v4_walk_kernel"), "cuModuleGetFunction PoM kernel");
    check(cuModuleGetFunction(&scatter_kernel, module, "cache_scatter_kernel"), "cuModuleGetFunction cache scatter kernel");
    stream.create();
    transfer_start.create();
    copy_end.create();
    scatter_start.create();
    scatter_end.create();
    kernel_start.create();
    kernel_end.create();
    transfer_end.create();
    const std::size_t max_tile_bytes = max_nonces * steps_cuda * tile_bytes_cuda;
    const std::size_t max_slot_bytes = max_nonces * steps_cuda * sizeof(std::uint32_t);
    const std::size_t max_offset_bytes = max_nonces * steps_cuda * sizeof(std::uint64_t);
    const std::size_t max_final_bytes = max_nonces * tile_bytes_cuda;
    const std::size_t max_trace_bytes = max_nonces * (steps_cuda + 1) * tile_bytes_cuda;
    const std::size_t base_allocation = max_nonces * sizeof(std::uint64_t) +
        max_tile_bytes + max_nonces * steps_cuda * sizeof(std::uint32_t) +
        max_slot_bytes + max_offset_bytes + max_final_bytes + max_trace_bytes;
    std::size_t free_bytes = 0, total_bytes = 0;
    check(cuMemGetInfo(&free_bytes, &total_bytes), "cuMemGetInfo selected GPU");
    constexpr std::size_t reserve_bytes = 256ULL * 1024 * 1024;
    if (free_bytes < base_allocation + reserve_bytes + max_nonces * steps_cuda * tile_bytes_cuda)
      throw std::runtime_error("insufficient free VRAM for bounded persistent engine: free=" +
          std::to_string(free_bytes) + " required=" + std::to_string(base_allocation + reserve_bytes));
    // A single winning-proof verifier is destroyed after use; its cache needs
    // only the current microbatch, not 32 MiB of cross-batch history.
    const auto cache_limit = proof_only ? max_nonces * steps_cuda * tile_bytes_cuda : tile_cache_max_bytes;
    const auto cache_budget = std::min(cache_limit,
        free_bytes - base_allocation - reserve_bytes);
    cache_capacity_tiles = cache_budget / tile_bytes_cuda;
    if (cache_capacity_tiles < max_nonces * steps_cuda)
      throw std::runtime_error("free VRAM cannot hold one complete bounded microbatch in the persistent tile cache");
    device = *found;
    device.total_memory = total_bytes;
    device.free_memory = free_bytes;
    seeds = DeviceBuffer(max_nonces * sizeof(std::uint64_t));
    tiles = DeviceBuffer(cache_capacity_tiles * tile_bytes_cuda);
    upload_tiles = DeviceBuffer(max_tile_bytes);
    upload_slots = DeviceBuffer(max_nonces * steps_cuda * sizeof(std::uint32_t));
    slots = DeviceBuffer(max_slot_bytes);
    offsets = DeviceBuffer(max_offset_bytes);
    final_states = DeviceBuffer(max_final_bytes);
    pinned_upload.allocate(max_tile_bytes);
    pinned_inputs.allocate(max_nonces * sizeof(std::uint64_t) + max_slot_bytes + max_slot_bytes);
    pinned_outputs.allocate(max_offset_bytes + max_final_bytes + max_trace_bytes);
    cache_entries.resize(cache_capacity_tiles);
    cache_by_tile.reserve(cache_capacity_tiles);
    cache_lru.clear();
    check(cuMemGetInfo(&free_bytes, &total_bytes), "cuMemGetInfo after persistent buffer allocation");
    device.free_memory = free_bytes;
    owned_device_bytes = max_nonces * sizeof(std::uint64_t) +
        cache_capacity_tiles * tile_bytes_cuda + max_tile_bytes +
        max_nonces * steps_cuda * sizeof(std::uint32_t) + max_slot_bytes +
        max_offset_bytes + max_final_bytes;
  }

  ~Impl() {
    if (context.value) {
      cuCtxSetCurrent(context.value);
      if (stream.value) cuStreamSynchronize(stream.value);
    }
    if (module) cuModuleUnload(module);
  }

  void run(std::uint64_t tile_count, const std::string &model_cache_identity,
           const std::vector<std::uint64_t> &seed_values,
           const std::vector<std::uint8_t> &unique_tiles,
           const std::vector<std::uint64_t> &unique_tile_ids,
           const std::vector<std::uint32_t> &tile_slots,
           bool diagnostic_states, CudaWalkBatch &result,
           const std::function<void(bool)> &after_kernel_enqueue) {
    const auto nonce_count = seed_values.size();
    if (model_cache_identity.empty() || tile_count == 0 || nonce_count == 0 || nonce_count > max_nonces ||
        unique_tiles.empty() || unique_tiles.size() % tile_bytes_cuda != 0 ||
        tile_slots.size() != nonce_count * steps_cuda ||
        unique_tile_ids.size() != unique_tiles.size() / tile_bytes_cuda)
      throw std::invalid_argument("invalid CUDA PoM physical microbatch dimensions");
    const auto unique_count = unique_tiles.size() / tile_bytes_cuda;
    if (unique_count > nonce_count * steps_cuda || unique_count > cache_capacity_tiles ||
        std::any_of(tile_slots.begin(), tile_slots.end(),
                    [&](auto slot) { return slot >= unique_count; }) ||
        std::any_of(unique_tile_ids.begin(), unique_tile_ids.end(),
                    [&](auto id) { return id >= tile_count; }))
      throw std::invalid_argument("CUDA tile cache slot outside bounded microbatch cache");
    if (active_model_identity != model_cache_identity) {
      cache_by_tile.clear();
      cache_lru.clear();
      for (auto &entry : cache_entries) entry.occupied = false;
      active_model_identity = model_cache_identity;
      cache_committed = true;
    }
    if (!cache_committed) {
      cache_by_tile.clear();
      cache_lru.clear();
      for (auto &entry : cache_entries) entry.occupied = false;
      cache_committed = true;
    }
    std::unordered_set<std::uint64_t> requested_ids;
    requested_ids.reserve(unique_count);
    for (const auto id : unique_tile_ids)
      if (!requested_ids.insert(id).second)
        throw std::invalid_argument("duplicate global GGUF tile id in CUDA cache input");

    std::size_t misses = 0;
    for (const auto id : unique_tile_ids) misses += cache_by_tile.contains(id) ? 0U : 1U;
    cache_committed = false;
    std::vector<std::uint32_t> free_slots;
    free_slots.reserve(misses);
    for (std::uint32_t slot = 0; slot < cache_entries.size() && free_slots.size() < misses; ++slot)
      if (!cache_entries[slot].occupied) free_slots.push_back(slot);
    for (auto it = cache_lru.begin(); it != cache_lru.end() && free_slots.size() < misses;) {
      const auto slot = *it;
      if (requested_ids.contains(cache_entries[slot].tile_id)) { ++it; continue; }
      cache_by_tile.erase(cache_entries[slot].tile_id);
      cache_entries[slot].occupied = false;
      it = cache_lru.erase(it);
      free_slots.push_back(slot);
    }
    if (free_slots.size() != misses)
      throw std::runtime_error("persistent GGUF tile cache cannot reserve a complete microbatch safely");

    std::vector<std::uint32_t> local_to_cache(unique_count);
    std::vector<std::uint32_t> upload_destinations;
    upload_destinations.reserve(misses);
    std::size_t packed_bytes = 0, next_free = 0, hits = 0;
    for (std::size_t local = 0; local < unique_count; ++local) {
      const auto id = unique_tile_ids[local];
      const auto found = cache_by_tile.find(id);
      std::uint32_t slot{};
      if (found != cache_by_tile.end()) {
        slot = found->second;
        ++hits;
        cache_lru.splice(cache_lru.end(), cache_lru, cache_entries[slot].lru_position);
        cache_entries[slot].lru_position = std::prev(cache_lru.end());
      } else {
        slot = free_slots[next_free++];
        const auto *source = unique_tiles.data() + local * tile_bytes_cuda;
        std::copy_n(source, tile_bytes_cuda, pinned_upload.ptr + packed_bytes);
        packed_bytes += tile_bytes_cuda;
        upload_destinations.push_back(slot);
        cache_entries[slot].tile_id = id;
        cache_entries[slot].occupied = true;
        cache_lru.push_back(slot);
        cache_entries[slot].lru_position = std::prev(cache_lru.end());
        cache_by_tile.emplace(id, slot);
      }
      local_to_cache[local] = slot;
    }
    std::vector<std::uint32_t> device_slots(tile_slots.size());
    std::transform(tile_slots.begin(), tile_slots.end(), device_slots.begin(),
                   [&](std::uint32_t local) { return local_to_cache[local]; });
    const std::size_t upload_slot_bytes = upload_destinations.size() * sizeof(std::uint32_t);
    const std::size_t slot_bytes = tile_slots.size() * sizeof(std::uint32_t);
    const std::size_t offset_bytes = nonce_count * steps_cuda * sizeof(std::uint64_t);
    const std::size_t final_bytes = nonce_count * tile_bytes_cuda;
    const std::size_t trace_bytes = diagnostic_states
        ? nonce_count * (steps_cuda + 1) * tile_bytes_cuda : 0;
    if (diagnostic_states && !trace.ptr) {
      std::size_t free_before_trace = 0, total_before_trace = 0;
      check(cuMemGetInfo(&free_before_trace, &total_before_trace),
            "cuMemGetInfo before diagnostic trace allocation");
      constexpr std::size_t reserve_bytes = 256ULL * 1024 * 1024;
      if (free_before_trace < trace_bytes + reserve_bytes)
        throw std::runtime_error("insufficient VRAM for optional diagnostic state trace");
      const std::size_t max_trace_bytes = max_nonces * (steps_cuda + 1) * tile_bytes_cuda;
      trace = DeviceBuffer(max_trace_bytes);
      owned_device_bytes += max_trace_bytes;
    }
    const auto wall_start = std::chrono::steady_clock::now();
    auto *input_seeds = reinterpret_cast<std::uint64_t *>(pinned_inputs.ptr);
    auto *input_slots = reinterpret_cast<std::uint32_t *>(input_seeds + max_nonces);
    auto *input_upload_slots = input_slots + max_nonces * steps_cuda;
    std::copy(seed_values.begin(), seed_values.end(), input_seeds);
    std::copy(device_slots.begin(), device_slots.end(), input_slots);
    if (packed_bytes) std::copy(upload_destinations.begin(), upload_destinations.end(), input_upload_slots);
    check(cuEventRecord(transfer_start.value, stream.value), "record transfer start");
    check(cuMemcpyHtoDAsync(seeds.ptr, input_seeds, nonce_count * sizeof(std::uint64_t), stream.value), "copy seeds to GPU");
    check(cuMemcpyHtoDAsync(slots.ptr, input_slots, slot_bytes, stream.value), "copy microbatch tile map");
    if (packed_bytes) {
      check(cuMemcpyHtoDAsync(upload_tiles.ptr, pinned_upload.ptr, packed_bytes, stream.value), "copy missing GGUF cache tiles");
      check(cuMemcpyHtoDAsync(upload_slots.ptr, input_upload_slots, upload_slot_bytes, stream.value), "copy GGUF cache destinations");
    }
    check(cuEventRecord(copy_end.value, stream.value), "record H2D copy completion");
    if (packed_bytes) {
      check(cuEventRecord(scatter_start.value, stream.value), "record cache scatter start");
      CUdeviceptr cache_arg = tiles.ptr, packed_arg = upload_tiles.ptr, destinations_arg = upload_slots.ptr;
      const auto count_arg = static_cast<std::uint32_t>(upload_destinations.size());
      void *scatter_params[] = {&cache_arg, &packed_arg, &destinations_arg, const_cast<std::uint32_t *>(&count_arg)};
      check(cuLaunchKernel(scatter_kernel, count_arg, 1, 1, 256, 1, 1, 0, stream.value,
                           scatter_params, nullptr), "cuLaunchKernel cache scatter");
      check(cuEventRecord(scatter_end.value, stream.value), "record cache scatter end");
    }
    check(cuEventRecord(kernel_start.value, stream.value), "record kernel start");
    CUdeviceptr seeds_arg = seeds.ptr, tiles_arg = tiles.ptr, slots_arg = slots.ptr;
    CUdeviceptr offsets_arg = offsets.ptr, final_arg = final_states.ptr, trace_arg = trace.ptr;
    std::uint64_t tile_count_arg = tile_count;
    bool diagnostics_arg = diagnostic_states;
    void *pom_params[] = {&seeds_arg, &tiles_arg, &slots_arg, &tile_count_arg,
                          &offsets_arg, &final_arg, &trace_arg, &diagnostics_arg};
    check(cuLaunchKernel(pom_kernel, static_cast<unsigned>(nonce_count), 1, 1, 256, 1, 1,
                         0, stream.value, pom_params, nullptr), "cuLaunchKernel PoM v4 baseline");
    check(cuEventRecord(kernel_end.value, stream.value), "record kernel end");
    if (after_kernel_enqueue) {
      const auto event_status = cuEventQuery(kernel_end.value);
      const bool kernel_in_flight = event_status == CUDA_ERROR_NOT_READY;
      if (event_status != CUDA_ERROR_NOT_READY) check(event_status, "query PoM kernel completion at enqueue hook");
      after_kernel_enqueue(kernel_in_flight);
    }
    result.device = device;
    result.unique_tile_count = unique_count;
    result.h2d_bytes = nonce_count * sizeof(std::uint64_t) + packed_bytes +
        upload_slot_bytes + slot_bytes;
    result.tile_cache_hits = hits;
    result.tile_cache_misses = misses;
    result.tile_cache_vram_bytes = cache_capacity_tiles * tile_bytes_cuda;
    result.pinned_host_bytes = pinned_upload.bytes + pinned_inputs.bytes + pinned_outputs.bytes;
    result.offsets.resize(nonce_count * steps_cuda);
    result.final_states.resize(final_bytes);
    if (diagnostic_states) result.diagnostic_states.resize(trace_bytes);
    else result.diagnostic_states.clear();
    auto *output_offsets = reinterpret_cast<std::uint64_t *>(pinned_outputs.ptr);
    auto *output_final = reinterpret_cast<std::uint8_t *>(output_offsets + max_nonces * steps_cuda);
    auto *output_trace = output_final + max_nonces * tile_bytes_cuda;
    check(cuMemcpyDtoHAsync(output_offsets, offsets.ptr, offset_bytes, stream.value), "copy offsets from GPU");
    check(cuMemcpyDtoHAsync(output_final, final_states.ptr, final_bytes, stream.value), "copy final state from GPU");
    if (diagnostic_states)
      check(cuMemcpyDtoHAsync(output_trace, trace.ptr, trace_bytes, stream.value), "copy diagnostic states from GPU");
    check(cuEventRecord(transfer_end.value, stream.value), "record transfer end");
    check(cuEventSynchronize(transfer_end.value), "synchronize PoM v4 microbatch");
    std::copy_n(output_offsets, result.offsets.size(), result.offsets.data());
    std::copy_n(output_final, result.final_states.size(), result.final_states.data());
    if (diagnostic_states) std::copy_n(output_trace, result.diagnostic_states.size(), result.diagnostic_states.data());
    cache_committed = true;
    std::size_t free_after_run = 0, total_after_run = 0;
    check(cuMemGetInfo(&free_after_run, &total_after_run), "cuMemGetInfo after PoM microbatch");
    device.free_memory = free_after_run;
    result.device.total_memory = total_after_run;
    result.device.free_memory = free_after_run;
    result.cuda_allocated_bytes = owned_device_bytes;
    float h2d = 0, d2h = 0, kernel = 0;
    check(cuEventElapsedTime(&h2d, transfer_start.value, copy_end.value), "measure H2D time");
    float scatter = 0;
    if (packed_bytes)
      check(cuEventElapsedTime(&scatter, scatter_start.value, scatter_end.value), "measure cache scatter time");
    check(cuEventElapsedTime(&kernel, kernel_start.value, kernel_end.value), "measure kernel time");
    check(cuEventElapsedTime(&d2h, kernel_end.value, transfer_end.value), "measure D2H time");
    result.transfer_ms = h2d;
    result.cache_scatter_ms = scatter;
    result.device_to_host_ms = d2h;
    result.kernel_ms = kernel;
    result.total_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - wall_start).count();
  }
};

CudaWalkEngine::CudaWalkEngine(const std::string &uuid, std::size_t max_nonces)
    : CudaWalkEngine(uuid, max_nonces, false) {}
CudaWalkEngine::CudaWalkEngine(const std::string &uuid, std::size_t max_nonces, bool proof_only)
    : impl_(std::make_unique<Impl>(uuid, max_nonces, proof_only)) {}
CudaWalkEngine::~CudaWalkEngine() = default;
CudaWalkEngine::CudaWalkEngine(CudaWalkEngine &&) noexcept = default;
CudaWalkEngine &CudaWalkEngine::operator=(CudaWalkEngine &&) noexcept = default;
void CudaWalkEngine::run(std::uint64_t tile_count,
                         const std::string &model_cache_identity,
                         const std::vector<std::uint64_t> &seeds,
                         const std::vector<std::uint8_t> &unique_tiles,
                         const std::vector<std::uint64_t> &unique_tile_ids,
                         const std::vector<std::uint32_t> &tile_slots,
                         bool diagnostic_states, CudaWalkBatch &result,
                         const std::function<void(bool)> &after_kernel_enqueue) {
  // CUDA contexts are current per host thread; restore the retained device
  // primary context before using its Driver API streams, events, and module.
  ensure_context(impl_->context.value);
  impl_->run(tile_count, model_cache_identity, seeds, unique_tiles, unique_tile_ids,
             tile_slots, diagnostic_states, result, after_kernel_enqueue);
}
} // namespace keryx::nexus::consensus
