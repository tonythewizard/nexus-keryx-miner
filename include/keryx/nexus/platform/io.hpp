#pragma once
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#define O_NOFOLLOW 0x100000
#define O_NONBLOCK 0
#define O_DIRECTORY 0x200000
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m)&_S_IFMT)==_S_IFREG)
#define S_ISLNK(m) (((m)&_S_IFMT)==0120000)
#endif
using ssize_t = std::intptr_t;
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace keryx::nexus::platform::io {
#ifdef _WIN32
struct Stat {
  unsigned st_mode{}, st_uid{};
  std::uint64_t st_dev{}, st_ino{};
  std::int64_t st_size{};
};
inline int error() {
  switch(GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno=ENOENT;break;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: errno=EEXIST;break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno=EACCES;break;
    default: errno=EIO;break;
  }
  return -1;
}
class LocalMemory {
 public:
  void* value{};
  ~LocalMemory() { if(value) LocalFree(value); }
};
inline bool private_descriptor(LocalMemory& descriptor) {
  HANDLE token{};
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return false;
  DWORD size{}; GetTokenInformation(token,TokenUser,nullptr,0,&size);
  std::string storage(size,'\0');
  bool ok=GetTokenInformation(token,TokenUser,storage.data(),size,&size)!=FALSE;
  CloseHandle(token);
  if(!ok) return false;
  LocalMemory sid_text;
  if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,
    reinterpret_cast<LPWSTR*>(&sid_text.value))) return false;
  const std::wstring acl=L"D:P(A;;FA;;;"+std::wstring(static_cast<wchar_t*>(sid_text.value))+L")(A;;FA;;;SY)";
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(
    acl.c_str(),SDDL_REVISION_1,
    reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value),nullptr)) return false;
  return true;
}
inline bool owned_private(HANDLE file) {
  HANDLE token{};
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return false;
  DWORD size{}; GetTokenInformation(token,TokenUser,nullptr,0,&size);
  std::string storage(size,'\0');
  const bool token_ok=GetTokenInformation(token,TokenUser,storage.data(),size,&size)!=FALSE;
  CloseHandle(token);
  if(!token_ok) return false;
  auto* user=reinterpret_cast<TOKEN_USER*>(storage.data());
  PSID owner{}; PACL dacl{}; LocalMemory descriptor;
  if(GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
      &owner,nullptr,&dacl,nullptr,reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value))!=ERROR_SUCCESS ||
      !owner || !EqualSid(owner,user->User.Sid) || !dacl) return false;
  BYTE system_sid[SECURITY_MAX_SID_SIZE]; DWORD system_size=sizeof(system_sid);
  if(!CreateWellKnownSid(WinLocalSystemSid,nullptr,system_sid,&system_size)) return false;
  for(DWORD i=0;i<dacl->AceCount;++i) {
    void* entry{}; if(!GetAce(dacl,i,&entry)) return false;
    auto* header=static_cast<ACE_HEADER*>(entry);
    if(header->AceFlags & INHERIT_ONLY_ACE) continue;
    if(header->AceType==ACCESS_DENIED_ACE_TYPE) continue;
    // Reject unfamiliar/callback/object ACEs instead of guessing access.
    if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE) return false;
    auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(entry);
    PSID sid=&ace->SidStart;
    if(ace->Mask && !EqualSid(sid,user->User.Sid) && !EqualSid(sid,system_sid)) return false;
  }
  return true;
}
inline int open(const std::filesystem::path& path,int flags,int mode=0600) {
  DWORD access=(flags&_O_RDWR)?GENERIC_READ|GENERIC_WRITE:
      (flags&_O_WRONLY)?GENERIC_WRITE:GENERIC_READ;
  const bool directory=(flags&O_DIRECTORY)!=0;
  DWORD creation=(flags&_O_CREAT)?((flags&_O_EXCL)?CREATE_NEW:
      (flags&_O_TRUNC)?CREATE_ALWAYS:OPEN_ALWAYS):
      (flags&_O_TRUNC)?TRUNCATE_EXISTING:OPEN_EXISTING;
  LocalMemory descriptor;
  SECURITY_ATTRIBUTES security{sizeof(security),nullptr,FALSE};
  if(flags&_O_CREAT) {
    // Protected DACL: the owner and SYSTEM only. Existing files retain their
    // ACL and must pass owned_private at the relevant authorization boundary.
    if(!private_descriptor(descriptor)) return error();
    security.lpSecurityDescriptor=descriptor.value;
  }
  DWORD attributes=FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT;
  if(directory) attributes|=FILE_FLAG_BACKUP_SEMANTICS;
  HANDLE handle=CreateFileW(path.c_str(),access|READ_CONTROL,
      FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&security,creation,attributes,nullptr);
  if(handle==INVALID_HANDLE_VALUE) return error();
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if(!GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&tag,sizeof(tag)) ||
      ((flags&O_NOFOLLOW) && (tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) ||
      (!directory && (tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY))) {
    CloseHandle(handle); errno=EACCES; return -1;
  }
  int crt=(flags&(_O_RDONLY|_O_WRONLY|_O_RDWR|_O_APPEND))|_O_BINARY|_O_NOINHERIT;
  const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),crt);
  if(fd<0) CloseHandle(handle);
  (void)mode;
  return fd;
}
inline int close(int fd) {return _close(fd);}
inline ssize_t read(int fd,void* data,std::size_t size) {
  return _read(fd,data,static_cast<unsigned>(std::min<std::size_t>(size,INT_MAX)));
}
inline ssize_t write(int fd,const void* data,std::size_t size) {
  return _write(fd,data,static_cast<unsigned>(std::min<std::size_t>(size,INT_MAX)));
}
inline int fsync(int fd) {
  HANDLE handle=reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if(!GetFileInformationByHandleEx(handle,FileAttributeTagInfo,&tag,sizeof(tag))) return error();
  // Windows doesn't expose POSIX directory fsync. Atomic replacements use
  // MoveFileEx(WRITE_THROUGH); file contents are flushed before publication.
  if(tag.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) return 0;
  return FlushFileBuffers(handle)?0:error();
}
inline int fstat(int fd,Stat* st) {
  HANDLE handle=reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  BY_HANDLE_FILE_INFORMATION info{};
  if(!GetFileInformationByHandle(handle,&info)) return error();
  const bool private_file=owned_private(handle);
  st->st_mode=((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)?0120000:
      (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?_S_IFDIR:_S_IFREG) |
      (private_file?0600:0666);
  st->st_uid=private_file?0:1;
  st->st_dev=info.dwVolumeSerialNumber;
  st->st_ino=(std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
  st->st_size=static_cast<std::int64_t>((std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow);
  return 0;
}
inline int lstat(const std::filesystem::path& path,Stat* st) {
  HANDLE handle=CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  if(handle==INVALID_HANDLE_VALUE) return error();
  const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDONLY|_O_BINARY|_O_NOINHERIT);
  if(fd<0) {CloseHandle(handle);return -1;}
  const int result=fstat(fd,st);close(fd);return result;
}
inline int stat(const std::filesystem::path& path,Stat* st) {return lstat(path,st);}
inline int unlink(const std::filesystem::path& path) {return DeleteFileW(path.c_str())?0:error();}
inline int link(const std::filesystem::path& from,const std::filesystem::path& to) {
  return CreateHardLinkW(to.c_str(),from.c_str(),nullptr)?0:error();
}
inline int getpid() {return _getpid();}
inline unsigned getuid() {return 0;}
inline unsigned geteuid() {return 0;}
inline void replace(const std::filesystem::path& from,const std::filesystem::path& to) {
  if(!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    throw std::filesystem::filesystem_error("Atomic replacement failed",from,to,
        std::error_code(static_cast<int>(GetLastError()),std::system_category()));
}
#else
using Stat = struct stat;
using ::open; using ::close; using ::read; using ::write; using ::fsync;
using ::fstat; using ::stat; using ::lstat; using ::unlink; using ::link;
using ::getuid; using ::geteuid; using ::getpid;
inline void replace(const std::filesystem::path& from,const std::filesystem::path& to) {
  std::filesystem::rename(from,to);
}
#endif
} // namespace keryx::nexus::platform::io
