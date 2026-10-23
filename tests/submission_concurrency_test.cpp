#include "keryx/nexus/network/submission_gate.hpp"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
using namespace keryx::nexus::network;
int main(){
 SubmissionGate gate;std::atomic_uint accepted{},duplicates{};
 VerifiedSubmission base{"fixture",1,0,true,true,true,true,true,true,true,true};
 std::vector<std::thread> threads;
 for(unsigned t=0;t<8;++t)threads.emplace_back([&,t]{
  for(unsigned i=0;i<128;++i){auto c=base;c.nonce=t*128+i;gate.reserve(c);++accepted;}
  auto shared=base;shared.job_id="shared";shared.nonce=999;
  try{gate.reserve(shared);++accepted;}catch(const std::invalid_argument&){++duplicates;}
 });
 for(auto&t:threads)t.join();
 if(accepted!=1025||duplicates!=7||gate.reserved_count()!=1025)return 1;
 std::cout<<"SUBMISSION_CONCURRENCY=PASS unique=1024 shared_accepted=1 duplicates_rejected=7\n";
}
