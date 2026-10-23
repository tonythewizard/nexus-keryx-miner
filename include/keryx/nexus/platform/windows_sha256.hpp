#pragma once
#ifdef _WIN32
#include "keryx/nexus/platform/read_only_file.hpp"
#include <openssl/evp.h>
#include <array>
#include <malloc.h>
#include <memory>
#include <string>
namespace keryx::nexus {
inline std::string windows_file_sha256(const std::string& path,FileHashIoStats* stats,bool prefer_direct) {
  const auto native=std::filesystem::u8path(path);
  // Keep the identity handle open without write/delete sharing for the entire
  // hash; no replacement or concurrent writer may change the input.
  platform::ReadOnlyFile file(native);
  const auto before=file.identity();
  constexpr std::size_t chunk=1024*1024;
  std::unique_ptr<void,decltype(&_aligned_free)> buffer(_aligned_malloc(chunk,65536),&_aligned_free);
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
  if(!buffer || !ctx || EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1)
    throw std::runtime_error("SHA-256 initialization failed");
  HANDLE raw=INVALID_HANDLE_VALUE;
  if(prefer_direct) raw=CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL|FILE_FLAG_NO_BUFFERING,nullptr);
  struct Guard {HANDLE h;~Guard(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}} guard{raw};
  FileHashIoStats io;io.direct_io=raw!=INVALID_HANDLE_VALUE;
  for(std::uint64_t offset=0;offset<before.size;){
    auto wanted=static_cast<std::size_t>(std::min<std::uint64_t>(chunk,before.size-offset));
    if(io.direct_io){
      DWORD got{};
      if(!ReadFile(raw,buffer.get(),static_cast<DWORD>(chunk),&got,nullptr) || got<wanted)
        throw std::runtime_error("Unbuffered SHA-256 read failed");
    }else file.read_at(buffer.get(),wanted,offset);
    if(EVP_DigestUpdate(ctx.get(),buffer.get(),wanted)!=1)throw std::runtime_error("SHA-256 update failed");
    offset+=wanted;io.bytes=offset;
  }
  if(file.identity()!=before)throw std::runtime_error("SHA-256 input changed");
  std::array<unsigned char,32> digest{};unsigned length{};
  if(EVP_DigestFinal_ex(ctx.get(),digest.data(),&length)!=1 || length!=digest.size())
    throw std::runtime_error("SHA-256 finalization failed");
  std::string result;result.reserve(64);
  for(auto byte:digest){result+="0123456789abcdef"[byte>>4];result+="0123456789abcdef"[byte&15];}
  if(stats)*stats=io; // Buffered Windows reads make no cache-eviction claim.
  return result;
}
}
#endif
