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
using csr::BcspTransport;
using Bytes=std::vector<uint8_t>;
struct Server{
 int listener=-1,peer=-1;std::string directory,path;
 Server(){char name[]="/tmp/frond-csr-board-owned-XXXXXX";auto* p=mkdtemp(name);assert(p);directory=p;path=directory+"/hci";
  listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(listener>=0);sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,path.c_str());assert(!bind(listener,(sockaddr*)&a,sizeof(a)));assert(!listen(listener,4));fprintf(stderr,"owned-test-socket=%s\n",path.c_str());}
 void Accept(){if(peer>=0)close(peer);peer=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);assert(peer>=0);}
 void Send(const Bytes& p){assert(send(peer,p.data(),p.size(),MSG_NOSIGNAL)==ssize_t(p.size()));}
 void NoNew(){assert(accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC)<0&&errno==EAGAIN);}
 // A new controller connection starts with the machine's address (Write_BD_ADDR), the same each time.
 Bytes address;
 void ReceiveReset(){
  uint8_t b[10];assert(recv(peer,b,4,MSG_DONTWAIT|MSG_PEEK)==4);
  if(!memcmp(b,"\1\1\374\6",4)){
   assert(recv(peer,b,10,MSG_DONTWAIT)==10);if(address.empty())address.assign(b+4,b+10);
   assert(Bytes(b+4,b+10)==address);Send({4,14,4,1,1,0xfc,0});
  }
  assert(recv(peer,b,4,MSG_DONTWAIT)==4);assert(!memcmp(b,"\1\3\14\0",4));
 }
 bool Eof(){char c;return recv(peer,&c,1,MSG_DONTWAIT|MSG_PEEK)==0;}
 ~Server(){if(peer>=0)close(peer);close(listener);unlink(path.c_str());rmdir(directory.c_str());}
};
struct Machine{
 Bytes rom=Bytes(4096);SocPXA soc{deviceTypeTG50,16u<<20,rom.data(),uint32_t(rom.size()),nullptr,0,-1,0};
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
static void connect(Machine& m,Server& server,BcspTransport& peer){
 std::string error;assert(m.soc.SetBluetooth(server.path,error));server.Accept();m.Mode();m.Power(true);server.ReceiveReset();server.Send({4,14,4,1,3,0x0c,0});peer.Start(m.soc.GetTime());
 for(unsigned i=0;i<3000 && peer.State()!=BcspTransport::Link::active;++i)m.Step(peer);
 assert(peer.State()==BcspTransport::Link::active);server.NoNew();
}
static void deferred(bool initial){
 Machine m;Server server;BcspTransport peer({250000000,250000000,12});
 if(initial)connect(m,server,peer);else m.Mode();
 auto baseline=m.Save();
 {SoC::InputCheckpoint outer(m.soc);
  m.Power(!initial);auto intermediate=m.Save();
  {SoC::InputCheckpoint inner(m.soc);assert(inner.Restore(intermediate.size(),intermediate.data()));assert(m.Save()==intermediate);}
  assert(outer.Restore(baseline.size(),baseline.data()));
 }
 assert(m.Save()==baseline);if(initial)assert(!server.Eof());
}
static void active(){
 Machine m;Server server;BcspTransport peer({250000000,250000000,12});connect(m,server,peer);
 server.Send({4,14,11,1,5,16,0,192,0,72,1,0,1,0});
 for(unsigned i=0;i<200;++i)m.Step(peer);
 assert(peer.Peek()&&peer.Peek()->channel==5);peer.Pop();
 Bytes acl{1,0x20,192,0};for(unsigned i=0;i<192;++i)acl.push_back(uint8_t(i*37));
 assert(peer.Queue(6,acl.data(),acl.size()));Bytes got;
 for(unsigned i=0;i<3000&&got.size()<197;++i){m.Step(peer);uint8_t bytes[256];auto n=recv(server.peer,bytes,sizeof(bytes),MSG_DONTWAIT);if(n>0)got.insert(got.end(),bytes,bytes+n);}
 Bytes expected{2};expected.insert(expected.end(),acl.begin(),acl.end());assert(got==expected);
 // A partial provider frame and a real outstanding credit stay owned at rollback.
 server.Send({2,1,0x20,0x88,0x13,0x41});for(unsigned i=0;i<20;++i)m.Step(peer);auto saved=m.Save();
 {SoC::InputCheckpoint cp(m.soc);server.Send({0x42,0x43});for(unsigned i=0;i<20;++i)m.Step(peer);assert(cp.Restore(saved.size(),saved.data()));}
 assert(m.Save()==saved&&!server.Eof());
 Bytes suffix(4997);for(unsigned i=0;i<suffix.size();++i)suffix[i]=uint8_t(i*19);server.Send(suffix);
 Bytes received;unsigned fragments=0;
 for(unsigned i=0;i<10000&&received.size()<5000;++i){m.Step(peer);if(auto* p=peer.Peek()){assert(p->channel==6&&p->payload.size()<=416);assert((p->payload[1]&0x30)==(fragments?0x10:0x20));received.insert(received.end(),p->payload.begin()+4,p->payload.end());++fragments;peer.Pop();}}
 Bytes full{0x41,0x42,0x43};full.insert(full.end(),suffix.begin(),suffix.end());assert(received==full&&fragments==13);
 server.Send({4,19,5,1,1,0,1,0});for(unsigned i=0;i<200;++i)m.Step(peer);assert(peer.Peek()&&peer.Peek()->channel==5);peer.Pop();
 assert(m.soc.Load(saved.size(),saved.data())&&server.Eof());
 puts("PASS realSoC ACL192 genuine credit, partial5000 provider frame frozen through checkpoint/exact rollback then13fragments, accepted fence");
}
static void committed(bool initial){
 Machine m;Server server;BcspTransport peer({250000000,250000000,12});
 if(initial)connect(m,server,peer);else{m.Mode();std::string error;assert(m.soc.SetBluetooth(server.path,error));server.Accept();}
 {SoC::InputCheckpoint cp(m.soc);m.Power(!initial);auto pending=m.Save();assert(!server.Eof());m.Run(1000);assert(!server.Eof());}
 assert(!server.Eof());server.ReceiveReset();server.Send({4,14,4,1,3,0x0c,0});server.NoNew();
 if(!initial)assert(m.soc.BluetoothState().find("restart required")!=std::string::npos);
 else assert(m.soc.BluetoothState().find("radio=off")!=std::string::npos);
}
static void externalDeferred(bool rollback){
 Machine m;Server server;BcspTransport peer({250000000,250000000,12});connect(m,server,peer);auto baseline=m.Save();
 {SoC::InputCheckpoint outer(m.soc);m.soc.Reset();auto pending=m.Save();assert(!server.Eof());
  {SoC::InputCheckpoint inner(m.soc);assert(inner.Restore(pending.size(),pending.data()));}
  if(rollback)assert(outer.Restore(baseline.size(),baseline.data()));
  assert(!server.Eof());
 }
 assert(server.Eof()!=rollback);if(rollback)assert(m.Save()==baseline);server.NoNew();
}
static void gated(){
 Machine m;Server server;BcspTransport peer({250000000,250000000,12});connect(m,server,peer);
 const uint8_t request[]{1,16,0};assert(peer.Queue(5,request,sizeof(request)));Bytes got;
 for(unsigned i=0;i<3000&&got.size()<4;++i){m.Step(peer);uint8_t bytes[4];auto n=recv(server.peer,bytes,4,MSG_DONTWAIT);if(n>0)got.insert(got.end(),bytes,bytes+n);}assert(got==Bytes({1,1,16,0}));
 m.reg(1,0);m.Power(false);uint8_t byte;assert(recv(server.peer,&byte,1,MSG_DONTWAIT)<0&&errno==EAGAIN);
 server.Send({4,14,12,1,1,16,0,5,0,0,5,0xf1,5,0,0});server.Send({2,1,0x20,1,0,0x71});
 m.Run(80000);server.ReceiveReset();server.Send({4,14,4,1,3,0x0c,0});m.Run(40000);assert(!server.Eof()&&m.soc.BluetoothState().find("reset pending")==std::string::npos);server.NoNew();
 m.Power(true);server.ReceiveReset();server.Send({4,14,4,1,3,0x0c,0});m.reg(1,0x41);peer.Start(m.soc.GetTime());for(unsigned i=0;i<3000&&peer.State()!=BcspTransport::Link::active;++i)m.Step(peer);assert(peer.State()==BcspTransport::Link::active);for(unsigned i=0;i<100;++i)m.Step(peer);assert(!peer.Peek());server.NoNew();
 puts("PASS realSoC UUE-gated A6 waits old genuine CC/credit then sends/finishes Reset while off, keeps socket, discards stale ACL and re-establishes BCSP without replay");
}

int main(){gated();externalDeferred(false);externalDeferred(true);puts("PASS nested externalReset interimSave/Load commit closes controller; exact rollback retains original controller");committed(false);committed(true);puts("PASS committed deferred raw A6 both directions preserve controller through release with genuine reset");deferred(false);deferred(true);puts("PASS raw A6 off/on and on/off intermediate ownSave/Load nested checkpoint exact rollback");active();}
