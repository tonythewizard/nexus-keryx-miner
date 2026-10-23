// Run only in a delegated nexus-dual-*.service; no GPU initialization.
#include "keryx/nexus/scoped_file_cache_guard.hpp"
#include <array>
#include <vector>
int main(int argc,char**argv){
  try{
    if(argc!=2)throw std::runtime_error("Expected read-only model fixture path");
    const int fd=::open(argv[1],O_RDONLY|O_CLOEXEC);
    if(fd<0)throw std::runtime_error("Cannot open model fixture");
    constexpr std::uint64_t length=384ULL*1024*1024;
    // File-local clean-cache advice, not host-wide cache manipulation.
    if(::posix_fadvise(fd,0,length,POSIX_FADV_DONTNEED)!=0){::close(fd);return 1;}
    {
      keryx::nexus::ScopedFileCacheGuard guard;
      std::vector<char> buffer(8*1024*1024);
      for(std::uint64_t off=0;off<length;off+=buffer.size()){
        if(::pread(fd,buffer.data(),buffer.size(),off)!=static_cast<ssize_t>(buffer.size())){
          ::close(fd);throw std::runtime_error("Short read-only model fixture");
        }
        guard.check();std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));guard.check();
    }
    ::close(fd);std::cout<<"NATIVE_FILE_CACHE_GUARD=PASS read_bytes="<<length<<" model_modified=false gpu_started=false\n";
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
