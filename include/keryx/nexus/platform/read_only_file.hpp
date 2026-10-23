#pragma once
// Positional reads share no mutable seek position. The model and Merkle readers
// use the same bounds and canonical byte layout on Windows and Linux.
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace keryx::nexus::platform {
class ReadOnlyFile final {
 public:
  struct Identity {
    std::uint64_t device{}, index{}, size{}, modified{}, changed{};
    bool operator==(const Identity&) const = default;
  };
  explicit ReadOnlyFile(const std::filesystem::path& path, bool no_atime = false):path_(path) {
#ifdef _WIN32
    no_atime_=no_atime;
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | (no_atime ? FILE_WRITE_ATTRIBUTES : 0),
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open read-only file");
    try {
      if (GetFileType(handle_) != FILE_TYPE_DISK)
        throw std::runtime_error("Regular disk file required");
      const auto info = identity();
      (void)info;
      if (no_atime) {
        // Documented Windows sentinel: suppress last-access updates on this
        // handle, without changing existing on-disk timestamps.
        FILETIME unchanged{0xffffffff, 0xffffffff};
        if (!SetFileTime(handle_, nullptr, &unchanged, nullptr))
          throw std::runtime_error("Cannot suppress last-access updates");
      }
    } catch (...) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; throw; }
#else
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | (no_atime ? O_NOATIME : 0));
    if (fd_ < 0) throw std::runtime_error("Cannot open read-only file");
    try { (void)identity(); }
    catch (...) { ::close(fd_); fd_ = -1; throw; }
#endif
  }
  ReadOnlyFile(const ReadOnlyFile&) = delete;
  ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;
  ~ReadOnlyFile() {
#ifdef _WIN32
    if (mapped_) UnmapViewOfFile(mapped_);
    if (mapping_) CloseHandle(mapping_);
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (mapped_) ::munmap(mapped_, mapped_size_);
    if (fd_ >= 0) ::close(fd_);
#endif
  }
  Identity identity() const {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandle(handle_, &info) ||
        !GetFileInformationByHandleEx(handle_, FileBasicInfo, &basic, sizeof(basic)) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      throw std::runtime_error("Cannot identify regular file");
    return {info.dwVolumeSerialNumber,
        (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart),
        static_cast<std::uint64_t>(basic.ChangeTime.QuadPart)};
#else
    struct stat st{};
    if (::fstat(fd_, &st) || !S_ISREG(st.st_mode) || st.st_size < 0)
      throw std::runtime_error("Cannot identify regular file");
    return {static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino),
        static_cast<std::uint64_t>(st.st_size),
        std::uint64_t(st.st_mtim.tv_sec) * 1'000'000'000 + std::uint64_t(st.st_mtim.tv_nsec),
        std::uint64_t(st.st_ctim.tv_sec) * 1'000'000'000 + std::uint64_t(st.st_ctim.tv_nsec)};
#endif
  }
#ifdef _WIN32
  bool same_file(HANDLE other) const {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(other,&info))return false;
    const auto current=identity();
    return current.device==info.dwVolumeSerialNumber &&
        current.index==((std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow) &&
        current.size==((std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow);
  }
#endif
  const std::uint8_t* map() {
    if (mapped_) return static_cast<const std::uint8_t*>(mapped_);
    const auto size = identity().size;
    if (!size || size > std::numeric_limits<std::size_t>::max()) return nullptr;
#ifdef _WIN32
    mapping_ = CreateFileMappingW(handle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_) return nullptr;
    mapped_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
    if (!mapped_) { CloseHandle(mapping_); mapping_ = nullptr; }
#else
    void* value = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_SHARED, fd_, 0);
    if (value != MAP_FAILED) { mapped_ = value; mapped_size_ = static_cast<std::size_t>(size); }
#endif
    return static_cast<const std::uint8_t*>(mapped_);
  }
  void read_at(void* data, std::size_t bytes, std::uint64_t offset) const {
    if (offset > std::numeric_limits<std::uint64_t>::max() - bytes)
      throw std::out_of_range("File read offset overflow");
    auto* output = static_cast<std::uint8_t*>(data);
    while (bytes) {
#ifdef _WIN32
      OVERLAPPED request{};
      request.Offset = static_cast<DWORD>(offset);
      request.OffsetHigh = static_cast<DWORD>(offset >> 32);
      request.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!request.hEvent) throw std::runtime_error("Cannot allocate positional read event");
      DWORD got{};
      const auto wanted = static_cast<DWORD>(std::min<std::size_t>(bytes, 1024 * 1024));
      bool ok = ReadFile(handle_, output, wanted, &got, &request) != FALSE;
      if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(handle_, &request, &got, TRUE) != FALSE;
      CloseHandle(request.hEvent);
      if (!ok || !got) throw std::runtime_error("Short positional file read");
      const auto n = static_cast<std::size_t>(got);
#else
      if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        throw std::out_of_range("File read offset exceeds platform range");
      const auto got = ::pread(fd_, output, bytes, static_cast<off_t>(offset));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) throw std::runtime_error("Short positional file read");
      const auto n = static_cast<std::size_t>(got);
#endif
      output += n; bytes -= n; offset += n;
    }
  }
  void read_at_streaming(void* data,std::size_t bytes,std::uint64_t offset) const {
#ifdef _WIN32
    // Unaligned GGUF tiles are served through a bounded aligned bounce buffer.
    // This handle bypasses the filesystem cache instead of faking fadvise.
    constexpr std::size_t alignment=65536, chunk=1024*1024;
    HANDLE raw=CreateFileW(path_.c_str(),GENERIC_READ | (no_atime_ ? FILE_WRITE_ATTRIBUTES : 0),FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_NO_BUFFERING,nullptr);
    if(raw==INVALID_HANDLE_VALUE)throw std::runtime_error("Unbuffered streaming unavailable");
    struct Guard{HANDLE h;~Guard(){CloseHandle(h);}} guard{raw};
    if(!same_file(raw))throw std::runtime_error("Streaming file identity changed");
    if(no_atime_){
      FILETIME unchanged{0xffffffff,0xffffffff};
      if(!SetFileTime(raw,nullptr,&unchanged,nullptr))
        throw std::runtime_error("Cannot suppress streaming last-access updates");
    }
    std::unique_ptr<void,decltype(&_aligned_free)> buffer(_aligned_malloc(chunk,alignment),&_aligned_free);
    if(!buffer)throw std::runtime_error("Streaming buffer allocation failed");
    auto* output=static_cast<std::uint8_t*>(data);
    while(bytes){
      const auto base=offset/alignment*alignment;
      const auto skip=static_cast<std::size_t>(offset-base);
      const auto wanted=std::min<std::size_t>(bytes,chunk-skip);
      const auto rounded=(skip+wanted+alignment-1)/alignment*alignment;
      LARGE_INTEGER position{};position.QuadPart=static_cast<LONGLONG>(base);
      DWORD got{};
      if(!SetFilePointerEx(raw,position,nullptr,FILE_BEGIN) ||
          !ReadFile(raw,buffer.get(),static_cast<DWORD>(rounded),&got,nullptr) || got<skip+wanted)
        throw std::runtime_error("Short unbuffered streaming read");
      std::memcpy(output,static_cast<std::uint8_t*>(buffer.get())+skip,wanted);
      output+=wanted;bytes-=wanted;offset+=wanted;
    }
#else
    read_at(data,bytes,offset);release_cache(offset,bytes);
#endif
  }
  void release_cache(std::uint64_t offset, std::uint64_t size) const {
#ifdef _WIN32
    // Windows has no POSIX_FADV_DONTNEED equivalent for a file range.
    // Do not pretend a read-ahead hint guarantees cache eviction.
    (void)offset; (void)size;
    throw std::runtime_error("Strict file-cache eviction is unavailable on Windows");
#else
    if (::posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(size), POSIX_FADV_DONTNEED))
      throw std::runtime_error("Cannot release completed file-cache range");
#endif
  }
  void prefetch(std::uint64_t offset, std::uint64_t size) const {
#ifndef _WIN32
    (void)::posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(size), POSIX_FADV_WILLNEED);
#else
    (void)offset; (void)size; // Optional hint; never affects proof correctness.
#endif
  }
 private:
  std::filesystem::path path_;
  void* mapped_{};
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE}, mapping_{};
  bool no_atime_{};
#else
  int fd_{-1};
  std::size_t mapped_size_{};
#endif
};
} // namespace keryx::nexus::platform
