// Controller contract through public UART callbacks and a real local H4 peer.
// No firmware, adapter privileges or physical Bluetooth timing is required.
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "uartdev_bcm2035.h"
#include "pxa_GPIO.h"
#include "savestate/savestateAll.h"
extern "C" void uarmAbort(){std::abort();}
struct PxaGpio {bool powered=false,reset=true;};
PxaGpioState pxaGpioGetState(PxaGpio*g,uint_fast8_t pin){return ((pin==17||pin==32)?g->powered:g->reset)?PxaGpioStateHigh:PxaGpioStateLow;}
static PxaUartReadF rx;static PxaUartWriteF tx;static PxaUartClientIsActive active;static PxaUartClientTick tick;static void* context;
void pxaUartSetFuncs(PxaUart*,PxaUartReadF r,PxaUartWriteF w,PxaUartClientIsActive a,PxaUartClientTick t,void*c){rx=r;tx=w;active=a;tick=t;context=c;}
void pxaUartKick(PxaUart*){}
static bool receiveReady=true;
uint8_t pxaUartGetModemOutputs(const PxaUart*){return receiveReady?PXA_UART_RTS:0;}
bool pxaUartHwReceiveReady(const PxaUart*){return receiveReady;}
struct Server {
 int listener=-1,peer=-1;std::string dir,path;
 Server(){char name[]="/tmp/frond-controller-XXXXXX";auto*p=mkdtemp(name);assert(p);dir=p;path=dir+"/hci";listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,path.c_str());assert(!bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a)));assert(!listen(listener,4));}
 void Accept(){if(peer>=0)close(peer);peer=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);assert(peer>=0);}
 ~Server(){if(peer>=0)close(peer);close(listener);unlink(path.c_str());rmdir(dir.c_str());}
 void Send(const std::vector<uint8_t>&v){assert(send(peer,v.data(),v.size(),MSG_NOSIGNAL)==ssize_t(v.size()));}
};
struct Model {
 Bcm2035* b;uint64_t now=0;PxaGpio gpio;
 explicit Model(bool txBoard){b=bcm2035Init({[](void*p){return static_cast<Model*>(p)->now;},this},&gpio,txBoard);bcm2035RegisterWithUart(b,reinterpret_cast<PxaUart*>(this));}
 ~Model(){bcm2035Stop(b);assert(!rx&&!tx&&!context);}
 template<class S>void Save(S&s){bcm2035Save(b,s);}
 void Load(SavestateLoader<ChunkType>&l){bcm2035Load(b,l);}
 void Run(){now+=1000000;tick(context);}
 void Power(bool on){gpio.powered=on;bcm2035PowerChanged(b);}
 void Write(const std::vector<uint8_t>&v){for(auto c:v)tx(c,context);}
 std::vector<uint8_t> Read(){std::vector<uint8_t>v;for(unsigned i=0;i<1024;++i){const auto c=rx(context);if(c==UART_CHAR_NONE)return v;v.push_back(c);}assert(false);return v;}
};
int main(){
 for(bool txBoard:{false,true}){
    Server s;Model m(txBoard);std::string error;
    assert(bcm2035BackendState(m.b)=="off");
    assert(bcm2035SetBackend(m.b,s.path,error));s.Accept();uint8_t byte;assert(recv(s.peer,&byte,1,0)==0); // Validation connection is deliberately closed.
    m.Power(true);m.Write({1,3,12,0});s.Accept();m.Run();uint8_t bytes[32];assert(recv(s.peer,bytes,sizeof(bytes),0)==4 && !memcmp(bytes,"\1\3\14\0",4));
    assert(bcm2035BackendState(m.b)=="connected");
    assert(!bcm2035SetBackend(m.b,s.path+"-missing",error));assert(bcm2035BackendState(m.b)=="connected");
    const std::vector<uint8_t> reply{4,14,4,1,3,12,0};s.Send(reply);m.Run();assert(m.Read()==reply);
    // TX checks its programmed factory address against Read_BD_ADDR. Forward
    // the known Broadcom command to a capable controller; never fake success.
    const std::vector<uint8_t> address{1,1,0xfc,6,0x2a,0,0,0,0,2};
    m.Write(address);m.Run();
    if(txBoard){
        assert(recv(s.peer,bytes,sizeof(bytes),0)==ssize_t(address.size()));
        assert(!memcmp(bytes,address.data(),address.size()));
        const std::vector<uint8_t> actual{4,14,4,1,1,0xfc,0};s.Send(actual);m.Run();assert(m.Read()==actual);
    }else{
        assert(recv(s.peer,bytes,sizeof(bytes),MSG_DONTWAIT)<0);
        assert((m.Read()==std::vector<uint8_t>{4,14,4,1,1,0xfc,1}));
    }
    if(txBoard){
        // Original TX mode-2 initialization needs an 11-byte activation and
        // explicit UART sleep/wake messaging, independently of the HCI radio.
        m.Write({1,0x27,0xfc,10,2,4,4,1,1,0,1,1,0,0});
        assert((m.Read()==std::vector<uint8_t>{4,14,4,1,0x27,0xfc,0,4,0xef,1,4}));
        m.Write({1,0x29,0xfc,1,3});assert((m.Read()==std::vector<uint8_t>{4,0xef,1,6}));
        s.Send(reply);m.Run();assert((m.Read()==std::vector<uint8_t>{4,0xef,1,3}));
        for(unsigned i=0;i<20;++i){m.Run();assert(m.Read().empty());} // Same bytes held until wake acknowledgment.
        Savestate<ChunkType> wakeSave;assert(wakeSave.Save(m));
        std::vector<uint8_t> wake(static_cast<uint8_t*>(wakeSave.GetBuffer()),static_cast<uint8_t*>(wakeSave.GetBuffer())+wakeSave.GetSize());
        auto invalid=wake;reinterpret_cast<uint32_t*>(invalid.data())[invalid.size()/4-4]=0xffffffff;
        SavestateLoader<ChunkType> migrate;
        bcm2035BeginCheckpoint(m.b);m.Write({1,0x29,0xfc,1,2});assert(m.Read().empty());
        assert(!migrate.Load(invalid.data(),invalid.size(),m));assert(migrate.Load(wake.data(),wake.size(),m));
        bcm2035EndCheckpoint(m.b);m.Run();assert(m.Read().empty());
        m.Write({1,0x29,0xfc,1,2});assert(m.Read()==reply);
        assert(recv(s.peer,bytes,sizeof(bytes),MSG_DONTWAIT)<0);
        // Present v0/v1 chunks remain readable; they have no UART messaging.
        for(unsigned version:{0u,1u}){
            auto old=wake;
            if(version==1){old.resize(old.size()-16);reinterpret_cast<uint32_t*>(old.data())[2]-=4;}
            else {old.resize(44);reinterpret_cast<uint32_t*>(old.data())[2]=8;}
            reinterpret_cast<uint32_t*>(old.data())[3]=version;
            assert(migrate.Load(old.data(),old.size(),m));
            m.Write({1,0x29,0xfc,1,1});assert((m.Read()==std::vector<uint8_t>{4,14,4,1,0x29,0xfc,0x0c}));
            assert(migrate.Load(wake.data(),wake.size(),m));m.Write({1,0x29,0xfc,1,2});assert(m.Read().empty());
        }
    }
    // Local vendor completions cannot preempt a partly clocked controller
    // event/ACL/SCO packet; doing so would corrupt the guest's H4 framing.
    for(const std::vector<uint8_t> frame:{std::vector<uint8_t>{4,14,10,1,9,16,0,14,232,12,224,7,0},
            std::vector<uint8_t>{2,1,0x20,3,0,0xaa,0xbb,0xcc},std::vector<uint8_t>{3,1,0,3,0xaa,0xbb,0xcc}}){
        s.Send(frame);m.Run();assert(rx(context)==frame[0]);m.Write({1,0x27,0xfc,0});m.Run();
        auto tail=std::vector<uint8_t>(frame.begin()+1,frame.end());
        const std::vector<uint8_t> unknown{4,14,4,1,0x27,0xfc,uint8_t(txBoard?0x12:1)};tail.insert(tail.end(),unknown.begin(),unknown.end());
        assert(m.Read()==tail);
    }
    receiveReady=false;s.Send(reply);m.Run();assert(m.Read().empty());
    receiveReady=true;m.Run();assert(m.Read()==reply); // Controller flow control never consumes held bytes.
    Savestate<ChunkType> save;assert(save.Save(m));std::vector<uint8_t> snapshot(static_cast<uint8_t*>(save.GetBuffer()),static_cast<uint8_t*>(save.GetBuffer())+save.GetSize());
    auto restoreRadio=bcm2035CaptureInput(m.b);
    bcm2035BeginCheckpoint(m.b);bcm2035BeginCheckpoint(m.b);
    m.Write({1,3,12,0});m.Power(false);bcm2035Suspend(m.b);s.Send(reply);m.Run();assert(m.Read().empty());
    assert(bcm2035BackendState(m.b)=="connected");
    assert(recv(s.peer,bytes,sizeof(bytes),MSG_DONTWAIT)<0); // No speculative HCI escaped.
    m.Power(true);restoreRadio();SavestateLoader<ChunkType> loader;assert(loader.Load(snapshot.data(),snapshot.size(),m));
    bcm2035EndCheckpoint(m.b);m.Run();assert(m.Read().empty());
    bcm2035EndCheckpoint(m.b);m.Run();assert(m.Read()==reply);assert(recv(s.peer,bytes,sizeof(bytes),MSG_DONTWAIT)<0);
    // A refused load followed by the owning SoC's rollback keeps the same live
    // peer and pending host event. Neither load may reconnect or send commands.
    bcm2035BeginCheckpoint(m.b);s.Send(reply);
    assert(!loader.Load(snapshot.data(),snapshot.size()-4,m));assert(loader.Load(snapshot.data(),snapshot.size(),m));
    bcm2035EndCheckpoint(m.b);m.Run();assert(m.Read()==reply);assert(bcm2035BackendState(m.b)=="connected");
    // Committed sleep invalidates the external peer only when the last fence ends.
    bcm2035BeginCheckpoint(m.b);bcm2035Suspend(m.b);assert(bcm2035BackendState(m.b)=="connected");
    bcm2035EndCheckpoint(m.b,true);assert(recv(s.peer,bytes,sizeof(bytes),0)==0);assert(m.Read().empty());
    m.Write({1,3,12,0});assert(bcm2035BackendState(m.b)!="connected"); // No guessed live-link continuation.
    m.Power(false);m.Power(true);m.Write({1,3,12,0});s.Accept();m.Run();assert(recv(s.peer,bytes,sizeof(bytes),0)==4);
    s.Send(reply);m.Run();assert(m.Read()==reply);
    // A committed off/on pulse invalidates the old peer even if final GPIO
    // levels match. Inner rollback must retain the outer committed history.
    bcm2035BeginCheckpoint(m.b);m.Power(false);m.Power(true);
    auto restoreInner=bcm2035CaptureInput(m.b);bcm2035BeginCheckpoint(m.b);
    m.Power(false);m.Power(true);restoreInner();bcm2035EndCheckpoint(m.b);
    s.Send(reply);bcm2035EndCheckpoint(m.b);const auto closed=recv(s.peer,bytes,sizeof(bytes),0);assert(closed==0 || (closed<0 && errno==ECONNRESET));assert(m.Read().empty());
    m.Write({1,3,12,0});s.Accept();m.Run();assert(recv(s.peer,bytes,sizeof(bytes),0)==4);
    s.Send(reply);m.Run();assert(m.Read()==reply);
    bcm2035Suspend(m.b);assert(recv(s.peer,bytes,sizeof(bytes),0)==0); // Accepted replacement/reset shares this invalidation boundary.
    assert(bcm2035SetBackend(m.b,"",error));assert(bcm2035BackendState(m.b)=="off");
    printf("PASS %s controller: exact HCI, atomic endpoint failure, nested rollback/refused load, GPIO fencing, sleep close and real power-cycle reconnect\n",txBoard?"TX":"E2");
 }
}
