#include "keryx/nexus/stratum_v4_client.hpp"
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace keryx::nexus::stratum;
using Object=google::protobuf::Struct;
using Clock=std::chrono::steady_clock;
void check(bool b,const char *s){if(!b)throw std::runtime_error(s);}
Object parse(const std::string &s){Object o;check(google::protobuf::util::JsonStringToMessage(s,&o).ok(),"invalid wire JSON");return o;}
std::string method(const std::string&s){return parse(s).fields().at("method").string_value();}
std::uint64_t id(const std::string&s){return static_cast<std::uint64_t>(parse(s).fields().at("id").number_value());}
void reply(StratumV4Client &c,std::uint64_t n,std::string_view body){check(c.process_inbound_line("{\"id\":"+std::to_string(n)+","+std::string(body)+"}"),"reply not handled");}
std::string authorize(StratumV4Client &c,Clock::time_point now){
 check(c.connect_and_handshake_once(),"mock handshake failed");auto h=c.take_outbound_wire_log();check(h.size()==2,"handshake sent unsolicited metadata");
 check(parse(h[0]).fields().at("params").list_value().values(0).string_value()==nexus_pool_agent,"Nexus impersonated another miner");
 c.poll_pool_extensions(now);check(c.take_outbound_wire_log().empty(),"metadata before authorization");
 reply(c,id(h[1]),"\"result\":true,\"error\":null");c.poll_pool_extensions(now);check(c.take_outbound_wire_log().empty(),"metadata before subscribe ACK");
 reply(c,id(h[0]),"\"result\":[[],\"aa\",7],\"error\":null");c.poll_pool_extensions(now);auto frames=c.take_outbound_wire_log();
 check(frames.size()==2&&method(frames[0])=="mining.declare_capabilities"&&method(frames[1])=="mining.hello","post-auth extension order incorrect");
 check(parse(frames[0]).fields().at("params").list_value().values_size()==0,"unqualified pool AI route advertised models");return frames[1];
}
int main(){try{
 StratumClientConfig cfg;cfg.pool_url="stratum+tcp://127.0.0.1:1";cfg.worker="synthetic";
 cfg.pool_telemetry.enabled=true;cfg.pool_telemetry.interval_seconds=1;
 cfg.pool_telemetry.model_id=std::string(64,'3');cfg.declared_model_id_hex=cfg.pool_telemetry.model_id;cfg.pool_telemetry.build="cuda";
 cfg.pool_telemetry.devices={{0,"NVIDIA","synthetic GPU"}};
 auto unqualified=cfg;unqualified.pool_telemetry.trace_ready=true;bool refused=false;
 try{StratumV4Client unsafe(unqualified);}catch(const std::runtime_error&){refused=true;}
 check(refused,"unqualified pool AI route enabled by configuration");
 for (const auto &host : {"krx.suprnova.cc", "stratum-eu2.suprnova.cc", "stratum-us.suprnova.cc", "s1.keryxpool.com", "custom.invalid"}) {
  for (const bool h16 : {false, true}) {
   auto probe_cfg=cfg;probe_cfg.pool_url=std::string("stratum+tcp://")+host+":4404";
   probe_cfg.h16_protocol_compatible=h16;
   StratumV4Client probe(probe_cfg);probe.enable_loopback_test_mode(true);
   check(probe.connect_and_handshake_once(),"compatibility handshake failed");
   auto handshake=probe.take_outbound_wire_log();
   const auto agent=parse(handshake[0]).fields().at("params").list_value().values(0).string_value();
   const bool expected=h16&&std::string_view(host).ends_with(".suprnova.cc");
   check(agent==(expected?"keryx-miner/0.6.1-compatible (Nexus/0.0.5-dev; native-H16)":nexus_pool_agent),"compatibility agent scope/admission incorrect");
  }
 }
 const auto t=Clock::now();StratumV4Client c(cfg);c.enable_loopback_test_mode(true);c.update_pool_work(0,t);
 const auto hello=authorize(c,t);
 check(c.process_inbound_line("{\"id\":41,\"method\":\"mining.ai_request\",\"params\":[]}"),"pool AI request unhandled");
 auto decline=c.take_outbound_wire_log();check(decline.size()==1&&decline[0].find("46")!=std::string::npos,"unqualified AI request silently swallowed");auto obj=parse(hello).fields().at("params").list_value().values(0).struct_value();
 check(obj.fields().at("v").number_value()==1&&obj.fields().at("trace").number_value()==0,"schema or trace capability incorrect");
 check(obj.fields().at("miner").struct_value().fields().at("name").string_value()=="nexus-keryx-miner","miner identity forged");
 check(hello.find("hostname")==std::string::npos&&hello.find("uuid")==std::string::npos,"unrequested persistent host identity leaked");
 reply(c,id(hello),"\"result\":true,\"error\":null");
 c.poll_pool_extensions(t+std::chrono::seconds(29));check(c.take_outbound_wire_log().empty(),"telemetry minimum interval violated");
 c.update_pool_work(3000,t+std::chrono::seconds(30));c.poll_pool_extensions(t+std::chrono::seconds(30));auto frames=c.take_outbound_wire_log();
 check(frames.size()==1&&method(frames[0])=="mining.telemetry","dynamic frame missing");
 auto metrics=parse(frames[0]).fields().at("params").list_value().values(0).struct_value();
 check(metrics.fields().at("hashrate_total").number_value()==100,"measured completed-walk rate incorrect");
 check(metrics.fields().at("models").list_value().values_size()==0,"unverified pool trace serving advertised");
 c.poll_pool_extensions(t+std::chrono::seconds(60));frames=c.take_outbound_wire_log();check(frames.size()==1,"stale sample frame missing");
 metrics=parse(frames[0]).fields().at("params").list_value().values(0).struct_value();check(!metrics.fields().contains("hashrate_total"),"missing data invented zero hashrate");
 c.update_pool_work(3000,t+std::chrono::seconds(60));c.set_ai_inference_busy(true);
 c.update_pool_work(3000,t+std::chrono::seconds(90));c.poll_pool_extensions(t+std::chrono::seconds(90));frames=c.take_outbound_wire_log();
 check(frames.size()==2,"periodic capability refresh missing");metrics=parse(frames.back()).fields().at("params").list_value().values(0).struct_value();
 check(metrics.fields().at("hashrate_total").number_value()==0&&metrics.fields().at("busy").bool_value(),"measured AI pause not distinguished from missing data");
 reply(c,id(frames.back()),"\"result\":null,\"error\":[20,\"unknown method\",null]");
 auto state=c.telemetry_snapshot();check(state.pool_telemetry_state=="UNSUPPORTED"&&state.rejected_shares==0&&state.accepted_shares==0,"metadata failure altered share receipts");
 c.poll_pool_extensions(t+std::chrono::seconds(120));check(c.take_outbound_wire_log().empty(),"unsupported extension was retried in same session");
 const auto next=authorize(c,t+std::chrono::seconds(121));check(c.telemetry_snapshot().pool_telemetry_state=="HELLO_SENT","failover retained old session status");
 c.update_pool_work(6000,t+std::chrono::seconds(151));c.poll_pool_extensions(t+std::chrono::seconds(151));frames=c.take_outbound_wire_log();
 check(c.telemetry_snapshot().pool_telemetry_state=="ACK_UNKNOWN","old pool ACK attributed to new session");
 reply(c,id(next),"\"result\":false,\"error\":null");check(c.telemetry_snapshot().pool_telemetry_state=="REJECTED","negative metadata result accepted");
 c.set_ai_inference_busy(false);
 c.qualify_pool_ai();
 c.poll_pool_extensions(t+std::chrono::seconds(152));c.take_outbound_wire_log();
 check(c.process_inbound_line("{\"method\":\"mining.ai_request\",\"params\":[]}"),"qualified AI request unhandled");
 auto task=c.take_pool_ai();check(task.has_value(),"qualified AI request not queued");
 check(c.submit_pool_ai_result(task->scope,"{\"method\":\"mining.ai_response\",\"params\":[]}"),"AI result not written");
 frames=c.take_outbound_wire_log();check(frames.size()==1,"AI result frame count");
 reply(c,id(frames[0]),"\"result\":null,\"error\":[20,\"synthetic AI rejection\",null]");
 check(c.telemetry_snapshot().rejected_shares==0,"AI rejection counted as rejected share");
 check(!c.submit_pool_ai_result("another-pool","{\"method\":\"mining.ai_response\",\"params\":[]}"),"AI result crossed pool scope");
 check(!c.decline_pool_ai(*task),"AI request without an ID declined");check(c.take_outbound_wire_log().empty(),"uncorrelated decline sent");
 check(c.process_inbound_line("{\"id\":77,\"method\":\"mining.ai_request\",\"params\":[]}"),"identified AI request unhandled");
 auto unserved=c.take_pool_ai();check(unserved&&unserved->request_id==77u&&unserved->method=="mining.ai_request","AI request ID not retained");
 auto moved=*unserved;moved.scope="another-pool";check(!c.decline_pool_ai(moved),"AI decline crossed pool scope");
 check(c.decline_pool_ai(*unserved),"unserved AI request not declined");frames=c.take_outbound_wire_log();
 {auto o=parse(frames.at(0));const auto &e=o.fields().at("error").list_value();
  check(frames.size()==1&&id(frames[0])==77&&e.values(0).number_value()==46,"unserved AI request not declined as busy");}
 check(c.process_inbound_line("{\"id\":78,\"method\":\"mining.ai_opening\",\"params\":[]}"),"identified opening unhandled");
 auto opening=c.take_pool_ai();check(opening&&!c.decline_pool_ai(*opening)&&c.take_outbound_wire_log().empty(),"audit opening declined as busy");
 check(c.telemetry_snapshot().rejected_shares==0,"AI decline counted as rejected share");
 auto json=pool_metrics_json(cfg.pool_telemetry,0,{},std::numeric_limits<std::uint64_t>::max(),0,0,false);
 check(json.find("18446744073709551615")!=std::string::npos,"u64 receipt count rounded through double");
 auto invalid=cfg.pool_telemetry;invalid.devices.push_back(invalid.devices[0]);bool denied=false;
 try{pool_hello_json(invalid);}catch(const std::invalid_argument&){denied=true;}check(denied,"duplicate GPU mapping accepted");
 {
  StratumV4Client w(cfg);w.enable_loopback_test_mode(true);const auto hi=authorize(w,t);
  reply(w,id(hi),"\"result\":true,\"error\":null");w.qualify_pool_ai();
  w.poll_pool_extensions(t+std::chrono::seconds(1));w.take_outbound_wire_log();
  w.set_pool_mining_available(false);w.poll_pool_extensions(t+std::chrono::seconds(2));
  const auto withdrawn=w.take_outbound_wire_log();bool empty_caps=false,empty_models=false;
  for(const auto &frame:withdrawn){auto o=parse(frame);const auto m=o.fields().at("method").string_value();
   if(m=="mining.declare_capabilities")empty_caps=o.fields().at("params").list_value().values_size()==0;
   if(m=="mining.telemetry")empty_models=o.fields().at("params").list_value().values(0).struct_value().fields().at("models").list_value().values_size()==0;
  }
  check(empty_caps&&empty_models,"OOM withdrawal still advertised mined models");
  check(w.process_inbound_line("{\"id\":99,\"method\":\"mining.ai_request\",\"params\":[]}"),"withdrawn AI request unhandled");
  check(!w.take_pool_ai(),"withdrawn model accepted a new request");w.take_outbound_wire_log();
  check(w.process_inbound_line("{\"method\":\"mining.ai_opening\",\"params\":[]}"),"retained opening unhandled");
  check(w.take_pool_ai().has_value(),"OOM withdrawal abandoned retained openings");
 }
 std::cout<<"POOL_TELEMETRY=PASS honest_agent=true post_auth=true measured_rate=true stale_omitted=true pause_zero=true receipt_isolation=true reconnect_reset=true bounded_identity=true mainnet_shares=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"POOL_TELEMETRY=FAIL "<<e.what()<<'\n';return 1;}}
