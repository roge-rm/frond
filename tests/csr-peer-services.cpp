// Packet ownership, controller credits and receive fragmentation at the TG50 bridge.
#include "BcspH4Bridge.h"
#include <cassert>
#include <cstdio>
#include <algorithm>
using csr::BcspTransport;
using Bytes=BcspTransport::Bytes;
struct Backend {
 static constexpr size_t capacity=1024*1024,maxFrame=65540;
 bool connected=true;size_t occupied{};std::deque<uint8_t> input;std::vector<Bytes> sent;
 bool Connected()const{return connected;}size_t Queued()const{return occupied;}size_t Pending()const{return input.size();}
 void Poll(bool=true){}bool Write(const uint8_t*p,size_t n){assert(n<=capacity-occupied);sent.emplace_back(p,p+n);return connected;}
 uint16_t Read(){if(input.empty())return 0x100;auto c=input.front();input.pop_front();return c;}
 void Close(){connected=false;input.clear();}void Receive(const Bytes& p){input.insert(input.end(),p.begin(),p.end());}
};
struct Fixture {
 BcspTransport t{{100,200,12}};Backend b;csr::BcspH4Bridge<Backend> bridge{t,b};uint8_t seq{},ack{};
 Fixture(){t.Start(0);Drain();Link(BcspTransport::syncResponse);Drain();Link(BcspTransport::confResponse);assert(t.State()==BcspTransport::Link::active);}
 void Feed(const Bytes& w){for(auto c:w)t.Write(c,0);}
 void Link(const std::array<uint8_t,4>& p){Feed(BcspTransport::FrameBytes(1,{p.begin(),p.end()}));}
 Bytes Drain(){Bytes w;for(unsigned i=0;i<10000;++i){auto c=t.Read(0);if(c==0x100)return w;w.push_back(c);}assert(false);return {};}
 void Guest(uint8_t channel,const Bytes& p){Feed(BcspTransport::FrameBytes(channel,p,true,seq,ack));seq=(seq+1)&7;bridge.Pump(0);}
 std::vector<Bytes> Collect(uint8_t channel=6,bool doAck=true){auto wire=Drain();std::vector<Bytes> out;Bytes raw;bool esc=false;
  for(auto c:wire){if(c==0xc0){if(raw.size()>=4 && (raw[1]&15)==channel){size_t n=(raw[1]>>4)|(size_t(raw[2])<<4);assert(raw.size()==n+4+2*bool(raw[0]&64));out.emplace_back(raw.begin()+4,raw.begin()+4+n);ack=((raw[0]&7)+1)&7;}raw.clear();continue;}if(esc){assert(c==0xdc||c==0xdd);raw.push_back(c==0xdc?0xc0:0xdb);esc=false;}else if(c==0xdb)esc=true;else raw.push_back(c);}
  if(doAck)Feed(BcspTransport::FrameBytes(0,{},false,0,ack));
  return out;
 }
 void Negotiate(){b.Receive({4,14,11,1,5,16,0,192,0,72,1,0,1,0});bridge.Pump(0);assert(Collect(5).size()==1);}
 void Flow(unsigned slots=2,unsigned mtu=800){Guest(5,{0x33,0x0c,7,uint8_t(mtu),uint8_t(mtu>>8),0,uint8_t(slots),uint8_t(slots>>8),0,0});b.Receive({4,14,4,1,0x33,0x0c,0});bridge.Pump(0);Collect(5);Guest(5,{0x31,0x0c,1,1});b.Receive({4,14,4,1,0x31,0x0c,0});bridge.Pump(0);Collect(5);b.sent.clear();}
 void Complete(unsigned handle,unsigned count=1){b.Receive({4,19,5,1,uint8_t(handle),uint8_t(handle>>8),uint8_t(count),uint8_t(count>>8)});bridge.Pump(0);}
};
static Bytes acl(unsigned handle,unsigned n,unsigned flags=0x20){Bytes b{2,uint8_t(handle),uint8_t((handle>>8)|flags),uint8_t(n),uint8_t(n>>8)};for(unsigned i=0;i<n;++i)b.push_back(uint8_t(i*73+19));return b;}
static void fragments(){for(unsigned n:{0,1,412,413,824,825,65535})for(unsigned flags:{0x10,0x20,0x60,0xa0}){
 Fixture f;auto original=acl(0x321,n,flags);f.b.Receive(original);Bytes joined;unsigned count=0;
 for(unsigned i=0;i<200 && (i==0 || joined.size()<n);++i){f.bridge.Pump(0);for(auto& p:f.Collect()){assert(p.size()>=4&&p.size()<=416);assert(p[0]==0x21 && (p[1]&0xcf)==(original[2]&0xcf));assert((p[1]&0x30)==(count?0x10:(flags&0x30)));assert(p.size()==size_t(4+p[2]+(unsigned(p[3])<<8)));joined.insert(joined.end(),p.begin()+4,p.end());++count;}}
 assert(count==std::max(1u,(n+411)/412)&&joined==Bytes(original.begin()+5,original.end())&&f.bridge.Error().empty());
 }
 Fixture f;f.b.Receive(acl(1,65535));for(unsigned i=0;i<7;++i)f.bridge.Pump(0);assert(f.t.Outstanding()==6);assert(f.Collect(6,false).size()==6);
 for(unsigned i=0;i<5;++i)f.bridge.Pump(0);
 assert(f.t.Outstanding()==6);f.Feed(BcspTransport::FrameBytes(0,{},false,0,f.ack));f.bridge.Pump(0);assert(f.Collect().size()==1);
 for(unsigned flag:{0,0x30,0xe0}){Fixture bad;bad.b.Receive(acl(1,413,flag));bad.bridge.Pump(0);assert(!bad.bridge.Error().empty()&&!bad.b.connected);}
 puts("PASS ACL0/1/412/413/824/825/65535 exact payload, PB/BC/handle, six-slot backpressure and unsupported flags");
}
static Bytes tx(unsigned h,unsigned n=192){auto p=acl(h,n);p.erase(p.begin());return p;}
static void credits(){Fixture f;f.Negotiate();f.Guest(6,tx(1));assert(f.b.sent.size()==1);f.Guest(6,tx(2));assert(f.b.sent.size()==1&&f.t.Received()==1);f.Complete(1);assert(f.Collect(5).size()==1);f.bridge.Pump(0);assert(f.b.sent.size()==2&&f.t.Received()==0);assert(f.b.sent[0]==acl(1,192)&&f.b.sent[1]==acl(2,192));
 f.Complete(2);f.Collect(5);assert(f.bridge.Error().empty());f.Guest(6,tx(1,193));assert(!f.bridge.Error().empty()&&!f.b.connected);
 Fixture bad;bad.Negotiate();bad.Complete(1);assert(!bad.bridge.Error().empty());
 Fixture flow;flow.Guest(5,{0x31,0x0c,1,2});assert(!flow.bridge.Error().empty()&&flow.b.sent.empty());
 Fixture disconnect;disconnect.Negotiate();disconnect.Guest(6,tx(1));disconnect.Guest(6,tx(2));disconnect.b.Receive({4,5,4,0,1,0,0x13});disconnect.bridge.Pump(0);disconnect.Collect(5);disconnect.bridge.Pump(0);assert(disconnect.b.sent.size()==2);
 puts("PASS genuine192/one global credit across handles, exact completions, oversized TX/refused flow and disconnect credit release");
}
static void events(){
 Fixture f;const uint8_t tiny[]={0xff,0};for(unsigned i=0;i<6;++i)assert(f.t.Queue(5,tiny,sizeof(tiny)));
 Bytes event{4,0xff,255};for(unsigned i=0;i<255;++i)event.push_back(uint8_t(i));f.b.Receive(event);f.bridge.Pump(0);
 assert(f.Collect(5).size()==6);f.bridge.Pump(0);auto packets=f.Collect(5);assert(packets.size()==1&&packets[0]==Bytes(event.begin()+1,event.end()));
 Fixture duplicate;duplicate.Negotiate();duplicate.Guest(6,tx(1));duplicate.Complete(1);duplicate.Collect(5);duplicate.Complete(1);assert(!duplicate.bridge.Error().empty());
 Fixture over;over.Negotiate();over.Guest(6,tx(1));over.Complete(1,2);assert(!over.bridge.Error().empty());
 puts("PASS255-byte HCI event indivisible under six-slot backpressure; duplicate/over-completions refused");
}
static void hostflow(){
 Fixture f;f.Guest(5,{0x33,0x0c,7,0x20,3,0,2,0,0,0});f.b.Receive({4,14,4,1,0x33,0x0c,0});f.bridge.Pump(0);f.Collect(5);
 f.Guest(5,{0x31,0x0c,1,1});f.b.Receive({4,14,4,1,0x31,0x0c,0});f.bridge.Pump(0);f.Collect(5);f.b.sent.clear();
 f.b.Receive(acl(1,825));f.bridge.Pump(0);f.bridge.Pump(0);f.bridge.Pump(0);assert(f.Collect().size()==2);assert(f.b.sent.empty());
 f.Guest(5,{0x35,0x0c,5,1,1,0,2,0});assert(f.b.sent.empty());f.bridge.Pump(0);auto tail=f.Collect();assert(tail.size()==1&&tail[0].size()==5);
 f.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(f.b.sent==std::vector<Bytes>({{1,0x35,0x0c,5,1,1,0,1,0}}));
 f.b.sent.clear();f.b.Receive(acl(2,1));f.bridge.Pump(0);f.b.Receive(acl(3,1));f.bridge.Pump(0);f.b.Receive(acl(4,1));f.bridge.Pump(0);assert(f.Collect().size()==2);
 f.Guest(5,{0x35,0x0c,9,2,3,0,1,0,2,0,1,0});f.bridge.Pump(0);assert(f.Collect().size()==1);
 assert(f.b.sent==std::vector<Bytes>({{1,0x35,0x0c,9,2,3,0,1,0,2,0,1,0}}));
 f.Guest(5,{0x35,0x0c,5,1,4,0,0,0});assert(f.bridge.Error().empty());
 f.Guest(5,{0x35,0x0c,5,1,4,0,2,0});assert(!f.bridge.Error().empty());
 Fixture reset;reset.Negotiate();reset.b.Receive({4,14,4,1,3,0x0c,0});reset.bridge.Pump(0);reset.Collect(5);reset.Guest(6,tx(1));assert(reset.b.sent.size()==1);
 puts("PASS real hostflow completion mapping, partial original/last credit, global two-slot interleaved handles, zero/over-count and reset buffer constants");
}
static void retired(){
 Fixture f;f.Flow();f.b.Receive(acl(1,1));f.bridge.Pump(0);f.Collect();f.b.Receive({4,5,4,0,1,0,0x13});f.bridge.Pump(0);f.Collect(5);
 f.b.Receive(acl(1,1));f.bridge.Pump(0);f.Collect();f.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(f.b.sent.empty()&&f.bridge.Error().empty());f.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(f.b.sent.size()==1&&f.bridge.Error().empty());
 Fixture reset;reset.Flow();reset.b.Receive(acl(1,1));reset.bridge.Pump(0);reset.Collect();reset.b.Receive({4,14,4,1,3,0x0c,0});reset.bridge.Pump(0);reset.Collect(5);reset.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(reset.b.sent.empty()&&reset.bridge.Error().empty());
 Fixture partial;partial.Flow(1);partial.b.Receive(acl(1,825));partial.bridge.Pump(0);partial.Collect();const uint8_t event[]={0xff,0};for(unsigned i=0;i<6;++i)assert(partial.t.Queue(5,event,sizeof(event)));
 partial.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(partial.bridge.Error().empty());partial.Guest(5,{0x31,0x0c,1,0});assert(!partial.bridge.Error().empty());
 puts("PASS disconnect late RX ack and same-handle reuse, reset retired acknowledgements, partial-original flow transition refusal");
}
static void resetFence(){
 const Bytes reset{4,14,4,1,3,0x0c,0},ordinary{4,0xff,1,0x71};
 for(bool wholly:{false,true}) {
  Fixture f;f.Flow(1);
  if(wholly){f.b.Receive(acl(2,1));f.bridge.Pump(0);assert(f.Collect().size()==1);}
  f.b.Receive(acl(1,825));f.bridge.Pump(0);
  if(!wholly){auto head=f.Collect();assert(head.size()==1&&head[0].size()==416);}
  assert(f.t.Outstanding()==0);
  f.b.occupied=Backend::capacity;f.Guest(5,{3,0x0c,0});
  assert(f.b.sent.empty()&&f.t.Received()==1);
  f.b.Receive(ordinary);f.b.Receive(acl(1,1));f.b.Receive(reset);
  f.bridge.Pump(0);assert(f.Collect(5).empty()&&f.b.Pending()==ordinary.size()+6+reset.size());
  f.b.occupied=0;f.bridge.Pump(0);
  assert(f.b.sent==std::vector<Bytes>({{1,3,0x0c,0}}));
  assert(f.Collect(5)==std::vector<Bytes>({Bytes(ordinary.begin()+1,ordinary.end())}));
  f.bridge.Pump(0);assert(f.Collect().empty());
  f.bridge.Pump(0);assert(f.Collect(5)==std::vector<Bytes>({Bytes(reset.begin()+1,reset.end())}));
  assert(f.b.Pending()==0&&f.bridge.ResetStatus()==0&&f.bridge.Error().empty());
  // Valid late completions before the new epoch return no controller credit.
  f.Guest(5,{0x35,0x0c,5,1,uint8_t(wholly?2:1),0,1,0});
  assert(f.b.sent.size()==1&&f.bridge.Error().empty());
  f.Flow(1);f.b.Receive(acl(1,1));f.bridge.Pump(0);assert(f.Collect().size()==1);
  f.Guest(5,{0x35,0x0c,5,1,1,0,1,0});
  assert(f.b.sent==std::vector<Bytes>({{1,0x35,0x0c,5,1,1,0,1,0}}));
 }
 Fixture ordered;ordered.Flow(1);ordered.b.Receive(acl(1,825));ordered.bridge.Pump(0);ordered.Collect();ordered.b.Receive(ordinary);
 for(unsigned i=0;i<16;++i){ordered.bridge.Pump(0);assert(ordered.Collect(5).empty());}
 assert(ordered.b.Pending()==ordinary.size());
 for(unsigned i=0;i<2;++i){ordered.Guest(5,{0x35,0x0c,5,1,1,0,1,0});auto part=ordered.Collect();assert(part.size()==1&&part[0].size()==(i?5:416));}
 ordered.bridge.Pump(0);assert(ordered.Collect(5).size()==1);
 Fixture queued;queued.Flow(1);queued.b.Receive(acl(1,825));queued.bridge.Pump(0);
 assert(queued.t.Outstanding()==1);queued.Guest(5,{3,0x0c,0});queued.b.Receive(reset);queued.bridge.Pump(0);
 // Reliable flight preserves the old prefix ahead of the genuine completion.
 auto raw=queued.Drain();
 std::vector<Bytes> wirePackets;Bytes packet;bool escaped=false;
 for(auto byte:raw){if(byte==0xc0){if(packet.size()>=4&&(packet[0]&0x80))wirePackets.push_back(packet);packet.clear();continue;}if(escaped){packet.push_back(byte==0xdc?0xc0:0xdb);escaped=false;}else if(byte==0xdb)escaped=true;else packet.push_back(byte);}
 assert(wirePackets.size()==2&&queued.t.Outstanding()==2);
 assert((wirePackets[0][1]&15)==6&&(wirePackets[1][1]&15)==5);
 assert(((wirePackets[1][0]-wirePackets[0][0])&7)==1);
 const auto prefix=acl(1,825);
 assert(Bytes(wirePackets[0].begin()+8,wirePackets[0].end())==Bytes(prefix.begin()+5,prefix.begin()+417));
 assert(Bytes(wirePackets[1].begin()+4,wirePackets[1].end())==Bytes(reset.begin()+1,reset.end()));
 Fixture failed;failed.Flow(1);failed.b.Receive(acl(1,825));failed.bridge.Pump(0);failed.Collect();failed.Guest(5,{3,0x0c,0});
 const Bytes failure{4,14,4,1,3,0x0c,0x0c};failed.b.Receive(failure);failed.bridge.Pump(0);
 assert(failed.bridge.ResetStatus()==0x0c&&failed.bridge.Error().empty());
 assert(failed.Collect(5,false)==std::vector<Bytes>({Bytes(failure.begin()+1,failure.end())}));
 for(unsigned i=0;i<16;++i){failed.bridge.Pump(0);assert(failed.bridge.Error().empty()&&failed.b.connected);}
 failed.Feed(BcspTransport::FrameBytes(0,{},false,0,failed.ack));
 failed.bridge.Pump(0);assert(!failed.bridge.Error().empty()&&!failed.b.connected);
 Fixture timeout;timeout.Guest(5,{3,0x0c,0});timeout.bridge.Pump(3000000000ull);assert(!timeout.bridge.Error().empty());
 Fixture malformed;malformed.Guest(5,{3,0x0c,1,0});assert(!malformed.bridge.Error().empty()&&malformed.b.sent.empty());
 Fixture repeat;repeat.Guest(5,{3,0x0c,0});repeat.Guest(5,{3,0x0c,0});assert(repeat.b.sent.size()==1&&repeat.t.Received()==1);
 repeat.b.Receive(reset);repeat.bridge.Pump(0);repeat.Collect(5);repeat.bridge.Pump(0);assert(repeat.b.sent.size()==2);
 Fixture abandoned;abandoned.Flow(1,23);abandoned.b.Receive(acl(1,1));abandoned.bridge.Pump(0);abandoned.Collect();abandoned.Guest(5,{3,0x0c,0});abandoned.b.Receive(reset);abandoned.bridge.Pump(0);abandoned.Collect(5);
 abandoned.bridge.Pump(10000000000ull);assert(abandoned.bridge.Error().empty());abandoned.b.sent.clear();
 abandoned.Guest(5,{0x31,0x0c,1,1});assert(abandoned.b.sent==std::vector<Bytes>({{1,0x31,0x0c,1,1}}));abandoned.b.Receive({4,14,4,1,0x31,0x0c,0});abandoned.bridge.Pump(0);abandoned.Collect(5);abandoned.b.sent.clear();
 abandoned.b.Receive(acl(1,30));abandoned.bridge.Pump(0);auto first=abandoned.Collect();assert(first.size()==1&&first[0].size()==27);abandoned.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(abandoned.b.sent.empty());auto last=abandoned.Collect();assert(last.size()==1&&last[0].size()==11);abandoned.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(abandoned.b.sent==std::vector<Bytes>({{1,0x35,0x0c,5,1,1,0,1,0}}));
 Fixture delivery;delivery.Flow(1);delivery.b.Receive(acl(1,1));delivery.bridge.Pump(0);delivery.Collect();delivery.Guest(5,{3,0x0c,0});const auto before=delivery.ack;delivery.b.Receive(reset);delivery.bridge.Pump(0);delivery.Collect(5,false);const auto after=delivery.ack;delivery.ack=before;
 delivery.Guest(5,{0x31,0x0c,1,1});assert(delivery.b.sent.size()==1&&delivery.t.Received()==1);auto saved=delivery.t.Save(0);uint64_t clock=0;assert(delivery.t.Load(saved,clock));delivery.bridge.Pump(0);assert(delivery.b.sent.size()==1);
 delivery.ack=after;delivery.Feed(BcspTransport::FrameBytes(0,{},false,0,after));delivery.bridge.Pump(0);assert(delivery.b.sent.size()==2);delivery.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(delivery.b.sent.size()==2&&delivery.bridge.Error().empty());delivery.b.Receive({4,14,4,1,0x31,0x0c,0});delivery.bridge.Pump(0);delivery.Collect(5);
 for(unsigned i=0;i<24;++i){delivery.b.Receive(ordinary);delivery.bridge.Pump(0);assert(delivery.Collect(5).size()==1);}assert(delivery.bridge.Error().empty());
 Fixture blocked;blocked.Flow(2);blocked.b.Receive(acl(1,1));blocked.bridge.Pump(0);blocked.Collect();blocked.b.Receive(acl(2,1));blocked.bridge.Pump(0);blocked.Collect();blocked.Guest(5,{3,0x0c,0});blocked.b.Receive(reset);blocked.bridge.Pump(0);blocked.Collect(5);blocked.b.occupied=Backend::capacity;
 blocked.Guest(5,{0x31,0x0c,1,1});blocked.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(blocked.b.sent.size()==1&&blocked.t.Received()==2);blocked.b.occupied=0;blocked.bridge.Pump(0);blocked.bridge.Pump(0);assert(blocked.b.sent.size()==2&&blocked.bridge.Error().empty());
 blocked.b.Receive({4,14,4,1,0x31,0x0c,0x0c});blocked.bridge.Pump(0);blocked.Collect(5);blocked.Guest(5,{0x35,0x0c,5,1,2,0,1,0});assert(blocked.b.sent.size()==2&&blocked.bridge.Error().empty());
 Fixture geometry;geometry.Flow(2,23);for(unsigned h:{1,2}){geometry.b.Receive(acl(h,1));geometry.bridge.Pump(0);geometry.Collect();}geometry.Guest(5,{3,0x0c,0});geometry.b.Receive(reset);geometry.bridge.Pump(0);geometry.Collect(5);
 geometry.Guest(5,{0x33,0x0c,7,17,0,0,1,0,0,0});geometry.b.Receive({4,14,4,1,0x33,0x0c,0x0c});geometry.bridge.Pump(0);geometry.Collect(5);auto count=geometry.b.sent.size();geometry.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(geometry.b.sent.size()==count);
 geometry.Guest(5,{0x33,0x0c,7,17,0,0,1,0,0,0});geometry.b.Receive({4,14,4,1,0x33,0x0c,0});geometry.bridge.Pump(0);geometry.Collect(5);geometry.Guest(5,{0x31,0x0c,1,1});geometry.b.Receive({4,14,4,1,0x31,0x0c,0});geometry.bridge.Pump(0);geometry.Collect(5);geometry.b.sent.clear();geometry.b.Receive(acl(2,30));geometry.bridge.Pump(0);assert(geometry.Collect()[0].size()==21);geometry.Guest(5,{0x35,0x0c,5,1,2,0,1,0});assert(geometry.b.sent.empty());assert(geometry.Collect()[0].size()==17);geometry.Guest(5,{0x35,0x0c,5,1,2,0,1,0});assert(geometry.b.sent==std::vector<Bytes>({{1,0x35,0x0c,5,1,2,0,1,0}}));
 Fixture window;window.Flow(1);window.b.Receive(acl(1,825));window.bridge.Pump(0);const uint8_t notification[]{0xff,0};for(unsigned i=0;i<5;++i)assert(window.t.Queue(5,notification,2));window.Guest(5,{3,0x0c,0});window.b.Receive(reset);window.bridge.Pump(0);window.Guest(5,{0x31,0x0c,1,1});assert(window.b.sent.size()==1&&window.t.Received()==1);assert(window.Collect(5,false).size()==5);window.Feed(BcspTransport::FrameBytes(0,{},false,0,window.ack));window.bridge.Pump(0);assert(window.b.sent.size()==1);assert(window.Collect(5,false).size()==1);window.bridge.Pump(0);assert(window.b.sent.size()==1);window.Feed(BcspTransport::FrameBytes(0,{},false,0,window.ack));window.bridge.Pump(0);assert(window.b.sent.size()==2);window.b.Receive({4,14,4,1,0x31,0x0c,0});window.bridge.Pump(0);window.Collect(5);window.b.sent.clear();window.b.Receive(acl(1,1));window.bridge.Pump(0);assert(window.Collect().size()==1);window.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(window.b.sent==std::vector<Bytes>({{1,0x35,0x0c,5,1,1,0,1,0}}));
 Fixture unknown;unknown.Guest(5,{3,0x0c,0});unknown.b.Receive(reset);unknown.bridge.Pump(0);unknown.Collect(5);unknown.Guest(5,{0x31,0x0c,1,1});unknown.b.Receive({4,14,4,1,0x31,0x0c,0});unknown.bridge.Pump(0);unknown.Collect(5);assert(unknown.bridge.Error().empty());unknown.b.Receive(acl(1,1));unknown.bridge.Pump(0);assert(!unknown.bridge.Error().empty()&&unknown.b.sent.size()==2);
 Fixture unannounced;unannounced.Guest(5,{0x33,0x0c,7,17,0,0,1,0,0,0});unannounced.b.Receive({4,14,4,1,0x33,0x0c,0x0c});unannounced.bridge.Pump(0);unannounced.Collect(5);unannounced.Guest(5,{0x31,0x0c,1,1});unannounced.b.Receive({4,14,4,1,0x31,0x0c,0});unannounced.bridge.Pump(0);unannounced.Collect(5);unannounced.b.Receive(acl(1,1));unannounced.bridge.Pump(0);assert(!unannounced.bridge.Error().empty()&&unannounced.b.sent.size()==2);
 Fixture flowFirst;flowFirst.Guest(5,{0x31,0x0c,1,1});flowFirst.b.Receive({4,14,4,1,0x31,0x0c,0});flowFirst.bridge.Pump(0);flowFirst.Collect(5);flowFirst.Guest(5,{0x33,0x0c,7,17,0,0,1,0,0,0});flowFirst.b.Receive({4,14,4,1,0x33,0x0c,0});flowFirst.bridge.Pump(0);flowFirst.Collect(5);flowFirst.b.sent.clear();flowFirst.b.Receive(acl(1,18));flowFirst.bridge.Pump(0);assert(flowFirst.Collect()[0].size()==21);flowFirst.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(flowFirst.b.sent.empty());assert(flowFirst.Collect()[0].size()==5);flowFirst.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(flowFirst.b.sent==std::vector<Bytes>({{1,0x35,0x0c,5,1,1,0,1,0}}));
 Fixture noReset;noReset.Flow(1);noReset.b.Receive(acl(1,1));noReset.bridge.Pump(0);noReset.Collect();noReset.b.Receive({4,5,4,0,1,0,0x13});noReset.bridge.Pump(0);noReset.Collect(5);noReset.Guest(5,{0x33,0x0c,7,17,0,0,1,0,0,0});noReset.b.Receive({4,14,4,1,0x33,0x0c,0});noReset.bridge.Pump(0);noReset.Collect(5);const auto sent=noReset.b.sent.size();noReset.Guest(5,{0x35,0x0c,5,1,1,0,1,0});assert(noReset.b.sent.size()==sent&&noReset.bridge.Error().empty());
 Fixture full;full.Flow(1);full.b.Receive(acl(1,825));full.bridge.Pump(0);full.Collect();const uint8_t note[]{0xff,0};for(unsigned i=0;i<6;++i)assert(full.t.Queue(5,note,2));
 full.Guest(5,{3,0x0c,0});full.b.Receive(failure);full.bridge.Pump(0);assert(full.bridge.ResetStatus()==0x0c&&full.bridge.Error().empty());assert(full.Collect(5).size()==6);
 full.bridge.Pump(0);assert(full.Collect(5,false)==std::vector<Bytes>({Bytes(failure.begin()+1,failure.end())}));full.bridge.Pump(0);assert(full.bridge.Error().empty());full.Feed(BcspTransport::FrameBytes(0,{},false,0,full.ack));full.bridge.Pump(0);assert(!full.bridge.Error().empty());
 Fixture bound;bound.Flow(1);for(unsigned i=0;i<8192;++i){bound.b.Receive(acl(1,1));bound.bridge.Pump(0);assert(bound.Collect().size()==1);bound.b.Receive({4,5,4,0,1,0,0x13});bound.bridge.Pump(0);bound.Collect(5);}bound.b.Receive(acl(1,1));bound.bridge.Pump(0);assert(!bound.bridge.Error().empty()&&!bound.b.connected);
 puts("PASS exact ResetCC frame ACK gate/checkpoint/sequence reuse, real successful Flow-only epoch with actual23-byte geometry, abandoned old tokens/no invented credits, blocked/failed Flow and ordinaryBuffer no cutover,8192-token bound");
 puts("PASS Reset partial/wholly-held fence, blocked admission, ordinary event order, cancelled ACL, retired ACK/handle reuse, queued prefix, genuine failure/status, timeout and repeated request");
}
int main(){fragments();credits();events();hostflow();retired();resetFence();}
