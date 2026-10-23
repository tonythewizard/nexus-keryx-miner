#include "keryx/nexus/opoi.hpp"

#include <cuda_runtime_api.h>
#include <openssl/evp.h>
#include <dlfcn.h>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr char uuid[]="GPU-00000000-0000-0000-0000-000000000000";
constexpr char model_sha[]="946072b16f5d672e60357410f900888f6522b3ae49f241b5bf2142cb89637fb6";
constexpr char model_id[]="bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710";
using Abi=int(*)();using Load=void*(*)(const char*,int,int);using Generate=int(*)(void*,const char*,int,char*,int);using Free=void(*)(void*);using Error=const char*(*)();
template<class T>T symbol(void*lib,const char*name){auto value=dlsym(lib,name);if(!value)throw std::runtime_error(std::string("official runtime symbol missing: ")+name);return reinterpret_cast<T>(value);}
std::string sha256(const std::string&path){
  std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("cannot open official GGUF");
  EVP_MD_CTX*raw=EVP_MD_CTX_new();if(!raw)throw std::runtime_error("EVP_MD_CTX_new failed");
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>ctx(raw,EVP_MD_CTX_free);EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr);
  std::array<char,1024*1024>buf{};while(in){in.read(buf.data(),buf.size());const auto n=in.gcount();if(n>0)EVP_DigestUpdate(ctx.get(),buf.data(),static_cast<std::size_t>(n));}
  if(!in.eof())throw std::runtime_error("GGUF read failed");std::array<unsigned char,32>digest{};unsigned size{};EVP_DigestFinal_ex(ctx.get(),digest.data(),&size);if(size!=32)throw std::runtime_error("SHA-256 length mismatch");
  constexpr char hex[]="0123456789abcdef";std::string out(64,'0');for(std::size_t i=0;i<32;++i){out[2*i]=hex[digest[i]>>4];out[2*i+1]=hex[digest[i]&15];}return out;
}
void verify_device(){
  const char*visible=std::getenv("CUDA_VISIBLE_DEVICES");if(!visible||std::string(visible)!=uuid)throw std::runtime_error("CUDA visibility is not restricted to the authorized RTX UUID");
  int count{};if(cudaGetDeviceCount(&count)!=cudaSuccess||count!=1)throw std::runtime_error("CUDA runtime must expose exactly one selected GPU");
  cudaDeviceProp props{};if(cudaGetDeviceProperties(&props,0)!=cudaSuccess||props.major!=8||props.minor!=6||std::string(props.name)!="NVIDIA GeForce primary NVIDIA GPU")throw std::runtime_error("visible CUDA device is not the primary NVIDIA GPU SM 8.6");
}
std::string challenge_prompt(const std::string&nonce){
  // Transcribed from official v0.5.7-PoM src/client/grpc.rs and src/llm.rs:
  // the gRPC challenge body is passed through the registered Qwen3.5 ChatML template.
  constexpr char system_prompt[]="You are a Keryx Network AI, a decentralized assistant served by the GPU miners of the Keryx network. Answer in the language of the user's message. Do not introduce yourself or describe Keryx unless the user asks about it. If asked: Keryx is a proof-of-work BlockDAG derived from Kaspa (about 10 blocks per second) where each mining GPU proves on every block that it holds a model in VRAM (Proof-of-Model), so mining and inference are the same job; requests are on-chain transactions paid in KRX. Web search results, facts or earlier messages included in the request are your sources for recent or specific information: use them. Without them, answer from your training knowledge and say when it may be out of date; never claim to have searched the web or cite sources that are not included in the request. Never mention your underlying model name or the company that trained it: if asked, you are a Keryx Network AI. Be thorough but concise.";
  const std::string user="Keryx inference challenge "+nonce+": briefly describe what you are.";
  return std::string("<|im_start|>system\n")+system_prompt+"<|im_end|>\n<|im_start|>user\n"+user+"<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
}
std::string run_official_abi(const std::string&library,const std::string&model,const std::string&nonce){
  void*lib=dlopen(library.c_str(),RTLD_NOW|RTLD_LOCAL);if(!lib)throw std::runtime_error(std::string("dlopen official runtime: ")+dlerror());
  try{
    const auto abi=symbol<Abi>(lib,"keryx_llama_abi");const auto load=symbol<Load>(lib,"keryx_llama_load");const auto generate=symbol<Generate>(lib,"keryx_llama_generate");const auto free_model=symbol<Free>(lib,"keryx_llama_free");const auto error=symbol<Error>(lib,"keryx_llama_last_error");
    if(abi()!=3)throw std::runtime_error("stock llama ABI mismatch");void*handle=load(model.c_str(),0,8192);if(!handle)throw std::runtime_error(std::string("official model load failed: ")+(error()?error():"unknown"));
    std::vector<char>output(64*1024,'\0');const auto prompt=challenge_prompt(nonce);const int n=generate(handle,prompt.c_str(),64,output.data(),static_cast<int>(output.size()));
    if(n<=0||static_cast<std::size_t>(n)>=output.size()){free_model(handle);throw std::runtime_error("official generation failed");}
    std::string answer(output.data(),static_cast<std::size_t>(n));free_model(handle);dlclose(lib);return answer;
  }catch(...){dlclose(lib);throw;}
}
void write_answer(const std::string&path,const std::string&answer){std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(answer.data(),static_cast<std::streamsize>(answer.size()));if(!out)throw std::runtime_error("could not persist OPoI result");}
}

int main(int argc,char**argv){
  try{
    if(argc!=6)throw std::runtime_error("usage: opoi-reference-runner official|nexus MODEL LIBRARY NONCE_HEX OUTPUT");
    const std::string mode=argv[1],model=argv[2],library=argv[3],nonce=argv[4],output=argv[5];
    verify_device();if(sha256(model)!=model_sha)throw std::runtime_error("GGUF SHA-256 mismatch");
    std::string answer;
    if(mode=="official")answer=run_official_abi(library,model,nonce);
    else if(mode=="nexus"){
      const std::string wire=std::string(model_id)+":"+nonce;
      const auto challenge=keryx::nexus::opoi::parse_challenge(wire);
      keryx::nexus::opoi::StockLlamaRuntime runtime(library,model);
      answer=runtime.answer(challenge);
    }else throw std::runtime_error("unknown OPoI reference mode");
    if(answer.empty())throw std::runtime_error("OPoI inference returned empty output");
    write_answer(output,answer);
    std::cout<<"OPOI_REFERENCE_RUN=PASS mode="<<mode<<" uuid="<<uuid<<" sm=86 context=8192 max_tokens=64 "
             <<"gguf_sha256="<<model_sha<<" output_bytes="<<answer.size()<<" output_sha256="<<sha256(output)<<"\n";
    return 0;
  }catch(const std::exception&e){std::cerr<<"OPOI_REFERENCE_RUN=FAIL detail="<<e.what()<<"\n";return 1;}
}
