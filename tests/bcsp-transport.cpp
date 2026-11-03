// Focused CSR BCSP framing, reliability, bounded state and H4 ownership checks.
#include "BcspTransport.h"
#include "BcspH4Bridge.h"
#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>

using csr::BcspTransport;
using Bytes=BcspTransport::Bytes;
static constexpr BcspTransport::Timing timing{100,200,11};
static void Feed(BcspTransport& t,const Bytes& p,uint64_t now=0) {
    for(auto c:p)t.Write(c,now);
}
static Bytes Drain(BcspTransport& t,uint64_t now=0) {
    Bytes out;
    for(size_t bound=0;bound<10000;++bound) {
        const auto c=t.Read(now);
        if(c==0x100)return out;
        out.push_back(uint8_t(c));
    }
    assert(false && "bounded wire drain");return {};
}
static Bytes Link(const std::array<uint8_t,4>& p) {
    return BcspTransport::FrameBytes(1,Bytes(p.begin(),p.end()));
}
static void Ready(BcspTransport& t) {
    t.Start(0);assert(Drain(t)==Link(BcspTransport::sync));
    Feed(t,Link(BcspTransport::syncResponse));
    assert(Drain(t)==Link(BcspTransport::conf));
    Feed(t,Link(BcspTransport::conf));
    const auto reply=Drain(t);auto expected=Link(BcspTransport::confResponse);
    const auto conf=Link(BcspTransport::conf);expected.insert(expected.end(),conf.begin(),conf.end());
    assert(reply==expected);
    Feed(t,Link(BcspTransport::confResponse));assert(t.State()==BcspTransport::Link::active);
}
static Bytes Command(uint8_t seq=0,uint8_t ack=0,bool crc=false) {
    return BcspTransport::FrameBytes(5,{1,0x10,0},true,seq,ack,crc);
}
static void Vectors() {
    assert(Link(BcspTransport::sync)==Bytes({0xc0,0,0x41,0,0xbe,0xda,0xdc,0xed,0xed,0xc0}));
    assert(Command()==Bytes({0xc0,0x80,0x35,0,0x4a,1,0x10,0,0xc0}));
    assert(BcspTransport::FrameBytes(0,{},false,0,1)==Bytes({0xc0,8,0,0,0xf7,0xc0}));
    const Bytes check{'1','2','3','4','5','6','7','8','9'};
    assert(BcspTransport::Crc(check.data(),check.size())==0x89f6);
    const auto escaped=BcspTransport::FrameBytes(2,{0xc0,0xdb},true);
    assert(escaped==Bytes({0xc0,0x80,0x22,0,0x5d,0xdb,0xdc,0xdb,0xdd,0xc0}));
}
static void Parser() {
    const Bytes payload{0xc0,0xdb,0x11,0x13};
    const auto bytes=BcspTransport::FrameBytes(2,payload,true,0,0,true);
    for(size_t split=0;split<=bytes.size();++split) {
        BcspTransport t(timing);Ready(t);
        Feed(t,Bytes(bytes.begin(),bytes.begin()+split));
        Feed(t,Bytes(bytes.begin()+split,bytes.end()));
        assert(t.Received()==1 && t.Expected()==1);
        assert(t.Peek()->channel==2 && t.Peek()->payload==payload);
    }
    BcspTransport t(timing);Ready(t);
    auto bad=Command();bad[4]^=1;Feed(t,bad);assert(!t.Received());
    bad=Command(0,0,true);bad[bad.size()-2]^=1;Feed(t,bad);assert(!t.Received());
    Feed(t,{0xc0,0xdb,0xc0});assert(!t.Received());
    Feed(t,{0xc0,0xdb,0x11,0xc0});assert(!t.Received());
    Feed(t,{0xc0,0,0,0xc0});assert(!t.Received());
    Feed(t,Command());assert(t.Received()==1);
    t.Pop();Drain(t);
    // Unsupported unreliable HCI, invalid ACK payload and unknown channels.
    Feed(t,BcspTransport::FrameBytes(5,{1,0x10,0},false));
    Feed(t,BcspTransport::FrameBytes(0,{0},false));
    Feed(t,BcspTransport::FrameBytes(7,{0},true,1));
    assert(!t.Received() && t.Expected()==1);
    Bytes overflow(2000,0x41);overflow.front()=overflow.back()=0xc0;
    Feed(t,overflow);Feed(t,Command(1));assert(t.Received()==1);
}
static void Reliability() {
    BcspTransport t(timing);Ready(t);
    Feed(t,Command());Feed(t,Command());assert(t.Received()==1 && t.Expected()==1);
    assert(Drain(t)==BcspTransport::FrameBytes(0,{},false,0,1));t.Pop();
    Feed(t,Command(2));assert(!t.Received() && t.Expected()==1);Drain(t);
    for(unsigned i=1;i<9;++i){Feed(t,Command(i&7));assert(t.Received()==1);t.Pop();Drain(t);}
    assert(t.Expected()==1);
    const Bytes event{0x0e,4,1,1,0x10,0};
    assert(t.Queue(5,event.data(),event.size()));
    const auto first=Drain(t,0);assert(!first.empty());assert(Drain(t,199).empty());
    const auto repeat=Drain(t,200);assert(repeat==first && t.Outstanding()==1);
    Feed(t,BcspTransport::FrameBytes(0,{},false,0,1),200);assert(!t.Outstanding());
    assert(Drain(t,1000).empty());
    for(unsigned i=0;i<6;++i)assert(t.Queue(5,event.data(),event.size()));
    assert(!t.Queue(5,event.data(),event.size()));Drain(t,1000);
    // Old ACK0 is outside outstanding1..6, it cannot release the window.
    Feed(t,BcspTransport::FrameBytes(0,{},false,0,0));assert(t.Outstanding()==6);
    Feed(t,BcspTransport::FrameBytes(0,{},false,0,7));assert(!t.Outstanding());
    assert(t.Queue(5,event.data(),event.size()));Drain(t,1000);
    Feed(t,BcspTransport::FrameBytes(0,{},false,0,0));assert(!t.Outstanding());
}
static void LengthBoundaries() {
    BcspTransport t(timing);Ready(t);
    Bytes maximum(416,0xdb);
    Feed(t,BcspTransport::FrameBytes(2,maximum,true,0,0,true));
    assert(t.Received()==1 && t.Peek()->payload==maximum);t.Pop();Drain(t);
    // Decoded 417-byte packet is syntactically framed but exceeds guest policy.
    Bytes oversized{0xc0,0x81,0x12,0x1a,0x52};
    oversized.insert(oversized.end(),417,0x55);oversized.push_back(0xc0);
    Feed(t,oversized);assert(!t.Received() && t.Expected()==1);
    Feed(t,Command(1));assert(t.Received()==1);
    // Unsent queued packets cannot be acknowledged by an eager/malformed peer.
    const Bytes event{0x0e,4,1,1,0x10,0};assert(t.Queue(5,event.data(),event.size()));
    Feed(t,BcspTransport::FrameBytes(0,{},false,0,1));assert(t.Outstanding()==1);
}
static void Backpressure() {
    BcspTransport t(timing);Ready(t);
    for(unsigned i=0;i<8;++i){Feed(t,Command(i));Drain(t);}
    assert(t.Received()==8 && t.Expected()==0);
    Feed(t,Command(0));assert(t.Received()==8 && t.Expected()==0);
    t.Pop();Feed(t,Command(0));assert(t.Received()==8 && t.Expected()==1);
    const Bytes event{0x0e,4,1,1,0x10,0};
    assert(t.Queue(5,event.data(),event.size()));
    t.Tick(10);assert(t.Read(10,false)==0x100);
    // Closed RTS preserves the exact wire suffix, without repeated enqueueing.
    const auto start=t.Read(10);assert(start==0xc0);
    for(unsigned i=0;i<100;++i)assert(t.Read(20,false)==0x100);
    const auto suffix=Drain(t,20);assert(!suffix.empty() && suffix.back()==0xc0);
}
static void ResetAndTimeout() {
    BcspTransport t(timing);Ready(t);Feed(t,{0xc0,0xdb});
    t.Stop();assert(t.Read(0)==0x100);Ready(t);Feed(t,Command());assert(t.Received()==1);
    Feed(t,Link(BcspTransport::sync));assert(t.State()==BcspTransport::Link::sync);
    assert(!t.Received() && !t.Outstanding());
    BcspTransport retry(timing);Ready(retry);
    const uint8_t event[]{0x0e,4,1,1,0x10,0};assert(retry.Queue(5,event,sizeof(event)));
    Drain(retry,0);
    for(unsigned i=1;i<=11;++i)Drain(retry,200*i);
    assert(retry.Failed());
    BcspTransport syncTimeout(timing);syncTimeout.Start(0);Drain(syncTimeout);
    for(unsigned i=1;i<=11;++i)Drain(syncTimeout,100*i);
    assert(syncTimeout.Failed());
}
struct FakeBackend {
    static constexpr size_t capacity=1024*1024,maxFrame=65540;
    bool connected=true;size_t occupied{};
    std::deque<uint8_t> input;std::vector<Bytes> sent;
    bool Connected() const{return connected;}
    size_t Queued() const{return occupied;}
    size_t Pending() const{return input.size();}
    void Poll(bool=true){}
    bool Write(const uint8_t* p,size_t n) {
        if(!connected || n>capacity-occupied){Close();return false;}
        sent.emplace_back(p,p+n);return true;
    }
    uint16_t Read(){if(input.empty())return 0x100;auto c=input.front();input.pop_front();return c;}
    void Close(){connected=false;input.clear();}
    void Receive(const Bytes& p){input.insert(input.end(),p.begin(),p.end());}
};
static void Bridge() {
    BcspTransport t(timing);Ready(t);FakeBackend b;csr::BcspH4Bridge<FakeBackend> bridge(t,b);
    Feed(t,Command());bridge.Pump(0);
    assert(b.sent.size()==1 && b.sent[0]==Bytes({1,1,0x10,0}));
    Feed(t,Command());bridge.Pump(0);assert(b.sent.size()==1);
    b.Receive({4,0x0e,4,1,1,0x10,0});bridge.Pump(0);
    Drain(t);assert(t.Outstanding()==1);
    Feed(t,BcspTransport::FrameBytes(0,{},false,0,1));
    const Bytes control{0x01,0x00,0x02,0x00,0xdb,0xc0};
    Feed(t,BcspTransport::FrameBytes(2,control,true,1,1));bridge.Pump(10);
    assert(bridge.UnsupportedControl() && bridge.Controls().size()==1);
    assert(bridge.Controls()[0].request==control && bridge.Controls()[0].at==10);
    assert(b.sent.size()==1 && !t.Outstanding()); // no fabricated CSR reply
    Drain(t);
    // Blocked backend keeps a guest HCI packet intact until it can be written.
    b.occupied=FakeBackend::capacity;
    Feed(t,Command(2,1));bridge.Pump(0);assert(t.Received()==1 && b.connected);
    b.occupied=0;bridge.Pump(0);assert(!t.Received() && b.sent.size()==2);
    b.Receive({2,1,0x20,2,0,0xaa,0xbb});bridge.Pump(0);Drain(t);assert(t.Outstanding()==1);
    bridge.Reset();assert(!b.connected);
}
static void OversizedHost() {
    BcspTransport t(timing);Ready(t);FakeBackend b;csr::BcspH4Bridge<FakeBackend> bridge(t,b);
    Bytes acl{2,1,0,0x9d,1};acl.resize(5+413,0xaa);b.Receive(acl);bridge.Pump(0);
    assert(!bridge.Error().empty() && !b.connected && !t.Outstanding());
}
static void StateAndRetiredWire() {
    BcspTransport t(timing);Ready(t);
    Bytes p(416,0xc0);assert(t.Queue(6,p.data(),p.size(),true));Drain(t,0);
    assert(t.Read(200)==0xc0);
    for(unsigned i=0;i<31;++i)assert(t.Read(200)!=0x100);
    Feed(t,{0xc0,0xdb},200);
    auto partial=t.Save(200);uint64_t clock=0;BcspTransport copy(timing);
    assert(copy.Load(partial,clock) && clock==200 && copy.Save(clock)==partial);
    for(size_t i=0;i<partial.size();++i){
        auto bad=Bytes(partial.begin(),partial.begin()+i);
        assert(!copy.Load(bad,clock) && copy.Save(200)==partial);
    }
    auto bad=partial;bad[13]=2;assert(!copy.Load(bad,clock));
    Feed(t,{0xc0},200);Feed(copy,{0xc0},200);
    Feed(t,BcspTransport::FrameBytes(0,{},false,0,1),200);
    Feed(copy,BcspTransport::FrameBytes(0,{},false,0,1),200);
    assert(!t.Outstanding() && t.HasWire());
    const auto retired=t.Save(200);assert(copy.Load(retired,clock));
    assert(Drain(t,200)==Drain(copy,200));
    const Bytes event{0x0e,4,1,1,0x10,0};
    for(unsigned n=0;n<18;++n){assert(t.Queue(5,event.data(),event.size()));Drain(t,200);
        Feed(t,BcspTransport::FrameBytes(0,{},false,0,(n+2)&7),200);assert(!t.Outstanding());}
    std::cout<<"PASS bounded candidate state, every truncation, retired retransmit suffix and sequence reuse\n";
}
int main() {
    StateAndRetiredWire();Vectors();Parser();Reliability();LengthBoundaries();Backpressure();ResetAndTimeout();Bridge();OversizedHost();
    std::cout<<"PASS BCSP draft transport and H4 bridge\n";
}
