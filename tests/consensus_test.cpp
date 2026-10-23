#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/consensus/verified_walk_view.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace keryx::nexus::consensus;
static std::uint32_t u32(const std::vector<std::uint8_t> &b, std::size_t &p) {
  if (p + 4 > b.size())
    throw std::runtime_error("truncated fixture");
  auto x = std::uint32_t(b[p]) | (std::uint32_t(b[p + 1]) << 8) |
           (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
  p += 4;
  return x;
}
static std::uint64_t u64(const std::vector<std::uint8_t> &b, std::size_t &p) {
  if (p + 8 > b.size())
    throw std::runtime_error("truncated fixture");
  std::uint64_t x = 0;
  for (unsigned i = 0; i < 8; i++)
    x |= std::uint64_t(b[p + i]) << (8 * i);
  p += 8;
  return x;
}
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::runtime_error(
          "usage: nexus-consensus-tests ORACLE_FIXTURE CANDIDATE_OUT");
    std::ifstream f(argv[1], std::ios::binary);
    if (!f)
      throw std::runtime_error("missing official fixture");
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    std::size_t p = 0;
    if (b.size() < 4 || std::string(b.begin(), b.begin() + 4) != "NXV4")
      throw std::runtime_error("fixture magic mismatch");
    p = 4;
    const auto seed = u64(b, p), chunks = u64(b, p);
    if (p + 33 > b.size())
      throw std::runtime_error("fixture metadata truncated");
    const auto tier = b[p++];
    Hash root{};
    std::copy_n(b.data() + p, 32, root.data());
    p += 32;
    const auto expected_final = u64(b, p);
    std::vector<std::uint64_t> expected_offsets(256);
    for (auto &x : expected_offsets)
      x = u64(b, p);
    std::vector<std::array<std::uint8_t, 1024>> expected_states(257);
    for (auto &s : expected_states) {
      if (p + s.size() > b.size())
        throw std::runtime_error("state trace truncated");
      std::copy_n(b.data() + p, s.size(), s.data());
      p += s.size();
    }
    const auto proof_size = u32(b, p);
    if (p + proof_size != b.size())
      throw std::runtime_error("proof length mismatch");
    std::vector<std::uint8_t> expected_wire(
        b.begin() + static_cast<std::ptrdiff_t>(p), b.end());
    auto blob = make_reference_test_blob();
    const auto cpp_root = merkle_root(blob);
    if (blob.size() / 32 != chunks || cpp_root != root)
      throw std::runtime_error("C++ blob/root differs from official fixture");
    auto walk = walk_v4(seed, blob);
    std::vector<std::uint8_t> cuda_states;
    for(const auto &state:expected_states)cuda_states.insert(cuda_states.end(),state.begin(),state.end());
    const auto &view=verified_walk_view(walk,expected_offsets,cuda_states);
    if(&view!=&walk)throw std::runtime_error("verified witness unexpectedly copied");
    auto rejected=[&](auto offsets,auto states) {
      bool failed=false;try{verified_walk_view(walk,offsets,states);}catch(const std::runtime_error &){failed=true;}
      if(!failed)throw std::runtime_error("invalid witness accepted");
    };
    rejected(std::span<const std::uint64_t>(expected_offsets).first(255),std::span<const std::uint8_t>(cuda_states));
    rejected(std::span<const std::uint64_t>(expected_offsets),std::span<const std::uint8_t>(cuda_states).first(cuda_states.size()-1));
    cuda_states.back()^=1;rejected(expected_offsets,cuda_states);cuda_states.back()^=1;
    expected_offsets[255]^=1;rejected(expected_offsets,cuda_states);expected_offsets[255]^=1;
    std::uint64_t timing_sink=0;
    for(unsigned pair=0;pair<6;++pair)for(unsigned v=0;v<2;++v) {
      const bool candidate=(pair+v)%2;constexpr unsigned iterations=200;
      const auto start=std::chrono::steady_clock::now();
      for(unsigned i=0;i<iterations;++i) {
        const auto &verified=verified_walk_view(walk,expected_offsets,cuda_states);
        if(candidate)timing_sink+=final_state(verified);
        else {Walk copied;copied.offsets=expected_offsets;copied.states.resize(walk_steps+1);
          for(std::size_t s=0;s<=walk_steps;++s)std::copy_n(cuda_states.data()+s*tile_bytes,tile_bytes,copied.states[s].data());
          timing_sink+=final_state(copied);}
      }
      const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
      std::cout<<"WITNESS_VIEW_MEASUREMENT pair="<<pair<<" variant="<<(candidate?"candidate":"baseline")<<" ns_per_witness="<<elapsed/iterations<<'\n';
    }
    std::cout<<"WITNESS_VIEW=PASS dimensions_checked=true last_state_corruption_rejected=true offset_corruption_rejected=true sink="<<timing_sink<<'\n';
    for (std::size_t iteration = 0; iteration < expected_states.size();
         ++iteration) {
      if (walk.states[iteration] == expected_states[iteration])
        continue;
      for (std::size_t byte = 0; byte < tile_bytes; ++byte) {
        if (walk.states[iteration][byte] != expected_states[iteration][byte])
          throw std::runtime_error(
              "state mismatch nonce=fixture-seed iteration=" +
              std::to_string(iteration) + " byte=" + std::to_string(byte) +
              " cpp=" + std::to_string(walk.states[iteration][byte]) +
              " rust=" + std::to_string(expected_states[iteration][byte]));
      }
    }
    for (std::size_t iteration = 0; iteration < expected_offsets.size();
         ++iteration)
      if (walk.offsets[iteration] != expected_offsets[iteration])
        throw std::runtime_error(
            "offset mismatch nonce=fixture-seed iteration=" +
            std::to_string(iteration) +
            " cpp=" + std::to_string(walk.offsets[iteration]) +
            " rust=" + std::to_string(expected_offsets[iteration]));
    const auto fs = final_state(walk);
    if (fs != expected_final)
      throw std::runtime_error("final state differs from Rust");
    const auto ordered = gather_v4_model_tiles(
        seed, blob.size() / tile_bytes, [&](std::uint64_t offset) {
          std::array<std::uint8_t, tile_bytes> tile{};
          std::copy_n(blob.data() + offset * tile_bytes, tile_bytes,
                      tile.data());
          return tile;
        });
    const auto gathered_walk =
        walk_v4_from_tile_sequence(seed, blob.size() / tile_bytes, ordered.tiles);
    if (ordered.offsets != walk.offsets || gathered_walk.offsets != walk.offsets ||
        gathered_walk.states != walk.states)
      throw std::runtime_error("ordered model tile walk differs from CPU reference");
    auto proof = prove_v4(seed, tier, blob);
    const auto proof_from_gathered = prove_v4_from_walk(
        verified_walk_view(gathered_walk,expected_offsets,cuda_states), tier, ordered.tiles, [&](std::uint64_t offset) {
          const auto it = std::find(walk.offsets.begin(), walk.offsets.end(), offset);
          if (it == walk.offsets.end())
            throw std::runtime_error("missing reference path for gathered offset");
          return proof.paths[static_cast<std::size_t>(it - walk.offsets.begin())];
        });
    if (proof_from_gathered.tiles != proof.tiles ||
        proof_from_gathered.paths != proof.paths)
      throw std::runtime_error("proof rebuilt from ordered walk differs from CPU proof");
    Hash pph{};
    pph.fill(0x42);
    auto pow = pom_pow_value_h3(fs, pph);
    auto wire = borsh_v4_container(proof, fs, pow);
    if (wire != expected_wire)
      throw std::runtime_error("Borsh proof differs byte-for-byte from Rust");
    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(wire.data()),
              static_cast<std::streamsize>(wire.size()));
    if (!out)
      throw std::runtime_error("candidate proof write failed");
    bool bad_tier = false;
    try {
      (void)prove_v4(seed, 5, blob);
    } catch (const std::out_of_range &) {
      bad_tier = true;
    }
    if (!bad_tier)
      throw std::runtime_error("out-of-range PoM tier accepted");
    keryx::nexus::JobPipeline jobs;
    keryx::nexus::TemplateFields fields;
    fields.response_wire = {1, 2, 3};
    fields.block_wire = {4, 5, 6};
    fields.daa_score = 121211211;
    auto job = jobs.accept(fields);
    auto binding = bind_candidate(job, 17, 23, Hash{});
    if (!candidate_matches_job(binding, job) ||
        candidate_binding_status(binding, job) != CandidateBindingStatus::Valid)
      throw std::runtime_error("candidate failed binding to current job");
    auto corrupted_binding = binding;
    corrupted_binding.job_fingerprint[0] ^= 1U;
    if (candidate_binding_status(corrupted_binding, job) !=
        CandidateBindingStatus::IntegrityMismatch)
      throw std::runtime_error("active-lease candidate fingerprint corruption was not fatal");
    jobs.invalidate();
    if (candidate_binding_status(binding, job) != CandidateBindingStatus::StaleLease)
      throw std::runtime_error("invalidated template was not classified as recoverable stale work");
    auto stale = jobs.accept(fields);
    if (candidate_matches_job(binding, stale))
      throw std::runtime_error("stale generation candidate accepted");
    bool bad_blob = false;
    try {
      (void)walk_v4(
          seed, std::span<const std::uint8_t>(blob.data(), blob.size() - 1));
    } catch (const std::invalid_argument &) {
      bad_blob = true;
    }
    if (!bad_blob)
      throw std::runtime_error("non chunk-aligned blob accepted");
    std::cout << "CPP_RUST_PARITY=PASS states=257 offsets=256 final_state="
              << fs << " proof_bytes=" << wire.size()
              << " generation_binding=PASS negatives=PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "CONSENSUS_TEST=FAIL " << e.what() << "\n";
    return 1;
  }
}
