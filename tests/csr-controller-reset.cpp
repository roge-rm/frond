#include "CsrControllerReset.h"
#include <cassert>
#include <cstdio>
#include <deque>
#include <vector>
using Bytes=std::vector<uint8_t>;
struct Backend {
 static constexpr size_t capacity=1024*1024,maxFrame=65540;
 bool connected=true;size_t occupied{};std::deque<uint8_t> input;std::vector<Bytes> sent;
 bool Connected()const{return connected;}size_t Queued()const{return occupied;}size_t Pending()const{return input.size();}
 void Poll(bool=true){}bool Write(const uint8_t*p,size_t n){assert(n<=capacity-occupied);sent.emplace_back(p,p+n);return connected;}
 uint16_t Read(){if(input.empty())return 0x100;auto c=input.front();input.pop_front();return c;}
 void Close(){connected=false;input.clear();}void Receive(const Bytes&p){input.insert(input.end(),p.begin(),p.end());}
};
int main(){
 const Bytes reset{4,14,4,1,3,0x0c,0};
 Backend b;csr::ControllerReset<Backend> r;r.Begin(0,{},1);b.occupied=Backend::capacity;
 b.Receive({2,1,0x20,3,0,1,2,3});r.Pump(b,0);assert(r.Pending()&&b.sent.empty()&&b.Pending()==0);
 b.occupied=0;r.Pump(b,0);assert(b.sent==std::vector<Bytes>({{1,3,0x0c,0}}));
 r.Begin(100,{},1);r.Pump(b,100);assert(b.sent.size()==1);
 b.Receive({4,5,4,0,1,0,0x13});b.Receive(reset);r.Pump(b,200);assert(!r.Pending()&&r.Error().empty()&&b.connected&&b.Pending()==0);
 Backend old;csr::ControllerReset<Backend> two;two.Begin(0,{0x0c03},0);two.Pump(old,0);assert(old.sent.empty());old.Receive(reset);two.Pump(old,0);assert(two.Pending()&&old.sent.empty());two.Pump(old,1);assert(old.sent.size()==1);old.Receive(reset);two.Pump(old,2);assert(!two.Pending()&&two.Error().empty());
 Backend failure;csr::ControllerReset<Backend> f;f.Begin(0,{},1);failure.Receive({4,14,4,1,3,0x0c,0x0c});f.Pump(failure,0);assert(!failure.connected&&!f.Error().empty());
 Backend eof;csr::ControllerReset<Backend> e;e.Begin(0,{},1);eof.connected=false;e.Pump(eof,0);assert(!e.Error().empty());
 Backend timeout;csr::ControllerReset<Backend> t;t.Begin(0,{},1);t.Pump(timeout,0);t.Begin(2000000000ull,{},1);t.Pump(timeout,3000000000ull);assert(!timeout.connected&&!t.Error().empty());
 Backend bounded;csr::ControllerReset<Backend> q;q.Begin(0,{},1);for(unsigned i=0;i<33;++i)bounded.Receive({4,0xff,0});q.Pump(bounded,0);assert(bounded.Pending()==3&&q.Pending());
 Backend credit;csr::ControllerReset<Backend> c;c.Begin(0,{0x1001},0);c.Pump(credit,0);assert(credit.sent.empty());credit.Receive({4,15,4,0,0,1,16});c.Pump(credit,0);c.Pump(credit,0);assert(credit.sent.empty());credit.Receive({4,14,3,1,0,0});c.Pump(credit,0);assert(credit.sent.empty());c.Pump(credit,0);assert(credit.sent.size()==1);credit.Receive(reset);c.Pump(credit,1);assert(!c.Pending()&&c.Error().empty());
 Backend denied;csr::ControllerReset<Backend> d;d.Begin(0,{0x0c03},0);denied.Receive({4,14,4,1,3,0x0c,0x0c});d.Pump(denied,0);assert(denied.sent.empty()&&!denied.connected&&!d.Error().empty());
 puts("PASS genuine reset admission/backpressure, one generation, old-reset completion count, ACL/event discard, failure/EOF/3s bound/no repeated-reset escape and32frame pump bound");
}
