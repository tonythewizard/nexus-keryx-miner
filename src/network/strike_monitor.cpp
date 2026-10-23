#include "keryx/nexus/strike_monitor.hpp"

#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <system_error>
#include <unistd.h>

namespace keryx::nexus::ai {
namespace {
std::string escaped(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20U) { out.push_back(' '); continue; }
    out.push_back(c);
  }
  return out;
}
template<class T> std::string optional_number(const std::optional<T> &value) {
  return value ? std::to_string(*value) : "null";
}

std::optional<std::string> json_string(std::string_view line, std::string_view key) {
  const std::string marker = "\"" + std::string(key) + "\":\"";
  const auto found = line.find(marker);
  if (found == std::string_view::npos) return std::nullopt;
  std::string result;
  for (std::size_t i = found + marker.size(); i < line.size(); ++i) {
    const char c = line[i];
    if (c == '"') return result;
    if (c != '\\') { result.push_back(c); continue; }
    if (++i >= line.size()) return std::nullopt;
    switch (line[i]) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      default: return std::nullopt; // Our writer emits no unicode escapes.
    }
  }
  return std::nullopt;
}

std::optional<std::uint64_t> json_u64(std::string_view line, std::string_view key,
                                     bool nullable = false) {
  const std::string marker = "\"" + std::string(key) + "\":";
  const auto found = line.find(marker);
  if (found == std::string_view::npos) return std::nullopt;
  auto pos = found + marker.size();
  if (nullable && line.substr(pos, 4) == "null") return std::nullopt;
  auto first = pos;
  while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') ++pos;
  if (pos == first) return std::nullopt;
  std::uint64_t value{};
  for (; first < pos; ++first) {
    const auto digit = static_cast<unsigned>(line[first] - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
      return std::nullopt;
    value = value * 10U + digit;
  }
  return value;
}

bool json_bool(std::string_view line, std::string_view key, bool &value) {
  const std::string marker = "\"" + std::string(key) + "\":";
  const auto found = line.find(marker);
  if (found == std::string_view::npos) return false;
  const auto pos = found + marker.size();
  if (line.substr(pos, 4) == "true") { value = true; return true; }
  if (line.substr(pos, 5) == "false") { value = false; return true; }
  return false;
}

void write_all(int fd, std::string_view data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto amount = ::write(fd, data.data() + offset, data.size() - offset);
    if (amount < 0 && errno == EINTR) continue;
    if (amount <= 0) throw std::system_error(errno, std::generic_category(), "write strike journal");
    offset += static_cast<std::size_t>(amount);
  }
}
}

StrikeMonitor::StrikeMonitor(std::string identity, std::filesystem::path journal,
                             Poll poll, Sink sink, std::chrono::milliseconds interval)
    : identity_(std::move(identity)), journal_(std::move(journal)), poll_(std::move(poll)),
      sink_(std::move(sink)), interval_(interval) {
  if (identity_.size() != 64 || interval_ <= std::chrono::milliseconds::zero() ||
      !journal_.is_absolute() || journal_.string().starts_with("/tmp/"))
    throw std::invalid_argument("invalid strike monitor identity, interval, or persistent journal path");
  snapshot_.identity = identity_;
}

StrikeMonitor::~StrikeMonitor() { stop(); }

void StrikeMonitor::start() {
  std::lock_guard lock(mutex_);
  if (started_) throw std::logic_error("strike monitor already started");
  recover_journal();
  started_ = true;
  thread_ = std::thread([this] { run(); });
}

void StrikeMonitor::stop() {
  {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard lock(mutex_);
  started_ = false;
}

StrikeSnapshot StrikeMonitor::snapshot() const {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void StrikeMonitor::run() {
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) return;
    }
    StrikeSnapshot observed;
    try {
      observed = poll_();
      if (observed.identity != identity_) {
        observed = {};
        observed.identity = identity_;
        observed.error = "strike RPC identity mismatch";
      }
    } catch (const std::exception &e) {
      observed.identity = identity_;
      observed.error = e.what();
    } catch (...) {
      observed.identity = identity_;
      observed.error = "unknown strike polling failure";
    }
    observed.observed_unix_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
    {
      std::lock_guard lock(mutex_);
      if (observed.availability == StrikeSnapshot::Availability::Available && observed.error.empty()) {
        observed.stale = false;
        snapshot_ = observed;
      } else {
        if (snapshot_.availability == StrikeSnapshot::Availability::Available) {
          snapshot_.stale = true;
          snapshot_.observed_unix_ms = observed.observed_unix_ms;
          snapshot_.error = observed.error.empty() ? "strike data unavailable" : observed.error;
        } else {
          observed.availability = StrikeSnapshot::Availability::Unknown;
          observed.stale = true;
          snapshot_ = observed;
        }
      }
      observed = snapshot_;
    }
    try { persist(observed); }
    catch (const std::exception &e) {
      std::lock_guard lock(mutex_);
      snapshot_.stale = true;
      snapshot_.error = std::string("strike journal persistence failed: ") + e.what();
      observed = snapshot_;
    }
    if (sink_) sink_(observed);

    std::unique_lock lock(mutex_);
    if (cv_.wait_for(lock, interval_, [&] { return stopping_; })) return;
  }
}

void StrikeMonitor::persist(const StrikeSnapshot &s) {
  const auto parent = journal_.parent_path();
  std::filesystem::create_directories(parent);
  const auto temp_marker = journal_.string();
  if (temp_marker.starts_with("/tmp/")) throw std::invalid_argument("strike journal cannot use /tmp");
  const std::string line = "{\"identity\":\"" + escaped(s.identity) +
      "\",\"availability\":\"" +
      (s.availability == StrikeSnapshot::Availability::Available ? "AVAILABLE" : "UNKNOWN") +
      "\",\"consecutive\":" + optional_number(s.consecutive) +
      ",\"lifetime\":" + optional_number(s.lifetime) +
      ",\"last_strike_daa\":" + optional_number(s.last_strike_daa) +
      ",\"suspended_until_daa\":" + optional_number(s.suspended_until_daa) +
      ",\"pending_burns\":" + std::to_string(s.pending_burns) +
      ",\"pending_burn_sompi\":" + std::to_string(s.pending_burn_sompi) +
      ",\"virtual_daa\":" + std::to_string(s.virtual_daa) +
      ",\"observed_unix_ms\":" + std::to_string(s.observed_unix_ms) +
      ",\"stale\":" + (s.stale ? "true" : "false") +
      ",\"error\":\"" + escaped(s.error) + "\"}\n";
  const int fd = ::open(journal_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "open strike journal");
  try {
    write_all(fd, line);
    if (::fsync(fd) != 0) throw std::system_error(errno, std::generic_category(), "fsync strike journal");
  } catch (...) { (void)::close(fd); throw; }
  if (::close(fd) != 0) throw std::system_error(errno, std::generic_category(), "close strike journal");
}

void StrikeMonitor::recover_journal() {
  if (recovered_) return;
  recovered_ = true;
  std::ifstream input(journal_, std::ios::binary);
  if (!input) return;
  std::string line;
  bool malformed = false;
  while (std::getline(input, line)) {
    if (line.size() > 4096 || line.empty() || line.back() != '}') {
      malformed = true;
      break;
    }
    const auto identity = json_string(line, "identity");
    const auto availability = json_string(line, "availability");
    const auto consecutive = json_u64(line, "consecutive", true);
    const auto lifetime = json_u64(line, "lifetime", true);
    const auto last_strike = json_u64(line, "last_strike_daa", true);
    const auto suspended = json_u64(line, "suspended_until_daa", true);
    const auto burns = json_u64(line, "pending_burns");
    const auto burn_amount = json_u64(line, "pending_burn_sompi");
    const auto virtual_daa = json_u64(line, "virtual_daa");
    const auto observed = json_u64(line, "observed_unix_ms");
    bool stale = false;
    if (!identity || *identity != identity_ || !availability ||
        (*availability != "AVAILABLE" && *availability != "UNKNOWN") ||
        !burns || !burn_amount || !virtual_daa || !observed ||
        !json_bool(line, "stale", stale)) {
      malformed = true;
      break;
    }
    StrikeSnapshot restored;
    restored.identity = identity_;
    restored.availability = *availability == "AVAILABLE"
        ? StrikeSnapshot::Availability::Available : StrikeSnapshot::Availability::Unknown;
    if (consecutive && *consecutive <= std::numeric_limits<std::uint32_t>::max())
      restored.consecutive = static_cast<std::uint32_t>(*consecutive);
    else if (line.find("\"consecutive\":null") == std::string::npos) { malformed = true; break; }
    if (lifetime && *lifetime <= std::numeric_limits<std::uint32_t>::max())
      restored.lifetime = static_cast<std::uint32_t>(*lifetime);
    else if (line.find("\"lifetime\":null") == std::string::npos) { malformed = true; break; }
    restored.last_strike_daa = last_strike;
    restored.suspended_until_daa = suspended;
    if ((*availability == "AVAILABLE") != (restored.consecutive.has_value() && restored.lifetime.has_value())) {
      malformed = true;
      break;
    }
    if (*burns > std::numeric_limits<std::uint32_t>::max()) { malformed = true; break; }
    restored.pending_burns = static_cast<std::uint32_t>(*burns);
    restored.pending_burn_sompi = *burn_amount;
    restored.virtual_daa = *virtual_daa;
    restored.observed_unix_ms = *observed;
    restored.stale = stale;
    const auto error = json_string(line, "error");
    if (!error) { malformed = true; break; }
    restored.error = *error;
    snapshot_ = std::move(restored);
  }
  if (input.bad()) malformed = true;
  if (malformed) {
    snapshot_.identity = identity_;
    snapshot_.stale = true;
    snapshot_.error = "strike journal has a corrupt or truncated record; last valid observation retained";
    if (snapshot_.availability != StrikeSnapshot::Availability::Available)
      snapshot_.availability = StrikeSnapshot::Availability::Unknown;
  }
  if (snapshot_.availability == StrikeSnapshot::Availability::Available) {
    snapshot_.stale = true;
    if (snapshot_.error.empty()) snapshot_.error = "restored from durable journal; awaiting fresh poll";
  }
}

} // namespace keryx::nexus::ai
