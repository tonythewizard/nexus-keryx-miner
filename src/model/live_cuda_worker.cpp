#include "keryx/nexus/developer_config.hpp"
#include "keryx/nexus/live_gpu_selection.hpp"
#include "keryx/nexus/scoped_file_cache_guard.hpp"
#include "keryx/nexus/ai_backend_config.hpp"
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
#include "keryx/nexus/amd_submission.hpp"
#include "keryx/nexus/amd_memory_handoff.hpp"
#include "keryx/nexus/vulkan_inference_process.hpp"
#endif
#include "keryx/nexus/capture.hpp"
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
#include "keryx/nexus/gpu_work_coordinator.hpp"
#include "keryx/nexus/secondary_work_lane.hpp"
#include "keryx/nexus/secondary_vulkan_config.hpp"
#include "keryx/nexus/amd_candidate_pipeline.hpp"
#include "keryx/nexus/primary_vulkan_workload.hpp"
#include "keryx/nexus/primary_vulkan_policy.hpp"
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
#include "keryx/nexus/vulkan_route_capability.hpp"
#endif
#endif
#include "keryx/nexus/consensus/cuda_engine.hpp"
#include "keryx/nexus/consensus/cuda_resident.hpp"
#include "keryx/nexus/consensus/resident_batch_policy.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/gpu_preemption.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/pipeline.hpp"
#include "keryx/nexus/network/submission_pipeline.hpp"
#include "keryx/nexus/network/worker_mode.hpp"
#include "keryx/nexus/network/submit_block_client.hpp"
#include "keryx/nexus/ai_response_service.hpp"
#include "keryx/nexus/escrow_watcher.hpp"
#include "keryx/nexus/escrow_claim_policy.hpp"
#include "keryx/nexus/strike_monitor.hpp"
#include "keryx/nexus/startup_readiness.hpp"
#include "keryx/nexus/live_worker_config.hpp"
#include "keryx/nexus/native_telemetry.hpp"
#include "keryx/nexus/proof_artifacts.hpp"
#include "keryx/nexus/proof_artifact_buffers.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include "keryx/nexus/rpc_template_cache.hpp"
#include "keryx/nexus/prepared_rpc_template.hpp"
#include "keryx/nexus/active_template_job.hpp"
#include "keryx/nexus/consensus/verified_walk_view.hpp"
#include "messages.pb.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <syncstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <spawn.h>
#include <csignal>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <chrono>
#include <future>
#include <ctime>
#include <deque>
#include <condition_variable>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <regex>
#include <set>
#include <openssl/evp.h>
#include <fcntl.h>
#include <unistd.h>

using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
namespace fs = std::filesystem;
extern char **environ;
namespace {
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
class SecondaryDeviceLease final {
public:
  explicit SecondaryDeviceLease(const fs::path &root,bool nvidia=false) {
    const auto directory=root/"state/gpu-devices";
    fs::create_directories(directory);
    const auto path=directory/developer_device_lease_name(nvidia);
    const int fd=::open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)throw std::runtime_error("Cannot open exclusive AMD device lease");
    struct stat st{};
    if(::fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=::geteuid()||(st.st_mode&0077)!=0){
      ::close(fd);throw std::runtime_error("Unsafe AMD device lease file");
    }
    if(::flock(fd,LOCK_EX|LOCK_NB)!=0){::close(fd);throw std::runtime_error("secondary AMD GPU already has an active device lease");}
    fd_=fd;
  }
  int descriptor() const noexcept {return fd_;}
  ~SecondaryDeviceLease(){if(fd_>=0)::close(fd_);}
private:int fd_{-1};
};
#endif

std::uint64_t monotonic_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uint64_t thread_cpu_ns() {
  timespec ts{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
    throw std::runtime_error("clock_gettime(CLOCK_THREAD_CPUTIME_ID) failed");
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}
std::atomic_bool process_stop{false};
static_assert(std::atomic_bool::is_always_lock_free,
              "signal handler requires lock-free atomic_bool");
extern "C" void request_stop(int) { process_stop.store(true,std::memory_order_relaxed); }
constexpr std::string_view gpu_uuid = "" NEXUS_PRIMARY_GPU_UUID "";
constexpr std::uint64_t first_nonce = 0x4b455259584d3231ULL;
constexpr Hash official_model{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
constexpr Hash official_root{0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};
struct QueuedAiRequest { protowire::RpcTransaction transaction; std::uint64_t daa{}; std::string id; std::uint64_t queued_ns{}; };
class AiRequestQueue final {
public:
  bool push(const protowire::RpcBlock &block) {
    if (!block.has_header()) return true;
    for (const auto &tx : block.transactions()) {
      if (tx.subnetworkid() != ai::request_subnetwork) continue;
      std::string why;
      const auto parsed=ai::parse_request(tx,block.header().daascore(),&why,"mainnet");
      if (!parsed) {
        std::cerr<<"AI_REQUEST_INVALID daa="<<block.header().daascore()<<" reason="<<why<<"\n"<<std::flush;
        continue;
      }
      if(parsed->model_id!=official_model) continue;
      std::lock_guard lock(mutex_);
      if (seen_.contains(parsed->stable_id)) continue;
      if (pending_.size() >= ai::max_pending_requests || seen_.size() >= 10000) {
        overflow_=true;
        return false;
      }
      seen_.insert(parsed->stable_id);
      pending_.push_back({tx,block.header().daascore(),parsed->stable_id,monotonic_ns()});
      std::cout << "{\"event\":\"AI_REQUEST_QUEUED\",\"request_id\":\""
                << parsed->stable_id << "\",\"daa\":" << block.header().daascore()
                << ",\"pending\":" << pending_.size() << "}\n" << std::flush;
    }
    return true;
  }
  std::optional<QueuedAiRequest> pop() {
    std::lock_guard lock(mutex_);
    if (pending_.empty()) return std::nullopt;
    auto item=std::move(pending_.front());pending_.pop_front();return item;
  }
  bool overflowed() const noexcept { std::lock_guard lock(mutex_);return overflow_; }
  std::size_t size() const noexcept { std::lock_guard lock(mutex_);return pending_.size(); }
private:
  mutable std::mutex mutex_;
  std::deque<QueuedAiRequest> pending_;
  std::unordered_set<std::string> seen_;
  bool overflow_{};
};

std::vector<std::uint8_t> decode_hex_secret(std::string_view value) {
  if(value.size()!=64) throw std::runtime_error("existing escrow key has invalid format");
  auto nibble=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
  std::vector<std::uint8_t> out(32);
  for(std::size_t i=0;i<32;++i){const int a=nibble(value[i*2]),b=nibble(value[i*2+1]);if(a<0||b<0)throw std::runtime_error("existing escrow key has invalid format");out[i]=static_cast<std::uint8_t>((a<<4)|b);}
  return out;
}
std::array<std::uint8_t,32> load_existing_escrow_secret(const fs::path &path) {
  struct stat st{};
  if(::stat(path.c_str(),&st)!=0||!S_ISREG(st.st_mode)||(st.st_mode&0077)!=0)
    throw std::runtime_error("existing escrow key is absent, non-regular, or has unsafe permissions");
  std::ifstream in(path);std::string text;in>>text;
  if(!in||text.size()!=64)throw std::runtime_error("existing escrow key file has invalid format");
  const auto decoded=decode_hex_secret(text);std::array<std::uint8_t,32> secret{};std::copy(decoded.begin(),decoded.end(),secret.begin());return secret;
}
std::array<std::uint8_t,64> delegation_from_extra(std::string_view extra) {
  const auto e=extra.find("/escrow:"),s=extra.find("/esig:");
  if(e==std::string_view::npos||s==std::string_view::npos||s!=e+8+64||extra.size()<s+6+128)
    throw std::runtime_error("escrow delegation certificate is missing or malformed");
  auto decoded=decode_hex_secret(extra.substr(s+6,64));
  auto decoded2=decode_hex_secret(extra.substr(s+6+64,64));
  std::array<std::uint8_t,64> out{};std::copy(decoded.begin(),decoded.end(),out.begin());std::copy(decoded2.begin(),decoded2.end(),out.begin()+32);return out;
}
struct PreparedNonce {
  std::uint64_t nonce{};
  std::uint64_t started_ns{};
  std::uint64_t seed{};
  CandidateBinding binding;
  ModelWalkInput input;
  Walk cpu;
  std::vector<std::uint64_t> seeds;
  std::vector<std::uint64_t> tile_ids;
  std::vector<std::uint32_t> slots;
  std::vector<std::uint8_t> packed;
  double gather_wall_ms{};
  double gather_cpu_ms{};
  double reference_cpu_ms{};
  double pack_cpu_ms{};
};
struct PrefetchCancelled final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void put(std::ostream &o, const void *p, std::size_t n) {
  o.write(static_cast<const char *>(p), static_cast<std::streamsize>(n));
  if (!o) throw std::runtime_error("live worker artifact write failed");
}
void u32(std::ostream &o, std::uint32_t x) {
  std::array<char,4> b{}; for (unsigned i=0;i<4;++i) b[i]=static_cast<char>(x>>(8*i)); put(o,b.data(),b.size());
}
void u64(std::ostream &o, std::uint64_t x) {
  std::array<char,8> b{}; for (unsigned i=0;i<8;++i) b[i]=static_cast<char>(x>>(8*i)); put(o,b.data(),b.size());
}
void hash(std::ostream &o, const Hash &h) { put(o,h.data(),h.size()); }
std::array<std::uint8_t,tile_bytes> read_tile(const model::ReadOnlyGguf &gguf, std::uint64_t offset) {
  std::array<std::uint8_t,tile_bytes> out{};gguf.read_tile_into(offset,out);return out;
}
std::string safe_name(std::uint64_t generation,std::uint64_t nonce) {
  return "live-generation-"+std::to_string(generation)+"-nonce-"+std::to_string(nonce);
}
void profile_phase(std::uint64_t generation,std::uint64_t nonce,std::string_view phase,
                   std::uint64_t start_ns,std::uint64_t end_ns,std::string_view detail={},
                   std::uint64_t cpu_duration_ns=0) {
  std::cout<<"{\"event\":\"LIVE_PHASE_PROFILE\",\"generation\":"<<generation
           <<",\"nonce\":"<<nonce<<",\"phase\":\""<<phase
           <<"\",\"start_monotonic_ns\":"<<start_ns
           <<",\"end_monotonic_ns\":"<<end_ns
           <<",\"duration_ms\":"<<(end_ns-start_ns)/1.0e6
           <<",\"thread_cpu_ms\":"<<cpu_duration_ns/1.0e6;
  if(!detail.empty())std::cout<<','<<detail;
  std::cout<<"}\n"<<std::flush;
}

std::string sha256_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read escrow import source");
  EVP_MD_CTX *raw = EVP_MD_CTX_new();
  if (!raw) throw std::runtime_error("cannot allocate escrow import digest context");
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(raw, EVP_MD_CTX_free);
  if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("cannot initialize escrow import digest");
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0 && EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
      throw std::runtime_error("cannot update escrow import digest");
  }
  if (!input.eof()) throw std::runtime_error("escrow import source read failed");
  std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1 || length != 32)
    throw std::runtime_error("cannot finish escrow import digest");
  return hex(digest.data(), length);
}

void copy_file_durable_new(const fs::path &source, const fs::path &destination,
                           std::string_view expected_sha256) {
  struct stat st{};
  if (::lstat(source.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
    throw std::runtime_error("escrow backup is not a regular file");
  if (sha256_file(source) != expected_sha256)
    throw std::runtime_error("escrow backup checksum differs from the read-only audit");
  const int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (in < 0) throw std::runtime_error("cannot open verified escrow backup");
  const auto importing = fs::path(destination.string() + ".importing");
  const int out = ::open(importing.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (out < 0) { const int saved = errno; ::close(in); errno = saved; throw std::runtime_error("cannot create NEXUS escrow import staging file without overwrite"); }
  bool ok = true;
  std::array<char, 64 * 1024> buffer{};
  for (;;) {
    const auto n = ::read(in, buffer.data(), buffer.size());
    if (n == 0) break;
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) { ok = false; break; }
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(n)) {
      const auto written = ::write(out, buffer.data() + offset, static_cast<std::size_t>(n) - offset);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) { ok = false; break; }
      offset += static_cast<std::size_t>(written);
    }
    if (!ok) break;
  }
  if (ok && ::fsync(out) != 0) ok = false;
  const int close_out = ::close(out);
  const int close_in = ::close(in);
  if (!ok || close_out != 0 || close_in != 0 || sha256_file(importing) != expected_sha256) {
    (void)::unlink(importing.c_str());
    throw std::runtime_error("durable escrow backup copy failed integrity verification");
  }
  if (::link(importing.c_str(), destination.c_str()) != 0) {
    const int saved = errno;
    (void)::unlink(importing.c_str());
    errno = saved;
    throw std::runtime_error("cannot atomically publish escrow import without overwriting existing state");
  }
  if (::unlink(importing.c_str()) != 0) throw std::runtime_error("cannot remove completed escrow import staging link");
  const int directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) throw std::runtime_error("cannot open escrow state directory for durability sync");
  const int sync_result = ::fsync(directory);
  const int close_result = ::close(directory);
  if (sync_result != 0 || close_result != 0) throw std::runtime_error("escrow state directory durability sync failed");
}

void backup_escrow_before_offchain(const fs::path &state_path, std::string_view block_hash) {
  const auto journal = fs::path(state_path.string() + ".journal");
  const auto backup_dir = state_path.parent_path() / "validation-backups";
  fs::create_directories(backup_dir);
  const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const auto stem = "pre-offchain-" + std::string(block_hash) + "-" + std::to_string(stamp);
  copy_file_durable_new(state_path, backup_dir / (stem + ".snapshot"), sha256_file(state_path));
  copy_file_durable_new(journal, backup_dir / (stem + ".journal"), sha256_file(journal));
  const int directory = ::open(backup_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory < 0) throw std::runtime_error("cannot open selected-chain escrow backup directory");
  const int sync_result = ::fsync(directory);
  const int close_result = ::close(directory);
  if (sync_result != 0 || close_result != 0)
    throw std::runtime_error("cannot durably sync selected-chain escrow backup directory");
}

void append_escrow_validation_audit(const fs::path &audit_path,
                                   const BlockChainStatus &status) {
  fs::create_directories(audit_path.parent_path());
  struct stat st{};
  if (::lstat(audit_path.c_str(), &st) == 0 &&
      (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode) || (st.st_mode & 0077) != 0))
    throw std::runtime_error("escrow validation audit identity or permissions are unsafe");
  const char *state = "PENDING";
  switch (status.state) {
    case BlockValidationState::Pending: state = "PENDING"; break;
    case BlockValidationState::ChainValid: state = "CHAIN_VALID"; break;
    case BlockValidationState::OffChain: state = "OFF_CHAIN"; break;
    case BlockValidationState::Unknown: state = "UNKNOWN"; break;
    case BlockValidationState::Error: state = "ERROR"; break;
  }
  const auto detail = status.error.substr(0, 1024);
  const auto line = std::to_string(monotonic_ns()) + "\t" + status.hash + "\t" + state + "\t" +
                    hex(reinterpret_cast<const std::uint8_t *>(detail.data()), detail.size()) + "\n";
  const int fd = ::open(audit_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("cannot open persistent escrow selected-chain audit");
  bool ok = true;
  std::size_t offset = 0;
  while (offset < line.size()) {
    const auto n = ::write(fd, line.data() + offset, line.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { ok = false; break; }
    offset += static_cast<std::size_t>(n);
  }
  if (ok && ::fsync(fd) != 0) ok = false;
  struct stat opened{};
  if (::fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) || (opened.st_mode & 0077) != 0) ok = false;
  const int close_result = ::close(fd);
  if (!ok || close_result != 0) throw std::runtime_error("persistent escrow selected-chain audit write failed");
}

fs::path prepare_nexus_escrow_state() {
#ifndef NEXUS_RECOVERY_ROOT
#error NEXUS_RECOVERY_ROOT must identify the persistent operational state root
#endif
  const fs::path root = fs::path(NEXUS_RECOVERY_ROOT);
  const auto state_dir = root / "state" / "ai-service";
  fs::create_directories(state_dir);
  const auto state = state_dir / "escrow_state.json";
  const auto journal = state_dir / "escrow_state.journal";
  const bool has_state = fs::exists(state);
  const bool has_journal = fs::exists(journal);
  if (has_state != has_journal)
    throw std::runtime_error("NEXUS escrow state is incomplete; refusing to overwrite or reconstruct either file");
  if (!has_state) {
    const auto backup = root / "artifacts" / "escrow-backups" / "20261011";
    copy_file_durable_new(backup / "stock-state.json", state,
        "c890191b98ad5f0bf6e63fd974ad2e0c2d0eb1ee4347f49d8e8bb43447cd5f3e");
    try {
      copy_file_durable_new(backup / "stock-state.journal", journal,
          "516db87538ba0b2d226946f556b2a6b00f7defee27660c31ae649113136cfa9b");
    } catch (...) { (void)::unlink(state.c_str()); throw; }
    std::cout << "ESCROW_IMPORT=PASS source=verified_read_only_backup destination=nexus_private_state originals_modified=false\n" << std::flush;
  }
  if (sha256_file(state).empty() || sha256_file(journal).empty())
    throw std::runtime_error("NEXUS escrow snapshot/journal failed readable integrity check");
  struct stat state_stat{}, journal_stat{};
  if (::lstat(state.c_str(), &state_stat) != 0 || !S_ISREG(state_stat.st_mode) || S_ISLNK(state_stat.st_mode) ||
      (state_stat.st_mode & 0077) != 0 || ::lstat(journal.c_str(), &journal_stat) != 0 ||
      !S_ISREG(journal_stat.st_mode) || S_ISLNK(journal_stat.st_mode) || (journal_stat.st_mode & 0077) != 0)
    throw std::runtime_error("NEXUS escrow state permissions or file identity are unsafe");
  return state;
}

std::array<std::uint8_t, 32> parse_hash32(std::string_view text) {
  const auto bytes = decode_hex_secret(text);
  std::array<std::uint8_t, 32> result{};
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}
std::unique_ptr<opoi::StockLlamaRuntime> initialize_inference(
    StartupReadiness &startup, const fs::path &model_path,bool run_self_test=true) {
    const char *library_env = std::getenv("NEXUS_OPOI_LIBRARY");
    if (!library_env || !*library_env)
      throw std::runtime_error("NEXUS_OPOI_LIBRARY is required for inference readiness");
    opoi::Challenge self_test_challenge = opoi::parse_challenge(
        "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710:0011aaff");
    std::string self_test_wire;
    auto shared_runtime=std::make_unique<opoi::StockLlamaRuntime>(library_env,model_path.string());
    {
      auto &runtime=*shared_runtime;
      startup.record("CUDA_ENGINE_READY", StartupResult::Pass,
                     "stock llama ABI, authorized CUDA device selection and official GGUF loaded");
      if(!run_self_test)return shared_runtime; // The selected AMD backend must pass INFERENCE_SELF_TEST next.
      const auto answer = runtime.answer(self_test_challenge);
      self_test_wire = opoi::format_inference_result(self_test_challenge, answer);
      const auto prefix = self_test_challenge.model_id_hex + ":" + self_test_challenge.nonce_hex + ":";
      const bool has_visible_text = std::any_of(answer.begin(), answer.end(), [](unsigned char c) {
        return c > 0x20U;
      });
      if (!has_visible_text || self_test_wire.rfind(prefix, 0) != 0 || self_test_wire.size() <= prefix.size())
        throw std::runtime_error("local inference self-test produced an invalid protocol response");
      startup.record("INFERENCE_SELF_TEST", StartupResult::Pass,
                     "real stock CUDA fixture inference completed and protocol response validated");
    }
    std::cout << "INFERENCE_SELF_TEST=PASS source=local_fixture response_bytes="
              << self_test_wire.size() << " submitted=false cuda_runtime_retained_for_shared_resident_model=true\n" << std::flush;
    return shared_runtime;
}

std::future<void> start_chain_observation(network::SubmitBlockClient &client,
    network::SubmitBlockResult result) {
    return std::async(std::launch::async,[&client,result]{try {
                const auto chain=client.observe_chain(result,std::chrono::seconds(30),
                    std::chrono::seconds(2),6);
                std::osyncstream(std::cout)<<"LIVE_CHAIN_STATUS state="<<network::submission_state_name(chain.state)
                         <<" hash="<<chain.block_hash
                         <<" selected_chain="<<(chain.currently_selected_chain?"true":"false")
                         <<" blue_score="<<chain.block_blue_score
                         <<" selected_tip_blue_score="<<chain.selected_tip_blue_score
                         <<" reorg_observed="<<(chain.reorg_observed?"true":"false")
                         <<" confirmation_policy=6_blue_score_depth_not_finality\n"<<std::flush;
                  }catch(const std::exception &e){std::osyncstream(std::cout)<<"LIVE_CHAIN_OBSERVATION=UNKNOWN hash="<<result.block_hash<<" detail="<<e.what()<<"\n"<<std::flush;}});
}

void request_worker_stop(std::atomic_bool &stop, GpuPreemptionCoordinator &gpu,
    LiveJobQueue &queue) {
    stop.store(true,std::memory_order_release);
    gpu.request_stop();
    queue.close();
}

}

int main(int argc,char **argv) {
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
  if(argc==3&&std::string_view(argv[1])=="--probe-bda-route"){
    try{probe_bda_route(argv[2]);std::cout<<"BDA_ROUTE_CAPABILITY=PASS\n";return 0;}
    catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 13;}
  }
#endif
  if(argc==2&&std::string_view(argv[1])=="--inference-capabilities"){
    std::cout<<"{\"schema_version\":1,\"default\":\"cuda\",\"vulkan_helper\":"
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
      <<"true"
#else
      <<"false"
#endif
      <<",\"primary_vulkan\":"
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
      <<"true"
#else
      <<"false"
#endif
      <<",\"primary_vulkan_bda\":"
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
      <<"true"
#else
      <<"false"
#endif
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
      <<",\"independent_vulkan_lanes\":true,\"secondary_vulkan_bda\":true"
#else
      <<",\"independent_vulkan_lanes\":false,\"secondary_vulkan_bda\":false"
#endif
      <<",\"trace_capture\":false,\"activation_override\":false}\n";
    return 0;
  }
  network::GrpcRuntime grpc_runtime;
  std::signal(SIGINT,request_stop);
  std::signal(SIGTERM,request_stop);
  auto &stop=process_stop;
  LiveJobQueue queue;
  std::unique_ptr<ScopedFileCacheGuard> memory_guard;
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
  // The combined build reserves both lanes from this one owner. Secondary
  // admission remains disabled until its live lifecycle is wired and tested.
  GpuWorkCoordinator gpu_work(queue,first_nonce,65536,secondary_vulkan_batch_size);
#endif
  AiRequestQueue ai_requests;
  GpuPreemptionCoordinator gpu_ownership;
  std::thread ingress;
  std::thread shutdown_watcher;
  std::unique_ptr<ai::EscrowWatcher> escrow_watcher;
  std::unique_ptr<ai::StrikeMonitor> strike_monitor;
  bool submit_enabled=false;
  bool ai_responses_enabled=false;
  bool escrow_claims_enabled=false;
  std::future<void> escrow_claim_task;
  std::atomic_uint64_t next_claim_check_ns{0};
  fs::path claim_journal;
  fs::path claim_cadence_file;
  std::uint64_t last_claim_check_unix=0;
  std::mutex claim_ids_mutex;
  std::set<std::string> own_claim_ids;
  NativePomTelemetry telemetry(monotonic_ns());
  std::shared_ptr<LiveGpuSelection> live_selection;
  std::thread telemetry_thread;
  bool telemetry_started=false;
  StartupReadiness startup([](std::string_view event) { std::cout << event << '\n' << std::flush; });
  try {
    if(const char *enabled=std::getenv("NEXUS_BOUND_FILE_CACHE")) {
      if(std::string_view(enabled)=="1")memory_guard=std::make_unique<ScopedFileCacheGuard>();
      else if(std::string_view(enabled)!="0")throw std::runtime_error("NEXUS_BOUND_FILE_CACHE must be 0 or 1");
    }

    startup.record("BOOT", StartupResult::Pass, "worker process entered startup gate");
    const auto config = parse_live_worker_config(argc, argv,
        std::getenv("CUDA_VISIBLE_DEVICES"), gpu_uuid,
        std::getenv("NEXUS_POM_BACKEND"), std::getenv("NEXUS_POM_DEVICE_ID"),
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        true
#else
        false
#endif
    );
    const bool is_bda_vulkan=config.gpu_backend==GpuBackend::VulkanBda;
    const bool primary_vulkan=config.gpu_backend==GpuBackend::VulkanJit || is_bda_vulkan;
    telemetry.set_pom_backend(is_bda_vulkan?"vulkan-bda":(primary_vulkan?"vulkan-jit":"cuda-driver"));
    const auto &endpoint=config.endpoint, &pay=config.pay, &extra=config.extra;
    const auto &model_path=config.model, &tree_path=config.tree, &meta_path=config.metadata;
    const auto &oracle=config.oracle, &outdir=config.output;
    const auto nonce_limit=config.nonce_limit, microbatch=config.microbatch;
    const auto submit_policy=config.policy;
    submit_enabled=submit_policy.enabled();
    ai_responses_enabled=submit_policy.ai_responses_enabled();
    escrow_claims_enabled=config.escrow_claims;
    const bool primary_pom_enabled = primary_pom_selection(std::getenv("NEXUS_PRIMARY_POM_ENABLED"));
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
    const auto secondary_config=secondary_vulkan_config(std::getenv("NEXUS_SECONDARY_VULKAN_MODE"),
      std::getenv("NEXUS_SECONDARY_VULKAN_MODULE"),std::getenv("NEXUS_SECONDARY_VULKAN_SHA256"),submit_enabled);
    const auto primary_vulkan_config=primary_vulkan?
      (is_bda_vulkan?
        secondary_vulkan_config("resident-live","bda-native","bda-native",submit_enabled):
        secondary_vulkan_config("resident-live",std::getenv("NEXUS_PRIMARY_VULKAN_MODULE"),
          std::getenv("NEXUS_PRIMARY_VULKAN_SHA256"),submit_enabled)):std::nullopt;
#endif
    const auto vulkan_ai_config=opoi::ai_backend_config(std::getenv("NEXUS_AI_BACKEND"),
        std::getenv("NEXUS_VULKAN_AI_HELPER"),std::getenv("NEXUS_VULKAN_AI_HELPER_SHA256"),
        std::getenv("NEXUS_VULKAN_AI_LIBRARY"),std::getenv("NEXUS_VULKAN_AI_LIBRARY_SHA256"),
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        primary_vulkan||(secondary_config&&secondary_config->resident)
#else
        false
#endif
    );
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
    validate_primary_vulkan_policy(primary_vulkan,primary_pom_enabled,bool(secondary_config),bool(vulkan_ai_config),
        std::getenv("NEXUS_INDEPENDENT_VULKAN_LANES") && std::string_view(std::getenv("NEXUS_INDEPENDENT_VULKAN_LANES"))=="1");
#endif
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
    const bool secondary_pom_enabled=bool(secondary_config)&&primary_pom_selection(std::getenv("NEXUS_SECONDARY_POM_ENABLED"));
#endif
    live_selection=std::make_shared<LiveGpuSelection>(std::getenv("NEXUS_GPU_SELECTION_FILE"),
        primary_pom_enabled,
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        secondary_pom_enabled, primary_pom_enabled||!vulkan_ai_config, bool(secondary_config)
#else
        false, primary_pom_enabled||!vulkan_ai_config, false
#endif
    );
    const auto escrow_pos=extra.find("/escrow:");
    RustProofVerifier rust_verifier(oracle.string());
    startup.record("CONFIG_VALIDATION", StartupResult::Pass,
                   "arguments, explicit submit policy, RTX UUID, payout and escrow syntax validated; secrets redacted");

    const auto node = probe_node_readiness(endpoint, std::chrono::seconds(10));
    if (node.network != "mainnet" || !node.synced) {
      startup.record("NODE_SYNC", StartupResult::Blocked,
                     "GetCurrentNetwork/GetInfo did not report synced mainnet");
      return 78;
    }
    // The pinned official mainnet rules have H15 disabled. A node upgrade must
    // be reviewed against its actual activation rules before this workflow can
    // advertise service; text-only Vulkan generation is not canonical tracing.
    if(primary_vulkan&&node.version!="1.6.4")
      throw std::runtime_error("Vulkan AI requires reviewed mainnet protocol rules for the node version; no CUDA fallback");
    startup.record("NODE_SYNC", StartupResult::Pass,
                   "bounded GetCurrentNetwork/GetInfo reports synced mainnet; version=" + node.version);

    if (!probe_local_ipfs_api(std::chrono::seconds(2))) {
      startup.record("IPFS_READY", StartupResult::Blocked,
                     "local Kubo /api/v0/version did not return HTTP 200; daemon was not started or changed");
      return 78;
    }
    startup.record("IPFS_READY", StartupResult::Pass,
                   "local Kubo HTTP API responded; no daemon process was started or changed");

    fs::create_directories(outdir);
    model::ReadOnlyGguf gguf(model_path);
    const auto sidecar=gguf.verify_sidecar(tree_path,meta_path,official_model,official_root,203469888);
    if(sidecar.declared_root!=official_root) throw std::runtime_error("GGUF sidecar root mismatch");
    startup.record("MODEL_VERIFIED", StartupResult::Pass,
                   "GGUF structure, tier model ID, sidecar checksum, chunk count and official Merkle root verified; stock loader also checks full-file SHA-256");

    std::unique_ptr<opoi::StockLlamaRuntime> shared_runtime;
    if (!primary_vulkan && (primary_pom_enabled || !vulkan_ai_config))
      shared_runtime=initialize_inference(startup,model_path,!vulkan_ai_config);
    else
      startup.record("CUDA_ENGINE_READY", StartupResult::Pass,
          "selected Vulkan workflow: no CUDA model/context created; Vulkan inference must pass real self-test next");
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
    std::unique_ptr<SecondaryDeviceLease> secondary_device_lease;
    std::unique_ptr<SecondaryDeviceLease> primary_vulkan_lease;
    std::unique_ptr<AmdMemoryHandoff> amd_memory_handoff;
    std::unique_ptr<opoi::VulkanInferenceProcess> vulkan_ai_process;
    if(vulkan_ai_config){
      if(primary_vulkan)primary_vulkan_lease=std::make_unique<SecondaryDeviceLease>(NEXUS_RECOVERY_ROOT,true);
      else {
        secondary_device_lease=std::make_unique<SecondaryDeviceLease>(NEXUS_RECOVERY_ROOT);
        amd_memory_handoff=std::make_unique<AmdMemoryHandoff>();
      }
      if(bounded_file_sha256(vulkan_ai_config->library)!=vulkan_ai_config->library_sha)
        throw std::runtime_error("Vulkan AI library identity differs from pinned configuration");
      vulkan_ai_process=std::make_unique<opoi::VulkanInferenceProcess>(vulkan_ai_config->helper,
          vulkan_ai_config->helper_sha,vulkan_ai_config->library,vulkan_ai_config->library_sha,
          model_path.string(),primary_vulkan?primary_vulkan_lease->descriptor():secondary_device_lease->descriptor());
      const auto challenge=opoi::parse_challenge(
          "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710:0011aaff");
      const auto answer=vulkan_ai_process->infer(opoi::official_qwen35_prompt(challenge.nonce_hex),64,
          [&]{return stop.load(std::memory_order_relaxed);});
      (void)opoi::format_inference_result(challenge,answer);
      if(amd_memory_handoff)amd_memory_handoff->wait_released([&]{return stop.load(std::memory_order_relaxed);});
      startup.record("INFERENCE_SELF_TEST",StartupResult::Pass,
          "real selected Vulkan Qwen self-test completed under the exclusive device lease; no transaction sent");
      std::cout<<"VULKAN_AI_SELF_TEST=PASS official_model=true submitted=false\n"<<std::flush;
      std::cout<<"AI_BACKEND=VULKAN device="<<(primary_vulkan?std::string(gpu_uuid):"" NEXUS_SECONDARY_GPU_ID "")<<" isolated_process=true trace_capture=false\n"<<std::flush;
    }
#endif
    std::cout<<"{\"event\":\"AI_BACKEND_SELECTED\",\"schema_version\":1,\"backend\":\""
             <<(vulkan_ai_config?"vulkan":"cuda")<<"\",\"device_id\":\""
             <<(vulkan_ai_config&&!primary_vulkan?std::string_view("" NEXUS_SECONDARY_GPU_ID ""):gpu_uuid)
             <<"\",\"trace_capture\":false}\n"<<std::flush;
    // Build the actual AiRequest/AiResponse runtime before admitting PoM. The existing escrow
    // key is read-only; this path never creates or rotates key material.
    ai::ServiceRpcSnapshot service_rpc;
    std::optional<std::string> service_identity;
    std::string service_identity_error;
    service_identity = ai::service_identity_from_payout(pay, &service_identity_error);
    const auto escrow_key = extra.substr(escrow_pos + 8, 64);
    const char *key_env=std::getenv("NEXUS_ESCROW_KEY_FILE");
    const fs::path escrow_path=key_env&&*key_env?fs::path(key_env):fs::path(NEXUS_RECOVERY_ROOT)/"state/setup/escrow.key";
    const char *ipfs_env=std::getenv("NEXUS_IPFS_API_URL");
    const std::string ipfs_api=ipfs_env&&*ipfs_env?ipfs_env:"http://127.0.0.1:5001";
    std::unique_ptr<ai::AiResponseService> ai_response_service;
    ai::AiResponseTransport ai_response_transport(endpoint);
    try {
      // Omitted windowDaa asks the node for its current exact service-eligibility window;
      // widening this to the API maximum would admit miners who are not in the active cohort.
      service_rpc = probe_ai_service_rpc(endpoint, 0,
          service_identity ? std::string_view(*service_identity) : std::string_view{},
          std::chrono::seconds(5), ServiceRpcQuery::ProvidersOnly);
      const auto listing = ai::provider_listing_status(service_rpc, 0,
          hex(official_model.data(), official_model.size()),
          service_identity ? std::string_view(*service_identity) : std::string_view{}, escrow_key);
      const char *listing_text = listing == ai::ProviderListingStatus::Listed ? "LISTED" :
          listing == ai::ProviderListingStatus::NotListed ? "NOT_LISTED" : "UNKNOWN";
      std::cout << "{\"event\":\"AI_SERVICE_RPC_SNAPSHOT\",\"providers_available\":"
                << (service_rpc.providers_available ? "true" : "false")
                << ",\"providers\":" << service_rpc.providers.size()
                << ",\"provider_listing\":\"" << listing_text << "\""
                << ",\"identity\":\"" << (service_identity ? *service_identity : "UNKNOWN") << "\""
                << ",\"strikes_state\":\""
                << (service_rpc.strikes_available ? "AVAILABLE" : "UNKNOWN") << "\""
                << ",\"consecutive\":" << (service_rpc.strikes.consecutive
                    ? std::to_string(*service_rpc.strikes.consecutive) : "null")
                << ",\"lifetime\":" << (service_rpc.strikes.lifetime
                    ? std::to_string(*service_rpc.strikes.lifetime) : "null")
                << ",\"pending_burns\":"
                << (service_rpc.strikes_available ? std::to_string(service_rpc.strikes.pending_burns) : "null")
                << ",\"suspended_until_daa\":" << (service_rpc.strikes.suspended_until_daa
                    ? std::to_string(*service_rpc.strikes.suspended_until_daa) : "null") << "}\n" << std::flush;
      if (listing == ai::ProviderListingStatus::Unknown)
        throw std::runtime_error("authoritative GetServiceProviders snapshot unavailable: " + service_rpc.providers_error);
      std::cout << "PROVIDER_LISTED=" << listing_text
                << " meaning=current selected-chain cohort; AiRequest assignment remains cohort-gated\n" << std::flush;
    } catch (const std::exception &e) {
      // Cohort discovery precedes escrow readiness. Report failure at the
      // current gate so the original RPC error is preserved, without skipping
      // the identity gate or admitting mining with an unknown cohort.
      startup.record("ESCROW_IDENTITY_READY", StartupResult::Blocked,
                     std::string("cannot obtain authoritative provider cohort for request assignment: ")+e.what());
      return 78;
    }
    std::array<std::uint8_t,32> escrow_secret{};
    std::array<std::uint8_t,32> escrow_pubkey{};
    std::array<std::uint8_t,64> delegation{};
    try {
      const auto secret=load_existing_escrow_secret(escrow_path);escrow_secret=secret;
      escrow_pubkey=ai::stock_escrow_pubkey_from_secret(escrow_secret);
      if(hex(escrow_pubkey.data(),escrow_pubkey.size())!=escrow_key)
        throw std::runtime_error("existing escrow secret does not match configured escrow public key");
      delegation=delegation_from_extra(extra);
      if(!ai::stock_verify_escrow_delegation(pay,escrow_pubkey,delegation))
        throw std::runtime_error("stock Schnorr escrow delegation verification failed");
      if(!service_identity) throw std::runtime_error("payout service identity unavailable: "+service_identity_error);
    } catch (const std::exception &e) {
      startup.record("ESCROW_IDENTITY_READY", StartupResult::Blocked,
                     std::string("existing escrow identity/delegation validation failed: ")+e.what());
      return 78;
    }
    startup.record("ESCROW_IDENTITY_READY", StartupResult::Pass,
                   "existing escrow secret matches configured x-only key; official payout delegation signature verified");
    telemetry.set_wallet(pay, hex(escrow_pubkey.data(), escrow_pubkey.size()),
                         *service_identity, true);
    startup.record("AI_REQUEST_SERVICE_READY", StartupResult::Pass,
                   "official block subscription/backfill feeds bounded AiRequest queue; provider cohort is authoritative for request assignment but current non-membership does not block first PoM; private opening uses stock bridge");
    if(ipfs_api.rfind("http://127.0.0.1:",0)!=0) {
      startup.record("AI_RESPONSE_TRANSPORT_READY", StartupResult::Blocked,
                     "public response publisher must use explicitly configured local IPFS API");
      return 78;
    }
    ai::AiResponseConfig ai_config;ai_config.escrow_secret=escrow_secret;ai_config.escrow_pubkey=escrow_pubkey;
    ai_config.delegation_signature=delegation;ai_config.model_id=official_model;ai_config.payout_address=pay;
    ai_config.service_identity=*service_identity;ai_config.network="mainnet";ai_config.tier=0;
    ai_config.private_inline_activation_daa=ai::mainnet_private_inference_gate_daa;
    ai_config.signed_response_activation_daa=76'316'623ULL;
    // Keep response intent/reconciliation state across candidate restarts. The session
    // output directory is timestamped and therefore cannot serve as a recovery journal.
    ai_config.journal_path=fs::path(NEXUS_RECOVERY_ROOT)/"state"/"ai-service"/"ai-response.journal";
    try {ai_response_service=std::make_unique<ai::AiResponseService>(std::move(ai_config));}
    catch(const std::exception&e){startup.record("AI_RESPONSE_TRANSPORT_READY",StartupResult::Blocked,
        std::string("AiResponse production service initialization failed: ")+e.what());return 78;}
    startup.record("AI_RESPONSE_TRANSPORT_READY", StartupResult::Pass,
                   "official signed AiResponse serialization, local IPFS add/readback, SubmitTransaction and mempool reconciliation adapters configured; no transaction sent during startup");
    try {ai_response_service->recover();}
    catch(const std::exception&e){startup.record("PENDING_REQUEST_RECOVERY",StartupResult::Blocked,
        std::string("durable AiResponse journal recovery failed closed: ")+e.what());return 78;}
    startup.record("PENDING_REQUEST_RECOVERY", StartupResult::Pass,
                   "checksummed response journal recovered; gRPC startup block backfill will rediscover requests and ambiguous submissions are reconciled before any retry");
    try {
      const auto state_path = prepare_nexus_escrow_state();
      claim_journal=state_path.parent_path()/"claim-submissions.jsonl";
      claim_cadence_file=state_path.parent_path()/"claim-check-unix.sec";
      if(escrow_claims_enabled&&fs::exists(claim_cadence_file)) {
        std::ifstream cadence(claim_cadence_file);std::string trailing;
        if(!(cadence>>last_claim_check_unix)||(cadence>>trailing)||last_claim_check_unix>253402300799ULL)
          throw std::runtime_error("durable claim cadence record invalid");
      }
      if(escrow_claims_enabled&&fs::exists(claim_journal)) {
        std::ifstream prior_claims(claim_journal);std::string line;
        const std::regex id_pattern("\"txid\":\"([0-9a-f]{64})\"");std::smatch match;
        while(std::getline(prior_claims,line))if(std::regex_search(line,match,id_pattern)) {
          if(own_claim_ids.size()>=10000&&!own_claim_ids.contains(match[1].str()))throw std::runtime_error("claim reconciliation history exceeds bounded capacity");
          own_claim_ids.insert(match[1].str());
        }
      }
      escrow_watcher = std::make_unique<ai::EscrowWatcher>(escrow_secret, pay, state_path);
      escrow_watcher->set_claim_construction_enabled(false);
      startup.record("ESCROW_STATE_RECOVERED", StartupResult::Pass,
                     "official stock snapshot/journal loaded in separate NEXUS state; source files unchanged; claim construction disabled");
      const auto hashes = escrow_watcher->start_state_validation();
      const auto escrow_audit = state_path.parent_path() / "selected-chain-validation.tsv";
      std::vector<std::string> encoded;
      encoded.reserve(hashes.size());
      for (const auto &hash : hashes) encoded.push_back(hex(hash.data(), hash.size()));
      std::size_t selected = 0, orphaned = 0, unknown = 0, rpc_errors = 0;
      if (!encoded.empty()) {
        const auto statuses = probe_blocks_selected_chain(endpoint, encoded, std::chrono::seconds(120));
        if (statuses.size() != hashes.size())
          throw std::runtime_error("GetBlock selected-chain validation result count mismatch");
        for (std::size_t i = 0; i < hashes.size(); ++i) {
          const auto &status = statuses[i];
          if (status.hash != encoded[i])
            throw std::runtime_error("selected-chain validation response correlation mismatch");
          switch (status.state) {
            case BlockValidationState::ChainValid:
              if (status.selected_chain != std::optional<bool>(true))
                throw std::runtime_error("CHAIN_VALID result lacks explicit isChainBlock=true");
              append_escrow_validation_audit(escrow_audit, status);
              ++selected;
              if (!escrow_watcher->validation_result(hashes[i], true))
                throw std::runtime_error("stock escrow watcher did not consume CHAIN_VALID result");
              break;
            case BlockValidationState::OffChain:
              if (status.selected_chain != std::optional<bool>(false))
                throw std::runtime_error("OFF_CHAIN result lacks explicit isChainBlock=false");
              // The stock transition permanently slashes ghost entries. Preserve the exact
              // durable state pair before applying that irreversible official transition.
              backup_escrow_before_offchain(state_path, encoded[i]);
              append_escrow_validation_audit(escrow_audit, status);
              ++orphaned;
              if (!escrow_watcher->validation_result(hashes[i], false))
                throw std::runtime_error("stock escrow watcher did not consume OFF_CHAIN result");
              break;
            case BlockValidationState::Unknown:
            case BlockValidationState::Error:
            case BlockValidationState::Pending: {
              append_escrow_validation_audit(escrow_audit, status);
              if (status.state == BlockValidationState::Error) ++rpc_errors;
              ++unknown;
              // Stock consumes the validation slot but preserves the entry. Its claim path
              // now separately quarantines this hash until a later authoritative verdict.
              const auto detail = "GetBlock selected-chain " + std::string(
                  status.state == BlockValidationState::Error ? "ERROR" :
                  status.state == BlockValidationState::Pending ? "PENDING" : "UNKNOWN") +
                  " for " + encoded[i] + ": " + status.error;
              if (!escrow_watcher->validation_error(detail))
                throw std::runtime_error("stock escrow watcher did not consume inconclusive block status");
              break;
            }
          }
        }
      }
      escrow_watcher->flush();
      std::cout << "ESCROW_CHAIN_VALIDATION selected=" << selected << " orphaned=" << orphaned
                << " unknown=" << unknown << " rpc_errors=" << rpc_errors
                << " claim_submission=DISABLED unknown_claim_eligibility=DENIED audit="
                << escrow_audit.string() << "\n" << std::flush;
      if (escrow_watcher->validation_in_progress()) {
        startup.record("ESCROW_VALIDATION_COMPLETE", StartupResult::Blocked,
                       "bounded GetBlock validation left a request pending; service cannot assume completion");
        return 78;
      }
      startup.record("ESCROW_VALIDATION_COMPLETE", StartupResult::Pass,
                     "selected-chain verdicts applied; OFF_CHAIN entries backed up before official purge; UNKNOWN/ERROR entries retained and quarantined from claim selection");
      startup.record("ESCROW_CLAIM_ENGINE_READY", StartupResult::Pass,
                     std::string(escrow_claims_enabled ? "stock claims explicitly authorized; checks every 1800 seconds after complete backfill; stock maturity, fee and batching retained" : "stock claim engine is durable and disabled for network effects") + "; unknown historical outpoints are individually ineligible" +
                       (unknown == 0 ? std::string{} : "; unresolved=" + std::to_string(unknown) +
                        " rpc_errors=" + std::to_string(rpc_errors)));
    } catch (const std::exception &e) {
      startup.record("ESCROW_STATE_RECOVERED", StartupResult::Blocked,
                     std::string("stock escrow recovery/selected-chain validation failed closed: ") + e.what());
      return 78;
    }

    try {
      strike_monitor = std::make_unique<ai::StrikeMonitor>(*service_identity,
          fs::path(NEXUS_RECOVERY_ROOT) / "state" / "ai-service" / "strikes.jsonl",
          [endpoint, identity = *service_identity] {
            const auto rpc = probe_ai_service_rpc(endpoint, 0, identity, std::chrono::seconds(5),
                                                   ServiceRpcQuery::StrikesOnly);
            auto observed = rpc.strikes;
            if (observed.identity.empty()) observed.identity = identity;
            if (!rpc.strikes_available && observed.error.empty()) observed.error = "GetServiceStrikes unavailable";
            return observed;
          }, [](const ai::StrikeSnapshot &s) {
            std::cout << "{\"event\":\"SERVICE_STRIKE_POLL\",\"identity\":\"" << s.identity
                      << "\",\"availability\":\""
                      << (s.availability == ai::StrikeSnapshot::Availability::Available ? "AVAILABLE" : "UNKNOWN")
                      << "\",\"stale\":" << (s.stale ? "true" : "false")
                      << ",\"consecutive\":" << (s.consecutive ? std::to_string(*s.consecutive) : "null")
                      << ",\"lifetime\":" << (s.lifetime ? std::to_string(*s.lifetime) : "null")
                      << ",\"pending_burns\":"
                      << (s.availability == ai::StrikeSnapshot::Availability::Available ? std::to_string(s.pending_burns) : "null")
                      << ",\"suspended_until_daa\":"
                      << (s.suspended_until_daa ? std::to_string(*s.suspended_until_daa) : "null")
                      << ",\"observed_unix_ms\":" << s.observed_unix_ms << "}\n" << std::flush;
          }, std::chrono::seconds(60));
      strike_monitor->start();
      startup.record("STRIKE_MONITOR_STATUS", StartupResult::Pass,
                     "periodic identity-scoped GetServiceStrikes polling active; no unavailable value is converted to zero");
    } catch (const std::exception &e) {
      startup.record("STRIKE_MONITOR_STATUS", StartupResult::Degraded,
                     std::string("strike telemetry thread unavailable; value remains UNKNOWN: ") + e.what());
    }

    startup.record("SERVICE_READY", StartupResult::Pass,
                   "functional node/model/inference/AiRequest/AiResponse/journal/escrow recovery gates passed; unknown outpoints remain claim-quarantined; strike telemetry degradation does not disable AI service capability; production transaction modes remain governed by explicit command policy");
    if (!startup.service_ready()) {
      std::cerr << "MINING_GATE=BLOCKED reason=AI_SERVICE_INCOMPLETE submission_disabled_by_startup=true\n" << std::flush;
      return 78;
    }

    const auto tile_count=gguf.chunk_count()/32;
    model::SparseMerkleIndex merkle(gguf,tree_path);
    std::unique_ptr<CudaWalkEngine> engine;
    std::unique_ptr<CudaResidentSearch> resident;
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
    std::unique_ptr<PrimaryVulkanWorkload> primary_vulkan_workload;
    if(primary_vulkan)primary_vulkan_workload=std::make_unique<PrimaryVulkanWorkload>(std::string(gpu_uuid),
        primary_vulkan_config->module,primary_vulkan_config->sha256,gpu_work,gguf,merkle,rust_verifier,pay,extra);
#endif
    std::atomic_uint64_t cuda_engine_initializations{0};
    std::atomic_bool startup_backfill_done{false}, startup_backfill_complete{false}, ingress_failed{false};
    std::atomic_size_t startup_backfill_hashes{0}, startup_backfill_unavailable{0};
    const auto model_identity=hex(official_model.data(),official_model.size())+hex(official_root.data(),official_root.size());
    // Submission authority comes only from the parsed command-line mode and
    // explicit flag. Ambient environment variables are never authorization.
    // Shadow has no SubmitBlock client instance or submission call path.
    // Live constructs one gate/client for the worker lifetime so ambiguous
    // outcomes cannot be retried as duplicates in this process.
    std::unique_ptr<network::SubmitBlockClient> submit_client;
    if(submit_enabled) submit_client=std::make_unique<network::SubmitBlockClient>(endpoint);
    std::vector<std::future<void>> chain_observers; // bounded read-only RPC observers; never own CUDA
    std::mutex chain_observers_mutex;
    const auto observe_submission=[&](const network::SubmitBlockResult &result){
      if(result.block_hash.empty() || (result.state!=network::SubmissionState::RpcAcknowledged &&
         result.state!=network::SubmissionState::UnknownOutcome && result.state!=network::SubmissionState::UnknownAfterTimeout))return;
      std::lock_guard lock(chain_observers_mutex);
      std::erase_if(chain_observers,[](auto &f){if(f.wait_for(std::chrono::seconds(0))==std::future_status::ready){f.get();return true;}return false;});
      if(chain_observers.size()<4)chain_observers.push_back(start_chain_observation(*submit_client,result));
      else std::osyncstream(std::cout)<<"LIVE_CHAIN_OBSERVATION=PENDING hash="<<result.block_hash<<" reason=bounded_observer_capacity evidence=persistent_submit_record\n";
    };
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
    // Separate bounded Merkle cache and sequential verifier, shared pread-only
    // model. Declaration order keeps these alive until the lane has joined.
    std::unique_ptr<consensus::VulkanWalkEngine> secondary_engine;
    std::unique_ptr<model::SparseMerkleIndex> secondary_merkle;
    std::unique_ptr<RustProofVerifier> secondary_verifier;
    std::unique_ptr<AmdCandidatePipeline> secondary_pipeline;
    std::unique_ptr<SecondaryWorkLane> secondary_lane;
    std::atomic_bool secondary_batch_ready{false}; // Executor writes; readiness telemetry reads.
    if(secondary_config) {
      if(!secondary_device_lease)secondary_device_lease=std::make_unique<SecondaryDeviceLease>(NEXUS_RECOVERY_ROOT);
      if(secondary_pom_enabled||!secondary_config->resident)
        secondary_engine=std::make_unique<consensus::VulkanWalkEngine>("" NEXUS_SECONDARY_GPU_ID "",secondary_config->module,secondary_config->sha256,secondary_vulkan_batch_size,
            secondary_config->resident?gguf.chunk_count()/32:0);
      secondary_merkle=std::make_unique<model::SparseMerkleIndex>(gguf,tree_path);
      secondary_verifier=std::make_unique<RustProofVerifier>(oracle.string());
      if(secondary_engine)secondary_pipeline=std::make_unique<AmdCandidatePipeline>(gpu_work,*secondary_engine,gguf,*secondary_merkle,*secondary_verifier,pay,extra);
      secondary_lane=std::make_unique<SecondaryWorkLane>(gpu_work,[&](auto ticket,const auto &cancelled,const auto &commit){
        const auto selection_token=live_selection->token(LiveGpuSelection::Amd);
        if(!live_selection->allows(LiveGpuSelection::Amd,selection_token))return false;
        auto candidate=secondary_pipeline->execute(std::move(ticket),[&]{return stop.load(std::memory_order_relaxed)||cancelled()||!live_selection->allows(LiveGpuSelection::Amd,selection_token);});
        if(!secondary_batch_ready&&secondary_pipeline->startup_parity_verified()) {
          secondary_lane->set_batch_size(secondary_vulkan_batch_size);
          secondary_batch_ready=true;
          std::osyncstream(std::cout)<<"AMD_BATCH_READY batch="<<secondary_vulkan_batch_size<<" startup_rust_parity=PASS\n"<<std::flush;
        }
        if(!candidate||!live_selection->allows(LiveGpuSelection::Amd,selection_token))return false;
        if(!candidate->target_met()) {
          if(!candidate->rust_summary().empty())std::osyncstream(std::cout)<<"AMD_STARTUP_PARITY=PASS nonce="<<candidate->nonce()<<" rust=PASS target_satisfied=false submit=DISABLED\n";
          return true; // Completed non-winning screening is not stale.
        }
        telemetry.target_satisfied();
        return commit([&]{
          const auto &cand_job=candidate->job();
          const auto cand_nonce=candidate->nonce();
          const bool is_target_met=candidate->target_met();
          const bool do_submit=secondary_config->live_submit&&submit_client&&is_target_met;
          std::osyncstream(std::cout)<<"AMD_CANDIDATE_VERIFIED nonce="<<cand_nonce
            <<" generation="<<cand_job.generation<<" rust=PASS target_satisfied="<<(is_target_met?"true":"false")
            <<" submit="<<(do_submit?"ENABLED":"DISABLED")<<" device=" NEXUS_SECONDARY_GPU_ID "\n"<<std::flush;
          if(do_submit) {
            telemetry.set_state("ACTIVE","IDLE","","SUBMIT_PENDING");
            network::SubmitBlockResult result;
            try {
              result=submit_amd_candidate(*submit_client,*candidate,true,[&]{
                return stop.load(std::memory_order_relaxed)||ai_requests.size()!=0||
                       gpu_ownership.opoi_pending()||!cand_job.lease.valid()||
                       !live_selection->allows(LiveGpuSelection::Amd,selection_token);
              });
            }catch(const AmdAdmissionChanged &) {
              telemetry.set_state("ACTIVE","IDLE","",submit_enabled?"ARMED":"DISABLED");
              std::osyncstream(std::cout)<<"AMD_SUBMIT_SKIPPED reason=ADMISSION_CHANGED nonce="<<cand_nonce<<"\n";
              return;
            }
            telemetry.set_state("ACTIVE","IDLE","",network::submission_state_name(result.state));
            std::osyncstream(std::cout)<<"LIVE_SUBMIT_RESULT source=AMD_VULKAN state="<<network::submission_state_name(result.state)
                <<" node_error_hex="<<hex(reinterpret_cast<const std::uint8_t*>(result.node_error.data()),std::min<std::size_t>(result.node_error.size(),1024))
                <<" reject_reason="<<result.reason<<" hash="<<result.block_hash<<" nonce="<<cand_nonce<<" submit=ENABLED\n"<<std::flush;
            observe_submission(result);
          }
        });
      },[&]{return live_selection->enabled(LiveGpuSelection::Amd)&&!stop.load(std::memory_order_relaxed)&&ai_requests.size()==0&&!gpu_ownership.opoi_pending();},1,
      secondary_config->resident?SecondaryWorkLane::Prepare([&](const auto &cancelled){
        if(cancelled())return false;
        if(!secondary_engine){
          secondary_engine=std::make_unique<consensus::VulkanWalkEngine>("" NEXUS_SECONDARY_GPU_ID "",secondary_config->module,secondary_config->sha256,secondary_vulkan_batch_size,gguf.chunk_count()/32);
          secondary_pipeline=std::make_unique<AmdCandidatePipeline>(gpu_work,*secondary_engine,gguf,*secondary_merkle,*secondary_verifier,pay,extra);
        }
        return secondary_engine->load_resident(
            [&](std::uint64_t tile,std::span<std::uint8_t> bytes){
              if(vulkan_ai_config)gguf.read_tiles_streaming(tile,bytes);else gguf.read_tiles_into(tile,bytes);
            },
            [&]{return stop.load(std::memory_order_relaxed)||cancelled();});
      }):SecondaryWorkLane::Prepare{});
    }
#endif
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
    // Call only under run_opoi's ownership lock and a drained SecondaryLanePause.
    const auto release_amd_for_inference=[&]{
      if(!vulkan_ai_process||!secondary_lane||!amd_memory_handoff)return;
      secondary_lane->invalidate_preparation();secondary_pipeline.reset();
      secondary_lane->set_batch_size(1);secondary_batch_ready=false;
      if(secondary_engine){secondary_engine->release_resident();secondary_engine.reset();}
      const auto elapsed=amd_memory_handoff->wait_released([&]{return stop.load(std::memory_order_relaxed);});
      std::cout<<"AI_GPU_PREEMPTION=AMD_RELEASED wait_ms="<<elapsed<<" pci=0000:00:00.0\n"<<std::flush;
    };
    const auto infer_amd=[&](std::string_view prompt,std::uint32_t tokens,const std::function<bool()> &cancelled,
        std::chrono::seconds watchdog=std::chrono::seconds(120)){
      auto answer=vulkan_ai_process->infer(prompt,tokens,cancelled,watchdog);
      if(amd_memory_handoff){
        const auto elapsed=amd_memory_handoff->wait_released([&]{return stop.load(std::memory_order_relaxed);});
        std::cout<<"AI_GPU_PREEMPTION=AMD_INFERENCE_RELEASED wait_ms="<<elapsed<<"\n"<<std::flush;
      }else std::cout<<"AI_GPU_PREEMPTION=NVIDIA_VULKAN_INFERENCE_RELEASED child_reaped=true\n"<<std::flush;
      return answer;
    };
#endif
    std::atomic_bool ai_queue_overflow{false};
    std::atomic_uint64_t ai_current_daa{service_rpc.providers_virtual_daa};

    auto serve_ai_request=[&](QueuedAiRequest request) {
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
      SecondaryLanePause secondary_pause(secondary_lane.get());
#endif
      telemetry.set_gpu_activity("PAUSED_FOR_AI");
      telemetry.set_state("PAUSED", "QUEUED", "AI_REQUEST", submit_enabled ? "ARMED" : "DISABLED");
      if(!ai_response_service) throw std::runtime_error("AiResponse service unavailable after readiness gate");
      if(ai_queue_overflow.load(std::memory_order_acquire))
        throw std::runtime_error("bounded AiRequest queue overflowed; refusing to continue productive PoM");
      const auto ai_service_started_ns=monotonic_ns();
      const auto record_phase=[&](std::string_view phase,std::uint64_t elapsed_ns) {
        std::osyncstream(std::cout)<<"{\"event\":\"AI_REQUEST_PROFILE\",\"request_id\":\""
            <<request.id<<"\",\"phase\":\""<<phase<<"\",\"elapsed_ns\":"<<elapsed_ns
            <<",\"observed_monotonic_ns\":"<<monotonic_ns()<<"}\n"<<std::flush;
      };
      if(request.queued_ns)record_phase("queue_wait",ai_service_started_ns-request.queued_ns);
      const auto assignment_started_ns=monotonic_ns();
      auto snapshot=probe_ai_service_rpc(endpoint,0,*service_identity,std::chrono::seconds(5),
                                         ServiceRpcQuery::ProvidersOnly);
      record_phase("assignment_rpc",monotonic_ns()-assignment_started_ns);
      if(!snapshot.providers_available)
        throw std::runtime_error("could not refresh authoritative AI service assignment before inference");
      auto observed=ai_current_daa.load(std::memory_order_relaxed);
      const auto refreshed_daa=std::max({observed,snapshot.providers_virtual_daa,request.daa});
      ai_current_daa.store(refreshed_daa,std::memory_order_release);
      std::string parse_error;
      const auto parsed_request = ai::parse_request(request.transaction, request.daa, &parse_error, "mainnet");
      if (!parsed_request) throw std::runtime_error("AiRequest changed or failed official parsing before service");
      ai::AiResponseEffects effects;
      effects.phase_timing=record_phase;
      effects.assigned=[&](std::string_view request_id) {
        std::cout << "{\"event\":\"AI_REQUEST_ASSIGNED\",\"request_id\":\""
                  << request_id << "\",\"daa\":" << request.daa << "}\n" << std::flush;
      };
      effects.infer=[&](std::string_view prompt,std::uint32_t max_tokens) {
        telemetry.set_state("PAUSED", "RUNNING", "AI_INFERENCE", submit_enabled ? "ARMED" : "DISABLED");
        const auto preemption_started_ns=monotonic_ns();
        return gpu_ownership.run_opoi(
          [&] {
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
            release_amd_for_inference();
            if(primary_vulkan_workload)primary_vulkan_workload->release();
#endif
            resident.reset();
            if(engine) {
              engine.reset();
              std::cout<<"AI_GPU_PREEMPTION=POM_RELEASED request_id="<<request.id<<"\n"<<std::flush;
            }
          },
          [&] {
            record_phase("gpu_preemption",monotonic_ns()-preemption_started_ns);
            const auto inference_started_ns=monotonic_ns();
            std::string answer;
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
            if(vulkan_ai_process){
              const auto deadline=ai::service_response_deadline(request.daa,0,parsed_request->max_tokens,
                  request.daa>=ai::mainnet_signed_response_activation_daa);
              if(!deadline)throw std::runtime_error("Assigned AiRequest has no verified service deadline");
              answer=infer_amd(opoi::official_qwen35_ai_prompt(prompt),max_tokens,[&]{
                return stop.load(std::memory_order_relaxed)||ai_current_daa.load(std::memory_order_acquire)>*deadline;
              },std::chrono::hours(1)); // Watchdog only; it never overrides or extends the authoritative DAA deadline.
            }else
#endif
            answer=shared_runtime->infer(opoi::official_qwen35_ai_prompt(prompt),max_tokens);
            record_phase("model_inference",monotonic_ns()-inference_started_ns);
            if(answer.empty()) throw std::runtime_error("stock CUDA inference returned an empty AiResponse");
            return answer;
          },
          [&] {
            if(!stop.load(std::memory_order_relaxed)) {
              std::cout<<"AI_GPU_PREEMPTION=POM_READY_LAZY allocation=deferred_until_pom_admission\n"<<std::flush;
            }
          });
      };
      effects.publish_and_readback=[&](std::span<const std::uint8_t> body) {
        return ai::ipfs_publish_and_readback(ipfs_api,body);
      };
      effects.submit=[&](const protowire::RpcTransaction &tx,std::string_view txid) {
        telemetry.set_state("PAUSED", "RESPONSE_READY", "AI_RESPONSE_SUBMISSION", "AI_RESPONSE_SUBMIT_SENT");
        if(!ai_responses_enabled)
          return ai::AiResponseEffects::SubmitResult{ai::AiResponseEffects::SubmitResult::State::Rejected,
              "AiResponse transaction authorization is disabled"};
        return ai_response_transport.submit(tx,txid);
      };
      effects.reconcile=[&](std::string_view txid) {
        return ai_response_transport.known(txid);
      };
      effects.current_daa=[&] { return ai_current_daa.load(std::memory_order_acquire); };
      effects.wait_before_retry=[&](std::uint32_t milliseconds) {
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
        while(!stop.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<until)
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return !stop.load(std::memory_order_acquire);
      };
      effects.submission_diagnostic=[&](std::string_view txid,std::uint32_t attempt,
                                       std::string_view reason,std::string_view digest) {
        std::osyncstream(std::cout)<<"{\"event\":\"AI_RESPONSE_SUBMISSION\",\"request_id\":\""
            <<request.id<<"\",\"txid\":\""<<txid<<"\",\"attempt\":"<<attempt
            <<",\"reason\":\""<<reason<<"\",\"node_detail_sha256\":\""<<digest<<"\"}\n"<<std::flush;
      };
      const auto result=ai_response_service->serve(request.transaction,request.daa,
                                                   snapshot.providers,effects,
          ai::service_response_deadline(request.daa,0,parsed_request->max_tokens,
            request.daa>=ai::mainnet_signed_response_activation_daa),refreshed_daa);
      if (escrow_watcher && parsed_request->inference_reward != 0 &&
          (result == ai::ServeResult::Submitted || result == ai::ServeResult::Duplicate)) {
        const auto record = ai_response_service->journal().get(parsed_request->stable_id);
        if (record && (record->state == ai::ResponseState::RpcAcknowledged ||
                       record->state == ai::ResponseState::ChainObserved ||
                       record->state == ai::ResponseState::Finalized)) {
          // The stock miner registers output[1] only after a response is accepted/reconciled.
          // Its watcher deduplicates by request txid and journals the actual inference reward.
          escrow_watcher->track_inference_outpoint(
              hex(parsed_request->txid.data(), parsed_request->txid.size()),
              request.daa, parsed_request->inference_reward);
          escrow_watcher->flush();
        }
      }
      record_phase("service_total",monotonic_ns()-ai_service_started_ns);
      const auto response_record = ai_response_service->journal().get(request.id);
      std::cout << "{\"event\":\"AI_RESPONSE_LIFECYCLE\",\"request_id\":\""
                << request.id << "\",\"state\":\""
                << (response_record ? ai::response_state_name(response_record->state) : "UNKNOWN")
                << "\",\"serve_result\":" << static_cast<int>(result)
                << ",\"daa\":" << request.daa << ",\"pending\":" << ai_requests.size()
                << ",\"details\":\"redacted\"}\n" << std::flush;
      if(result==ai::ServeResult::WaitingReconciliation)
        throw std::runtime_error("AiResponse submission outcome is ambiguous; reconciliation required before mining resumes");
      if(result==ai::ServeResult::Failed)
        throw std::runtime_error("assigned AiRequest could not be served; fail-closed before resuming PoM");
      telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
      telemetry.set_gpu_activity("WAITING_TEMPLATE");
    };

    // Join callback threads BEFORE destroying the shared model/engines on any exception.
    auto callback_cleanup=std::unique_ptr<int,std::function<void(int*)>>(new int(0),[&](int *p){
      request_worker_stop(stop,gpu_ownership,queue);
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
      if(secondary_lane)secondary_lane->request_stop();
#endif
      if(telemetry_thread.joinable())telemetry_thread.join();
      if(ingress.joinable())ingress.join();
      if(shutdown_watcher.joinable())shutdown_watcher.join();
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
      if(secondary_lane)secondary_lane->join();
#endif
      delete p;
    });
    ingress=std::thread([&]{
      try {
        const auto ingress_rc=run_live_job_client(endpoint,pay,extra,queue,0,0,stop,[&](const std::string &wire) {
        const char *library_env=std::getenv("NEXUS_OPOI_LIBRARY");
        if(!vulkan_ai_config&&(!library_env||!*library_env)) throw std::runtime_error("NEXUS_OPOI_LIBRARY is not configured");
        const auto challenge=opoi::parse_challenge(wire);
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        SecondaryLanePause secondary_pause(secondary_lane.get());
#endif
        std::string answer;
          telemetry.set_gpu_activity("PAUSED_FOR_OPOI");
          telemetry.set_state("PAUSED", "RUNNING", "OPOI_CHALLENGE", submit_enabled ? "ARMED" : "DISABLED");
          const auto response_result = gpu_ownership.run_opoi(
          [&] {
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
            release_amd_for_inference();
            if(primary_vulkan_workload)primary_vulkan_workload->release();
#endif
            resident.reset();
            if(engine) {
              engine.reset();
              std::cout<<"GPU_PREEMPTION=POM_RELEASED reason=opoi batch_boundary=true\n"<<std::flush;
            }
          },
          [&] {
            std::cout<<"OPOI_RUNTIME=START priority=before_new_pom_batches device="<<(vulkan_ai_config&&!primary_vulkan?std::string_view("" NEXUS_SECONDARY_GPU_ID ""):gpu_uuid)<<'\n'<<std::flush;
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
            if(vulkan_ai_process)answer=infer_amd(opoi::official_qwen35_prompt(challenge.nonce_hex),64,
                [&]{return stop.load(std::memory_order_relaxed);});else
#endif
            answer=shared_runtime->answer(challenge);
            return opoi::format_inference_result(challenge,answer);
          },
          [&] {
            if(!stop.load(std::memory_order_relaxed)) {
              std::cout<<"GPU_PREEMPTION=POM_READY_LAZY allocation=deferred_until_pom_admission\n"<<std::flush;
            }
          });
        std::cout<<"OPOI_RUNTIME=PASS answer_bytes="<<answer.size()<<'\n'<<std::flush;
        telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
        telemetry.set_gpu_activity("WAITING_TEMPLATE");
        return response_result;
      },[&](const protowire::RpcBlock &block,bool) {
        if(block.has_header()) {
          if(escrow_claims_enabled)for(const auto &tx:block.transactions()) {
            const auto digest=ai::consensus_transaction_id(tx);if(!digest)continue;
            const auto txid=hex(digest->data(),digest->size());
            bool ours=false;{std::lock_guard lock(claim_ids_mutex);ours=own_claim_ids.contains(txid);}
            if(ours)std::osyncstream(std::cout)<<"ESCROW_CLAIM_CHAIN_OBSERVED txid="<<txid<<" daa="<<block.header().daascore()
                <<" selected_chain="<<(block.has_verbosedata()&&block.verbosedata().ischainblock()?"true":"false")<<" finality=NOT_ASSERTED\n"<<std::flush;
          }
          auto prior=ai_current_daa.load(std::memory_order_relaxed);
          while(prior<block.header().daascore()&&!ai_current_daa.compare_exchange_weak(
              prior,block.header().daascore(),std::memory_order_release,std::memory_order_relaxed)) {}
          const auto response_observed=ai_response_service->observe_block(block);
          if(response_observed)std::cout<<"AI_RESPONSE_CHAIN_OBSERVED count="<<response_observed
              <<" daa="<<block.header().daascore()<<" finality=NOT_CLAIMED\n"<<std::flush;
          for(const auto &tx:block.transactions()) if(tx.subnetworkid()==ai::request_subnetwork)
            (void)ai_response_service->discover(tx,block.header().daascore());
          if(!ai_requests.push(block)) {
            ai_queue_overflow.store(true,std::memory_order_release);
            throw std::runtime_error("bounded AiRequest queue full; service admission suspended");
          }
        }
        if (escrow_watcher) {
          const auto now=monotonic_ns();
          const auto unix_now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
          const bool task_free=!escrow_claim_task.valid()||escrow_claim_task.wait_for(std::chrono::seconds(0))==std::future_status::ready;
          if(escrow_claim_task.valid()&&task_free)escrow_claim_task.get();
          const bool claim_check=escrow_claims_enabled&&task_free&&startup_backfill_complete.load(std::memory_order_acquire)&&
              startup_backfill_done.load(std::memory_order_acquire)&&!escrow_watcher->validation_in_progress()&&
              ai::escrow_claim_check_due(now,next_claim_check_ns.load(std::memory_order_acquire),unix_now,last_claim_check_unix)&&!stop.load(std::memory_order_relaxed);
          if(claim_check) {
            const auto temporary=claim_cadence_file.string()+".new."+std::to_string(::getpid());
            const int fd=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
            if(fd<0)throw std::runtime_error("cannot persist escrow claim check cadence");
            const auto text=std::to_string(unix_now)+"\n";
            const bool written=::write(fd,text.data(),text.size())==static_cast<ssize_t>(text.size())&&::fsync(fd)==0;
            ::close(fd);if(!written){::unlink(temporary.c_str());throw std::runtime_error("escrow claim cadence write failed");}
            fs::rename(temporary,claim_cadence_file);
            const int dirfd=::open(claim_cadence_file.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            if(dirfd<0)throw std::runtime_error("claim cadence directory unavailable");
            const bool synced=::fsync(dirfd)==0;::close(dirfd);if(!synced)throw std::runtime_error("claim cadence directory sync failed");
            last_claim_check_unix=unix_now;
          }
          // Discovery stays active on every block. Only one stock claim-selection
          // attempt is admitted per 30-minute interval, outside historical backfill.
          escrow_watcher->set_claim_construction_enabled(false);
          const auto escrow = escrow_watcher->handle_block(block);
          escrow_watcher->set_claim_construction_enabled(false);
          if(claim_check) {
            next_claim_check_ns.store(now+1'800'000'000'000ULL,std::memory_order_release);
            std::osyncstream(std::cout)<<"ESCROW_CLAIM_CHECK interval_sec=1800 candidate=UTXO_QUERY_PENDING"<<"\n"<<std::flush;
          }
          if (claim_check) {
            escrow_claim_task=std::async(std::launch::async,[&,endpoint,claim_block=block]() mutable {
              protowire::GetUtxosByAddressesResponseMessage live;
              try {
                // A DAG notification can lag the virtual state. Use a fresh local
                // RPC anchor for maturity only; never rewrite a PoM candidate.
                const auto trigger_daa=claim_block.header().daascore();
                const auto virtual_daa=ai::query_escrow_virtual_daa(endpoint);
                // Discovery already consumed the original block above. Selection
                // must not rediscover its transactions under a different DAA.
                claim_block.Clear();
                claim_block.mutable_header()->set_daascore(virtual_daa);
                std::osyncstream(std::cout)<<"ESCROW_CLAIM_ANCHOR trigger_daa="<<trigger_daa<<" virtual_daa="<<virtual_daa<<" nominal_batch=87\n"<<std::flush;
                const auto ops=escrow_watcher->claim_outpoints(claim_block.header().daascore());
                live=ai::query_escrow_utxos(endpoint,ops);
                std::osyncstream(std::cout)<<"ESCROW_UTXO_RECONCILIATION queried="<<ops.size()<<" live="<<live.entries_size()<<" absent="<<(ops.size()-live.entries_size())<<" records_preserved=true selected_chain=NOT_ASSERTED\n"<<std::flush;
              } catch(const std::exception &e) {
                const std::string detail=e.what();
                std::osyncstream(std::cout)<<"ESCROW_CLAIM_CHECK_BLOCKED detail_hex="<<hex(reinterpret_cast<const std::uint8_t*>(detail.data()),detail.size())<<"\n"<<std::flush;
                return;
              }
              if(stop.load(std::memory_order_relaxed))return;
              auto candidate=escrow_watcher->verified_claim(claim_block,live);
              if(!candidate){std::osyncstream(std::cout)<<"ESCROW_CLAIM_VERIFIED candidate=NONE\n"<<std::flush;return;}
              auto tx=std::move(*candidate);
              const auto digest=ai::consensus_transaction_id(tx);
              if(!digest||tx.inputs_size()<1||tx.inputs_size()>87||tx.outputs_size()!=1||tx.subnetworkid()!=std::string(40,'0'))
                throw std::runtime_error("stock escrow claim structure or canonical transaction ID invalid");
              const auto id=hex(digest->data(),digest->size());
              {std::lock_guard lock(claim_ids_mutex);if(own_claim_ids.size()>=10000&&!own_claim_ids.contains(id))throw std::runtime_error("bounded claim history exhausted");own_claim_ids.insert(id);}
              const auto record=[&](std::string_view stage,std::string_view detail={}) {
                const std::string line="{\"event\":\"ESCROW_CLAIM_SUBMISSION\",\"state\":\""+std::string(stage)+"\",\"txid\":\""+id+"\",\"inputs\":"+std::to_string(tx.inputs_size())+",\"payout_sompi\":"+std::to_string(tx.outputs(0).amount())+",\"fee_sompi\":30000000,\"observed_monotonic_ns\":"+std::to_string(monotonic_ns())+",\"detail_hex\":\""+hex(reinterpret_cast<const std::uint8_t*>(detail.data()),detail.size())+"\"}\n";
                const int fd=::open(claim_journal.c_str(),O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
                if(fd<0)throw std::runtime_error("cannot open durable claim submission journal");
                std::size_t done=0;bool ok=true;
                while(done<line.size()){const auto n=::write(fd,line.data()+done,line.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0){ok=false;break;}done+=static_cast<std::size_t>(n);}
                if(ok)ok=::fsync(fd)==0;::close(fd);
                if(!ok)throw std::runtime_error("cannot persist claim submission stage");
                std::osyncstream(std::cout)<<line<<std::flush;
              };
              // No HTTP explorer verdict can authorize a spend. The exact stock
              // signer builds the TX; the local node validates UTXO/CSV/signatures.
              record("SUBMIT_INTENT");
              const auto result=ai::submit_ai_response_transaction(endpoint,tx,id);
              if(result.state==ai::AiResponseEffects::SubmitResult::State::Acknowledged) {
                record("SUBMIT_SENT");
                record("RPC_ACKNOWLEDGED",result.detail);
                if(escrow_watcher->submit_result(id,std::nullopt)!=2)throw std::runtime_error("stock claim ACK correlation failed");
              } else {
                if(result.state==ai::AiResponseEffects::SubmitResult::State::Rejected)record("SUBMIT_SENT");
                record(result.state==ai::AiResponseEffects::SubmitResult::State::Rejected?"RPC_REJECTED":"RPC_UNKNOWN",result.detail);
                const auto detail=result.detail.empty()?std::string("ambiguous RPC outcome"):result.detail;
                if(escrow_watcher->submit_result(id,detail)==0)throw std::runtime_error("stock claim error correlation failed");
              }
              escrow_watcher->flush();
            });
          }
          if (escrow.pending_count != 0)
            std::cout << "ESCROW_WATCHER_BLOCK daa="
                      << (block.has_header() ? block.header().daascore() : 0)
                      << " pending_count=" << escrow.pending_count
                      << " pending_amount_sompi=" << escrow.pending_amount_sompi
                      << " claim_submission="<<(escrow_claims_enabled?"ENABLED":"DISABLED")<<"\n" << std::flush;
        }
      },[&](const std::vector<std::string> &removed_hashes) {
        const auto ai_reorged=ai_response_service->observe_removed_chain_blocks(removed_hashes);
        if(ai_reorged)std::cout<<"AI_RESPONSE_REORG_RECONCILED requests="<<ai_reorged
            <<" action=await_reinclusion_or_absence_check\n"<<std::flush;
        if (!escrow_watcher) return;
        for (const auto &text : removed_hashes) {
          try {
            const auto raw = decode_hex_secret(text);
            std::array<std::uint8_t, 32> hash{};
            std::copy(raw.begin(), raw.end(), hash.begin());
            escrow_watcher->chain_block_removed(hash);
          } catch (const std::exception &) {
            throw std::runtime_error("node supplied malformed removed-chain block hash");
          }
        }
        escrow_watcher->flush();
        std::cout << "ESCROW_REORG_RECONCILED removed_blocks=" << removed_hashes.size()
                  << " claim_submission="<<(escrow_claims_enabled?"ENABLED":"DISABLED")<<"\n" << std::flush;
      },[&](bool complete,std::size_t hashes,std::size_t unavailable) {
        startup_backfill_hashes.store(hashes,std::memory_order_release);
        startup_backfill_unavailable.store(unavailable,std::memory_order_release);
        startup_backfill_complete.store(complete,std::memory_order_release);
        startup_backfill_done.store(true,std::memory_order_release);
      });
        if(ingress_rc!=0) throw std::runtime_error("live gRPC ingress returned a failure status");
      }
      catch(const std::exception &e){ ingress_failed.store(true,std::memory_order_release); std::cerr<<"LIVE_INGRESS_ERROR detail="<<e.what()<<'\n'; }
      queue.close();
    });
    shutdown_watcher=std::thread([&]{
      while(!stop.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      queue.close();
    });
    std::cout<<"AI_STARTUP_BACKFILL=WAITING before_pom=true transactions="
             <<(ai_responses_enabled?"AUTHORIZED":"DISABLED")<<"\n"<<std::flush;
    const auto backfill_startup_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(300);
    while(!startup_backfill_done.load(std::memory_order_acquire)&&!stop.load(std::memory_order_relaxed)) {
      if(ingress_failed.load(std::memory_order_acquire))
        throw std::runtime_error("live gRPC ingress failed before startup AiRequest backfill completed");
      if(std::chrono::steady_clock::now()>=backfill_startup_deadline)
        throw std::runtime_error("startup AiRequest backfill did not reach a bounded completion point within 300 seconds");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if(stop.load(std::memory_order_relaxed)) throw std::runtime_error("cooperative stop requested during startup AiRequest backfill");
    std::cout<<"AI_STARTUP_BACKFILL="
             <<(startup_backfill_complete.load(std::memory_order_acquire)?"COMPLETE":"INCOMPLETE")
             <<" hashes="<<startup_backfill_hashes.load(std::memory_order_acquire)
             <<" unavailable="<<startup_backfill_unavailable.load(std::memory_order_acquire)
             <<" known_pending="<<ai_requests.size()<<" before_pom=true\n"<<std::flush;
    // Serve every known startup obligation before opening the PoM admission gate.
    while(auto request=ai_requests.pop()) serve_ai_request(std::move(*request));
    if(ai_queue_overflow.load(std::memory_order_acquire))
      throw std::runtime_error("startup AiRequest queue overflow; PoM remains disabled");
    if(primary_pom_enabled && !primary_vulkan && !gpu_ownership.run_pom([&]{
      resident=std::make_unique<CudaResidentSearch>(std::string(gpu_uuid),gguf,shared_runtime->resident_tensors());
    }))throw std::runtime_error("PoM initialization preempted by shutdown");
    if(primary_pom_enabled&&!primary_vulkan) cuda_engine_initializations.fetch_add(1,std::memory_order_relaxed);
    startup.record("POM_READY", StartupResult::Pass, primary_pom_enabled ?
        (primary_vulkan?"PoM Vulkan configured; model upload deferred behind AI admission":"PoM CUDA engine initialized after live subscription, startup backfill, and known AiRequest service") :
        "NVIDIA PoM disabled by explicit selection; AI service preserved and secondary PoM independently configured");
    std::cout << "GPU_SELECTION primary_pom=" << (primary_pom_enabled ? "ENABLED" : "DISABLED")
              << " ai_device="<<(vulkan_ai_config?(primary_vulkan?"VULKAN_NVIDIA":"VULKAN_AMD"):"CUDA_NVIDIA")<<" ai_service=PRESERVED activation=STARTUP\n" << std::flush;
    std::cout<<"{\"event\":\"POM_BACKEND_SELECTED\",\"schema_version\":1,\"backend\":\""
      <<(is_bda_vulkan?"vulkan-bda":(primary_vulkan?"vulkan-jit":"cuda-driver"))<<"\",\"device_id\":\""<<gpu_uuid<<"\"}\n"<<std::flush;
    std::uint64_t nonce_cursor=first_nonce;
    std::size_t completed=0,jobs_seen=0;
    std::cout<<"LIVE_CUDA_WORKER=START uuid="<<gpu_uuid<<" cuda_engine_instances="<<cuda_engine_initializations.load(std::memory_order_relaxed)<<" submit="
             <<(submit_enabled?"ENABLED":"DISABLED")
             <<" ai_response="<<(ai_responses_enabled?"ENABLED":"DISABLED")
             <<" escrow_claims="<<(escrow_claims_enabled?"ENABLED":"DISABLED")<<"\n"<<std::flush;
    telemetry.set_state("ACTIVE", "IDLE", "", submit_enabled ? "ARMED" : "DISABLED");
    telemetry_thread=std::thread([&, live_selection] {
      while(!stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if(stop.load(std::memory_order_relaxed)) break;
        live_selection->refresh();
        telemetry.set_job_invalidations(queue.virtual_chain_invalidations());
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        if(secondary_lane){const auto s=secondary_lane->stats();
          if(live_selection->enabled(LiveGpuSelection::Amd)||!s.busy)live_selection->acknowledge(LiveGpuSelection::Amd);
          std::osyncstream(std::cout)<<"{\"event\":\"AMD_LANE_STATE\",\"attempts\":"<<s.attempts
            <<",\"backend\":\""<<(secondary_config->module=="bda-native"?"vulkan-bda":"vulkan-jit")<<"\",\"completed\":"<<s.completed<<",\"verified\":"<<s.verified<<",\"discarded\":"<<s.discarded
            <<",\"busy\":"<<(s.busy?"true":"false")<<",\"preparing\":"<<(s.preparing?"true":"false")<<",\"paused\":"<<(s.paused?"true":"false")
            <<",\"failed\":"<<(s.failed?"true":"false")<<"}\n"<<std::flush;}
#endif
        std::osyncstream(std::cout) << live_selection->snapshot() << '\n';
        auto snapshot=telemetry.json_snapshot(monotonic_ns());
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        // Report resolved execution routes, not saved UI preferences.
        snapshot.pop_back();
        snapshot+=",\"gpu_backends\":{\"" NEXUS_PRIMARY_GPU_UUID "\":\"";
        snapshot+=primary_pom_enabled?(is_bda_vulkan?"vulkan-bda":primary_vulkan?"vulkan-jit":"cuda-driver"):"disabled";
        snapshot+="\",\"" NEXUS_SECONDARY_GPU_ID "\":\"";
        snapshot+=secondary_pom_enabled?(secondary_config->module=="bda-native"?"vulkan-bda":"vulkan-jit"):"disabled";
        snapshot+="\"},\"gpu_ready\":{\"" NEXUS_SECONDARY_GPU_ID "\":";
        snapshot+=secondary_pom_enabled&&secondary_batch_ready.load()?"true":"false";
        snapshot+="}}";
#endif
        std::osyncstream(std::cout) << snapshot << '\n' << std::flush;
      }
    });
    telemetry_started=true;
    std::optional<TemplateJob> reusable_job;
    // Keep one decoded RPC snapshot. Compare the complete wire bytes, not only
    // the generation: duplicate refreshes can replace the RPC envelope.
    PreparedRpcTemplateCache template_cache;
    bool startup_parity_verified=false;
    bool primary_selection_paused=false;
    startup.record("MINING", StartupResult::Pass, "productive PoM admission enabled");
    if (!startup.mining_allowed())
      throw std::runtime_error("startup state machine did not authorize productive mining");
    while((nonce_limit==0||completed<nonce_limit)&&!stop.load(std::memory_order_relaxed)) {
      live_selection->acknowledge(LiveGpuSelection::Nvidia);
      if(memory_guard)memory_guard->check();
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
      if(secondary_lane)secondary_lane->rethrow_if_failed();
#endif
      if(ai_queue_overflow.load(std::memory_order_acquire))
        throw std::runtime_error("AiRequest discovery queue overflow; stopping PoM to preserve service obligations");
      while(auto request=ai_requests.pop()) serve_ai_request(std::move(*request));
      {std::lock_guard lock(chain_observers_mutex);
        std::erase_if(chain_observers,[](auto &f){if(f.wait_for(std::chrono::seconds(0))==std::future_status::ready){f.get();return true;}return false;});}
      const auto wait_start_ns=monotonic_ns();
      auto maybe_job=queue.wait_pop_for(std::chrono::milliseconds(reusable_job&&reusable_job->lease.valid()?0:100));
      auto *active_job=select_active_template_job(reusable_job,std::move(maybe_job));
      telemetry.record_template_wait(monotonic_ns()-wait_start_ns);
      if(!active_job) {
        telemetry.set_gpu_activity("WAITING_TEMPLATE");
        if(queue.closed()) break;
        continue;
      }
      telemetry.set_gpu_activity("PREPARING_CPU");
      const auto &job=*active_job; ++jobs_seen;
      const auto preparation_start_ns=monotonic_ns();
      try {
        require_mining_eligible(job.fields);
        if(!job.lease.valid()) continue;
        const auto &block=template_cache.block(job.fields.response_wire);
        if(job.fields.pay_address!=pay||!opoi::valid_coinbase_extra_data(job.fields.extra_data,extra))
          throw std::runtime_error("queued job payout/escrow request binding changed");
        const auto &prepared_template=template_cache.get(job.fields.response_wire);
        const auto &header=prepared_template.header;
        const auto anchor=official_tier_anchor(header.daa_score,header.pom_tier);
        if(anchor.root!=official_root||anchor.chunks!=gguf.chunk_count()) throw std::runtime_error("live job DAA/tier selects a different installed model");
        const auto &pph=prepared_template.pre_pow; const auto &target=prepared_template.target;
        if(target.zero) throw std::runtime_error("live job has invalid compact target");
        // A zero nonce limit means continuous live mining.  In that mode each
        // iteration still admits one bounded microbatch; subtracting completed
        // from zero previously produced a zero-sized batch forever.
        const auto remaining=nonce_limit==0?microbatch:nonce_limit-completed;
        const auto count=std::min<std::size_t>(microbatch,remaining);
        const auto preparation_end_ns=monotonic_ns();
        if(!queue.admission_allowed(job.generation)&&!queue.wait_refresh_resolved(job.generation))continue;
        const auto refresh_end_ns=monotonic_ns();
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        if(secondary_lane)secondary_lane->publish(job,pph);
#endif
        const auto primary_selection_token=live_selection->token(LiveGpuSelection::Nvidia);
        if(!live_selection->allows(LiveGpuSelection::Nvidia,primary_selection_token)) {
          gpu_ownership.run_pom([&]{resident.reset();
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
            if(primary_vulkan_workload)primary_vulkan_workload->release();
#endif
          });
          primary_selection_paused=true;
          telemetry.set_gpu_activity("POM_DISABLED_BY_SELECTION");
          if(!gpu_ownership.opoi_pending()&&ai_requests.size()==0)
            telemetry.set_state(live_selection->enabled(LiveGpuSelection::Amd)?"ACTIVE":"PAUSED",
                "IDLE",live_selection->enabled(LiveGpuSelection::Amd)?"":"GPU_SELECTION",
                submit_enabled?"ARMED":"DISABLED");
          // Wake as soon as a template is published instead of delaying its
          // delivery to AMD until a fixed polling sleep has elapsed.
          (void)select_active_template_job(reusable_job,queue.wait_pop_for(std::chrono::milliseconds(5)));
          continue;
        }
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        if(primary_vulkan_workload) {
          const auto batch_count=static_cast<std::uint32_t>(nonce_limit==0?PrimaryVulkanWorkload::batch_limit:
              std::min<std::uint64_t>(PrimaryVulkanWorkload::batch_limit,nonce_limit-completed));
          auto ticket=gpu_work.reserve(MiningLane::Nvidia,job,pph,batch_count);
          if(!ticket) {
            if(!gpu_work.nonce_range_available(batch_count))throw std::overflow_error("Vulkan primary nonce range exhausted");
            continue;
          }
          struct Retire {GpuWorkCoordinator &owner;const GpuWorkCoordinator::Ticket &ticket;bool active=true;
            ~Retire(){if(active)owner.finish(ticket);}} retire{gpu_work,*ticket};
          const auto cancelled=[&]{return stop.load(std::memory_order_relaxed)||ai_requests.size()!=0||
              gpu_ownership.opoi_pending()||!live_selection->allows(LiveGpuSelection::Nvidia,primary_selection_token)||
              !job.lease.valid();};
          const auto cancelled_prepare=[&]{return stop.load(std::memory_order_relaxed)||ai_requests.size()!=0||
              gpu_ownership.opoi_pending()||!live_selection->allows(LiveGpuSelection::Nvidia,primary_selection_token);};
          std::unique_ptr<AmdVerifiedCandidate> candidate;
          consensus::VulkanEngineStats before{},after{};
          bool prepared=false;
          telemetry.set_gpu_activity("VULKAN_RESIDENT_SEARCH");
          const bool ran=gpu_ownership.run_pom([&]{
            prepared=primary_vulkan_workload->prepare(cancelled_prepare);
            if(!prepared)return;
            before=primary_vulkan_workload->stats();
            retire.active=false; // execute owns retirement on every outcome.
            candidate=primary_vulkan_workload->execute(*ticket,cancelled);
            after=primary_vulkan_workload->stats();
          });
          if(!ran||!prepared)continue;
          const auto completed_kernels=after.completed-before.completed;
          const auto searched=(after.submitted>before.submitted)?batch_count:0;
          if(searched)telemetry.work_launched(searched);
          if(completed_kernels) {
            telemetry.work_completed(std::uint64_t(batch_count)*walk_steps,batch_count,completed_kernels);
            completed+=batch_count;
          }
          telemetry.record_search_host(preparation_end_ns-preparation_start_ns,refresh_end_ns-preparation_end_ns);
          if(!candidate||cancelled()) {
            if(completed_kernels)telemetry.work_stale(batch_count);
            continue;
          }
          // Screening stops at the first winner. Do not label the remaining
          // completed nonce outputs as target-checked attempts.
          const auto screened=candidate->target_met()?
              candidate->nonce()-ticket->first_nonce()+1:batch_count;
          telemetry.eligible_attempts(screened);
          if(!candidate->rust_summary().empty()) {
            telemetry.proof_completed();telemetry.proof_rust_verified();telemetry.proof_fresh(false);
            startup_parity_verified=true;
          }
          if(!candidate->target_met())continue;
          telemetry.target_satisfied();
          std::osyncstream(std::cout)<<"NVIDIA_VULKAN_CANDIDATE_VERIFIED nonce="<<candidate->nonce()
            <<" generation="<<candidate->job().generation<<" rust=PASS target_satisfied=true submit="
            <<(submit_enabled?"ENABLED":"DISABLED")<<" device="<<gpu_uuid<<"\n"<<std::flush;
          if(!submit_enabled)continue;
          try {
            const auto result=submit_amd_candidate(*submit_client,*candidate,true,cancelled);
            telemetry.set_state("ACTIVE","IDLE","",network::submission_state_name(result.state));
            std::osyncstream(std::cout)<<"LIVE_SUBMIT_RESULT source=NVIDIA_VULKAN state="<<network::submission_state_name(result.state)
              <<" reject_reason="<<result.reason<<" hash="<<result.block_hash<<" nonce="<<candidate->nonce()
              <<" node_error_hex="<<hex(reinterpret_cast<const std::uint8_t*>(result.node_error.data()),std::min<std::size_t>(result.node_error.size(),1024))
              <<" submit=ENABLED\n"<<std::flush;
            observe_submission(result);
          }catch(const AmdAdmissionChanged &) {
            std::osyncstream(std::cout)<<"NVIDIA_VULKAN_SUBMIT_SKIPPED reason=ADMISSION_CHANGED\n"<<std::flush;
          }
          continue;
        }
#endif
        const std::uint64_t batch_count=nonce_limit==0?live_resident_batch_size:std::min<std::uint64_t>(256,nonce_limit-completed);
        ResidentSearchResult search;
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
        auto ticket=gpu_work.reserve(MiningLane::Nvidia,job,pph,static_cast<std::uint32_t>(batch_count));
        if(!ticket) {
          if(!gpu_work.nonce_range_available(static_cast<std::uint32_t>(batch_count)))
            throw std::overflow_error("shared GPU nonce range exhausted");
          // A duplicate refresh can resolve between reserve() and this check.
          // It is recoverable even if admission is already open again.
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        struct ReleaseGpuTicket {GpuWorkCoordinator &owner;const GpuWorkCoordinator::Ticket &ticket;
          ~ReleaseGpuTicket(){owner.finish(ticket);}} ticket_release{gpu_work,*ticket};
        const auto batch_start=ticket->first_nonce();
        nonce_cursor=batch_start;
#else
        const auto batch_start=nonce_cursor;
#endif
        telemetry.set_gpu_activity("CUDA_RESIDENT_SEARCH");
        const bool searched=live_selection->allows(LiveGpuSelection::Nvidia,primary_selection_token)&&gpu_ownership.run_pom([&]{
          if(!resident) {
            resident=std::make_unique<CudaResidentSearch>(std::string(gpu_uuid),gguf,shared_runtime->resident_tensors());
            cuda_engine_initializations.fetch_add(1,std::memory_order_relaxed);
            std::cout<<"POM_ALLOCATION_READY lazy_restore=true engine_instances="
                     <<cuda_engine_initializations.load(std::memory_order_relaxed)<<"\n"<<std::flush;
          }
          if(primary_selection_paused) {
            telemetry.set_state("ACTIVE","IDLE","",submit_enabled?"ARMED":"DISABLED");
            primary_selection_paused=false;
          }
          telemetry.work_launched(batch_count);
          search=resident->run(pph,header.timestamp,header.daa_score,target.little_endian,batch_start,batch_count);
          telemetry.work_completed(search.completed*walk_steps,search.completed);
        });
        if(!searched)continue;
        telemetry.record_search_host(preparation_end_ns-preparation_start_ns,refresh_end_ns-preparation_end_ns);
        nonce_cursor+=search.completed;
        completed+=search.completed;
        telemetry.record_cuda_times(search.kernel_ms,0,0,search.kernel_ms);
        if(!live_selection->allows(LiveGpuSelection::Nvidia,primary_selection_token)||!job.lease.valid()||stop.load(std::memory_order_relaxed)||!queue.wait_refresh_resolved(job.generation)){
          telemetry.work_stale(search.completed);continue;
        }
        telemetry.eligible_attempts(search.completed);
        const bool expected_winner=search.winner.has_value();
        // One real-template witness per session verifies the complete live Rust path.
        // It is not a target win and cannot reach SubmitBlock unless the real target is met.
        if(!expected_winner&&startup_parity_verified)continue;
        // The RPC digest is used by witness artifacts and submission binding,
        // never by the ordinary nonce-search batch.
        const auto response_digest=blake3_digest(job.fields.response_wire.data(),job.fields.response_wire.size());
        const auto winning_nonce=search.winner.value_or(batch_start);
        std::cout<<(expected_winner?"LIVE_RESIDENT_TARGET=FOUND generation=":"LIVE_RESIDENT_PARITY=SAMPLE generation=")<<job.generation<<" nonce="<<winning_nonce
                 <<" completed="<<search.completed<<" kernel_ms="<<search.kernel_ms<<" rust_verification=PENDING\n"<<std::flush;
        std::optional<PreparedNonce> prefetched;
        auto prepare_nonce=[&](std::uint64_t nonce,bool lookahead)->PreparedNonce{
          PreparedNonce p;p.nonce=nonce;p.started_ns=monotonic_ns();auto phase=monotonic_ns();auto cpu_phase=thread_cpu_ns();
          p.seed=block_seed(pph,header.timestamp,nonce,header.daa_score);
          if(!job.lease.valid())throw PrefetchCancelled("template lease expired before candidate binding");
          try {
            p.binding=bind_candidate(job,nonce,header.timestamp,pph);
          } catch(const std::invalid_argument &) {
            if(!job.lease.valid())
              throw PrefetchCancelled("template lease expired during candidate binding");
            throw;
          }
          const auto binding_status=candidate_binding_status(p.binding,job);
          if(binding_status==CandidateBindingStatus::StaleLease)
            throw PrefetchCancelled("template lease expired during candidate binding");
          if(binding_status==CandidateBindingStatus::IntegrityMismatch)
            throw std::runtime_error("live candidate/job binding integrity mismatch before gather");
          auto end=monotonic_ns();auto cpu_end=thread_cpu_ns();profile_phase(job.generation,nonce,"seed_binding",phase,end,{},cpu_end-cpu_phase);phase=end;cpu_phase=cpu_end;
          const auto may_continue=[&](){return live_selection->allows(LiveGpuSelection::Nvidia,primary_selection_token)&&job.lease.valid()&&!stop.load(std::memory_order_relaxed)&&
              (!lookahead||(!gpu_ownership.opoi_pending()&&queue.admission_allowed(job.generation)));};
          std::vector<model::SparseMerkleIndex::TileRootInput> sibling_roots;
          sibling_roots.reserve(walk_steps);
          const bool gathered=gpu_ownership.run_pom([&]{
            if(!resident)throw std::runtime_error("resident model missing before winner tile gather");
            if(!may_continue()||gpu_ownership.opoi_pending())
              throw PrefetchCancelled("job refresh, OPoI, or shutdown cancelled winner gather");
            const auto &witness=resident->read_witness_tiles_cached(p.seed);
            std::size_t witness_step=0;
            p.input=gather_v4_model_tiles(p.seed,tile_count,[&](std::uint64_t offset){
              if(!may_continue()||gpu_ownership.opoi_pending())
                throw PrefetchCancelled("job refresh, OPoI, or shutdown cancelled winner gather");
              if(witness_step>=walk_steps||witness.offsets[witness_step]!=offset)
                throw std::runtime_error("bulk witness offsets disagree with independent CPU gather");
              const auto &pair=witness.pairs[witness_step++];
              const auto &tile=pair[0];
              const auto sibling=(offset^1U)<tile_count?(offset^1U):offset;
              sibling_roots.push_back({sibling,sibling==offset?tile:pair[1]});
              merkle.prefetch_tile_path(offset);
              return tile;
            });
          });
          if(!gathered)throw PrefetchCancelled("OPoI or shutdown preempted winner gather");
          if(!may_continue()||gpu_ownership.opoi_pending())
            throw PrefetchCancelled("job refresh, OPoI, or shutdown cancelled Merkle root batch");
          // CPU-only hashing occurs after GPU admission is released. Preserve
          // every canonical byte and the independent CPU/CUDA/Rust checks.
          merkle.cache_tile_roots(sibling_roots);
          end=monotonic_ns();cpu_end=thread_cpu_ns();profile_phase(job.generation,nonce,"gguf_gather",phase,end,
                        "\"tiles\":256,\"bytes\":262144",cpu_end-cpu_phase);
          p.gather_wall_ms=static_cast<double>(end-phase)/1.0e6;
          p.gather_cpu_ms=static_cast<double>(cpu_end-cpu_phase)/1.0e6;
          phase=end;cpu_phase=cpu_end;
          if(!job.lease.valid()||stop.load(std::memory_order_relaxed))
            throw PrefetchCancelled("job lease or shutdown changed during tile gather");
          p.cpu=walk_v4_from_tile_sequence(p.seed,tile_count,p.input.tiles);
          if(p.cpu.offsets!=p.input.offsets)throw std::runtime_error("CPU reference offset disagreement");
          end=monotonic_ns();cpu_end=thread_cpu_ns();profile_phase(job.generation,nonce,"cpu_reference_walk",phase,end,{},cpu_end-cpu_phase);
          p.reference_cpu_ms=static_cast<double>(cpu_end-cpu_phase)/1.0e6;
          phase=end;cpu_phase=cpu_end;
          p.seeds.push_back(p.seed);std::unordered_map<std::uint64_t,std::uint32_t> map;map.reserve(walk_steps);
          p.tile_ids.reserve(walk_steps);p.slots.reserve(walk_steps);p.packed.reserve(walk_steps*tile_bytes);
          for(std::size_t step=0;step<p.input.offsets.size();++step){const auto offset=p.input.offsets[step];auto it=map.find(offset);if(it==map.end()){
            const auto slot=static_cast<std::uint32_t>(p.tile_ids.size());map.emplace(offset,slot);p.tile_ids.push_back(offset);p.slots.push_back(slot);
            const auto *tile=p.input.tiles.data()+step*tile_bytes;p.packed.insert(p.packed.end(),tile,tile+tile_bytes);
          }else p.slots.push_back(it->second);}
          end=monotonic_ns();cpu_end=thread_cpu_ns();profile_phase(job.generation,nonce,"host_pack",phase,end,
                        "\"unique_tiles\":"+std::to_string(p.tile_ids.size())+",\"redundant_gguf_reads\":0",cpu_end-cpu_phase);
          p.pack_cpu_ms=static_cast<double>(cpu_end-cpu_phase)/1.0e6;
          return p;
        };
        for(std::size_t base=0;base<1;base++) {
          if(!job.lease.valid()||stop.load(std::memory_order_relaxed)) break;
          if(!queue.admission_allowed(job.generation)) {
            if(!queue.wait_refresh_resolved(job.generation)) break;
          }
          if(nonce_cursor==std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("nonce scheduler exhausted");
          const auto nonce=winning_nonce;
          PreparedNonce prepared;
          try {
            if(prefetched&&prefetched->nonce==nonce&&job.lease.valid()&&queue.admission_allowed(job.generation)){
              prepared=std::move(*prefetched);prefetched.reset();
              std::cout<<"LIVE_CPU_PREFETCH=CONSUMED generation="<<job.generation<<" nonce="<<nonce<<" queue_bound=1\n"<<std::flush;
            }else{prefetched.reset();prepared=prepare_nonce(nonce,false);}
          }catch(const PrefetchCancelled&e){
            std::cout<<"LIVE_CUDA_STALE_DISCARD generation="<<job.generation<<" nonce="<<nonce
                     <<" stage=before_kernel reason=preparation_cancelled detail="<<e.what()<<"\n"<<std::flush;break;
          }
          const auto nonce_started_ns=prepared.started_ns;const auto seed=prepared.seed;
          const auto &binding=prepared.binding;const auto &input=prepared.input;const auto &cpu=prepared.cpu;
          const auto &seeds=prepared.seeds;const auto &tile_ids=prepared.tile_ids;
          const auto &slots=prepared.slots;const auto &packed=prepared.packed;
          if(!job.lease.valid()||stop.load(std::memory_order_relaxed)) {
            std::cout<<"LIVE_CUDA_STALE_DISCARD generation="<<job.generation<<" nonce="<<nonce<<" stage=before_kernel elapsed_ms="<<(monotonic_ns()-nonce_started_ns)/1.0e6<<"\n"<<std::flush;
            break;
          }
          if(!queue.admission_allowed(job.generation)) {
            if(!queue.wait_refresh_resolved(job.generation)) break;
            if(!job.lease.valid()) break;
          }
          const auto kernel_start_ns=monotonic_ns();
          telemetry.set_gpu_activity("CUDA_ACTIVE");
          CudaWalkBatch gpu;bool ran=false;const auto cuda_wall_start_ns=monotonic_ns();
          const bool may_prefetch=false&&base+1<count&&nonce!=std::numeric_limits<std::uint64_t>::max()&&
              job.lease.valid()&&!stop.load(std::memory_order_relaxed)&&!gpu_ownership.opoi_pending()&&
              queue.admission_allowed(job.generation);
          std::future<std::pair<bool,CudaWalkBatch>> cuda_future;
          if(may_prefetch){
            cuda_future=std::async(std::launch::async,[&](){CudaWalkBatch result;const bool executed=gpu_ownership.run_pom([&]{
              if(resident&&!resident->diagnostic_headroom_available())resident.reset();
              if(!engine){engine=std::make_unique<CudaWalkEngine>(std::string(gpu_uuid),1,true);cuda_engine_initializations.fetch_add(1,std::memory_order_relaxed);}
              engine->run(tile_count,model_identity,seeds,packed,tile_ids,slots,true,result);
});
              return std::make_pair(executed,std::move(result));});
            const auto prefetch_nonce=nonce+1;const auto prefetch_start=monotonic_ns();
            try{prefetched=prepare_nonce(prefetch_nonce,true);}
            catch(const PrefetchCancelled&e){prefetched.reset();std::cout<<"LIVE_CPU_PREFETCH=DISCARDED generation="
              <<job.generation<<" nonce="<<prefetch_nonce<<" reason="<<e.what()<<"\n"<<std::flush;}
            const auto prefetch_end=monotonic_ns();
            profile_phase(job.generation,prefetch_nonce,"bounded_cpu_lookahead",prefetch_start,prefetch_end,
                          std::string("\"overlapped_cuda\":true,\"completed\":")+(prefetched?"true":"false"));
            auto result=cuda_future.get();ran=result.first;gpu=std::move(result.second);
          }else{
            ran=gpu_ownership.run_pom([&]{
              const bool retained=resident&&resident->diagnostic_headroom_available();
              if(!retained)resident.reset();
              std::cout<<"RESIDENT_PROOF_RETENTION="<<(retained?"KEPT":"RELEASED")<<" diagnostic_reserve_preserved=true\n"<<std::flush;
              if(!engine){engine=std::make_unique<CudaWalkEngine>(std::string(gpu_uuid),1,true);cuda_engine_initializations.fetch_add(1,std::memory_order_relaxed);}
              // Retain the bounded diagnostic workspace between winners.
              // AI preemption releases it before admitting inference.
              engine->run(tile_count,model_identity,seeds,packed,tile_ids,slots,true,gpu);
});
          }
          if(!ran) {
            prefetched.reset();
            telemetry.set_gpu_activity("PAUSED_FOR_AI_OR_SHUTDOWN");
            telemetry.set_state("PAUSED", "IDLE", "OPOI_PREEMPTION_OR_SHUTDOWN", submit_enabled ? "ARMED" : "DISABLED");
            std::cout<<"LIVE_CUDA_PAUSED reason=opoi_or_shutdown generation="<<job.generation<<'\n'<<std::flush;
            break;
          }
          const auto cuda_wall_end_ns=monotonic_ns();
          telemetry.set_gpu_activity("CPU_PROOF_AND_VERIFICATION");
          telemetry.record_cuda_times(gpu.kernel_ms, gpu.transfer_ms, gpu.device_to_host_ms, gpu.total_ms);
          profile_phase(job.generation,nonce,"cuda_run_wall",cuda_wall_start_ns,cuda_wall_end_ns,
                        "\"kernel_ms\":"+std::to_string(gpu.kernel_ms)+",\"h2d_bytes\":"+
                        std::to_string(gpu.h2d_bytes)+",\"h2d_ms\":"+
                        std::to_string(gpu.transfer_ms)+",\"d2h_ms\":"+
                        std::to_string(gpu.device_to_host_ms)+",\"cache_scatter_ms\":"+
                        std::to_string(gpu.cache_scatter_ms)+",\"cuda_engine_total_ms\":"+
                        std::to_string(gpu.total_ms));

          auto phase_ns=monotonic_ns();auto phase_cpu_ns=thread_cpu_ns();
          const auto &walk=verified_walk_view(cpu,gpu.offsets,gpu.diagnostic_states);
          // Parity is established; proof/artifact construction borrows the CPU
          // witness. Release redundant staging before allocating Merkle/Borsh
          // buffers, rather than retaining it through Rust and submission.
          std::vector<std::uint8_t>().swap(gpu.diagnostic_states);
          std::vector<std::uint8_t>().swap(prepared.packed);
          std::vector<std::uint64_t>().swap(prepared.tile_ids);
          std::vector<std::uint32_t>().swap(prepared.slots);
          auto phase_end_ns=monotonic_ns();auto phase_cpu_end_ns=thread_cpu_ns();
          profile_phase(job.generation,nonce,"cpu_cuda_parity",phase_ns,phase_end_ns,
                        "\"states\":257,\"offsets\":256",phase_cpu_end_ns-phase_cpu_ns);
          phase_ns=phase_end_ns;phase_cpu_ns=phase_cpu_end_ns;
          const auto final=final_state(walk);const auto pow=pom_pow_value_h3(final,pph);
          const bool target_met=little_endian_leq(pow,target.little_endian);
          if(target_met!=expected_winner)throw std::runtime_error("resident GPU/independent scalar CPU target decision mismatch");
          if(target_met) telemetry.target_satisfied();
          phase_end_ns=monotonic_ns();
          profile_phase(job.generation,nonce,"walk_copy_final_state_target",phase_ns,phase_end_ns,
                        std::string("\"target_satisfied\":")+(target_met?"true":"false"),
                        thread_cpu_ns()-phase_cpu_ns);
          const bool current_after_kernel=job.lease.valid()&&!stop.load(std::memory_order_relaxed)&&
              queue.wait_refresh_resolved(job.generation);
          if(!current_after_kernel&&!target_met) {
            telemetry.work_stale();
            std::cout<<"LIVE_CUDA_STALE_DISCARD generation="<<job.generation<<" nonce="<<nonce
                     <<" stage=after_kernel elapsed_ms="<<(monotonic_ns()-nonce_started_ns)/1.0e6
                     <<" kernel_ms="<<(monotonic_ns()-kernel_start_ns)/1.0e6<<"\n"<<std::flush;
            break;
          }
          const auto proof_start_ns=monotonic_ns();
          const auto proof_cpu_start_ns=thread_cpu_ns();
          std::uint64_t merkle_path_ns=0;
          std::uint64_t merkle_path_cpu_ns=0;
          const auto merkle_cache_before=merkle.path_cache_stats();
          auto proof=prove_v4_from_walk(walk,header.pom_tier,input.tiles,[&](std::uint64_t offset){
            const auto path_start=monotonic_ns();const auto path_cpu_start=thread_cpu_ns();
            auto path=merkle.tile_path(offset);merkle_path_ns+=monotonic_ns()-path_start;
            merkle_path_cpu_ns+=thread_cpu_ns()-path_cpu_start;return path;});
          const auto after_proof_assembly_ns=monotonic_ns();
          const auto merkle_cache_after=merkle.path_cache_stats();
          profile_phase(job.generation,nonce,"merkle_paths",proof_start_ns,proof_start_ns+merkle_path_ns,
                        "\"path_count\":256,\"cache_hits\":"+
                        std::to_string(merkle_cache_after.hits-merkle_cache_before.hits)+",\"cache_misses\":"+
                        std::to_string(merkle_cache_after.misses-merkle_cache_before.misses)+",\"cache_entries\":"+
                        std::to_string(merkle_cache_after.entries),merkle_path_cpu_ns);
          telemetry.record_host_times(prepared.gather_wall_ms, prepared.gather_cpu_ms,
                                      prepared.reference_cpu_ms, prepared.pack_cpu_ms,
                                      static_cast<double>(merkle_path_ns)/1.0e6,
                                      static_cast<double>(merkle_path_cpu_ns)/1.0e6);
          profile_phase(job.generation,nonce,"proof_assembly",proof_start_ns+merkle_path_ns,
                        after_proof_assembly_ns,{},thread_cpu_ns()-proof_cpu_start_ns-merkle_path_cpu_ns);
          phase_ns=after_proof_assembly_ns;phase_cpu_ns=thread_cpu_ns();
          const auto wire=borsh_v4_container(proof,final,pow);
          telemetry.proof_completed();
          phase_end_ns=monotonic_ns();
          profile_phase(job.generation,nonce,"borsh_serialize",phase_ns,phase_end_ns,
                        "\"proof_bytes\":"+std::to_string(wire.size()),thread_cpu_ns()-phase_cpu_ns);
          phase_ns=phase_end_ns;phase_cpu_ns=thread_cpu_ns();
          const auto proof_build_ms=(monotonic_ns()-proof_start_ns)/1.0e6;
          telemetry.record_proof_times(proof_build_ms, 0.0);
          const bool current_after_proof=job.lease.valid()&&binding.lease.valid()&&
              !stop.load(std::memory_order_relaxed)&&queue.wait_refresh_resolved(job.generation);
          if(!current_after_proof&&!target_met) {
            telemetry.proof_stale();
            std::cout<<"LIVE_CUDA_STALE_DISCARD generation="<<job.generation<<" nonce="<<nonce<<" stage=after_proof proof_build_ms="<<proof_build_ms<<" elapsed_ms="<<(monotonic_ns()-nonce_started_ns)/1.0e6<<"\n"<<std::flush;
            break;
          }
          const auto name=safe_name(job.generation,nonce);const auto proof_path=outdir/(name+".proof");
          const auto context_path=outdir/(name+".context");const auto trace_path=outdir/(name+".trace");
          for(const auto &p:{proof_path,context_path,trace_path}) if(fs::exists(p)) throw std::runtime_error("refusing to overwrite existing live proof artifact");
          const ProofArtifactContext artifact_context{seed,gguf.chunk_count(),header.timestamp,nonce,
              header.daa_score,job.generation,final,header.pom_tier,official_root,target.little_endian,
              pph,response_digest,pow};
          const auto artifacts=encode_proof_artifacts(artifact_context,walk,wire.size());
          {std::ofstream f(proof_path,std::ios::binary);put(f,wire.data(),wire.size());}
          {std::ofstream f(context_path,std::ios::binary);put(f,artifacts.context_prefix.data(),artifacts.context_prefix.size());put(f,wire.data(),wire.size());}
          {std::ofstream f(trace_path,std::ios::binary);put(f,artifacts.trace.data(),artifacts.trace.size());}
          phase_end_ns=monotonic_ns();
          profile_phase(job.generation,nonce,"artifact_write",phase_ns,phase_end_ns,{},thread_cpu_ns()-phase_cpu_ns);
          if(!job.lease.valid()) {
            if(!target_met){fs::remove(proof_path);fs::remove(context_path);fs::remove(trace_path);}
            telemetry.proof_stale();
            std::cout<<"LIVE_CUDA_STALE_DISCARD generation="<<job.generation<<" nonce="<<nonce
                     <<" stage=after_artifact_write elapsed_ms="<<(monotonic_ns()-nonce_started_ns)/1.0e6
                     <<" target_satisfied="<<(target_met?"true":"false")
                     <<" rust=NOT_RUN submit=DISABLED proof_preserved="<<(target_met?"true":"false")<<"\n"<<std::flush;
            break;
          }
          const auto rust_start_ns=monotonic_ns();
          const auto rust_result=rust_verifier.verify(artifacts.context_prefix,artifacts.trace,wire);
          const auto rust_child_cpu_ms=rust_result.cpu_ms;
          std::cout<<rust_result.summary<<"\n"<<std::flush;
          telemetry.proof_rust_verified();
          startup_parity_verified=true;
          const auto rust_verify_ms=(monotonic_ns()-rust_start_ns)/1.0e6;
          telemetry.record_proof_times(proof_build_ms, rust_verify_ms);
          profile_phase(job.generation,nonce,"official_rust_verify",rust_start_ns,monotonic_ns(),
                        "\"verifier_child_cpu_ms\":"+std::to_string(rust_child_cpu_ms));
          const bool current_after_rust=live_selection->allows(LiveGpuSelection::Nvidia,primary_selection_token)&&job.lease.valid()&&!stop.load(std::memory_order_relaxed)&&
              queue.wait_refresh_resolved(job.generation);
          if(!current_after_rust) {
            if(!target_met) {
              telemetry.proof_stale();
              fs::remove(proof_path);fs::remove(context_path);fs::remove(trace_path);
              std::cout<<"LIVE_CUDA_STALE_DISCARD generation="<<job.generation<<" nonce="<<nonce
                       <<" stage=after_rust_verify rust_verify_ms="<<rust_verify_ms
                       <<" elapsed_ms="<<(monotonic_ns()-nonce_started_ns)/1.0e6<<"\n"<<std::flush;
              break;
            }
            std::cout<<"LIVE_CUDA_STALE_WINNER_VERIFIED generation="<<job.generation<<" nonce="<<nonce
                     <<" rust=PASS target_satisfied=true submit=DISABLED proof_preserved=true\n"<<std::flush;
            break;
          }
          const std::array<fs::path,3> verified_artifacts{proof_path,context_path,trace_path};
          release_verified_nonwinner_artifacts(verified_artifacts,true,target_met);
          telemetry.proof_fresh(false);
          const auto now_ns=monotonic_ns();
          std::cout<<"LIVE_CUDA_NONCE=PASS generation="<<job.generation<<" nonce="<<nonce
                   <<" verification_kind="<<(expected_winner?"WINNER":"STARTUP_PARITY")<<" states=257 offsets=256 rust=PASS target_satisfied="<<(target_met?"true":"false")
                   <<" cuda_kernel_ms="<<gpu.kernel_ms<<" h2d_bytes="<<gpu.h2d_bytes
                   <<" proof_build_ms="<<proof_build_ms<<" rust_verify_ms="<<rust_verify_ms
                   <<" nonce_total_ms="<<(now_ns-nonce_started_ns)/1.0e6
                   <<" template_to_kernel_ms="<<(job.fields.received_monotonic_ns&&now_ns>=job.fields.received_monotonic_ns?(kernel_start_ns-job.fields.received_monotonic_ns)/1.0e6:0.0)
                   <<" cuda_engine_instances="<<cuda_engine_initializations.load(std::memory_order_relaxed)
                   <<" submit="<<(submit_enabled?"ARMED":"DISABLED")<<"\n"<<std::flush;
          if(target_met) {
            if(!live_selection->allows(LiveGpuSelection::Nvidia,primary_selection_token)||!job.lease.valid()||stop.load(std::memory_order_relaxed)||
               !queue.submission_allowed(job.generation))
              { std::cout<<"LIVE_CUDA_STALE_WINNER_VERIFIED generation="<<job.generation<<" nonce="<<nonce<<" stage=submission_gate rust=PASS submit=DISABLED\n"<<std::flush;break; }
            if(!submit_enabled) {
              std::cout<<"LIVE_SUBMIT=DISABLED_WINNER candidate_valid=true client_invoked=false\n"<<std::flush;
            } else {
            network::VerifiedSubmission verified;
            verified.job_id=hex(response_digest.data(),response_digest.size());
            verified.generation=job.generation;
            verified.nonce=nonce;
            verified.pom_verified=true;       // official Rust verifier returned success above
            verified.model_verified=true;     // installed GGUF SHA/sidecar and H6/H14 anchor checked
            verified.merkle_verified=true;    // proof paths were built from the verified index
            verified.header_seed_verified=candidate_matches_job(binding,job);
            verified.target_satisfied=true;   // computed from final PoM pow and template bits
            verified.job_fresh=job.lease.valid();
            verified.generation_matches=binding.generation==job.generation;
            verified.miner_configured=!job.fields.pay_address.empty()&&
                job.fields.pay_address==pay&&opoi::valid_coinbase_extra_data(job.fields.extra_data,extra);
            const std::string proof_bytes(reinterpret_cast<const char*>(wire.data()),wire.size());
            network::PomWinner winner{std::move(verified),header.daa_score,final,
                                      header.pom_tier,proof_bytes};
            telemetry.set_state("ACTIVE", "IDLE", "", "SUBMIT_SENT");
            const auto result=network::submit_pom_winner(
                  *submit_client,block,winner,job.fields.pay_address,
                  job.fields.extra_data,submit_enabled);
              telemetry.set_state("ACTIVE", "IDLE", "", network::submission_state_name(result.state));
              std::osyncstream(std::cout)<<"LIVE_SUBMIT_RESULT state="<<network::submission_state_name(result.state)
                       <<" node_error_hex="<<hex(reinterpret_cast<const std::uint8_t*>(result.node_error.data()),std::min<std::size_t>(result.node_error.size(),1024))<<" reject_reason="<<result.reason<<" hash="<<result.block_hash<<" nonce="<<nonce<<" submit=ENABLED\n"<<std::flush;
              const auto timing_value=[](const std::optional<double> &v){return v?std::to_string(*v):std::string("null");};
              std::osyncstream(std::cout)<<"{\"event\":\"POM_SUBMISSION_TIMING\",\"clock\":\"steady_monotonic\",\"hash\":\""<<result.block_hash
                <<"\",\"state\":\""<<network::submission_state_name(result.state)<<"\",\"channel_reused\":"<<(result.timing.channel_reused?"true":"false")
                <<",\"assembly_ms\":"<<timing_value(result.timing.assembly_ms)<<",\"validation_ms\":"<<timing_value(result.timing.validation_ms)
                <<",\"connect_ms\":"<<timing_value(result.timing.connect_ms)<<",\"stream_ms\":"<<timing_value(result.timing.stream_ms)
                <<",\"prepare_ms\":"<<timing_value(result.timing.prepare_ms)<<",\"write_ms\":"<<timing_value(result.timing.write_ms)
                <<",\"response_ms\":"<<timing_value(result.timing.response_ms)<<",\"finish_ms\":"<<timing_value(result.timing.finish_ms)
                <<",\"total_ms\":"<<timing_value(result.timing.total_ms)<<"}\n"<<std::flush;
              observe_submission(result);
            }
          }

        }
      } catch(const std::exception &e) {std::cerr<<"LIVE_JOB_FAILED generation="<<job.generation<<" detail="<<e.what()<<"\n";throw;}
    }
    request_worker_stop(stop,gpu_ownership,queue);
#ifdef NEXUS_ENABLE_SECONDARY_VULKAN
    if(secondary_lane){secondary_lane->request_stop();secondary_lane->join();secondary_lane->rethrow_if_failed();}
#endif
    if(telemetry_started&&telemetry_thread.joinable()) telemetry_thread.join();
    if(ingress.joinable())ingress.join();
    if(escrow_claim_task.valid())escrow_claim_task.get();
    if(shutdown_watcher.joinable())shutdown_watcher.join();
    if(strike_monitor) strike_monitor->stop();
    if(escrow_watcher) escrow_watcher->flush();
    std::cout<<"LIVE_CUDA_WORKER=STOP jobs_seen="<<jobs_seen<<" verified_nonces="<<completed
             <<" cuda_engine_instances="<<cuda_engine_initializations.load(std::memory_order_relaxed)<<" cooperative_shutdown=true submit="<<(submit_enabled?"ENABLED":"DISABLED")<<"\n";
    return nonce_limit==0||completed==nonce_limit?0:3;
  } catch(const std::exception &e) {
    startup.fail_current(e.what());
    request_worker_stop(stop,gpu_ownership,queue);
    if(telemetry_started&&telemetry_thread.joinable()) telemetry_thread.join();
    if(ingress.joinable())ingress.join();
    if(escrow_claim_task.valid()) {
      try{escrow_claim_task.get();}catch(const std::exception&claim_error){std::cerr<<"ESCROW_CLAIM_TASK=FAIL detail="<<claim_error.what()<<"\n"<<std::flush;}
    }
    if(shutdown_watcher.joinable())shutdown_watcher.join();
    if(strike_monitor) strike_monitor->stop();
    if(escrow_watcher) {
      try { escrow_watcher->flush(); }
      catch (const std::exception &flush_error) {
        std::cerr << "ESCROW_FINAL_FLUSH=FAIL detail=" << flush_error.what() << "\n" << std::flush;
      }
    }
    std::cerr<<"LIVE_CUDA_WORKER=BLOCKED_OR_FAIL detail="<<e.what()<<" submit="<<(submit_enabled?"ENABLED":"DISABLED")<<"\n";return 1;
  }
}
