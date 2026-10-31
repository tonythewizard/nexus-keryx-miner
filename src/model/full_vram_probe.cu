#include "keryx/nexus/developer_config.hpp"
#include <cuda_runtime.h>
#include "keryx/nexus/gpu_device_policy.hpp"
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr std::string_view authorized_uuid = "" NEXUS_PRIMARY_GPU_UUID "";
constexpr std::uint64_t expected_file_size = 6522004864ULL;
constexpr std::string_view expected_sha256 =
    "946072b16f5d672e60357410f900888f6522b3ae49f241b5bf2142cb89637fb6";
// Match the existing functional GPU guard's 2048 MiB free-VRAM floor.
constexpr std::uint64_t reserve_bytes = 2ULL << 30;
constexpr std::size_t staging_bytes = 8ULL << 20;
constexpr std::size_t sample_width = 64;
constexpr std::size_t sample_count = 8;

void check(cudaError_t status, const char *what) {
  if (status != cudaSuccess)
    throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
}

std::string uuid_string(const cudaUUID_t &uuid) {
  std::ostringstream out;
  out << "GPU-" << std::hex << std::setfill('0');
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(uuid.bytes[i]));
  }
  return out.str();
}

struct Snapshot {
  std::uint64_t available_kib{};
  std::uint64_t swap_free_kib{};
  std::uint64_t pswpout{};
  double psi_full_avg10{};
};

Snapshot host_snapshot() {
  Snapshot s;
  std::ifstream mem("/proc/meminfo");
  if (!mem) throw std::runtime_error("cannot read /proc/meminfo");
  std::string key, unit;
  std::uint64_t value{};
  while (mem >> key >> value >> unit) {
    if (key == "MemAvailable:") s.available_kib = value;
    else if (key == "SwapFree:") s.swap_free_kib = value;
  }
  if (!s.available_kib || !s.swap_free_kib)
    throw std::runtime_error("RAM/swap metrics unavailable");
  std::ifstream psi("/proc/pressure/memory");
  std::string line;
  bool found = false;
  while (std::getline(psi, line)) {
    if (line.rfind("full ", 0) != 0) continue;
    std::istringstream row(line);
    std::string field;
    while (row >> field) {
      if (field.rfind("avg10=", 0) == 0) {
        s.psi_full_avg10 = std::stod(field.substr(6));
        found = true;
      }
    }
  }
  if (!found) throw std::runtime_error("memory PSI full avg10 unavailable");
  std::ifstream vm("/proc/vmstat");
  bool have_swapout = false;
  while (vm >> key >> value) if (key == "pswpout") {
    s.pswpout = value;
    have_swapout = true;
    break;
  }
  if (!have_swapout) throw std::runtime_error("swap-out counter unavailable");
  return s;
}

void validate_start(const Snapshot &s) {
  constexpr std::uint64_t gib_kib = 1024ULL * 1024;
  if (s.available_kib < 2 * gib_kib || s.swap_free_kib < 2 * gib_kib ||
      s.psi_full_avg10 > 0.10)
    throw std::runtime_error("host admission blocked: MemAvailable=" +
      std::to_string(s.available_kib) + " KiB SwapFree=" +
      std::to_string(s.swap_free_kib) + " KiB memory_PSI_full_avg10=" +
      std::to_string(s.psi_full_avg10));
}

void validate_during(const Snapshot &s, std::uint64_t baseline_swapout) {
  constexpr std::uint64_t gib_kib = 1024ULL * 1024;
  if (s.available_kib < 2 * gib_kib || s.swap_free_kib < 2 * gib_kib ||
      s.psi_full_avg10 > 0.20 || s.pswpout != baseline_swapout)
    throw std::runtime_error("host emergency stop: memory/swap reserve, PSI, or swap-out changed");
}

void pread_exact(int fd, std::uint8_t *dst, std::size_t size, std::uint64_t offset) {
  std::size_t done = 0;
  while (done < size) {
    const auto n = ::pread(fd, dst + done, size - done,
                           static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("short GGUF read at byte " +
                                         std::to_string(offset + done));
    done += static_cast<std::size_t>(n);
  }
}

std::string digest_hex(const std::array<unsigned char, 32> &digest) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest) out << std::setw(2) << static_cast<unsigned>(byte);
  return out.str();
}

std::pair<std::uint64_t, std::uint64_t> rss_pss_kib() {
  std::ifstream in("/proc/self/smaps_rollup");
  if (!in) throw std::runtime_error("cannot read process smaps_rollup");
  std::uint64_t rss = 0, pss = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    std::string key, unit;
    std::uint64_t n{};
    if (!(row >> key >> n >> unit)) continue;
    if (key == "Rss:") rss = n;
    else if (key == "Pss:") pss = n;
  }
  if (!in.eof() || !rss || !pss) throw std::runtime_error("RSS/PSS unavailable");
  return {rss, pss};
}

__global__ void read_sample_kernel(const std::uint8_t *resident,
                                   const std::uint64_t *offsets,
                                   std::uint8_t *out) {
  const unsigned sample = blockIdx.x;
  const unsigned byte = threadIdx.x;
  if (sample < sample_count && byte < sample_width)
    out[sample * sample_width + byte] = resident[offsets[sample] + byte];
}

struct CudaResources {
  std::uint8_t *resident{};
  std::uint8_t *staging{};
  std::uint64_t *device_offsets{};
  std::uint8_t *device_samples{};
  cudaStream_t stream{};
  ~CudaResources() {
    if (stream) cudaStreamDestroy(stream);
    if (device_samples) cudaFree(device_samples);
    if (device_offsets) cudaFree(device_offsets);
    if (staging) cudaFreeHost(staging);
    if (resident) cudaFree(resident);
  }
};

int run(const char *model_path) {
  const char *visible = std::getenv("CUDA_VISIBLE_DEVICES");
  const char *requested = std::getenv("NEXUS_CUDA_DEVICE_UUID");
  if (!visible || std::string_view(visible) != authorized_uuid || !requested ||
      std::string_view(requested) != authorized_uuid)
    throw std::runtime_error("fail-closed: CUDA environment must expose only the authorized UUID");

  int device_count = 0;
  check(cudaGetDeviceCount(&device_count), "cudaGetDeviceCount");
  if (device_count != 1) throw std::runtime_error("CUDA runtime must expose exactly one device");
  cudaDeviceProp prop{};
  check(cudaGetDeviceProperties(&prop, 0), "cudaGetDeviceProperties authorized device");
  if (uuid_string(prop.uuid) != authorized_uuid)
    throw std::runtime_error("CUDA visible device UUID mismatch");
  keryx::nexus::require_cuda_model(prop.name, prop.major, prop.minor, std::getenv("NEXUS_PRIMARY_GPU_NAME"));
  check(cudaSetDevice(0), "cudaSetDevice authorized RTX");

  const int fd = ::open(model_path, O_RDONLY | O_CLOEXEC | O_NOATIME);
  if (fd < 0) throw std::runtime_error("cannot open GGUF read-only/no-atime");
  struct Fd { int value; ~Fd() { if (value >= 0) ::close(value); } } file{fd};
  struct stat before{};
  if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
      static_cast<std::uint64_t>(before.st_size) != expected_file_size)
    throw std::runtime_error("GGUF file type/size mismatch");

  Snapshot baseline{};
  for (int sample = 0; sample < 3; ++sample) {
    const auto current = host_snapshot();
    validate_start(current);
    if (sample && current.pswpout != baseline.pswpout)
      throw std::runtime_error("host admission blocked: swap-out changed during stable preflight");
    if (!sample) baseline = current;
    std::cout << "HOST_PREFLIGHT sample=" << sample + 1 << "/3 MemAvailable_kib="
              << current.available_kib << " SwapFree_kib=" << current.swap_free_kib
              << " PSI_full_avg10=" << current.psi_full_avg10 << " pswpout="
              << current.pswpout << std::endl;
    if (sample != 2) sleep(1);
  }

  std::size_t free_before = 0, total = 0;
  check(cudaMemGetInfo(&free_before, &total), "cudaMemGetInfo before allocation");
  if (free_before < expected_file_size + reserve_bytes)
    throw std::runtime_error("FULL_VRAM blocked before allocation: available CUDA memory is below model+existing-2GiB guard reserve");

  CudaResources cuda;
  check(cudaStreamCreateWithFlags(&cuda.stream, cudaStreamNonBlocking), "cudaStreamCreate");
  check(cudaMalloc(reinterpret_cast<void **>(&cuda.resident), expected_file_size),
        "cudaMalloc persistent full-GGUF-sized canonical region");
  check(cudaHostAlloc(reinterpret_cast<void **>(&cuda.staging), staging_bytes,
                      cudaHostAllocPortable), "cudaHostAlloc bounded 8MiB staging");
  check(cudaMalloc(reinterpret_cast<void **>(&cuda.device_offsets), sample_count * sizeof(std::uint64_t)),
        "cudaMalloc sample offsets");
  check(cudaMalloc(reinterpret_cast<void **>(&cuda.device_samples), sample_count * sample_width),
        "cudaMalloc sampled bytes");
  std::size_t free_after_alloc = 0;
  check(cudaMemGetInfo(&free_after_alloc, &total), "cudaMemGetInfo after full allocation");
  if (free_after_alloc < reserve_bytes)
    throw std::runtime_error("FULL_VRAM allocation consumed the existing 2GiB guard reserve");

  EVP_MD_CTX *raw_digest = EVP_MD_CTX_new();
  if (!raw_digest) throw std::runtime_error("SHA-256 context allocation failed");
  struct Digest { EVP_MD_CTX *p; ~Digest() { EVP_MD_CTX_free(p); } } digest{raw_digest};
  if (EVP_DigestInit_ex(raw_digest, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 initialization failed");
  cudaEvent_t begin{}, end{};
  check(cudaEventCreate(&begin), "cudaEventCreate upload begin");
  check(cudaEventCreate(&end), "cudaEventCreate upload end");
  struct Events { cudaEvent_t a,b; ~Events(){ if(a) cudaEventDestroy(a); if(b) cudaEventDestroy(b); } } events{begin,end};
  check(cudaEventRecord(begin, cuda.stream), "record upload begin");
  const auto wall_start = std::chrono::steady_clock::now();
  for (std::uint64_t offset = 0; offset < expected_file_size;) {
    const auto host = host_snapshot();
    validate_during(host, baseline.pswpout);
    std::size_t free_now = 0;
    check(cudaMemGetInfo(&free_now, &total), "cudaMemGetInfo during upload");
    if (free_now < reserve_bytes)
      throw std::runtime_error("FULL_VRAM upload paused: remaining GPU reserve fell below 2GiB");
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
        staging_bytes, expected_file_size - offset));
    pread_exact(fd, cuda.staging, count, offset);
    if (EVP_DigestUpdate(raw_digest, cuda.staging, count) != 1)
      throw std::runtime_error("SHA-256 update failed during stream");
    check(cudaMemcpyAsync(cuda.resident + offset, cuda.staging, count,
                          cudaMemcpyHostToDevice, cuda.stream), "stream GGUF bytes to RTX");
    check(cudaStreamSynchronize(cuda.stream), "synchronize bounded GGUF upload chunk");
    const int advice = ::posix_fadvise(fd, static_cast<off_t>(offset),
                                      static_cast<off_t>(count), POSIX_FADV_DONTNEED);
    if (advice != 0)
      throw std::runtime_error("posix_fadvise(DONTNEED) failed while bounding GGUF page-cache residency: " +
                               std::string(std::strerror(advice)));
    offset += count;
    if (offset % (256ULL << 20) < staging_bytes || offset == expected_file_size)
      std::cout << "UPLOAD_PROGRESS bytes=" << offset << "/" << expected_file_size << std::endl;
  }
  check(cudaEventRecord(end, cuda.stream), "record upload end");
  check(cudaEventSynchronize(end), "synchronize GGUF upload completion");
  const auto wall_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - wall_start).count();
  std::array<unsigned char, 32> hash{};
  unsigned hash_len = 0;
  if (EVP_DigestFinal_ex(raw_digest, hash.data(), &hash_len) != 1 || hash_len != hash.size())
    throw std::runtime_error("SHA-256 finalization failed");
  const auto actual_sha = digest_hex(hash);
  if (actual_sha != expected_sha256)
    throw std::runtime_error("GGUF SHA-256 changed during upload: " + actual_sha);
  struct stat after{};
  if (::fstat(fd, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
      before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
    throw std::runtime_error("GGUF identity changed during upload");

  std::array<std::uint64_t, sample_count> offsets{
      0, 4096, 10968448, expected_file_size / 4, expected_file_size / 2,
      (expected_file_size * 3) / 4, expected_file_size - 4096, expected_file_size - sample_width};
  check(cudaMemcpyAsync(cuda.device_offsets, offsets.data(), sizeof(offsets),
                        cudaMemcpyHostToDevice, cuda.stream), "copy sample offsets");
  check(cudaStreamSynchronize(cuda.stream), "synchronize sample offsets");
  std::array<std::uint8_t, sample_count * sample_width> actual{};
  std::array<std::uint8_t, sample_count * sample_width> expected{};
  for (std::size_t round = 0; round < 3; ++round) {
    read_sample_kernel<<<sample_count, sample_width, 0, cuda.stream>>>(
        cuda.resident, cuda.device_offsets, cuda.device_samples);
    check(cudaGetLastError(), "launch resident GGUF byte-sample kernel");
    check(cudaMemcpyAsync(actual.data(), cuda.device_samples, actual.size(),
                          cudaMemcpyDeviceToHost, cuda.stream), "copy sampled resident bytes back");
    check(cudaStreamSynchronize(cuda.stream), "synchronize resident sample read");
  }
  for (std::size_t i = 0; i < sample_count; ++i)
    pread_exact(fd, expected.data() + i * sample_width, sample_width, offsets[i]);
  if (actual != expected) throw std::runtime_error("GPU-resident sampled bytes differ from GGUF source");

  std::size_t free_after = 0;
  check(cudaMemGetInfo(&free_after, &total), "cudaMemGetInfo after repeated device reads");
  const auto [rss_kib, pss_kib] = rss_pss_kib();
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
  float upload_ms = 0;
  check(cudaEventElapsedTime(&upload_ms, begin, end), "measure upload CUDA event time");
  std::cout << "FULL_VRAM_PROBE=PASS uuid=" << authorized_uuid
            << " sm=" << prop.major << "." << prop.minor << " model_file_bytes=" << expected_file_size
            << " model_sha256=" << actual_sha << " allocation_bytes=" << expected_file_size
            << " uploaded_bytes=" << expected_file_size << " staging_bytes=" << staging_bytes
            << " sample_count=" << sample_count << " repeated_gpu_read_rounds=3"
            << " sample_parity=PASS cuda_free_after_mib=" << free_after / (1024 * 1024)
            << " reserve_bytes=" << reserve_bytes << " page_cache_advice=PASS upload_event_ms=" << upload_ms
            << " upload_wall_ms=" << wall_ms << " pcie_h2d_bytes=" << expected_file_size + sizeof(offsets)
            << " pcie_d2h_bytes=" << actual.size() * 3
            << " pinned_bytes=" << staging_bytes << " rss_kib=" << rss_kib
            << " pss_kib=" << pss_kib << " peak_rss_kib=" << usage.ru_maxrss << std::endl;
  return 0;
}
}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: nexus-full-vram-probe MODEL.gguf");
    return run(argv[1]);
  } catch (const std::exception &e) {
    std::cerr << "FULL_VRAM_PROBE=BLOCKED reason=" << e.what() << std::endl;
    return 13;
  }
}
