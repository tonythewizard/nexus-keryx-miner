#pragma once
// Local experimental inference transport. No node, wallet or submission access.
#include "keryx/nexus/bounded_file_sha256.hpp"
#include <spawn.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include <cerrno>
#include <cstdlib>
#include <string_view>
extern char **environ;
namespace keryx::nexus::opoi {
class VulkanInferenceProcess final {
  std::string executable_,library_,library_sha_,model_;
  int lease_;
public:
  VulkanInferenceProcess(std::string executable,const std::string &sha,std::string library,
      std::string library_sha,std::string model,int lease)
      :executable_(std::move(executable)),library_(std::move(library)),library_sha_(std::move(library_sha)),model_(std::move(model)),lease_(lease){
    if(lease_<0||bounded_file_sha256(executable_)!=sha)
      throw std::runtime_error("Vulkan inference helper identity mismatch");
  }
  std::string infer(std::string_view prompt,std::uint32_t tokens,
      const std::function<bool()> &cancelled={},std::chrono::seconds budget=std::chrono::seconds(120)) const {
    if(prompt.empty()||prompt.size()>60*1024||prompt.find('\0')!=std::string_view::npos||!tokens||tokens>4096||budget.count()<=0)
      throw std::invalid_argument("Invalid bounded child inference request");
    struct Resources {
      int input[2]{-1,-1},output[2]{-1,-1},lease{-1};pid_t pid{-1};
      ~Resources(){
        for(int fd:{input[0],input[1],output[0],output[1],lease})if(fd>=0)close(fd);
        // Do not release the caller's PoM pause while an inference child lives.
        if(pid>0){kill(pid,SIGINT);int status{};while(waitpid(pid,&status,0)<0&&errno==EINTR){}}
      }
    } r;
    if(cancelled&&cancelled())throw std::runtime_error("Vulkan inference cancelled before launch");
    if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,r.input)||pipe2(r.output,O_CLOEXEC))throw std::runtime_error("Inference pipe allocation failed");
    // Keep action sources above all destinations; closefrom avoids inherited sockets/secrets.
    auto move=[](int &fd){const int high=fcntl(fd,F_DUPFD_CLOEXEC,10);if(high<0)throw std::runtime_error("Inference descriptor duplication failed");close(fd);fd=high;};
    move(r.input[0]);move(r.input[1]);move(r.output[0]);move(r.output[1]);
    r.lease=fcntl(lease_,F_DUPFD_CLOEXEC,10);if(r.lease<0)throw std::runtime_error("Inference lease duplication failed");
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions))throw std::runtime_error("Inference spawn actions failed");
    int rc=posix_spawn_file_actions_adddup2(&actions,r.input[0],0);
    rc|=posix_spawn_file_actions_adddup2(&actions,r.output[1],1);
    rc|=posix_spawn_file_actions_adddup2(&actions,r.lease,3);
    rc|=posix_spawn_file_actions_addclosefrom_np(&actions,4);
    std::vector<std::string> args{executable_,library_,library_sha_,model_,std::to_string(tokens),"8192","--lease-fd=3"};
    std::vector<char*> argv;for(auto &arg:args)argv.push_back(arg.data());argv.push_back(nullptr);
    // Loader scope belongs to the child; never mutate the multi-lane parent's environment.
    std::vector<std::string> child_env;
    const char* ai_icd=std::getenv("NEXUS_VULKAN_AI_ICD");
    if(ai_icd && std::string_view(ai_icd)!="/usr/share/vulkan/icd.d/nvidia_icd.json" &&
       std::string_view(ai_icd)!="/usr/share/vulkan/icd.d/radeon_icd.json")rc=EINVAL;
    for(char** e=environ;*e;++e){
      const std::string_view item(*e);
      if(ai_icd&&(item.starts_with("VK_DRIVER_FILES=")||item.starts_with("VK_ICD_FILENAMES=")))continue;
      child_env.emplace_back(*e);
    }
    if(ai_icd){child_env.emplace_back(std::string("VK_DRIVER_FILES=")+ai_icd);child_env.emplace_back(std::string("VK_ICD_FILENAMES=")+ai_icd);}
    std::vector<char*> envp;for(auto &e:child_env)envp.push_back(e.data());envp.push_back(nullptr);
    if(!rc)rc=posix_spawn(&r.pid,executable_.c_str(),&actions,nullptr,argv.data(),envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if(rc)throw std::runtime_error("Vulkan inference child spawn failed");
    close(r.input[0]);r.input[0]=-1;close(r.output[1]);r.output[1]=-1;
    if(fcntl(r.input[1],F_SETFL,O_NONBLOCK)<0||fcntl(r.output[0],F_SETFL,O_NONBLOCK)<0)
      throw std::runtime_error("Inference pipe configuration failed");
    // The child remains exclusive under the inherited open-file-description lease.
    const auto until=std::chrono::steady_clock::now()+budget;std::size_t sent=0;
    std::string answer;bool eof=false,exited=false;int status{};
    while(true){
      if((cancelled&&cancelled())||std::chrono::steady_clock::now()>=until)
        throw std::runtime_error("Vulkan inference cancelled or deadline exhausted");
      struct pollfd fds[2]{{r.input[1],POLLOUT,0},{r.output[0],POLLIN,0}};
      if(poll(fds,2,25)<0&&errno!=EINTR)throw std::runtime_error("Inference pipe polling failed");
      if(r.input[1]>=0&&(fds[0].revents&POLLOUT)){
        if(fds[0].revents&(POLLERR|POLLHUP))throw std::runtime_error("Inference child closed its prompt pipe");
        const auto n=send(r.input[1],prompt.data()+sent,prompt.size()-sent,MSG_NOSIGNAL);
        if(n>0)sent+=n;else if(n<0&&errno!=EAGAIN&&errno!=EINTR)throw std::runtime_error("Inference prompt write failed");
        if(sent==prompt.size()){close(r.input[1]);r.input[1]=-1;}
      }
      char buffer[4096];
      for(;;){const auto n=read(r.output[0],buffer,sizeof buffer);
        if(n>0){if(answer.size()+n>65535)throw std::runtime_error("Inference answer exceeds its bound");answer.append(buffer,n);}
        else if(n==0){eof=true;break;}
        else {if(errno!=EAGAIN&&errno!=EINTR)throw std::runtime_error("Inference output read failed");break;}}
      const auto done=exited?0:waitpid(r.pid,&status,WNOHANG);
      if(!exited&&done==r.pid){r.pid=-1;exited=true;}
      if(exited&&eof){
        if(!WIFEXITED(status)||WEXITSTATUS(status)!=0||answer.empty())throw std::runtime_error("Vulkan inference child failed");
        return answer;
      }
      if(done<0&&errno!=EINTR)throw std::runtime_error("Inference child wait failed");
    }
  }
};
}
