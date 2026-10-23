#include "keryx/nexus/developer_config.hpp"
#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/consensus/cuda_engine.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/pipeline.hpp"
#include "messages.pb.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <unordered_map>

using namespace keryx::nexus;
using namespace keryx::nexus::consensus;
namespace fs = std::filesystem;
namespace {
constexpr std::uint64_t nonce_base = 0x4b455259584d3231ULL;
constexpr std::string_view gpu_uuid = "" NEXUS_PRIMARY_GPU_UUID "";
constexpr std::size_t max_microbatch_host_budget = 64ULL * 1024 * 1024;

void put(std::ostream &o, const void *p, std::size_t n) {
  o.write(static_cast<const char *>(p), static_cast<std::streamsize>(n));
  if (!o) throw std::runtime_error("artifact write failed");
}
void u32(std::ostream &o, std::uint32_t x) {
  std::array<char, 4> b{};
  for (unsigned i = 0; i < 4; ++i) b[i] = static_cast<char>(x >> (8 * i));
  put(o, b.data(), b.size());
}
void u64(std::ostream &o, std::uint64_t x) {
  std::array<char, 8> b{};
  for (unsigned i = 0; i < 8; ++i) b[i] = static_cast<char>(x >> (8 * i));
  put(o, b.data(), b.size());
}
void hash(std::ostream &o, const Hash &h) { put(o, h.data(), h.size()); }
void ensure_new(const fs::path &p) {
  if (fs::exists(p)) throw std::runtime_error("refusing to overwrite artifact: " + p.string());
}
std::pair<std::uint64_t, std::uint64_t> current_rss_pss_kib() {
  std::ifstream in("/proc/self/smaps_rollup");
  if (!in) throw std::runtime_error("cannot read process smaps_rollup for RSS/PSS accounting");
  std::uint64_t rss = 0, pss = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream row(line);
    std::string key, unit;
    std::uint64_t value = 0;
    if (!(row >> key >> value >> unit)) continue;
    if (key == "Rss:") rss = value;
    else if (key == "Pss:") pss = value;
  }
  if (!in.eof() || rss == 0 || pss == 0)
    throw std::runtime_error("smaps_rollup lacks valid RSS/PSS values");
  return {rss, pss};
}
std::array<std::uint8_t, tile_bytes> read_tile(const model::ReadOnlyGguf &gguf,
                                               std::uint64_t offset) {
  auto v = gguf.read_tile(offset);
  std::array<std::uint8_t, tile_bytes> out{};
  std::copy(v.begin(), v.end(), out.begin());
  return out;
}
void write_v2_header(std::ostream &f, std::size_t logical_batch,
                     std::uint64_t ntiles, std::uint64_t timestamp,
                     std::uint64_t daa, const Hash &pre_pow,
                     const Hash &target, std::uint64_t batch_nonce_base) {
  put(f, "NXCB", 4);
  u32(f, 2);
  u32(f, static_cast<std::uint32_t>(logical_batch));
  u64(f, ntiles);
  u64(f, batch_nonce_base);
  u64(f, timestamp);
  u64(f, daa);
  hash(f, pre_pow);
  hash(f, target);
}
void write_v2_nonce(std::ostream &f, std::uint64_t seed, std::uint64_t nonce,
                    const ModelWalkInput &input, const CudaWalkBatch &gpu,
                    std::size_t local_nonce) {
  u64(f, seed);
  u64(f, nonce);
  put(f, input.tiles.data(), input.tiles.size());
  for (std::size_t step = 0; step < walk_steps; ++step)
    u64(f, gpu.offsets[local_nonce * walk_steps + step]);
  const auto state_stride = (walk_steps + 1) * tile_bytes;
  put(f, gpu.diagnostic_states.data() + local_nonce * state_stride, state_stride);
}
std::size_t requested_microbatch(std::size_t logical_batch, int argc, char **argv) {
  std::size_t requested = 4;
  if (argc >= 8) requested = static_cast<std::size_t>(std::stoul(argv[7]));
  else if (const char *env = std::getenv("KERYX_CUDA_MICROBATCH"))
    requested = static_cast<std::size_t>(std::stoul(env));
  if (requested == 0 || requested > 8 || requested > logical_batch)
    throw std::runtime_error("microbatch must be between 1 and min(8, logical batch)");
  // Conservative upper bound includes input tile copies, CPU states, CUDA trace,
  // offsets/slots and container overhead. Device buffers have a separate VRAM check.
  constexpr std::size_t per_nonce_budget = 6 * 256 * 1024 + 64 * 1024;
  if (requested > max_microbatch_host_budget / per_nonce_budget)
    throw std::runtime_error("microbatch exceeds the 64 MiB host allocation budget");
  return requested;
}
}

int main(int argc, char **argv) {
  try {
    if (argc < 7 || argc > 9)
      throw std::runtime_error("usage: nexus-cuda-real-model CAPTURE_OR_DIRECTORY MODEL.gguf TREE.bin TREE.json SEGMENT_COUNT OUTPUT_DIR [MICROBATCH [NONCE_START_OFFSET]]");
    const fs::path capture_input = argv[1], model_path = argv[2], tree_path = argv[3],
                   meta_path = argv[4], outdir = argv[6];
    const auto batch = static_cast<std::size_t>(std::stoul(argv[5]));
    if (batch == 0 || batch > 256)
      throw std::runtime_error("segment count must be in [1,256]");
    const auto nonce_start_offset = argc == 9 ? std::stoull(argv[8]) : 0ULL;
    if (nonce_start_offset > std::numeric_limits<std::uint64_t>::max() - nonce_base - batch)
      throw std::runtime_error("nonce start range overflows uint64");
    const auto microbatch = requested_microbatch(batch, argc, argv);
    std::vector<fs::path> captures;
    if (fs::is_directory(capture_input)) {
      for (const auto &entry : fs::directory_iterator(capture_input))
        if (entry.is_regular_file() && entry.path().extension() == ".capture")
          captures.push_back(entry.path());
      std::sort(captures.begin(), captures.end());
      if (captures.size() < 2)
        throw std::runtime_error("persistent job session requires at least two capture files");
    } else {
      captures.push_back(capture_input);
    }
    model::ReadOnlyGguf model(model_path);
    const Hash official_model{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
    const Hash official_root{0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};
    const auto sidecar = model.verify_sidecar(tree_path, meta_path, official_model, official_root, 203469888);
    if (sidecar.declared_root != official_root)
      throw std::runtime_error("GGUF sidecar is not anchored to official H6 root");
    const auto ntiles = model.chunk_count() / 32;
    model::SparseMerkleIndex merkle(model, tree_path);
    CudaWalkEngine cuda_engine(std::string(gpu_uuid), microbatch);
    JobPipeline jobs;
    const auto cache_identity = hex(official_model.data(), official_model.size()) +
                                hex(official_root.data(), official_root.size());
    std::size_t completed_jobs = 0;
    std::uint64_t previous_job_nonce_end = 0;
    std::cout << "CUDA_PERSISTENT_SESSION=START jobs=" << captures.size()
              << " engine_capacity=" << microbatch << " device_uuid=" << gpu_uuid << '\n';
    for (std::size_t capture_index = 0; capture_index < captures.size(); ++capture_index) {
    const auto &capture_path = captures[capture_index];
    if (capture_index > (std::numeric_limits<std::uint64_t>::max() - nonce_start_offset) / batch)
      throw std::runtime_error("multi-job nonce offset overflows uint64");
    const auto job_nonce_offset = nonce_start_offset + capture_index * batch;
    if (job_nonce_offset > std::numeric_limits<std::uint64_t>::max() - nonce_base - batch)
      throw std::runtime_error("job nonce range overflows uint64");
    const auto segment_nonce_base = nonce_base + job_nonce_offset;
    if (capture_index > 0 && segment_nonce_base <= previous_job_nonce_end)
      throw std::runtime_error("persistent session would reuse a nonce across jobs");
    const auto started = std::chrono::steady_clock::now();
    const auto records = read_capture(capture_path);
    const CaptureRecord *selected = nullptr;
    protowire::KaspadResponse response;
    for (const auto &r : records) {
      protowire::KaspadResponse candidate;
      if (!candidate.ParseFromArray(r.response_wire.data(), static_cast<int>(r.response_wire.size())) ||
          !candidate.has_getblocktemplateresponse()) continue;
      const auto &t = candidate.getblocktemplateresponse();
      if (r.network == "mainnet" && t.issynced() && t.has_block() && t.block().has_header()) {
        selected = &r;
        response = std::move(candidate);
        break;
      }
    }
    if (!selected) throw std::runtime_error("no synced mainnet template in verified capture");
    const auto &templ = response.getblocktemplateresponse();
    const auto &block = templ.block();
    const auto header = header_from_rpc(block.header());
    const auto anchor = official_tier_anchor(header.daa_score, header.pom_tier);
    if (anchor.root != official_root || anchor.chunks != model.chunk_count())
      throw std::runtime_error("template DAA/tier does not select installed H6 model");
    const auto pph = pre_pow_hash(header);
    const auto target = decode_compact_target(header.bits);
    if (target.zero) throw std::runtime_error("invalid compact target in captured mainnet template");
    TemplateFields fields;
    fields.network = selected->network;
    fields.response_wire = selected->response_wire;
    const auto block_wire = block.SerializeAsString();
    fields.block_wire.assign(block_wire.begin(), block_wire.end());
    fields.daa_score = header.daa_score;
    fields.compact_target_bits = header.bits;
    fields.timestamp = header.timestamp;
    fields.template_nonce = header.nonce;
    fields.node_synced = templ.issynced();
    fields.inference_challenge = templ.inferencechallenge();
    require_mining_eligible(fields);
    const auto job = jobs.accept(std::move(fields));

    const auto job_outdir = captures.size() > 1 ? outdir / capture_path.stem() : outdir;
    fs::create_directories(job_outdir);
    const auto final_batch_path = job_outdir / ("batch-" + std::to_string(batch) + ".nxcb");
    const auto partial_batch_path = job_outdir / ("batch-" + std::to_string(batch) + ".nxcb.partial");
    ensure_new(final_batch_path);
    ensure_new(partial_batch_path);
    std::ofstream batch_file(partial_batch_path, std::ios::binary);
    if (!batch_file) throw std::runtime_error("cannot create streaming v2 batch artifact");
    write_v2_header(batch_file, batch, ntiles, header.timestamp, header.daa_score,
                    pph, target.little_endian, segment_nonce_base);

    std::uint64_t first_seed = 0;
    std::size_t verified = 0, target_candidates = 0, unique_total = 0;
    double tile_io_ms = 0, cpu_ms = 0, h2d_ms = 0, d2h_ms = 0,
           scatter_ms = 0, kernel_ms = 0, proof_ms = 0;
    std::size_t gpu_cache_hits = 0, gpu_cache_misses = 0, gpu_cache_bytes = 0,
                pinned_host_bytes = 0, h2d_bytes = 0;
    std::size_t max_host_working_set_bytes = 0;
    std::size_t cuda_allocated_bytes = 0, cuda_free_bytes = 0;
    CudaWalkBatch gpu;

    for (std::size_t base = 0; base < batch; base += microbatch) {
      const auto count = std::min(microbatch, batch - base);
      const auto logical_start = base;
      const auto cache_capacity = count * walk_steps;
      std::vector<std::uint64_t> seeds(count);
      std::vector<CandidateBinding> bindings;
      std::vector<ModelWalkInput> inputs;
      std::vector<std::uint8_t> unique_tiles;
      std::vector<std::uint64_t> unique_tile_ids;
      std::vector<std::uint32_t> tile_slots;
      std::unordered_map<std::uint64_t, std::uint32_t> cache_slots;
      bindings.reserve(count);
      inputs.reserve(count);
      unique_tiles.reserve(cache_capacity * tile_bytes);
      unique_tile_ids.reserve(cache_capacity);
      tile_slots.reserve(cache_capacity);
      cache_slots.reserve(cache_capacity);
      const auto gather_start = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < count; ++i) {
        const auto nonce = segment_nonce_base + logical_start + i;
        seeds[i] = block_seed(pph, header.timestamp, nonce, header.daa_score);
        bindings.push_back(bind_candidate(job, nonce, header.timestamp, pph));
        if (!candidate_matches_job(bindings.back(), job))
          throw std::runtime_error("candidate/job generation binding failed before tile fetch");
        inputs.push_back(gather_v4_model_tiles(seeds[i], ntiles, [&](std::uint64_t offset) {
          const auto found = cache_slots.find(offset);
          if (found == cache_slots.end()) {
            auto tile = read_tile(model, offset);
            const auto slot = static_cast<std::uint32_t>(unique_tiles.size() / tile_bytes);
            cache_slots.emplace(offset, slot);
            unique_tile_ids.push_back(offset);
            unique_tiles.insert(unique_tiles.end(), tile.begin(), tile.end());
            tile_slots.push_back(slot);
            return tile;
          }
          const auto slot = found->second;
          tile_slots.push_back(slot);
          std::array<std::uint8_t, tile_bytes> tile{};
          std::copy_n(unique_tiles.data() + static_cast<std::size_t>(slot) * tile_bytes,
                      tile_bytes, tile.data());
          return tile;
        }));
      }
      tile_io_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - gather_start).count();
      if (tile_slots.size() != count * walk_steps || unique_tiles.size() % tile_bytes != 0)
        throw std::runtime_error("bounded microbatch tile cache metadata mismatch");

      const auto cpu_start = std::chrono::steady_clock::now();
      std::vector<Walk> cpu_walks;
      cpu_walks.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        auto walk = walk_v4_from_tile_sequence(seeds[i], ntiles, inputs[i].tiles);
        if (walk.offsets != inputs[i].offsets)
          throw std::runtime_error("CPU gather/replay offset mismatch nonce=" +
                                   std::to_string(segment_nonce_base + logical_start + i));
        cpu_walks.push_back(std::move(walk));
      }
      cpu_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - cpu_start).count();

      cuda_engine.run(ntiles, cache_identity, seeds, unique_tiles, unique_tile_ids,
                      tile_slots, true, gpu);
      h2d_ms += gpu.transfer_ms;
      scatter_ms += gpu.cache_scatter_ms;
      d2h_ms += gpu.device_to_host_ms;
      kernel_ms += gpu.kernel_ms;
      gpu_cache_hits += gpu.tile_cache_hits;
      gpu_cache_misses += gpu.tile_cache_misses;
      gpu_cache_bytes = gpu.tile_cache_vram_bytes;
      pinned_host_bytes = gpu.pinned_host_bytes;
      h2d_bytes += gpu.h2d_bytes;
      cuda_allocated_bytes = gpu.cuda_allocated_bytes;
      cuda_free_bytes = gpu.device.free_memory;
      unique_total += gpu.unique_tile_count;
      const auto microbatch_working_set_bytes =
          unique_tiles.capacity() + tile_slots.capacity() * sizeof(std::uint32_t) +
          inputs.capacity() * sizeof(ModelWalkInput) +
          count * (walk_steps + 1) * tile_bytes * 3;
      max_host_working_set_bytes = std::max(max_host_working_set_bytes,
                                             microbatch_working_set_bytes);

      for (std::size_t i = 0; i < count; ++i) {
        const auto nonce = segment_nonce_base + logical_start + i;
        for (std::size_t step = 0; step < walk_steps; ++step) {
          const auto actual = gpu.offsets[i * walk_steps + step];
          if (actual != cpu_walks[i].offsets[step])
            throw std::runtime_error("offset mismatch nonce=" + std::to_string(nonce) +
                " iteration=" + std::to_string(step + 1) + " cpu=" +
                std::to_string(cpu_walks[i].offsets[step]) + " gpu=" + std::to_string(actual));
        }
        for (std::size_t step = 0; step <= walk_steps; ++step) {
          const auto *actual = gpu.diagnostic_states.data() +
              (i * (walk_steps + 1) + step) * tile_bytes;
          const auto &expected = cpu_walks[i].states[step];
          const auto diff = std::mismatch(expected.begin(), expected.end(), actual);
          if (diff.first != expected.end())
            throw std::runtime_error("state mismatch nonce=" + std::to_string(nonce) +
                " iteration=" + std::to_string(step) + " byte=" +
                std::to_string(diff.first - expected.begin()));
        }
        if (!candidate_matches_job(bindings[i], job) || !bindings[i].lease.valid())
          throw std::runtime_error("stale job generation for CUDA result nonce=" + std::to_string(nonce));
        write_v2_nonce(batch_file, seeds[i], nonce, inputs[i], gpu, i);
        ++verified;
        const auto final = final_state(cpu_walks[i]);
        if (little_endian_leq(pom_pow_value_h3(final, pph), target.little_endian))
          ++target_candidates;
        if (logical_start + i == 0) first_seed = seeds[i];
      }
      // Reconstruct before this microbatch's seeds/tiles go out of scope and
      // before the reusable GPU output buffers are overwritten by the next one.
      const auto proof_start = std::chrono::steady_clock::now();
      for (std::size_t i = 0; i < count; ++i) {
        const auto nonce = segment_nonce_base + logical_start + i;
        const auto binding = bind_candidate(job, nonce, header.timestamp, pph);
        if (!candidate_matches_job(binding, job) || !binding.lease.valid() || !job.lease.valid())
          throw std::runtime_error("job lease became stale during proof reconstruction nonce=" +
                                   std::to_string(nonce));
        Walk gpu_walk;
        gpu_walk.offsets.assign(gpu.offsets.begin() + static_cast<std::ptrdiff_t>(i * walk_steps),
                                gpu.offsets.begin() + static_cast<std::ptrdiff_t>((i + 1) * walk_steps));
        gpu_walk.states.resize(walk_steps + 1);
        for (std::size_t s = 0; s <= walk_steps; ++s)
          std::copy_n(gpu.diagnostic_states.data() +
                          (i * (walk_steps + 1) + s) * tile_bytes,
                      tile_bytes, gpu_walk.states[s].data());
        const auto state = final_state(gpu_walk);
        const auto pow = pom_pow_value_h3(state, pph);
        auto proof = prove_v4_from_walk(gpu_walk, header.pom_tier, inputs[i].tiles,
            [&](std::uint64_t offset) { return merkle.tile_path(offset); });
        const auto wire = borsh_v4_container(proof, state, pow);
        const auto prefix = job_outdir / ("proof-nonce-" + std::to_string(nonce));
        const auto proof_path = fs::path(prefix.string() + ".bin");
        const auto context_path = fs::path(prefix.string() + ".context");
        const auto trace_path = fs::path(prefix.string() + ".trace");
        for (const auto &path : {proof_path, context_path, trace_path}) ensure_new(path);
        { std::ofstream f(proof_path, std::ios::binary); put(f, wire.data(), wire.size()); }
        { std::ofstream f(context_path, std::ios::binary); put(f, "NXRF", 4); u64(f, seeds[i]);
          u64(f, model.chunk_count()); put(f, &header.pom_tier, 1); hash(f, official_root);
          hash(f, target.little_endian); hash(f, pph); u64(f, header.timestamp); u64(f, nonce);
          u64(f, header.daa_score); u64(f, job.generation); hash(f, selected->digest);
          u64(f, state); hash(f, pow); u32(f, static_cast<std::uint32_t>(wire.size()));
          put(f, wire.data(), wire.size()); }
        { std::ofstream f(trace_path, std::ios::binary); put(f, "NXST", 4); u64(f, seeds[i]);
          u64(f, nonce); u32(f, static_cast<std::uint32_t>(gpu_walk.states.size()));
          u32(f, static_cast<std::uint32_t>(gpu_walk.offsets.size()));
          for (const auto &s : gpu_walk.states) put(f, s.data(), s.size());
          for (auto offset : gpu_walk.offsets) u64(f, offset); }
      }
      proof_ms += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - proof_start).count();
    }
    batch_file.flush();
    if (!batch_file) throw std::runtime_error("streamed batch v2 flush failed");
    batch_file.close();
    if (verified != batch) throw std::runtime_error("logical batch completion count mismatch");
    fs::rename(partial_batch_path, final_batch_path);

    if (job_nonce_offset == 0) {
      const auto first_nonce = job_outdir / ("proof-nonce-" + std::to_string(segment_nonce_base));
      for (const auto &[source, suffix] :
           std::array<std::pair<fs::path, const char *>, 3>{{
               {fs::path(first_nonce.string() + ".bin"), ".bin"},
               {fs::path(first_nonce.string() + ".context"), ".context"},
               {fs::path(first_nonce.string() + ".trace"), ".trace"}}}) {
        const auto legacy = job_outdir / ("proof-batch-" + std::to_string(batch) + suffix);
        ensure_new(legacy);
        fs::create_hard_link(source, legacy);
      }
    }

    const auto total_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
      throw std::runtime_error("getrusage(RUSAGE_SELF) failed");
    const auto [rss_kib, pss_kib] = current_rss_pss_kib();
    const auto csv = job_outdir / "validation-timings.csv";
    const bool csv_new = !fs::exists(csv);
    std::ofstream cf(csv, std::ios::app);
    if (!cf) throw std::runtime_error("cannot append validation timing CSV");
    if (csv_new) cf << "logical_batch,microbatch,verified_nonce_count,tile_io_ms,cpu_walk_ms,h2d_ms,cache_scatter_ms,d2h_ms,kernel_ms,proof_ms,total_ms,unique_model_tiles,max_host_working_set_bytes,target_candidates,cache_hits,cache_misses,cache_vram_bytes,cuda_allocated_bytes,cuda_free_bytes,pinned_host_bytes,h2d_bytes,rss_kib,pss_kib,submit\n";
    cf << batch << ',' << microbatch << ',' << verified << ',' << tile_io_ms << ',' << cpu_ms
       << ',' << h2d_ms << ',' << scatter_ms << ',' << d2h_ms << ',' << kernel_ms << ',' << proof_ms << ','
       << total_ms << ',' << unique_total << ',' << max_host_working_set_bytes << ','
       << target_candidates << ',' << gpu_cache_hits << ',' << gpu_cache_misses << ','
       << gpu_cache_bytes << ',' << cuda_allocated_bytes << ',' << cuda_free_bytes << ','
       << pinned_host_bytes << ',' << h2d_bytes << ',' << rss_kib << ',' << pss_kib << ",DISABLED\n";
    std::cout << "CUDA_REAL_MODEL_SEGMENT=PASS device=primary GPU uuid=" << gpu_uuid
              << " SM=8.6 segment_nonces=" << batch << " physical_microbatch=" << microbatch
              << " verified_nonces=" << verified << " states_per_nonce=257 offsets_per_nonce=256"
              << " unique_model_tiles=" << unique_total << " max_microbatch_vector_capacity_bytes="
              << max_host_working_set_bytes << " tile_io_ms=" << tile_io_ms
              << " tile_cache_hits=" << gpu_cache_hits << " tile_cache_misses=" << gpu_cache_misses
              << " tile_cache_vram_bytes=" << gpu_cache_bytes
              << " cuda_owned_bytes=" << cuda_allocated_bytes << " cuda_free_bytes=" << cuda_free_bytes
              << " pinned_host_bytes=" << pinned_host_bytes << " h2d_bytes=" << h2d_bytes
              << " rss_kib=" << rss_kib << " pss_kib=" << pss_kib
              << " cpu_reference_ms=" << cpu_ms << " h2d_ms=" << h2d_ms
              << " cache_scatter_ms=" << scatter_ms << " d2h_ms=" << d2h_ms
              << " kernel_ms_sum=" << kernel_ms << " proof_ms=" << proof_ms
              << " end_to_end_ms=" << total_ms << " target_candidates=" << target_candidates
              << " process_peak_rss_kib=" << usage.ru_maxrss << " submit=DISABLED\n";
    std::cout << "batch_artifact=" << final_batch_path << " proof_directory=" << job_outdir
              << " capture_digest=" << hex(selected->digest.data(), selected->digest.size())
              << " generation=" << job.generation << " daa=" << header.daa_score
              << " nonce_base=" << segment_nonce_base << " seed0=" << first_seed << '\n';
    previous_job_nonce_end = segment_nonce_base + batch - 1;
    ++completed_jobs;
    }
    if (completed_jobs != captures.size())
      throw std::runtime_error("persistent session did not complete every job");
    std::cout << "CUDA_PERSISTENT_SESSION=PASS jobs=" << completed_jobs
              << " engine_reused=true unique_nonce_ranges=true submit=DISABLED\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "CUDA_REAL_MODEL_BATCH=FAIL " << e.what() << " submit=DISABLED\n";
    return 1;
  }
}
