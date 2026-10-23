#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/ai_response_service.hpp"

#include <chrono>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <array>
#include <cctype>
#include <vector>

namespace {
std::vector<std::uint8_t> decode_hex(std::string_view text, std::size_t expected) {
  if (text.size() != expected * 2) throw std::runtime_error("hex field length invalid");
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> result(expected);
  for (std::size_t i = 0; i < expected; ++i) {
    const int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
    if (hi < 0 || lo < 0) throw std::runtime_error("hex field contains invalid characters");
    result[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return result;
}

int node_readiness(const char *endpoint) {
  const auto info = keryx::nexus::probe_node_readiness(endpoint, std::chrono::seconds(10));
  std::cout << "NODE_READ_ONLY network=" << info.network << " version=" << info.version
            << " synced=" << (info.synced ? "true" : "false") << '\n';
  return info.network == "mainnet" && info.synced ? 0 : 3;
}

int selected_chain_audit(const char *endpoint, const char *hash_file, const char *details_file) {
  const auto persistent_path = [](const char *raw) {
    const auto path = std::filesystem::absolute(raw).lexically_normal();
    const auto text = path.string();
    if (text == "/tmp" || text.starts_with("/tmp/"))
      throw std::runtime_error("selected-chain audit files must remain outside /tmp");
    return path;
  };
  const auto hashes_path = persistent_path(hash_file);
  const auto details_path = persistent_path(details_file);
  std::ifstream in(hashes_path);
  if (!in) throw std::runtime_error("cannot open persistent block-hash list");
  std::vector<std::string> hashes;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (hashes.size() >= 10000) throw std::runtime_error("block-hash list exceeds bounded audit limit");
    hashes.push_back(line);
  }
  const auto statuses = keryx::nexus::probe_blocks_selected_chain(
      endpoint, hashes, std::chrono::seconds(120));
  std::ofstream details(details_path, std::ios::trunc);
  if (!details) throw std::runtime_error("cannot create persistent selected-chain detail file");
  std::size_t chain = 0, off_chain = 0, unknown = 0, pending = 0, errors = 0;
  const auto reason_class = [](const keryx::nexus::BlockChainStatus &status) {
    std::string normalized = status.error;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized.find("cannot find header") != std::string::npos ||
        normalized.find("block not found") != std::string::npos ||
        normalized.find("unknown block") != std::string::npos ||
        normalized.find("pruned") != std::string::npos)
      return std::string("PRUNED_OR_UNAVAILABLE_HEADER");
    if (normalized.find("some data is missing") != std::string::npos)
      return std::string("INCOMPLETE_BLOCK_DATA");
    if (status.state == keryx::nexus::BlockValidationState::Error)
      return std::string("RPC_OR_PROTOCOL_ERROR");
    return std::string("AUTHORITATIVE_FIELD_UNAVAILABLE");
  };
  std::map<std::string, std::size_t> inconclusive_reasons;
  for (const auto &status : statuses) {
    const char *state = "ERROR";
    switch (status.state) {
      case keryx::nexus::BlockValidationState::ChainValid: ++chain; state = "CHAIN_VALID"; break;
      case keryx::nexus::BlockValidationState::OffChain: ++off_chain; state = "OFF_CHAIN"; break;
      case keryx::nexus::BlockValidationState::Unknown:
        ++unknown;
        state = "UNKNOWN";
        ++inconclusive_reasons[reason_class(status)];
        break;
      case keryx::nexus::BlockValidationState::Pending: ++pending; state = "PENDING"; break;
      case keryx::nexus::BlockValidationState::Error:
        ++errors;
        ++inconclusive_reasons[reason_class(status)];
        break;
    }
    details << status.hash << '\t' << state << '\n';
  }
  details.flush();
  if (!details) throw std::runtime_error("selected-chain detail file write failed");
  std::cout << "ESCROW_BLOCK_AUDIT total=" << statuses.size() << " CHAIN_VALID=" << chain
            << " OFF_CHAIN=" << off_chain << " UNKNOWN=" << unknown
            << " PENDING=" << pending << " ERROR=" << errors << '\n';
  for (const auto &[reason, count] : inconclusive_reasons)
    std::cout << "ESCROW_BLOCK_INCONCLUSIVE count=" << count << " class=" << reason << '\n';
  return errors == 0 && pending == 0 ? 0 : 3;
}

int verify_escrow(const char *payout, const char *extra, const char *key_path) {
  std::ifstream key_file(key_path);
  std::string secret_hex;
  if (!key_file || !std::getline(key_file, secret_hex))
    throw std::runtime_error("cannot read configured escrow key");
  while (!secret_hex.empty() && std::isspace(static_cast<unsigned char>(secret_hex.back())))
    secret_hex.pop_back();
  const auto secret_bytes = decode_hex(secret_hex, 32);
  std::array<std::uint8_t, 32> secret{};
  std::copy(secret_bytes.begin(), secret_bytes.end(), secret.begin());
  const auto pubkey = keryx::nexus::ai::stock_escrow_pubkey_from_secret(secret);
  constexpr std::string_view escrow_tag = "/escrow:";
  constexpr std::string_view signature_tag = "/esig:";
  const std::string_view data(extra);
  const auto escrow_at = data.find(escrow_tag), signature_at = data.find(signature_tag);
  if (escrow_at == std::string_view::npos || signature_at != escrow_at + escrow_tag.size() + 64 ||
      data.size() != signature_at + signature_tag.size() + 128)
    throw std::runtime_error("configured payout delegation fields malformed");
  const auto configured_key = decode_hex(data.substr(escrow_at + escrow_tag.size(), 64), 32);
  const auto signature = decode_hex(data.substr(signature_at + signature_tag.size(), 128), 64);
  std::array<std::uint8_t, 32> configured_pubkey{};
  std::array<std::uint8_t, 64> cert{};
  std::copy(configured_key.begin(), configured_key.end(), configured_pubkey.begin());
  std::copy(signature.begin(), signature.end(), cert.begin());
  const bool key_match = pubkey == configured_pubkey;
  const bool delegation_valid = key_match &&
      keryx::nexus::ai::stock_verify_escrow_delegation(payout, pubkey, cert);
  std::string why;
  const auto identity = keryx::nexus::ai::service_identity_from_payout(payout, &why);
  std::cout << "ESCROW_IDENTITY_READ_ONLY key_matches_config=" << (key_match ? "true" : "false")
            << " delegation_valid=" << (delegation_valid ? "true" : "false")
            << " service_identity=" << (identity ? *identity : "UNKNOWN") << '\n';
  return delegation_valid && identity ? 0 : 3;
}
}

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--node") {
    try { return node_readiness(argv[2]); }
    catch (const std::exception &e) { std::cerr << "NODE_READ_ONLY=ERROR detail=" << e.what() << '\n'; return 3; }
  }
  if (argc == 5 && std::string_view(argv[1]) == "--blocks") {
    try { return selected_chain_audit(argv[2], argv[3], argv[4]); }
    catch (const std::exception &e) { std::cerr << "ESCROW_BLOCK_AUDIT=ERROR detail=" << e.what() << '\n'; return 3; }
  }
  if (argc == 5 && std::string_view(argv[1]) == "--verify-escrow") {
    try { return verify_escrow(argv[2], argv[3], argv[4]); }
    catch (const std::exception &e) { std::cerr << "ESCROW_IDENTITY_READ_ONLY=ERROR detail=" << e.what() << '\n'; return 3; }
  }
  if (argc != 3) {
    std::cerr << "usage: nexus-ai-service-probe ENDPOINT PAYOUT_ADDRESS\n"
                 "       nexus-ai-service-probe --node ENDPOINT\n"
                 "       nexus-ai-service-probe --blocks ENDPOINT HASH_FILE DETAILS_FILE\n"
                 "       nexus-ai-service-probe --verify-escrow PAYOUT EXTRA_DATA KEY_FILE\n";
    return 2;
  }
  try {
    std::string identity_error;
    const auto identity = keryx::nexus::ai::service_identity_from_payout(argv[2], &identity_error);
    if (!identity) throw std::runtime_error("payout identity unavailable: " + identity_error);
    const auto snapshot = keryx::nexus::probe_ai_service_rpc(argv[1], 0, *identity,
                                                              std::chrono::seconds(8));
    std::cout << "SERVICE_RPC identity=" << *identity
              << " providers=" << (snapshot.providers_available ? "AVAILABLE" : "UNKNOWN")
              << " strikes=" << (snapshot.strikes_available ? "AVAILABLE" : "UNKNOWN")
              << " virtual_daa=" << (snapshot.strikes_available
                    ? std::to_string(snapshot.strikes.virtual_daa) : "UNKNOWN") << '\n';
    if (!snapshot.providers_available) {
      std::cout << "AI_PROVIDER_SNAPSHOT=UNKNOWN\n";
    } else {
      for (const auto &provider : snapshot.providers)
        std::cout << "AI_PROVIDER tier=" << provider.tier << " model_id=" << provider.model_id
                  << " identity_match=" << (provider.identity == *identity ? "true" : "false")
                  << " escrow_pubkey_present=" << (!provider.escrow_pubkey.empty() ? "true" : "false") << '\n';
    }
    if (!snapshot.strikes_available) {
      std::cout << "SERVICE_STRIKES=UNKNOWN reason=" << snapshot.strikes.error << '\n';
    } else {
      const auto &s = snapshot.strikes;
      std::cout << "SERVICE_STRIKES identity_match=true consecutive=" << *s.consecutive
                << " lifetime=" << *s.lifetime
                << " last_strike_daa=" << (s.last_strike_daa ? std::to_string(*s.last_strike_daa) : "NONE")
                << " pending_burns=" << s.pending_burns
                << " pending_burn_sompi=" << s.pending_burn_sompi
                << " suspended_until_daa=" << (s.suspended_until_daa
                      ? std::to_string(*s.suspended_until_daa) : "NONE") << '\n';
    }
    return snapshot.providers_available && snapshot.strikes_available ? 0 : 3;
  } catch (const std::exception &e) {
    std::cerr << "SERVICE_RPC_PROBE=UNKNOWN detail=" << e.what() << '\n';
    return 3;
  }
}
