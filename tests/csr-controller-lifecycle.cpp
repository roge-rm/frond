#include "CsrBcspRadio.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
static bool power;
static PxaUartClientTick tick;
static void* context;
void pxaUartSetPacing(PxaUart*,bool){}
void pxaUartSetFuncs(PxaUart*,PxaUartReadF,PxaUartWriteF,PxaUartClientIsActive,PxaUartClientTick t,void* p){tick=t;context=p;}
void pxaUartSetModeObserver(PxaUart*,void(*)(void*),void*){}
bool pxaUartSetModemInputs(PxaUart*,uint8_t){return true;}
PxaIrState pxaUartIrState(const PxaUart*){return {921600,0x1b,false,false,true};}
bool pxaUartReceiverEnabled(const PxaUart*){return true;}
bool pxaUartPlainEnabled(const PxaUart*){return true;}
void pxaUartKick(PxaUart*){}
uint8_t pxaUartGetModemOutputs(const PxaUart*){return 3;}
bool tg50ucGpioIsOutput(const TG50uc*,unsigned,unsigned){return true;}
bool tg50ucGpioState(const TG50uc*,unsigned,unsigned pin){return pin==6&&power;}
struct Server {
 int listener=-1,peer=-1;std::string directory,path;
 Server(){char name[]="/tmp/frond-csr-lifecycle-owned-XXXXXX";auto*p=mkdtemp(name);assert(p);directory=p;path=directory+"/hci";listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(listener>=0);sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,path.c_str());assert(!bind(listener,(sockaddr*)&a,sizeof(a)));assert(!listen(listener,4));}
 void Accept(){peer=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);assert(peer>=0);}
 void NoNew(){assert(accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC)<0&&errno==EAGAIN);}
 void Reply(unsigned status=0){const uint8_t event[]{4,14,4,1,3,0x0c,uint8_t(status)};assert(send(peer,event,sizeof(event),MSG_NOSIGNAL)==sizeof(event));}
 void ResetCommand(){uint8_t b[4];assert(recv(peer,b,sizeof(b),MSG_DONTWAIT)==sizeof(b));const uint8_t expected[]{1,3,0x0c,0};assert(!memcmp(b,expected,4));}
 ~Server(){if(peer>=0)close(peer);close(listener);unlink(path.c_str());rmdir(directory.c_str());}
};
static void failure(bool immediate){
 power=false;uint64_t now=0;CsrBcspRadio radio({[](void*p)->uint64_t{return *static_cast<uint64_t*>(p);},&now},nullptr,nullptr);Server s;std::string error;assert(radio.SetBackend(s.path,error));s.Accept();
 if(immediate){close(s.peer);s.peer=-1;}
 power=true;radio.PinsChanged();
 if(!immediate){s.ResetCommand();s.Reply(0x0c);now=1;tick(context);}
 assert(radio.State().find("error:")==0);
 for(unsigned i=0;i<4;++i){power=!power;radio.PinsChanged();now+=1000;tick(context);assert(radio.State().find("error:")==0);s.NoNew();}
 assert(radio.SetBackend("",error)&&radio.State()=="off");
}
static void success(){
 power=false;uint64_t now=0;CsrBcspRadio radio({[](void*p)->uint64_t{return *static_cast<uint64_t*>(p);},&now},nullptr,nullptr);Server s;std::string error;assert(radio.SetBackend(s.path,error));s.Accept();power=true;radio.PinsChanged();s.ResetCommand();
 power=false;radio.PinsChanged();power=true;radio.PinsChanged();s.NoNew();s.Reply();now=1;tick(context);assert(radio.State().find("error:")!=0&&radio.State().find("provider connected")!=std::string::npos);
 {auto cp=radio.CaptureInput();power=false;radio.PinsChanged();power=true;cp();}s.NoNew();
 radio.Suspend();s.ResetCommand();s.Reply();now=3000000001ull;tick(context);assert(radio.State().find("error:")!=0);s.NoNew();
 radio.Reset();char b;assert(recv(s.peer,&b,1,MSG_DONTWAIT)==0);
}
static void legacyExternal(unsigned version){
 power=false;uint64_t now=0;CsrBcspRadio radio({[](void*p)->uint64_t{return *static_cast<uint64_t*>(p);},&now},nullptr,nullptr);Server server;std::string error;assert(radio.SetBackend(server.path,error));server.Accept();power=true;radio.PinsChanged();server.ResetCommand();server.Reply();tick(context);auto saved=radio.Save();
 if(version==0)saved.clear();else if(version==1){saved[0]=1;saved.erase(saved.begin()+4,saved.begin()+7);}else{saved[0]=2;saved.erase(saved.begin()+6);}
 radio.BeginCheckpoint();radio.Reset();assert(radio.Load(saved));radio.EndCheckpoint(false);char byte;assert(recv(server.peer,&byte,1,MSG_DONTWAIT)==0);server.NoNew();
}
static void outgoingSuffix(){
 Server server;BluetoothBackend backend;std::string error;assert(backend.Open(server.path,error));server.Accept();
 std::vector<uint8_t> original;for(unsigned i=0;i<15;++i){std::vector<uint8_t> frame{2,1,0x20,0xff,0xff};for(unsigned j=0;j<65535;++j)frame.push_back(uint8_t(i*31+j*73));assert(backend.Write(frame.data(),frame.size()));original.insert(original.end(),frame.begin(),frame.end());}
 csr::ControllerReset<BluetoothBackend> reset;reset.Begin(0,{},1);reset.Pump(backend,0);assert(backend.Queued()>0&&backend.Queued()<original.size());original.insert(original.end(),{1,3,0x0c,0});
 std::vector<uint8_t> got;for(unsigned i=0;i<1000&&got.size()<original.size();++i){uint8_t bytes[16384];auto n=recv(server.peer,bytes,sizeof(bytes),MSG_DONTWAIT);if(n>0)got.insert(got.end(),bytes,bytes+n);else assert(errno==EAGAIN);reset.Pump(backend,i);assert(backend.Connected()&&reset.Pending());}
 assert(got==original&&!backend.Queued());server.Reply();reset.Pump(backend,1001);assert(!reset.Pending()&&reset.Error().empty());
 puts("PASS retained Reset under real short-write pressure preserves983100 original H4 bytes then exact Reset, no dropped/replayed suffix");
}
static void partialCapacity(){
 Server server;BluetoothBackend backend;std::string error;assert(backend.Open(server.path,error));server.Accept();
 std::vector<uint8_t> bytes;for(unsigned i=0;i<4000;++i){bytes.insert(bytes.end(),{4,0xff,255});bytes.insert(bytes.end(),255,0x71);}
 bytes.insert(bytes.end(),{2,1,0x20,0xff,0xff});bytes.insert(bytes.end(),65534,0x72);
 size_t at=0;while(at<bytes.size()){auto n=send(server.peer,bytes.data()+at,bytes.size()-at,MSG_DONTWAIT|MSG_NOSIGNAL);if(n>0)at+=size_t(n);else assert(errno==EAGAIN);backend.Poll();assert(backend.Connected());}
 for(unsigned i=0;i<100;++i)backend.Poll();
 assert(backend.Pending()==1032000);
 const uint8_t tail=0x73;assert(send(server.peer,&tail,1,MSG_NOSIGNAL)==1);
 csr::ControllerReset<BluetoothBackend> reset;reset.Begin(0,{},1);reset.Pump(backend,0);assert(backend.Connected()&&reset.Pending());server.ResetCommand();server.Reply();
 for(unsigned i=0;i<200&&reset.Pending();++i){reset.Pump(backend,i);assert(backend.Connected());}
 assert(!reset.Pending()&&reset.Error().empty()&&!backend.Pending());
 puts("PASS real backend1032000 complete bytes plus65539 hidden partial, last byte cannot overflow reset drain, bounded completion progress");
}
int main(){failure(true);failure(false);success();legacyExternal(0);legacyExternal(1);legacyExternal(2);outgoingSuffix();partialCapacity();puts("PASS real socket internalA6 retention/reset, immediateEOF+delayedreject latched across toggles, explicitOff recovery, checkpoint rollback, gated-resume completion before deadline and externalReset EOF");}
