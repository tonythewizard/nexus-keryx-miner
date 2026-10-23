#pragma once

#include <openssl/evp.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace keryx::nexus {

struct FileHashIoStats {
  std::uint64_t bytes{};
  std::uint64_t advised_bytes{};
  bool direct_io{};
};

// Hash every byte without populating the host's cache with a multi-GiB model.
// O_DIRECT changes only this read descriptor. Unsupported filesystems use
// bounded reads with completed-range cache advice; never skip verification.
inline std::string bounded_file_sha256(const std::string &path,
                                      FileHashIoStats *stats = nullptr,
                                      bool prefer_direct = true) {
  struct File {
    int fd{-1};
    ~File() { if (fd >= 0) ::close(fd); }
  } file;
  FileHashIoStats io;
  int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_DIRECT
  if (prefer_direct) {
    file.fd = ::open(path.c_str(), flags | O_DIRECT);
    if (file.fd >= 0) io.direct_io = true;
    else if (errno != EINVAL && errno != EOPNOTSUPP && errno != ENOSYS)
      throw std::runtime_error("cannot open GGUF for direct SHA-256 verification");
  }
#else
  (void)prefer_direct;
#endif
  if (file.fd < 0) file.fd = ::open(path.c_str(), flags);
  if (file.fd < 0) throw std::runtime_error("cannot open GGUF for SHA-256 verification");
  struct stat before{};
  if (::fstat(file.fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0)
    throw std::runtime_error("SHA-256 input must be a readable regular file");
  constexpr std::size_t chunk = 1024 * 1024;
  void *raw_buffer = nullptr;
  if (::posix_memalign(&raw_buffer, 4096, chunk))
    throw std::runtime_error("SHA-256 aligned buffer allocation failed");
  std::unique_ptr<void, decltype(&std::free)> buffer(raw_buffer, std::free);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 initialization failed");
  auto advise_buffered = [&] {
    if (::posix_fadvise(file.fd, 0, 0, POSIX_FADV_RANDOM) ||
        ::posix_fadvise(file.fd, 0, 0, POSIX_FADV_NOREUSE))
      throw std::runtime_error("cannot bound buffered SHA-256 read-ahead");
  };
  if (!io.direct_io) advise_buffered();
  for (;;) {
    const auto n = ::pread(file.fd, buffer.get(), chunk, static_cast<off_t>(io.bytes));
    if (n < 0) {
      if (errno == EINTR) continue;
      if (io.direct_io && (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOSYS)) {
        // Some filesystems accept O_DIRECT at open but reject the first read
        // or an unaligned EOF. Restart the full digest through the fallback.
        const int replacement = ::open(path.c_str(), flags);
        struct stat reopened{};
        if (replacement < 0) throw std::runtime_error("cannot reopen SHA-256 input");
        if (::fstat(replacement, &reopened) || reopened.st_dev != before.st_dev ||
            reopened.st_ino != before.st_ino || reopened.st_size != before.st_size) {
          ::close(replacement);
          throw std::runtime_error("SHA-256 input changed during direct-I/O fallback");
        }
        ::close(file.fd); file.fd = replacement;
        io = {};
        if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
          throw std::runtime_error("SHA-256 fallback initialization failed");
        advise_buffered();
        continue;
      }
      throw std::runtime_error("GGUF read failed during SHA-256 verification");
    }
    if (!n) break;
    if (static_cast<std::uint64_t>(n) > static_cast<std::uint64_t>(before.st_size) - io.bytes)
      throw std::runtime_error("GGUF grew during SHA-256 verification");
    if (EVP_DigestUpdate(ctx.get(), buffer.get(), static_cast<std::size_t>(n)) != 1)
      throw std::runtime_error("SHA-256 update failed");
    if (!io.direct_io) {
      if (::posix_fadvise(file.fd, static_cast<off_t>(io.bytes), n, POSIX_FADV_DONTNEED))
        throw std::runtime_error("cannot release completed SHA-256 file-cache range");
      io.advised_bytes += static_cast<std::uint64_t>(n);
    }
    io.bytes += static_cast<std::uint64_t>(n);
    if (io.bytes == static_cast<std::uint64_t>(before.st_size)) break;
  }
  struct stat after{}, named{};
  if (::fstat(file.fd, &after) || ::stat(path.c_str(), &named) ||
      io.bytes != static_cast<std::uint64_t>(before.st_size) ||
      after.st_size != before.st_size || named.st_dev != before.st_dev ||
      named.st_ino != before.st_ino || after.st_mtim.tv_sec != before.st_mtim.tv_sec ||
      after.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
      after.st_ctim.tv_sec != before.st_ctim.tv_sec ||
      after.st_ctim.tv_nsec != before.st_ctim.tv_nsec)
    throw std::runtime_error("GGUF changed during SHA-256 verification");
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1 || length != 32)
    throw std::runtime_error("SHA-256 finalization failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string result(64, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    result[2 * i] = hex[digest[i] >> 4];
    result[2 * i + 1] = hex[digest[i] & 15];
  }
  if (stats) *stats = io;
  return result;
}

} // namespace keryx::nexus
