#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace keryx::nexus::platform {
class WinHandle final {
 public:
  HANDLE value{INVALID_HANDLE_VALUE};
  WinHandle()=default;
  explicit WinHandle(HANDLE h):value(h){}
  WinHandle(const WinHandle&)=delete;
  WinHandle& operator=(const WinHandle&)=delete;
  ~WinHandle(){reset();}
  void reset(HANDLE h=INVALID_HANDLE_VALUE){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=h;}
  explicit operator bool()const{return value && value!=INVALID_HANDLE_VALUE;}
};
inline std::wstring quote_windows_argument(const std::wstring& argument) {
  std::wstring out=L"\"";std::size_t slashes=0;
  for(wchar_t c:argument){
    if(c==L'\\'){++slashes;continue;}
    if(c==L'"')out.append(slashes*2+1,L'\\');else out.append(slashes,L'\\');
    slashes=0;out+=c;
  }
  out.append(slashes*2,L'\\');return out+L'"';
}
// A single owned child. Overlapped named pipes enforce the deadline on both
// reads and writes; only the two child endpoints are inherited.
class WindowsVerifierProcess final {
 public:
  explicit WindowsVerifierProcess(const std::filesystem::path& executable) {
    if(!executable.is_absolute())throw std::runtime_error("Absolute verifier executable required");
    WinHandle child_input,child_output;
    pipe(input_,child_input,false);pipe(output_,child_output,true);
    std::array<HANDLE,2> handles{child_input.value,child_output.value};
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<std::uint8_t> attributes(size);
    auto* list=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if(!InitializeProcThreadAttributeList(list,1,0,&size))throw std::runtime_error("Child attribute setup failed");
    struct Guard {PPROC_THREAD_ATTRIBUTE_LIST p;~Guard(){DeleteProcThreadAttributeList(p);}} guard{list};
    if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles.data(),sizeof(handles),nullptr,nullptr))throw std::runtime_error("Child inheritance setup failed");
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=child_input.value;
    startup.StartupInfo.hStdOutput=child_output.value;
    startup.StartupInfo.hStdError=child_output.value;
    startup.lpAttributeList=list;
    auto command=quote_windows_argument(executable.wstring())+L" verify-real-stream";
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,
        executable.parent_path().c_str(),&startup.StartupInfo,&process))
      throw std::runtime_error("Official verifier could not start");
    process_.reset(process.hProcess);WinHandle thread(process.hThread);
    // Kill-on-close contains even a stuck verifier or a descendant. A failed
    // setup terminates only this newly-created suspended child.
    job_.reset(CreateJobObjectW(nullptr,nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job_ || !SetInformationJobObject(job_.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) ||
        !AssignProcessToJobObject(job_.value,process_.value) || ResumeThread(thread.value)==DWORD(-1)) {
      TerminateProcess(process_.value,1);WaitForSingleObject(process_.value,5000);
      throw std::runtime_error("Verifier ownership setup failed");
    }
  }
  ~WindowsVerifierProcess(){
    input_.reset();output_.reset();
    if(process_ && WaitForSingleObject(process_.value,200)!=WAIT_OBJECT_0) {
      job_.reset();WaitForSingleObject(process_.value,5000);
    }
  }
  void transfer(void* data,std::size_t bytes,bool writing,
      std::chrono::steady_clock::time_point deadline) {
    auto* cursor=static_cast<std::uint8_t*>(data);
    HANDLE handle=writing?input_.value:output_.value;
    while(bytes){
      auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
      if(left<=0)throw std::runtime_error("Official verifier timeout");
      OVERLAPPED request{};WinHandle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
      if(!event)throw std::runtime_error("Verifier I/O event unavailable");
      request.hEvent=event.value;DWORD done{};
      DWORD wanted=static_cast<DWORD>(std::min<std::size_t>(bytes,65536));
      bool ok=writing?WriteFile(handle,cursor,wanted,&done,&request)!=FALSE:
          ReadFile(handle,cursor,wanted,&done,&request)!=FALSE;
      if(!ok && GetLastError()==ERROR_IO_PENDING){
        const auto wait=WaitForSingleObject(event.value,static_cast<DWORD>(std::min<std::int64_t>(left,30000)));
        if(wait!=WAIT_OBJECT_0){
          CancelIoEx(handle,&request);
          GetOverlappedResult(handle,&request,&done,TRUE); // retire before stack/event destruction
          throw std::runtime_error("Official verifier transport timeout");
        }
        ok=GetOverlappedResult(handle,&request,&done,FALSE)!=FALSE;
      }
      if(!ok || !done)throw std::runtime_error("Official verifier transport closed");
      cursor+=done;bytes-=done;
    }
  }
 private:
  static void pipe(WinHandle& parent,WinHandle& child,bool parent_reads){
    static std::atomic_uint64_t sequence{};
    const auto name=L"\\\\.\\pipe\\nexus-verifier-"+std::to_wstring(GetCurrentProcessId())+L"-"+
        std::to_wstring(++sequence);
    parent.reset(CreateNamedPipeW(name.c_str(),(parent_reads?PIPE_ACCESS_INBOUND:PIPE_ACCESS_OUTBOUND)|
        FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr));
    if(!parent)throw std::runtime_error("Cannot create private verifier pipe");
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    child.reset(CreateFileW(name.c_str(),parent_reads?GENERIC_WRITE:GENERIC_READ,0,&security,OPEN_EXISTING,0,nullptr));
    if(!child)throw std::runtime_error("Cannot connect verifier pipe");
  }
  WinHandle input_,output_,process_,job_;
};
} // namespace keryx::nexus::platform
#endif
