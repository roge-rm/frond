#include "soc_PXA.h"
#include "CPU.h"
#include "pxa_UART.h"
#include "BcspTransport.h"
#include "mem.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <vector>
#include <thread>
#include <chrono>
using csr::BcspTransport;
using Bytes=std::vector<uint8_t>;
struct Server{
 int listener=-1,peer=-1;std::string directory,path;
 Server(){char name[]="/tmp/frond-csr-board-owned-XXXXXX";auto* p=mkdtemp(name);assert(p);directory=p;path=directory+"/hci";
  listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(listener>=0);sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,path.c_str());assert(!bind(listener,(sockaddr*)&a,sizeof(a)));assert(!listen(listener,4));fprintf(stderr,"owned-test-socket=%s\n",path.c_str());}
 void Accept(){if(peer>=0)close(peer);peer=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);assert(peer>=0);}
 void Send(const Bytes& p){assert(send(peer,p.data(),p.size(),MSG_NOSIGNAL)==ssize_t(p.size()));}
 void NoNew(){assert(accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC)<0&&errno==EAGAIN);}
 void ReceiveReset(){uint8_t b[4];assert(recv(peer,b,4,MSG_DONTWAIT)==4);assert(!memcmp(b,"\1\3\14\0",4));}
 bool Eof(){char c;return recv(peer,&c,1,MSG_DONTWAIT|MSG_PEEK)==0;}
 ~Server(){if(peer>=0)close(peer);close(listener);unlink(path.c_str());rmdir(directory.c_str());}
};
struct Machine{
 Bytes rom=[]{Bytes b(4096);const uint32_t loop=0xeafffffe;memcpy(b.data(),&loop,4);return b;}();SocPXA soc{deviceTypeTG50,16u<<20,rom.data(),uint32_t(rom.size()),nullptr,0,-1,0};
 Machine(){wr(0xa0003000,0xeafffffe);cpuInvalidateCodeCache(soc.GetCpu());cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);cpuSetReg(soc.GetCpu(),15,0xa0003000);}
 void wr(uint32_t a,uint32_t v){assert(memAccess(cpuGetMem(soc.GetCpu()),a,4,true,&v));}
 uint32_t rd(uint32_t a){uint32_t v=0;assert(memAccess(cpuGetMem(soc.GetCpu()),a,4,false,&v));return v;}
 void reg(unsigned r,unsigned v){wr(PXA_BTUART_BASE+4*r,v);}unsigned reg(unsigned r){return rd(PXA_BTUART_BASE+4*r);}
 void Mode(){reg(3,0x9b);reg(0,1);reg(1,0);reg(3,0x1b);reg(4,3);reg(2,7);reg(1,0x41);}
 void Power(bool on){wr(0x08000020,1u<<6);wr(on?0x08000040:0x08000060,1u<<6);}
 Bytes Save(){assert(soc.Save());auto b=soc.GetSavestate();return {(uint8_t*)b.data,(uint8_t*)b.data+b.size};}
 void Run(unsigned us=100){soc.Run(uint64_t(us)*100,100000000);}
 void Step(BcspTransport& peer){
  if(reg(5)&0x20){auto c=peer.Read(soc.GetTime());if(c!=0x100)reg(0,c);}
  Run();for(unsigned i=0;reg(5)&1;++i){assert(i<128);peer.Write(reg(0),soc.GetTime());}
 }
 void Raw(BcspTransport& peer,const Bytes& data){for(auto c:data){assert(reg(5)&0x20);reg(0,c);Run();while(reg(5)&1)peer.Write(reg(0),soc.GetTime());}}
};
static Bytes radio(const Bytes& state){
 const auto* w=(const uint32_t*)state.data();unsigned at=1+2*w[0];
 for(unsigned i=0;i<w[0];++i){if(w[1+2*i]==0x240)return {state.begin()+4*at,state.begin()+4*(at+w[2+2*i])};at+=w[2+2*i];}assert(false);return {};
}
static void connect(Machine& m,Server& server,BcspTransport& peer){
 std::string error;assert(m.soc.SetBluetooth(server.path,error));server.Accept();m.Mode();m.Power(true);server.ReceiveReset();server.Send({4,14,4,1,3,0x0c,0});peer.Start(m.soc.GetTime());
 for(unsigned i=0;i<3000 && peer.State()!=BcspTransport::Link::active;++i)m.Step(peer);
 assert(peer.State()==BcspTransport::Link::active);server.NoNew();
}
static Bytes withoutRadio(const Bytes& state){
 const auto* words=(const uint32_t*)state.data();unsigned at=1+2*words[0];std::vector<uint32_t> toc,bodies;
 for(unsigned i=0;i<words[0];++i){auto n=words[2+2*i];if(words[1+2*i]!=0x240){toc.push_back(words[1+2*i]);toc.push_back(n);bodies.insert(bodies.end(),words+at,words+at+n);}at+=n;}
 std::vector<uint32_t> result{unsigned(toc.size()/2)};result.insert(result.end(),toc.begin(),toc.end());result.insert(result.end(),bodies.begin(),bodies.end());
 return Bytes((uint8_t*)result.data(),(uint8_t*)result.data()+4*result.size());
}
static void sleep(Machine& m){m.wr(0xa0003000,0xee070e10);cpuInvalidateCodeCache(m.soc.GetCpu());cpuSetReg(m.soc.GetCpu(),15,0xa0003000);cpuSetReg(m.soc.GetCpu(),0,3);cpuCyclePure(m.soc.GetCpu());assert(m.soc.PowerSleeping());}
static void lifecycle(){
 {
  Machine m;Server server;BcspTransport peer({250000000,250000000,12});connect(m,server,peer);
  const uint8_t unknown[]={1,0,2,0,0xdb,0xc0};assert(peer.Queue(2,unknown,sizeof(unknown),true));
  for(unsigned i=0;i<100;++i)m.Step(peer);
  assert(m.soc.BluetoothState().find("Unsupported CSR channel 2")!=std::string::npos);
  char byte;assert(recv(server.peer,&byte,1,MSG_DONTWAIT)<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
  assert(!peer.Peek());
 }
 {
  Machine m;Server server;BcspTransport peer({250000000,250000000,12});connect(m,server,peer);auto saved=m.Save();
  {SoC::InputCheckpoint cp(m.soc);sleep(m);assert(!server.Eof());assert(cp.Restore(saved.size(),saved.data()));}
  assert(!m.soc.PowerSleeping() && m.Save()==saved && !server.Eof());
  m.wr(0x40f0000c,0x80000000);m.wr(0x40900008,0);m.wr(0x40900004,m.rd(0x40900000)+1);
  sleep(m);assert(!server.Eof());server.ReceiveReset();server.NoNew();server.Send({4,14,4,1,3,0x0c,0});m.Run(40000);assert(m.soc.PowerSleeping()&&m.soc.BluetoothState().find("reset pending")==std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));m.soc.Run(1100000,1000000);assert(!m.soc.PowerSleeping());m.wr(0x48000000,0x00001bcd);m.wr(0xa0003000,0xeafffffe);cpuInvalidateCodeCache(m.soc.GetCpu());cpuSetReg(m.soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);cpuSetReg(m.soc.GetCpu(),15,0xa0003000);m.Mode();m.Power(false);m.Power(true);server.ReceiveReset();
  server.Send({4,14,4,1,3,0x0c,0});peer.Start(m.soc.GetTime());for(unsigned i=0;i<3000&&peer.State()!=BcspTransport::Link::active;++i)m.Step(peer);assert(peer.State()==BcspTransport::Link::active);server.NoNew();
 }
 {
  Machine m;Server server;BcspTransport peer({250000000,250000000,12});connect(m,server,peer);auto saved=m.Save();
  {SoC::InputCheckpoint cp(m.soc);m.soc.Reset();assert(!server.Eof());assert(cp.Restore(saved.size(),saved.data()));}
  assert(m.Save()==saved && !server.Eof());
  m.soc.Reset();assert(server.Eof());
 }
 {
  Machine m;Server server;BcspTransport peer({250000000,250000000,12});connect(m,server,peer);auto legacy=withoutRadio(m.Save());
  assert(m.soc.Load(legacy.size(),legacy.data()) && server.Eof());
  Machine fresh;assert(fresh.soc.Load(legacy.size(),legacy.data()));
  assert(fresh.soc.BluetoothState()=="off" && radio(fresh.Save())[11]);
  Machine off;auto offLegacy=withoutRadio(off.Save());assert(off.soc.Load(offLegacy.size(),offLegacy.data()));
  assert(off.soc.BluetoothState()=="off" && !radio(off.Save())[9] && !radio(off.Save())[11]);
 }
 puts("PASS real true-sleep retains controller/reset quarantines guest; external reset closes peer, both deferred+restored checkpoints preserve exact state/peer; absent optional Bluetooth chunk awake/off and fresh quarantine");
}
int main(int argc,char**){
 if(argc>1){lifecycle();return 0;}
 Machine m,off;off.Mode();auto opposite=off.Save();
 auto* words=(uint32_t*)opposite.data();unsigned at=1+2*words[0];bool corrupt=false;
 for(unsigned i=0;i<words[0];++i){if(words[1+2*i]==0x1d0){words[at+37]=72;corrupt=true;}at+=words[2+2*i];}assert(corrupt);
 Server server;std::string error;assert(m.soc.SetBluetooth(server.path,error));server.Accept();
 m.Mode();m.Power(true);server.ReceiveReset();server.Send({4,14,4,1,3,0x0c,0});BcspTransport peer({250000000,250000000,12});peer.Start(0);
 for(unsigned i=0;i<3000 && peer.State()!=BcspTransport::Link::active;++i)m.Step(peer);
 assert(peer.State()==BcspTransport::Link::active);server.NoNew();
 assert(m.soc.BluetoothState().find("BCSP=3")!=std::string::npos);
 const uint8_t request[]={1,16,0};assert(peer.Queue(5,request,sizeof(request),true));
 Bytes got;for(unsigned i=0;i<3000 && got.size()<4;++i){m.Step(peer);uint8_t b[16];auto n=recv(server.peer,b,sizeof(b),MSG_DONTWAIT);if(n>0)got.insert(got.end(),b,b+n);}
 assert(got==Bytes({1,1,16,0}));
 server.Send({4,14});for(unsigned i=0;i<20;++i)m.Step(peer);
 m.Raw(peer,{0xc0,0xdb});auto saved=m.Save();auto radioBytes=radio(saved);uint32_t radioLength=0;assert(radioBytes.size()>=12);memcpy(&radioLength,radioBytes.data()+4,4);assert(radioLength>=4 && 8+radioLength<=radioBytes.size());
 BcspTransport verify({250000000,250000000,12});uint64_t verifiedClock=0;assert(verify.Load(Bytes(radioBytes.begin()+8+(radioBytes[8]==1?4:radioBytes[8]==2?6:7),radioBytes.begin()+8+radioLength),verifiedClock));
 auto memory=m.soc.GetMemoryData();Bytes ram((uint8_t*)memory.data,(uint8_t*)memory.data+memory.size);
 assert(!m.soc.Load(opposite.size(),opposite.data()));auto after=m.Save();
 if(after!=saved){auto* x=(uint32_t*)saved.data();auto* y=(uint32_t*)after.data();unsigned off=1+2*x[0];for(unsigned i=0;i<x[0];++i){for(unsigned k=0;k<x[2+2*i];++k)if(x[off+k]!=y[off+k]){fprintf(stderr,"rollback diff chunk=%x word=%u\n",x[1+2*i],k);break;}off+=x[2+2*i];}}
 assert(after==saved && !memcmp(memory.data,ram.data(),ram.size()));
 char c;assert(recv(server.peer,&c,1,MSG_DONTWAIT)<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
 {
  SoC::InputCheckpoint outer(m.soc);server.Send({4,1,1,16,0});m.Run(30000);
  assert(radio(m.Save())==radio(saved));
  {SoC::InputCheckpoint inner(m.soc);m.Run(30000);assert(inner.Restore(saved.size(),saved.data()));}
  assert(outer.Restore(saved.size(),saved.data()));
 }
 assert(m.Save()==saved);
 for(unsigned i=0;i<2000 && !peer.Peek();++i)m.Step(peer);
 assert(peer.Peek() && peer.Peek()->channel==5 && peer.Peek()->payload==Bytes({14,4,1,1,16,0}));peer.Pop();
 m.Raw(peer,BcspTransport::FrameBytes(1,{BcspTransport::sync.begin(),BcspTransport::sync.end()}));
 assert(!server.Eof() && m.soc.BluetoothState().find("peer reset")!=std::string::npos);server.ReceiveReset();server.Send({4,14,4,1,3,0x0c,0});m.Run(100);
 m.Power(false);m.Power(true);server.ReceiveReset();server.Send({4,14,4,1,3,0x0c,0});peer.Start(m.soc.GetTime());
 for(unsigned i=0;i<3000 && peer.State()!=BcspTransport::Link::active;++i)m.Step(peer);
 assert(peer.State()==BcspTransport::Link::active);server.NoNew();
 assert(m.soc.Load(saved.size(),saved.data()));assert(server.Eof());
 assert(m.soc.BluetoothState().find("restart required")!=std::string::npos || m.soc.BluetoothState().find("off and on")!=std::string::npos);
 Machine fresh;assert(fresh.soc.Load(saved.size(),saved.data()));assert(fresh.soc.BluetoothState()=="off" && radio(fresh.Save())[11]);
 puts("PASS real BTUART921600/8E1 + A6, H4 bytes, opposite-power refused load exact RAM/state/live peer, nested checkpoint, peer SYNC and accepted/fresh fence");
}
