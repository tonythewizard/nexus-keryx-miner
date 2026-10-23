#include "keryx/nexus/capture.hpp"
#include "messages.pb.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

int main(int argc,char **argv) {
  assert(argc==2); const std::filesystem::path p(argv[1]); std::filesystem::create_directories(p.parent_path());
  std::filesystem::remove(p);
  protowire::KaspadResponse response; response.set_id(9);
  auto *templ=response.mutable_getblocktemplateresponse(); templ->set_issynced(true);
  templ->mutable_block()->mutable_header()->set_daascore(123);
  const auto wire=response.SerializeAsString();
  keryx::nexus::CaptureWriter writer(p.string());
  writer.append("mainnet", {wire.begin(),wire.end()});
  auto records=keryx::nexus::read_capture(p.string()); assert(records.size()==1 && records[0].network=="mainnet");
  protowire::KaspadResponse decoded; assert(decoded.ParseFromArray(records[0].response_wire.data(),static_cast<int>(records[0].response_wire.size())));
  assert(decoded.getblocktemplateresponse().block().header().daascore()==123);
  auto bad=p; bad += ".bad"; std::filesystem::copy_file(p,bad,std::filesystem::copy_options::overwrite_existing);
  { std::fstream f(bad,std::ios::in|std::ios::out|std::ios::binary); f.seekg(10); char c; f.read(&c,1); f.seekp(10); c^=1; f.write(&c,1); }
  bool rejected=false; try { (void)keryx::nexus::read_capture(bad.string()); } catch(const std::runtime_error &) { rejected=true; }
  std::filesystem::remove(bad); assert(rejected);
}
