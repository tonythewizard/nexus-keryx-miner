#include "keryx/nexus/trace_capture.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus::opoi;
namespace {
int mode=0,seq_calls=0,replay_calls=0,context_calls=0;
void check(bool v){if(!v)throw std::runtime_error("ABI9 sequence assertion failed");}
template<class F>void reject(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw);}
int configure(void*,const void*,std::size_t,int){return 0;}
int count(void*){return 7;}
int generate(void*,const char*,const std::int32_t*,int,int,const char*,int,char *out,int,std::int32_t *tokens,int,int *prompt,int *fed,int,int){
 std::memcpy(out,"OK",2);tokens[0]=2;tokens[1]=3;*prompt=1;*fed=2;return 2;
}
int read(void*,std::uint8_t *out,std::size_t,std::uint32_t *batch,std::size_t,std::uint32_t *n){std::memset(out,1,2*7*32);batch[0]=batch[1]=1;*n=2;return 0;}
int replay(void*,const std::int32_t*,int,int,std::uint32_t,std::uint32_t,std::uint32_t,std::uint8_t*,std::size_t,std::uint8_t*,std::size_t,std::uint32_t*,std::uint32_t*){return -1;}
int trace(void*,const std::int32_t*,int){return 0;}
int acquire(void*,int n){++context_calls;return n;}
int seq_config(void*,const std::uint32_t *layers,std::uint32_t n,std::uint32_t a,std::uint32_t r,std::uint32_t h,std::uint32_t iv){
 ++seq_calls;if(n){check(n==2&&a==1&&r==1&&h==2&&iv==32);check(layers[0]==0&&layers[1]==1&&layers[5]==1&&layers[6]==3);}return mode==1?-1:0;
}
int roots(void*,std::uint8_t *out,std::size_t cap,std::uint32_t *n){check(cap==10*32);std::memset(out,0x21,cap);*n=mode==2?8:mode==3?11:10;return mode==4?-4:0;}
int seq_replay(void*,const std::int32_t *tok,int nt,int np,const std::uint32_t *stream,
 const std::uint64_t *lo,const std::uint64_t *hi,std::uint32_t n,std::uint32_t chunks,
 std::uint8_t *bytes,std::size_t cap,std::uint64_t *lens,std::uint8_t *cvs,std::size_t cv_cap,std::uint32_t *nc){
 ++replay_calls;check(nt==2&&np==2&&tok[0]==2&&n==2&&chunks==4&&cap==8&&cv_cap==256);
 check(stream[0]==0&&stream[1]==9&&lo[0]==1&&hi[1]==8);
 for(unsigned i=0;i<cap;++i)bytes[i]=i;
 std::memset(cvs,0x11,32);std::memset(cvs+32,0x22,64);
 lens[0]=mode==5?5:4;lens[1]=4;nc[0]=mode==6?5:mode==7?0:1;nc[1]=2;
 return mode==8?-1:0;
}
}
int main(){try{
 detail::TraceApi api{configure,count,generate,read,replay,nullptr};api.abi=9;api.trace_tokens=trace;api.context_acquire=acquire;
 api.seq_configure=seq_config;api.seq_roots=roots;api.seq_replay=seq_replay;
 std::array<std::uint8_t,16> manifest{};std::memcpy(manifest.data(),"KXMANIF2",8);manifest[12]=1;
 const std::array<SequenceLayer,2> layout{{{0,1,0,2,32},{1,3,0,2,32}}};
 const std::array<std::int32_t,2> tokens{2,3};const std::array<SequenceSpan,2> spans{{{0,1,5},{9,4,8}}};
 detail::TraceCapture cap(api,&mode,8192);cap.configure(manifest,128,7);
 auto no_seq=cap.trace_tokens(tokens);check(!no_seq.sequence_roots);
 reject([&]{cap.replay_sequence(no_seq,spans,4);});
 auto bad=layout;bad[1][0]=0;reject([&]{cap.configure_sequence(bad,1,1,2,32);});
 bad=layout;bad[1][1]=1;reject([&]{cap.configure_sequence(bad,1,1,2,32);});
 bad=layout;bad[1][4]=33;reject([&]{cap.configure_sequence(bad,1,1,2,32);});
 reject([&]{cap.configure_sequence(layout,512,512,256,32);});check(seq_calls==0);
 check(cap.configure_sequence(layout,1,1,2,32)==10);
 auto generated=cap.generate("hello",nullptr,1,0,0);check(generated.text=="OK"&&generated.sequence_roots&&generated.sequence_roots->size()==10);
 auto captured=cap.trace_tokens(tokens);check(captured.sequence_roots&&captured.sequence_roots->size()==10);
 auto result=cap.replay_sequence(captured,spans,4);
 check(result.size()==2&&result[0].bytes==std::vector<std::uint8_t>({0,1,2,3})&&result[1].bytes[0]==4);
 check(result[0].chunk_cvs.size()==1&&result[1].chunk_cvs.size()==2&&result[1].chunk_cvs[0][0]==0x22);
 const auto before=replay_calls;
 auto wrong=captured;wrong.sequence_roots.reset();reject([&]{cap.replay_sequence(wrong,spans,4);});
 wrong=captured;wrong.tokens[0]=-1;reject([&]{cap.replay_sequence(wrong,spans,4);});
 reject([&]{cap.replay_sequence(captured,{},4);});reject([&]{cap.replay_sequence(captured,spans,0);});
 for(auto span:std::array<SequenceSpan,4>{{{10,0,4},{0,4,3},{0,0,~std::uint64_t(0)},{0,4,4}}})
  reject([&]{cap.replay_sequence(captured,std::span(&span,1),4);});
 check(replay_calls==before);
 for(mode=2;mode<=4;++mode){reject([&]{cap.trace_tokens(tokens);});reject([&]{cap.generate("hello",nullptr,1,0,0);});}
 for(mode=5;mode<=8;++mode)reject([&]{cap.replay_sequence(captured,spans,4);});
 mode=1;reject([&]{cap.configure_sequence(layout,1,1,2,32);});
 reject([&]{cap.replay_sequence(captured,spans,4);});reject([&]{cap.trace_tokens(tokens);});mode=0;
 check(cap.configure_sequence({},0,0,0,0)==0);check(!cap.trace_tokens(tokens).sequence_roots);
 for(int abi:{6,8}){api.abi=abi;detail::TraceCapture old(api,&mode,8192);reject([&]{old.configure_sequence(layout,1,1,2,32);});}
 api.abi=9;api.seq_replay=nullptr;detail::TraceCapture incomplete(api,&mode,8192);reject([&]{incomplete.configure_sequence(layout,1,1,2,32);});
 std::cout<<"ABI9_SEQUENCE_CPU=PASS capture_replay=MOCK malformed=REJECTED gpu=NOT_USED\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
