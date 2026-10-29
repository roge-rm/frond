#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
#include "BluetoothUartMessaging.h"
struct Store {
 std::vector<uint8_t> bytes;size_t at{};bool loading{};
 Store& Do32(uint32_t& n){if(loading){assert(at+4<=bytes.size());n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(bytes[at++])<<(8*i);}else for(unsigned i=0;i<4;++i)bytes.push_back(n>>(8*i));return *this;}
 Store& DoBuffer(void*p,size_t n){auto*b=static_cast<uint8_t*>(p);if(loading){assert(at+n<=bytes.size());memcpy(b,bytes.data()+at,n);at+=n;}else bytes.insert(bytes.end(),b,b+n);return *this;}
};
int main(){
 BluetoothUartMessaging m;using R=BluetoothUartMessaging::Result;std::vector<uint8_t> out;
 auto send=[&](const uint8_t*b,size_t n){out.insert(out.end(),b,b+n);return true;};
 auto cmd=[&](std::vector<uint8_t> b){out.clear();return m.Command(b.data(),b.size(),send);};
 const std::vector<uint8_t> configure{1,0x27,0xfc,10,2,4,4,1,1,0,1,1,0,0};
 assert(m.Valid()&&m.CanReadController());assert(cmd(configure)==R::handled);
 assert((out==std::vector<uint8_t>{4,14,4,1,0x27,0xfc,0,4,0xef,1,4}));
 assert(cmd({1,0x28,0xfc,0})==R::handled&&out.size()==17);
 assert(std::equal(configure.begin()+4,configure.end(),out.begin()+7));
 assert(cmd({1,0x29,0xfc,1,3})==R::handled&&!m.CanReadController());
 assert((out==std::vector<uint8_t>{4,0xef,1,6}));out.clear();
 for(unsigned i=0;i<1000;++i)assert(m.PendingControllerInput(true,send));
 assert((out==std::vector<uint8_t>{4,0xef,1,3})&&!m.CanReadController());
 Store saved;m.Transfer(saved);BluetoothUartMessaging restored;Store reader=saved;reader.loading=true;restored.Transfer(reader);assert(restored.Valid()&&!restored.CanReadController());
 assert(cmd({1,0x29,0xfc,1,2})==R::handled&&out.empty()&&m.CanReadController());
 assert(cmd({1,0x29,0xfc,1,3})==R::handled);assert(cmd({1,0x29,0xfc,1,1})==R::handled&&m.CanReadController());assert((out==std::vector<uint8_t>{4,0xef,1,4}));
 Store before;m.Transfer(before);
 for(auto invalid:{std::vector<uint8_t>{1,0x27,0xfc,0},std::vector<uint8_t>{1,0x29,0xfc,1,99}}){assert(cmd(invalid)==R::handled&&out.back()==0x12);Store after;m.Transfer(after);assert(before.bytes==after.bytes);}
 for(auto mutation:{std::pair<size_t,uint8_t>{0,4},{4,0},{7,2}}){Store bad=saved;bad.loading=true;bad.bytes[mutation.first]=mutation.second;BluetoothUartMessaging forged;forged.Transfer(bad);assert(!forged.Valid());}
 auto refuse=[](const uint8_t*,size_t){return false;};assert(m.Command(configure.data(),configure.size(),refuse)==R::failed);
 assert(cmd({1,3,12,0})==R::unhandled&&m.Valid()&&m.CanReadController());assert(cmd({1,0x29,0xfc,1,1})==R::handled&&out.back()==0x0c);
 assert(cmd({1,0x99,0xfc,0})==R::unhandled&&out.empty());
 puts("PASS original TX UART messaging: exact activation/readback, sleep/wake acknowledgments, held input, bounded wake, saved-state validation and refusal");
}
