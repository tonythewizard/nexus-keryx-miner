#pragma once
#include "keryx/nexus/consensus/pom_v4.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;
namespace keryx::nexus {
// One bounded, sequential official verifier. Any transport/validation error is fatal.
class RustProofVerifier {
 public:
  struct Result {double cpu_ms; std::string summary;};
  explicit RustProofVerifier(std::string executable):executable_(std::move(executable)){}
  RustProofVerifier(const RustProofVerifier&)=delete;
  ~RustProofVerifier() {
    if(write_>=0)close(write_);
    if(read_>=0)close(read_);
    if(pid_>0) {
      int status{};
      for(int i=0;i<100;++i) {
        auto n=waitpid(pid_,&status,WNOHANG);
        if(n==pid_ || (n<0 && errno==ECHILD))return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      // Only our unreaped child, never a process group or a node process.
      kill(pid_,SIGTERM);
      for(int i=0;i<100;++i) {
        auto n=waitpid(pid_,&status,WNOHANG);
        if(n==pid_ || (n<0 && errno==ECHILD))return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
  }
  Result verify(std::span<const std::uint8_t> context,std::span<const std::uint8_t> trace,
                std::span<const std::uint8_t> proof) {
    if(failed_)throw std::runtime_error("official verifier is failed; no retry permitted");
    try {
      if(context.size()!=225 || trace.size()!=28+257*1024+256*8 || proof.empty() || proof.size()>2*1024*1024)
        throw std::runtime_error("invalid bounded verifier input dimensions");
      if(!pid_)start();
      if(++sequence_==0)throw std::runtime_error("verification sequence exhausted");
      deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(30);
      std::array<std::uint8_t,28> header{'N','X','V','R'};
      le(header,4,1,4);le(header,8,sequence_,8);le(header,16,context.size(),4);
      le(header,20,trace.size(),4);le(header,24,proof.size(),4);
      transfer(write_,header.data(),header.size(),true);
      transfer(write_,context.data(),context.size(),true);
      transfer(write_,trace.data(),trace.size(),true);
      transfer(write_,proof.data(),proof.size(),true);
      std::array<std::uint8_t,64> response{};
      transfer(read_,response.data(),response.size(),false);
      const auto digest=consensus::blake3_hash(proof);
      if(std::string_view(reinterpret_cast<char*>(response.data()),4)!="NXVA" || get(response,4,4)!=1 ||
         get(response,8,8)!=sequence_ || get(response,16,4)!=0 ||
         !std::equal(digest.begin(),digest.end(),response.begin()+28))
        throw std::runtime_error("official verifier response binding failed");
      const auto length=get(response,60,4),cpu=get(response,20,8);
      if(length==0 || length>2048 || cpu>30'000'000'000ULL)
        throw std::runtime_error("invalid official verifier response bounds");
      std::string summary(length,'\0');transfer(read_,summary.data(),length,false);
      if(!summary.starts_with("OFFICIAL_RUST_REAL_MODEL=PASS "))
        throw std::runtime_error("official verifier did not report full proof validation");
      return {cpu/1.0e6,std::move(summary)};
    } catch(...) {failed_=true;throw;}
  }
 private:
  template<class T>static void le(T &v,std::size_t p,std::uint64_t x,unsigned n){for(unsigned i=0;i<n;++i)v[p+i]=x>>(8*i);}
  template<class T>static std::uint64_t get(const T &v,std::size_t p,unsigned n){std::uint64_t x=0;for(unsigned i=0;i<n;++i)x|=std::uint64_t(v[p+i])<<(8*i);return x;}
  void start() {
    int input[2],output[2];
    if(pipe2(input,O_CLOEXEC))throw std::runtime_error("verifier input pipe failed");
    if(pipe2(output,O_CLOEXEC)){close(input[0]);close(input[1]);throw std::runtime_error("verifier output pipe failed");}
    posix_spawn_file_actions_t actions;
    int rc=posix_spawn_file_actions_init(&actions);
    if(rc==0) {
      rc=posix_spawn_file_actions_adddup2(&actions,input[0],STDIN_FILENO);
      if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,output[1],STDOUT_FILENO);
      char mode[]="verify-real-stream";
      std::array<char*,3> args{executable_.data(),mode,nullptr};
      if(!rc)rc=posix_spawn(&pid_,executable_.c_str(),&actions,nullptr,args.data(),environ);
      posix_spawn_file_actions_destroy(&actions);
    }
    close(input[0]);close(output[1]);
    if(rc){close(input[1]);close(output[0]);pid_=0;throw std::runtime_error("official verifier spawn failed");}
    write_=input[1];read_=output[0];
    if(fcntl(write_,F_SETFL,O_NONBLOCK)<0 || fcntl(read_,F_SETFL,O_NONBLOCK)<0)
      throw std::runtime_error("official verifier pipe setup failed");
  }
  void transfer(int fd,const void* data,std::size_t size,bool writing) {
    sigset_t mask,previous,pending;sigemptyset(&mask);sigaddset(&mask,SIGPIPE);
    if(pthread_sigmask(SIG_BLOCK,&mask,&previous))throw std::runtime_error("verifier signal mask failed");
    sigpending(&pending);bool already=sigismember(&pending,SIGPIPE);
    auto restore=[&]{if(!already){timespec zero{};sigtimedwait(&mask,nullptr,&zero);}pthread_sigmask(SIG_SETMASK,&previous,nullptr);};
    try {
      auto* bytes=static_cast<const std::uint8_t*>(data);
      while(size) {
        auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline_-std::chrono::steady_clock::now()).count();
        if(remaining<=0)throw std::runtime_error("official verifier timeout");
        pollfd p{fd,static_cast<short>(writing?POLLOUT:POLLIN),0};
        auto rc=poll(&p,1,static_cast<int>(remaining));
        if(rc<0 && errno==EINTR)continue;
        if(rc<=0)throw std::runtime_error("official verifier poll failed or timed out");
        auto n=writing?::write(fd,bytes,size) : ::read(fd,const_cast<std::uint8_t*>(bytes),size);
        if(n<0 && (errno==EINTR || errno==EAGAIN))continue;
        if(n<=0)throw std::runtime_error("official verifier rejected proof or closed transport");
        bytes+=n;size-=n;
      }
      restore();
    }catch(...){restore();throw;}
  }
  std::string executable_;pid_t pid_{};int read_{-1},write_{-1};std::uint64_t sequence_{};bool failed_{};
  std::chrono::steady_clock::time_point deadline_;
};
}
