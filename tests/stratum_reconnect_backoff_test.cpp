#include "keryx/nexus/stratum_v4_client.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace keryx::nexus::stratum;
using namespace std::chrono_literals;
int main() {
  int listener=socket(AF_INET,SOCK_STREAM,0);
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(listener<0 || bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener,32)) return 1;
  socklen_t length=sizeof(address);getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length);
  std::atomic_bool stop{};std::atomic_uint connections{};
  std::thread server([&] {
    while(!stop) {
      pollfd p{listener,POLLIN,0};if(poll(&p,1,20)<=0)continue;
      int peer=accept(listener,nullptr,nullptr);if(peer<0)break;
      ++connections;
      const char denied[]="{\"id\":2,\"result\":false,\"error\":[24,\"suspended\",null]}\n";
      send(peer,denied,sizeof(denied)-1,MSG_NOSIGNAL);
      shutdown(peer,SHUT_RDWR);close(peer);
    }
  });
  StratumClientConfig config;
  config.pool_url="stratum+tcp://127.0.0.1:"+std::to_string(ntohs(address.sin_port));
  config.worker="synthetic";config.reconnect_initial_ms=50;config.reconnect_max_ms=200;
  StratumV4Client client(config);client.start();
  std::this_thread::sleep_for(650ms);client.stop();stop=true;server.join();close(listener);
  if(connections<3 || connections>6) {
    std::cerr<<"TCP-success/disconnect retry storm: "<<connections<<'\n';return 1;
  }
  std::cout<<"STRATUM_RECONNECT_BACKOFF=PASS attempts="<<connections<<" window_ms=650\n";
}
