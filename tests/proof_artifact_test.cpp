#include "keryx/nexus/proof_artifacts.hpp"
#include <array>
#include <fstream>
#include <iostream>
int main(int argc,char**argv){
 if(argc!=2)return 2;
 namespace fs=std::filesystem;fs::path dir(argv[1]);fs::create_directories(dir);
 std::array<fs::path,3> paths{dir/"test.proof",dir/"test.context",dir/"test.trace"};
 for(const auto&p:paths){std::ofstream f(p);f<<"deterministic fixture";}
 bool refused=false;try{keryx::nexus::release_verified_nonwinner_artifacts(paths,false,false);}catch(const std::logic_error&){refused=true;}
 if(!refused)return 1;
 for(const auto&p:paths)if(!fs::exists(p))return 1;
 keryx::nexus::release_verified_nonwinner_artifacts(paths,true,true);
 for(const auto&p:paths)if(!fs::exists(p))return 1;
 keryx::nexus::release_verified_nonwinner_artifacts(paths,true,false);
 for(const auto&p:paths)if(fs::exists(p))return 1;
 fs::remove(dir);std::cout<<"PROOF_ARTIFACT_LIFETIME=PASS winner_preserved unverified_preserved verified_nonwinner_released\n";
}
