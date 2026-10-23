#include "keryx/nexus/node_stream_liveness.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using keryx::nexus::NodeStreamLiveness;
using Action=NodeStreamLiveness::Action;
void require(bool valid){if(!valid)throw std::runtime_error("node stream liveness regression");}
int main(){try{
  NodeStreamLiveness s;
  require(s.poll(1'000'000'000'000ULL)==Action::None);
  require(s.begin_wait(0));require(!s.begin_wait(1));require(s.poll(s.probe_ns-1)==Action::None);
  require(s.poll(s.probe_ns)==Action::Probe);require(s.poll(s.probe_ns)==Action::None);
  require(s.poll(2*s.probe_ns)==Action::Probe);
  require(s.poll(s.limit_ns)==Action::Reconnect);require(!s.end_wait());
  require(!s.begin_wait(2*s.limit_ns));require(s.poll(3*s.limit_ns)==Action::None);
  NodeStreamLiveness received;
  require(received.begin_wait(0));require(received.end_wait());
  require(received.poll(100*s.limit_ns)==Action::None); // synchronous AI processing is not silence
  require(received.begin_wait(100*s.limit_ns));
  require(received.poll(100*s.limit_ns+s.probe_ns)==Action::Probe);
  require(received.end_wait());require(received.begin_wait(200*s.limit_ns));
  require(received.poll(200*s.limit_ns-1)==Action::None); // invalid negative clock delta
  require(received.end_wait());
  for(unsigned i=0;i<1000;++i){
    NodeStreamLiveness raced;require(raced.begin_wait(0));
    std::atomic_bool go{false};Action action=Action::None;bool accepted=false;
    std::thread timer([&]{while(!go.load(std::memory_order_acquire)){}action=raced.poll(raced.limit_ns);});
    std::thread reader([&]{while(!go.load(std::memory_order_acquire)){}accepted=raced.end_wait();});
    go.store(true,std::memory_order_release);timer.join();reader.join();
    require(accepted==(action==Action::None)); // exactly one outcome; never a revived timed-out lease
  }
  std::cout<<"NODE_STREAM_LIVENESS=PASS thresholds=30s/90s ai_processing=EXCLUDED timeout_race=1000/1000\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
