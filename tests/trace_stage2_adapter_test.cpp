#include "keryx/nexus/trace_capture.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus::opoi;
namespace {
int mode=0,calls=0,context_calls=0,last_context=0,context_result=0;
int acquire(void*,int requested){++context_calls;last_context=requested;return context_result==-2 ? (requested>8192 ? -1 : requested) : context_result ? context_result : requested;}
int configure(void*,const void*,std::size_t,int){return 0;}
int count(void*){return mode==4?9:7;}
int generate(void*,const char*,const std::int32_t*,int,int,const char*,int,char*,int,std::int32_t*,int,int*,int*,int,int){return -1;}
int read(void*,std::uint8_t *out,std::size_t,std::uint32_t *batch,std::size_t,std::uint32_t *positions){std::memset(out,0x11,2*7*32);batch[0]=batch[1]=1;*positions=2;return 0;}
int replay(void*,const std::int32_t*,int,int,std::uint32_t,std::uint32_t,std::uint32_t,std::uint8_t*,std::size_t,std::uint8_t*,std::size_t,std::uint32_t*,std::uint32_t*){return -1;}
int trace(void*,const std::int32_t*,int){return 0;}
int multi(void*,const std::int32_t*,int,int,const std::uint32_t *pos,const std::uint32_t *leaf,
          std::uint32_t n,std::uint32_t slot,std::uint8_t *out,std::size_t,std::uint32_t *lens){
 ++calls;for(unsigned i=0;i<n;++i){out[i*slot]=pos[i];out[i*slot+1]=leaf[i];lens[i]=mode==1?slot+4:mode==2?3:4;}
 return mode==3?-1:0;
}
void check(bool ok){if(!ok)throw std::runtime_error("ABI8 adapter test failed");}
template<class F>void reject(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw);}
}
int main(){try{
 detail::TraceApi api{configure,count,generate,read,replay,nullptr};api.abi=8;api.trace_tokens=trace;api.replay_multi=multi;
 std::array<std::uint8_t,16> manifest{};std::memcpy(manifest.data(),"KXMANIF2",8);manifest[8]=1;manifest[12]=1;
 detail::TraceCapture cap(api,&mode,8192);reject([&]{cap.configure(manifest,128);});check(cap.configure(manifest,128,7)==7);
 const std::array<std::int32_t,2> tokens{2,3};auto capture=cap.trace_tokens(tokens);check(capture.tokens.size()==2&&capture.leaves.size()==14);
 const std::array<std::pair<std::uint32_t,std::uint32_t>,2> points{{{0,6},{1,0}}};
 auto result=cap.replay_multi(capture,points,8);check(calls==1&&result.size()==2&&result[0][1]==6&&result[1][0]==1);
 const auto before=calls;reject([&]{cap.replay_multi(capture,points,3);});reject([&]{cap.replay_multi(capture,{},8);});
 const std::array<std::pair<std::uint32_t,std::uint32_t>,1> bad{{{2,0}}};reject([&]{cap.replay_multi(capture,bad,8);});
 const std::array<std::int32_t,1> negative{-1};reject([&]{cap.trace_tokens(negative);});check(calls==before);
 for(mode=1;mode<=3;++mode)reject([&]{cap.replay_multi(capture,points,8);});
 mode=0;api.context_acquire=acquire;
 detail::TraceCapture task(api,&mode,8192);task.configure(manifest,128,7);
 task.use_task_context();check(context_calls==1&&last_context==64);
 task.use_task_context();check(context_calls==1);
 task.trace_tokens(tokens);task.replay_multi(capture,points,8);check(context_calls==1);
 task.ensure_full_context();check(context_calls==2&&last_context==8192);
 task.ensure_full_context();check(context_calls==2);
 context_result=-1;reject([&]{task.use_task_context();});
 reject([&]{task.trace_tokens(tokens);});
 context_result=1;reject([&]{task.trace_tokens(tokens);});
 context_result=0;task.trace_tokens(tokens);check(last_context==64);
 const std::array<std::int32_t,65> long_tokens{};
 // The mock read returns 2 positions: context grows first, then coverage fails.
 reject([&]{task.trace_tokens(long_tokens);});check(last_context==8192);
 detail::TraceCapture floor(api,&mode,32768,8192);floor.configure(manifest,128,7);
 floor.use_task_context();context_result=-2;floor.ensure_full_context();check(last_context==8192);
 const int floor_calls=context_calls;floor.ensure_full_context();check(context_calls==floor_calls);
 floor.trace_tokens(tokens);check(context_calls==floor_calls);context_result=0;
 mode=4;reject([&]{cap.configure(manifest,128,7);});mode=0;
 api.abi=6;detail::TraceCapture old(api,&mode,8192);reject([&]{old.configure(manifest,128,7);});
 std::cout<<"ABI8_TRACE_ADAPTER_CPU=PASS single_replay=PASS malformed_sizes=REJECTED gpu=NOT_USED\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
