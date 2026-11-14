#include "keryx/nexus/json_integer_wire.hpp"
#include "keryx/nexus/pool_ai_service.hpp"
#include "keryx/nexus/platform/io.hpp"
#include "keryx/nexus/stratum_v4_client.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <limits>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <zlib.h>

namespace keryx::nexus::ai {
namespace {
namespace io = platform::io;
using Obj = google::protobuf::Struct;
using Val = google::protobuf::Value;
using List = google::protobuf::ListValue;
using Hash = PoolAiService::Hash;
constexpr std::size_t max_prompt = 1U << 20, max_record = 64U << 20,
                      max_file = 70U << 20;
void need(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
std::string text(const Val &v) {
  need(v.kind_case() == Val::kStringValue, "Pool AI expected string");
  return v.string_value();
}
Val value(std::string_view s) {
  Val v;
  v.set_string_value(std::string(s));
  return v;
}
Val value(List s) {
  Val v;
  *v.mutable_list_value() = std::move(s);
  return v;
}
Val value(Obj s) {
  Val v;
  *v.mutable_struct_value() = std::move(s);
  return v;
}
std::string json(const Obj &o) {
  std::string s;
  need(google::protobuf::util::MessageToJsonString(o, &s).ok(),
       "Pool AI JSON serialization");
  return s;
}
Obj parse(std::string_view s) {
  Obj o;
  need(google::protobuf::util::JsonStringToMessage(preserve_json_integers(s), &o).ok(),
       "Pool AI JSON parse");
  return o;
}
const Val &at(const Obj &o, std::string_view key) {
  auto i = o.fields().find(std::string(key));
  need(i != o.fields().end(), "Pool AI field missing");
  return i->second;
}
std::uint64_t integer(const Val &v) {
  if (v.kind_case() == Val::kStringValue) {
    auto s = v.string_value();
    std::uint64_t n{};
    auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    need(!s.empty() && r.ec == std::errc{} && r.ptr == s.data() + s.size(),
         "Pool AI integer encoding");
    return n;
  }
  need(v.kind_case() == Val::kNumberValue, "Pool AI integer type");
  auto n = v.number_value();
  need(std::isfinite(n) && n >= 0 && n <= 9007199254740991.0 &&
           std::floor(n) == n,
       "Pool AI inexact integer");
  return static_cast<std::uint64_t>(n);
}
Hash digest(std::span<const std::uint8_t> b) {
  Hash h{};
  SHA256(b.data(), b.size(), h.data());
  return h;
}
Hash digest(std::string_view s) {
  return digest(
      std::span(reinterpret_cast<const std::uint8_t *>(s.data()), s.size()));
}
std::string hex(std::span<const std::uint8_t> b) {
  return stratum::hex_encode_bytes(b);
}
Hash hash(const Val &v) { return stratum::parse_hash32_hex(text(v)); }
std::string b64(std::string_view s) {
  std::string out(4 * ((s.size() + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                  reinterpret_cast<const unsigned char *>(s.data()),
                  static_cast<int>(s.size()));
  return out;
}
constexpr unsigned char compressed_prompt_magic[4] = {0x00, 'K', 'Z', 0x01};

std::string to_utf8_lossy(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size();) {
    const auto c = static_cast<unsigned char>(in[i++]);
    if (c < 128) {
      out.push_back(static_cast<char>(c));
      continue;
    }
    unsigned count = 0;
    unsigned char lo = 128, hi = 191;
    if (c >= 194 && c <= 223) {
      count = 1;
    } else if (c >= 224 && c <= 239) {
      count = 2;
      if (c == 224) lo = 160;
      if (c == 237) hi = 159;
    } else if (c >= 240 && c <= 244) {
      count = 3;
      if (c == 240) lo = 144;
      if (c == 244) hi = 143;
    } else {
      out.append("\xEF\xBF\xBD");
      continue;
    }
    if (i + count > in.size()) {
      out.append("\xEF\xBF\xBD");
      break;
    }
    const auto second = static_cast<unsigned char>(in[i]);
    if (second < lo || second > hi) {
      out.append("\xEF\xBF\xBD");
      continue;
    }
    bool valid = true;
    for (unsigned k = 1; k < count; ++k) {
      const auto next = static_cast<unsigned char>(in[i + k]);
      if (next < 128 || next > 191) {
        valid = false;
        break;
      }
    }
    if (!valid) {
      out.append("\xEF\xBF\xBD");
      continue;
    }
    out.push_back(static_cast<char>(c));
    for (unsigned k = 0; k < count; ++k) {
      out.push_back(in[i++]);
    }
  }
  return out;
}

std::string decode_private_prompt(std::string_view bytes) {
  if (bytes.size() < 4 ||
      static_cast<unsigned char>(bytes[0]) != compressed_prompt_magic[0] ||
      static_cast<unsigned char>(bytes[1]) != compressed_prompt_magic[1] ||
      static_cast<unsigned char>(bytes[2]) != compressed_prompt_magic[2] ||
      static_cast<unsigned char>(bytes[3]) != compressed_prompt_magic[3]) {
    return to_utf8_lossy(bytes);
  }

  std::string_view deflated = bytes.substr(4);
  z_stream strm{};
  strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(deflated.data()));
  strm.avail_in = static_cast<uInt>(deflated.size());
  if (inflateInit2(&strm, -15) != Z_OK) {
    return to_utf8_lossy(deflated);
  }

  std::string inflated;
  char buf[8192];
  int ret = Z_OK;
  while (ret == Z_OK && inflated.size() < max_prompt) {
    strm.next_out = reinterpret_cast<Bytef *>(buf);
    const std::size_t want = std::min<std::size_t>(sizeof(buf), max_prompt - inflated.size());
    strm.avail_out = static_cast<uInt>(want);
    ret = inflate(&strm, Z_NO_FLUSH);
    const std::size_t have = want - strm.avail_out;
    inflated.append(buf, have);
    if (ret == Z_STREAM_END) break;
  }
  inflateEnd(&strm);

  if (inflated.empty() && ret != Z_STREAM_END) {
    return to_utf8_lossy(deflated);
  }
  return to_utf8_lossy(inflated);
}

std::string decode(std::string_view s) {
  need(!s.empty() && s.size() % 4 == 0 &&
           s.size() <= 4 * ((max_prompt + 2) / 3),
       "Pool AI base64 bounds");
  std::string out(3 * (s.size() / 4), '\0');
  auto n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                           reinterpret_cast<const unsigned char *>(s.data()),
                           static_cast<int>(s.size()));
  need(n >= 0, "Pool AI invalid base64");
  if (s.ends_with("=="))
    n -= 2;
  else if (s.ends_with("="))
    --n;
  need(n >= 0 && static_cast<std::size_t>(n) <= max_prompt,
       "Pool AI prompt bounds");
  out.resize(static_cast<std::size_t>(n));
  need(b64(out) == s, "Pool AI noncanonical base64");
  return decode_private_prompt(out);
}
struct Fd {
  int fd{-1};
  ~Fd() {
    if (fd >= 0)
      io::close(fd);
  }
};
void private_directory(const std::filesystem::path &p, bool ancestors = false) {
  need(p.is_absolute(), "Pool trace storage must be absolute");
  for (auto part = p; !part.empty() && part != part.parent_path();
       part = part.parent_path())
    need(!std::filesystem::is_symlink(std::filesystem::symlink_status(part)),
         "Pool trace directory symlink");
  if (std::filesystem::create_directories(p))
    std::filesystem::permissions(p, std::filesystem::perms::owner_all);
  io::Stat st{};
  need(io::stat(p.c_str(), &st) == 0 && st.st_uid == io::getuid() &&
           !(st.st_mode & 0077),
       "Pool trace directory must be private");
  // Persist the namespace itself, including any newly created ancestors.
  for (auto part = p; !part.empty(); part = part.parent_path()) {
    Fd dir{io::open(part.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    need(dir.fd >= 0 && io::fsync(dir.fd) == 0,
         "Pool trace namespace fsync failed");
    if (part == part.parent_path() || (!ancestors && part == p.parent_path()))
      break;
  }
}
std::optional<std::vector<std::uint8_t>> read(const std::filesystem::path &p) {
  Fd f{io::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
  if (f.fd < 0) {
    need(errno == ENOENT, "Pool trace read open failed");
    return {};
  }
  io::Stat st{};
  need(io::fstat(f.fd, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == io::getuid() && !(st.st_mode & 0077) &&
           st.st_size >= 0 &&
           static_cast<std::uint64_t>(st.st_size) <= max_file,
       "Pool trace file privacy/bounds");
  std::vector<std::uint8_t> b(static_cast<std::size_t>(st.st_size));
  std::size_t at = 0;
  while (at < b.size()) {
    auto n = io::read(f.fd, b.data() + at, b.size() - at);
    if (n < 0 && errno == EINTR)
      continue;
    need(n > 0, "Pool trace read incomplete");
    at += static_cast<std::size_t>(n);
  }
  return b;
}
void publish(const std::filesystem::path &p, std::span<const std::uint8_t> b) {
  need(b.size() <= max_file, "Pool trace file bound");
  auto tmp = p;
  tmp += ".new";
  Fd f{io::open(tmp.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
  need(f.fd >= 0, "Pool trace temporary file already exists or unavailable");
  try {
    std::size_t at = 0;
    while (at < b.size()) {
      auto n = io::write(f.fd, b.data() + at, b.size() - at);
      if (n < 0 && errno == EINTR)
        continue;
      need(n > 0, "Pool trace write failed");
      at += static_cast<std::size_t>(n);
    }
    need(io::fsync(f.fd) == 0, "Pool trace fsync failed");
    need(io::link(tmp.c_str(), p.c_str()) == 0,
         "Pool trace immutable publication failed");
    need(io::unlink(tmp.c_str()) == 0, "Pool trace temporary unlink failed");
    Fd dir{io::open(p.parent_path().c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    need(dir.fd >= 0 && io::fsync(dir.fd) == 0,
         "Pool trace directory fsync failed");
  } catch (...) {
    io::unlink(tmp.c_str());
    throw;
  }
}
struct Stored {
  Obj meta;
  std::vector<std::uint8_t> capture;
};
Stored unpack(std::span<const std::uint8_t> b) {
  need(b.size() >= 44 && std::memcmp(b.data(), "NXPAL001", 8) == 0,
       "Pool trace record magic");
  std::uint32_t n{};
  for (unsigned i = 0; i < 4; ++i)
    n |= std::uint32_t(b[8 + i]) << (8 * i);
  need(n <= (4U << 20) && n <= b.size() - 44, "Pool trace metadata bound");
  auto d = digest(b.first(b.size() - 32));
  need(std::equal(d.begin(), d.end(), b.end() - 32),
       "Pool trace checksum mismatch");
  Stored s;
  s.meta =
      parse(std::string_view(reinterpret_cast<const char *>(b.data() + 12), n));
  s.capture.assign(b.begin() + 12 + n, b.end() - 32);
  need(s.capture.size() <= max_record, "Pool trace capture bound");
  return s;
}
void sync_parent(const std::filesystem::path &p) {
  Fd dir{io::open(p.parent_path().c_str(),
                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  need(dir.fd >= 0 && io::fsync(dir.fd) == 0,
       "Pool trace recovery directory fsync failed");
}
std::optional<std::vector<std::uint8_t>>
durable_read(const std::filesystem::path &p) {
  auto bytes = read(p);
  if (!bytes) {
    auto tmp = p;
    tmp += ".new";
    bytes = read(tmp);
    if (!bytes)
      return {};
    if (p.extension() == ".trace")
      (void)unpack(*bytes);
    else
      need(bytes->size() == 32, "Pool response binding recovery bounds");
    Fd file{io::open(tmp.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW)};
    need(file.fd >= 0 && io::fsync(file.fd) == 0,
         "Pool trace recovery file fsync failed");
    need(io::link(tmp.c_str(), p.c_str()) == 0,
         "Pool trace recovery immutable publish failed");
    sync_parent(p);
    need(io::unlink(tmp.c_str()) == 0,
         "Pool trace recovery temporary unlink failed");
  }
  sync_parent(p);
  return bytes;
}
void save(const std::filesystem::path &p, const Stored &s) {
  auto meta = json(s.meta);
  need(meta.size() <= 4U << 20 && s.capture.size() <= max_record,
       "Pool trace storage bounds");
  std::vector<std::uint8_t> b{'N', 'X', 'P', 'A', 'L', '0', '0', '1'};
  for (unsigned i = 0; i < 4; ++i)
    b.push_back(static_cast<std::uint8_t>(meta.size() >> (8 * i)));
  b.insert(b.end(), meta.begin(), meta.end());
  b.insert(b.end(), s.capture.begin(), s.capture.end());
  auto d = digest(b);
  b.insert(b.end(), d.begin(), d.end());
  publish(p, b);
}
std::string frame(std::string_view method, List args) {
  Obj o;
  (*o.mutable_fields())["method"] = value(method);
  (*o.mutable_fields())["params"] = value(std::move(args));
  return json(o);
}
std::string custody_scope(std::string_view scope) {
  try {
    auto endpoint = stratum::parse_stratum_endpoint(scope);
    auto host = endpoint.host;
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if ((host == "krx.suprnova.cc" || host == "stratum-eu2.suprnova.cc" ||
         host == "stratum-us.suprnova.cc") &&
        (endpoint.port == 4401 || endpoint.port == 4404 ||
         endpoint.port == 4405))
      return "operator:suprnova.cc:v1";
    if ((host == "s1.keryxpool.com" || host == "keryxpool.com") &&
        endpoint.port == 4401)
      return "operator:keryxpool.com:v1";
  } catch (const std::exception &) {
  } // Unknown/custom endpoints remain isolated.
  return std::string(scope);
}
std::uint64_t now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace
PoolAiService::PoolAiService(std::filesystem::path root, const Hash &model,
                             Infer infer, Open open)
    : root_(std::move(root)), model_(model), infer_(std::move(infer)),
      open_(std::move(open)) {
  private_directory(root_, true);
  need(bool(infer_) && bool(open_), "Pool AI callbacks missing");
}
std::string PoolAiService::handle(std::string_view scope,
                                  std::string_view worker,
                                  std::string_view wire) {
  std::lock_guard lock(mutex_);
  need(scope.size() <= 1024 && !scope.empty() && worker.size() <= 256 &&
           !worker.empty() && wire.size() <= 2U << 20,
       "Pool AI frame/scope bounds");
  auto o = parse(wire);
  auto method = text(at(o, "method"));
  const auto &p = at(o, "params");
  need(p.kind_case() == Val::kListValue, "Pool AI params must be array");
  const auto &v = p.list_value().values();
  const auto custody = custody_scope(scope);
  auto dir = root_ / hex(digest(std::to_string(custody.size()) + ":" + custody +
                                std::string(worker)));
  private_directory(dir);
  if (method == "mining.ai_request") {
    need(v.size() == 7 || v.size() == 8, "Pool AI request arity");
    auto request = hash(v[2]);
    need(hash(v[0]) == request, "Pool AI task/request mismatch");
    (void)hash(v[1]);
    need(hash(v[3]) == model_, "Pool AI model not served");
    auto tokens = integer(v[5]);
    need(tokens > 0 && tokens <= 4096, "Pool AI token limit");
    (void)integer(v[6]);
    auto prompt = decode(text(v[4]));
    need(!prompt.empty(), "Pool AI empty prompt");
    // Only the H15 traced contract is served. A legacy untraced request is not
    // sufficient for the current network's escrow obligations.
    need(v.size() == 8 && v[7].kind_case() == Val::kStructValue,
         "Pool AI traced extension required");
    const auto &tr = at(v[7].struct_value(), "trace");
    need(tr.kind_case() == Val::kStructValue, "Pool AI trace object required");
    const auto &t = tr.struct_value();
    const auto declared_prompt_len = integer(at(t, "prompt_len"));
    // The pool's on-chain trace bound can differ from the opened/inflated
    // prompt's byte count for compressed private requests.
    need(integer(at(t, "tier")) == 0 && declared_prompt_len <= max_prompt,
         "Pool AI trace model/prompt bounds");
    for (const char *key : {"chain", "attention"}) {
      auto i = t.fields().find(key);
      need(i == t.fields().end() || (i->second.kind_case() == Val::kBoolValue &&
                                     i->second.bool_value()),
           "Pool AI requires chain and attention tracing");
    }
    // Correlating RPC IDs are excluded from the identity.
    const auto identity = hex(request) + hex(hash(v[1])) + hex(model_) + ":" +
                          std::to_string(tokens) + ":" +
                          std::to_string(integer(v[6])) + ":" + prompt;
    auto fingerprint =
        hex(digest(std::to_string(declared_prompt_len) + ":" + identity));
    auto path = dir / (hex(request) + ".trace");
    auto bytes = durable_read(path);
    if (!bytes && std::filesystem::exists(dir / "archive")) {
      private_directory(dir / "archive");
      bytes = durable_read(dir / "archive" / path.filename());
    }
    Stored stored;
    if (bytes) {
      stored = unpack(*bytes);
      need(text(at(stored.meta, "fingerprint")) == fingerprint,
           "Pool AI conflicting duplicate request");
    } else {
      std::size_t count = 0;
      std::uintmax_t total = 0;
      for (auto &entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".trace") {
          const auto status = entry.symlink_status();
          need(std::filesystem::is_regular_file(status),
               "Pool trace inventory contains a nonregular file");
          if (std::filesystem::file_time_type::clock::now() -
                  entry.last_write_time() >
              std::chrono::hours(48)) {
            const auto archive = dir / "archive";
            private_directory(archive);
            std::filesystem::rename(entry.path(),
                                    archive / entry.path().filename());
            auto response = entry.path();
            response.replace_extension(".response");
            if (std::filesystem::exists(response))
              std::filesystem::rename(response, archive / response.filename());
            sync_parent(archive / "record");
            sync_parent(entry.path());
          } else {
            ++count;
            total += entry.file_size();
          }
        }
      need(count < 2048 && total < (4ULL << 30),
           "Pool trace retention capacity reached");
      auto capture =
          infer_(prompt, static_cast<std::uint32_t>(tokens), request);
      need(!capture.answer.answer.empty() &&
               capture.answer.answer.size() <= max_prompt &&
               !capture.answer.sealed_plaintext.empty() &&
               !capture.record.empty(),
           "Pool AI incomplete traced inference");
      std::uint32_t prompt_positions = 0;
      for (unsigned i = 0; i < 4; ++i)
        prompt_positions |= std::uint32_t(capture.answer.commitment[32 + i])
                            << (8 * i);
      need(prompt_positions > 0 && prompt_positions <= 8192 &&
               prompt_positions >= declared_prompt_len / 16,
           "Pool trace does not meet original prompt length bound");
      auto &fields = *stored.meta.mutable_fields();
      fields["fingerprint"] = value(fingerprint);
      fields["created_s"] = value(std::to_string(now()));
      fields["answer"] = value(b64(capture.answer.answer));
      fields["body"] = value(hex(capture.answer.sealed_plaintext));
      fields["trace"] = value(hex(capture.answer.commitment));
      stored.capture = std::move(capture.record);
      save(path, stored);
    }
    need(now() >= integer(at(stored.meta, "created_s")) &&
             now() - integer(at(stored.meta, "created_s")) <= 48 * 3600,
         "Pool request trace retention expired");
    List args;
    for (auto s : {std::string(worker), hex(request), hex(request), hex(model_),
                   text(at(stored.meta, "answer"))})
      *args.add_values() = value(s);
    Obj trace;
    (*trace.mutable_fields())["trace"] = at(stored.meta, "trace");
    (*trace.mutable_fields())["body"] = at(stored.meta, "body");
    *args.add_values() = value(std::move(trace));
    return frame("mining.ai_response", std::move(args));
  }
  need(method == "mining.ai_opening" && v.size() >= 3 && v.size() <= 6,
       "Pool AI opening method/arity");
  auto request = hash(v[0]), response = hash(v[1]), seed = hash(v[2]);
  std::vector<std::uint64_t> path;
  List encoded_path;
  if (v.size() > 3 && v[3].kind_case() != Val::kNullValue) {
    need(v[3].kind_case() == Val::kListValue &&
             v[3].list_value().values_size() <= 16,
         "Pool attention path bound");
    for (auto &item : v[3].list_value().values()) {
      auto n = integer(item);
      need(n <= 9007199254740991ULL, "Pool attention path precision");
      path.push_back(n);
      Val val;
      val.set_number_value(static_cast<double>(n));
      *encoded_path.add_values() = val;
    }
  }
  std::vector<std::uint8_t> previous;
  if (v.size() > 4 && v[4].kind_case() != Val::kNullValue) {
    auto s = text(v[4]);
    need(s.size() <= 224U << 10, "Pool prior partials bound");
    previous = stratum::hex_decode_bytes(s);
  }
  need(path.empty() == previous.empty(),
       "Pool attention path/partial mismatch");
  List args;
  for (auto s : {std::string(worker), hex(request), hex(response)})
    *args.add_values() = value(s);
  *args.add_values() = value(encoded_path);
  try {
    auto bytes = durable_read(dir / (hex(request) + ".trace"));
    need(bytes.has_value(), "Pool opening has no locally retained trace");
    auto stored = unpack(*bytes);
    need(now() >= integer(at(stored.meta, "created_s")) &&
             now() - integer(at(stored.meta, "created_s")) <= 48 * 3600,
         "Pool opening trace expired");
    // Verify replay before fixing the first response identity. Its binding is
    // durable before returning the opening to the network sender.
    auto binding = dir / (hex(request) + ".response");
    auto prior = durable_read(binding);
    if (prior)
      need(*prior ==
               std::vector<std::uint8_t>(response.begin(), response.end()),
           "Pool response identity changed");
    auto body = open_(stored.capture, request, response, seed, path, previous);
    need(!body.empty() && body.size() <= 512U << 10, "Pool opening body bound");
    if (!prior)
      publish(binding, response);
    *args.add_values() = value(hex(body));
    *args.add_values() = value("");
  } catch (const std::exception &) {
    *args.add_values() = value("");
    *args.add_values() =
        value("Nexus could not verify or recover this opening");
  }
  return frame("mining.ai_opening_result", std::move(args));
}
} // namespace keryx::nexus::ai
