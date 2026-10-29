// Firmware-free production STUART and live local endpoint ownership/lifecycle.
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "CPU.h"
#include "HostInfrared.h"
#include "ReadOnlyGuest.h"
#include "mem.h"
#include "pxa_UART.h"
#include "pxa_PWM.h"
#include "soc_PXA.h"
struct Machine {
    std::vector<uint8_t> nor,nand;
    SocPXA soc;
    Machine(DeviceType5 type):nor((type==deviceTypeTX?16:8)*1024*1024),
      nand(type==deviceTypeTX?132*1024*1024:34603008,0xff),
      soc(type,32*1024*1024,nor.data(),nor.size(),nand.data(),nand.size(),-1,type==deviceTypeTX?2:0) {
        uint32_t loop=0xeafffffe;
        std::memcpy(static_cast<uint8_t*>(soc.GetMemoryData().data)+0x3000,&loop,4);
        cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);
        cpuSetReg(soc.GetCpu(),15,0xa0003000);
        // Distinct channel state catches a load that aliases every PWM to PWM0.
        const uint32_t bases[]{PXA_PWM0_BASE,PXA_PWM1_BASE,PXA_PWM2_BASE,PXA_PWM3_BASE};
        for(unsigned channel=0;channel<(type==deviceTypeTX?4u:2u);++channel)
            for(unsigned reg=0;reg<3;++reg){
                uint32_t value=(channel+1)*17+reg;
                assert(memAccess(cpuGetMem(soc.GetCpu()),bases[channel]+4*reg,4,true,&value));
            }
    }
    void write(unsigned reg,uint32_t value){assert(memAccess(cpuGetMem(soc.GetCpu()),PXA_STUART_BASE+4*reg,4,true,&value));}
    uint32_t read(unsigned reg){uint32_t value=0;assert(memAccess(cpuGetMem(soc.GetCpu()),PXA_STUART_BASE+4*reg,4,false,&value));return value;}
    void run(unsigned ms=1){assert(soc.Run(uint64_t(ms)*100000,100000000)>=uint64_t(ms)*100000);}
    void power(bool enabled){
        uint32_t mask=1u<<((soc.GetDeviceType()==deviceTypeTX?40:48)-32);
        uint32_t dir=0;assert(memAccess(cpuGetMem(soc.GetCpu()),0x40e00010,4,false,&dir));dir|=mask;
        assert(memAccess(cpuGetMem(soc.GetCpu()),0x40e00010,4,true,&dir));
        assert(memAccess(cpuGetMem(soc.GetCpu()),enabled?0x40e00028:0x40e0001c,4,true,&mask));
    }
    void configure(unsigned ir,unsigned divisor=96,unsigned lcr=3){
        power(true);
        write(3,0x80|lcr);write(0,divisor&255);write(1,divisor>>8);write(3,lcr);
        write(4,8);write(1,0x41);write(2,7);write(8,ir);
    }
    std::vector<uint8_t> save(){assert(soc.Save());auto b=soc.GetSavestate();return {static_cast<uint8_t*>(b.data),static_cast<uint8_t*>(b.data)+b.size};}
    uint8_t receive(){for(unsigned i=0;i<100;++i){run();auto status=read(5);assert(!(status&2));if(status&1)return read(0);}assert(false);return 0;}
    void sleep(){
        uint32_t value=1u<<(soc.GetDeviceType()==deviceTypeTX?0:2);
        assert(memAccess(cpuGetMem(soc.GetCpu()),0x40f0000c,4,true,&value));
        assert(memAccess(cpuGetMem(soc.GetCpu()),0x40f00014,4,true,&value));
        value=0xee070e10;assert(memAccess(cpuGetMem(soc.GetCpu()),0xa0001000,4,true,&value));
        cpuInvalidateCodeCache(soc.GetCpu());cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);
        cpuSetReg(soc.GetCpu(),15,0xa0001000);cpuSetReg(soc.GetCpu(),0,3);cpuCyclePure(soc.GetCpu());
        assert(soc.PowerSleeping());
    }
};
struct FakePeer {
    std::deque<uint8_t> rx;
    std::vector<uint8_t> tx;
    unsigned reads{},polls{},replaced{};
    IrHostHooks hooks(){return {this,[](void* p)->uint_fast16_t {auto& s=*static_cast<FakePeer*>(p);++s.reads;if(s.rx.empty())return UART_CHAR_NONE;auto c=s.rx.front();s.rx.pop_front();return c;},
      [](uint_fast16_t c,void* p){static_cast<FakePeer*>(p)->tx.push_back(c);},
      [](void* p){auto& s=*static_cast<FakePeer*>(p);++s.polls;return !s.rx.empty();},
      [](void* p){auto& s=*static_cast<FakePeer*>(p);++s.replaced;s.rx.clear();}};}
};
static void models(DeviceType5 type){
    Machine m(type);FakePeer peer;auto hooks=peer.hooks();
    m.soc.AttachKeyboard(true);assert(!m.soc.SetIrHostHooks(&hooks));
    m.soc.AttachKeyboard(false);assert(m.soc.SetIrHostHooks(&hooks));
    m.soc.AttachKeyboard(true);assert(!m.soc.KeyboardAttached());
    peer.rx={0xc0};m.run(5);assert(!peer.reads && !peer.tx.size());
    m.configure(0);m.write(0,0xa5);m.run(3);assert(peer.tx.empty() && !peer.reads);
    m.configure(1,96,7); // IR TX only, 9600/8N2.
    m.write(0,0xc1);m.run();assert(peer.tx.empty());m.run();assert(peer.tx==std::vector<uint8_t>{0xc1});
    assert(!peer.reads); // TX does not opt into IR reception.
    m.power(false);assert(!m.soc.IrPowerEnabled());m.write(0,0x80);m.run(3);
    assert(peer.tx==std::vector<uint8_t>{0xc1} && !peer.reads);
    m.configure(2);assert(m.receive()==0xc0);
    const auto serial=pxaUartIrState(m.soc.GetIrUart());
    assert(serial.baud==9600 && serial.lcr==3 && serial.receive && !serial.transmit);
    m.configure(3,8); // Negotiated SIR115200, exact bytes including escapes.
    peer.rx={0x7d,0xe0,0xff};
    for(auto value:{0x7d,0xe0,0xff})assert(m.receive()==value);
    auto saved=m.save();auto reads=peer.reads,polls=peer.polls;const auto sent=peer.tx.size();
    {
        ReadOnlyGuest scope(m.soc);m.write(0,0x55);m.run(10);
        assert(peer.reads==reads && peer.polls==polls && peer.tx.size()==sent);
    }
    // Notification is deferred until real guest execution, preserving exact state.
    assert(m.save()==saved && !peer.replaced);m.run(1);
    assert(!m.soc.Load(saved.size()-4,saved.data()) && !peer.replaced);
    peer.rx={0x31};pxaUartKick(m.soc.GetIrUart());assert(m.receive()==0x31);
    assert(m.soc.Load(saved.size(),saved.data()) && peer.replaced==1);
    m.soc.SetIrHostHooks(nullptr);assert(!m.soc.IrHostAttached());
    m.soc.AttachKeyboard(true);assert(m.soc.KeyboardAttached());
    m.configure(2);assert(m.soc.KeyboardKey(0x1a,true));
    for(auto c:{0xff,0xc0,0x1a,0xe5,0x7d,0xc1})assert(m.receive()==c);
    m.soc.AttachKeyboard(false);for(auto c:{0xff,0xc0,0x9a,0x65,0xfd,0xc1})assert(m.receive()==c);
    assert(m.soc.SetIrHostHooks(&hooks));auto replacements=peer.replaced;
    m.soc.Reset();assert(peer.replaced==replacements+1);
    m.sleep();assert(peer.replaced==replacements+2);
    auto frozenReads=peer.reads,frozenPolls=peer.polls;const auto frozenTx=peer.tx.size();m.run(20);
    assert(peer.reads==frozenReads && peer.polls==frozenPolls && peer.tx.size()==frozenTx);
    m.soc.SetIrHostHooks(nullptr);
    printf("PASS %s SIR UART direction, pacing, bytes, exclusive accessory, checkpoint and load ownership\n",type==deviceTypeTX?"TX":"E2");
}
static int connectTo(const std::string& endpoint){
    auto colon=endpoint.rfind(':');assert(colon!=std::string::npos);
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(std::stoi(endpoint.substr(colon+1)));a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));
    assert(!fcntl(fd,F_SETFL,O_NONBLOCK));return fd;
}
template<class F>static void waitFor(F f){for(unsigned i=0;i<2000;++i){if(f())return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(false);}
static void endpoints(){
    Machine a(deviceTypeTX),b(deviceTypeE2);HostInfrared x,y;std::string error;
    const HostSerial::Config tcp{HostSerial::Kind::tcp,0};
    assert(x.Start(&a.soc,tcp,error) && y.Start(&b.soc,tcp,error));
    auto ax=connectTo(x.Endpoint()),by=connectTo(y.Endpoint());
    waitFor([&]{return x.Describe().find(" connected")!=std::string::npos && y.Describe().find(" connected")!=std::string::npos;});
    a.configure(1);b.configure(0);
    uint8_t disabled=0x77;assert(send(by,&disabled,1,MSG_NOSIGNAL)==1);
    waitFor([&]{return y.GetCounters().received==1;});
    assert(!y.PendingReceive() && y.DroppedReceive()==1);
    b.configure(2);b.run(3);assert(!(b.read(5)&1));
    // No Service call between a live-byte arrival and rapid RX/power transitions.
    auto stale=[&](bool power){
        uint8_t byte=0x39;auto received=y.GetCounters().received;
        assert(send(by,&byte,1,MSG_NOSIGNAL)==1);
        waitFor([&]{return y.GetCounters().received==received+1;});assert(y.PendingReceive()==1);
        if(power){b.power(false);b.power(true);}else{b.write(8,0);b.write(8,2);}
        assert(!y.PendingReceive());b.run(3);assert(!(b.read(5)&1));
    };
    stale(false);stale(true);
    // Ordinary TCP reconnect has its own receive generation, even with no Service.
    uint8_t oldByte=0x19;auto oldCount=y.GetCounters().received;
    assert(send(by,&oldByte,1,MSG_NOSIGNAL)==1);
    waitFor([&]{return y.GetCounters().received==oldCount+1;});assert(y.PendingReceive()==1);
    close(by);waitFor([&]{return y.Describe().find(" connected")==std::string::npos;});
    by=connectTo(y.Endpoint());waitFor([&]{return y.Describe().find(" connected")!=std::string::npos;});
    assert(!y.PendingReceive());b.run(3);assert(!(b.read(5)&1));
    const std::vector<uint8_t> bytes{0xff,0xc0,0x7d,0xe0,0xc1,0x00,0x80};
    for(auto c:bytes){
        a.write(0,c);a.run(2);uint8_t v=0;
        // A complete frame must wait for the frontend service boundary too.
        assert(recv(ax,&v,1,0)<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
        waitFor([&]{a.run();x.Service();auto n=recv(ax,&v,1,0);assert(n==1 || (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)));return n==1;});assert(v==c);
        assert(send(by,&v,1,MSG_NOSIGNAL)==1);
        uint8_t delivered=0;
        waitFor([&]{y.Service();b.run();if(!(b.read(5)&1))return false;delivered=b.read(0);return true;});assert(delivered==c);
    }
    assert(!x.DroppedTransmit() && !y.DroppedTransmit());
    // An incomplete frame remains staged across temporary guest execution.
    // Rollback must neither publish its bytes nor discard the real pending byte.
    a.write(0,0x33);a.run(2);
    {ReadOnlyGuest scope(a.soc);a.run(20);x.Service();
     uint8_t c;assert(recv(ax,&c,1,0)<0 && (errno==EAGAIN || errno==EWOULDBLOCK));}
    uint8_t staged=0;
    waitFor([&]{a.run();x.Service();return recv(ax,&staged,1,0)==1;});assert(staged==0x33);
    a.run(10);x.Service();assert(recv(ax,&staged,1,0)<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
    // A new TCP peer cannot inherit a frame staged for the disconnected peer.
    a.write(0,0x44);a.run(2);close(ax);
    waitFor([&]{return x.Describe().find(" connected")==std::string::npos;});
    ax=connectTo(x.Endpoint());waitFor([&]{return x.Describe().find(" connected")!=std::string::npos;});
    a.run(10);x.Service();assert(recv(ax,&staged,1,0)<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
    a.write(0,0x55);a.run(2);
    waitFor([&]{a.run();x.Service();return recv(ax,&staged,1,0)==1;});assert(staged==0x55);
    auto saved=b.save();auto endpoint=y.Endpoint();
    uint8_t pending=0x66;auto received=y.GetCounters().received;
    assert(send(by,&pending,1,MSG_NOSIGNAL)==1);
    waitFor([&]{return y.GetCounters().received==received+1;});assert(y.PendingReceive()==1);
    {ReadOnlyGuest scope(b.soc);b.write(8,0);b.power(false);b.power(true);b.write(8,2);
     y.Service();b.run(10);assert(y.PendingReceive()==1);}
    y.Service();assert(b.receive()==pending);
    assert(y.Endpoint()==endpoint && y.Describe().find(" connected")!=std::string::npos);
    assert(!b.soc.Load(saved.size()-4,saved.data()));y.Service();assert(y.Endpoint()==endpoint);
    // Refusal after loading different UART/GPIO values must preserve live input,
    // not merely reject a truncated container before any component is touched.
    b.configure(0);b.power(false);auto bad=b.save();b.configure(2);
    uint32_t chunkCount;std::memcpy(&chunkCount,bad.data(),4);size_t chunkAt=4+8*chunkCount;bool corrupted=false;
    for(unsigned i=0;i<chunkCount;++i){
        uint32_t type,words;std::memcpy(&type,bad.data()+4+8*i,4);std::memcpy(&words,bad.data()+8+8*i,4);
        if(type==static_cast<uint32_t>(ChunkType::irKeyboard)){
            uint32_t invalid=IrKeyboard::capacity+1;assert(words>=3);std::memcpy(bad.data()+chunkAt+8,&invalid,4);corrupted=true;
        }
        chunkAt+=4*words;
    }
    assert(corrupted);
    auto lateCount=y.GetCounters().received;uint8_t lateByte=0x61;
    assert(send(by,&lateByte,1,MSG_NOSIGNAL)==1);
    waitFor([&]{return y.GetCounters().received==lateCount+1;});assert(y.PendingReceive()==1);
    assert(!b.soc.Load(bad.size(),bad.data()) && y.PendingReceive()==1);
    assert(y.Describe().find(" connected")!=std::string::npos);y.Service();assert(b.receive()==lateByte);
    received=y.GetCounters().received;assert(send(by,&pending,1,MSG_NOSIGNAL)==1);
    waitFor([&]{return y.GetCounters().received==received+1;});assert(y.PendingReceive()==1);
    assert(b.soc.Load(saved.size(),saved.data()));y.Service();
    b.run(3);assert(!y.PendingReceive() && !(b.read(5)&1));
    uint8_t value;waitFor([&]{auto n=recv(by,&value,1,0);return n==0 || (n<0 && errno==ECONNRESET);});close(by);
    by=connectTo(y.Endpoint());waitFor([&]{return y.Describe().find(" connected")!=std::string::npos;});
    // New peer bytes are accepted after the old peer was closed by replacement.
    value=0x42;assert(send(by,&value,1,MSG_NOSIGNAL)==1);
    uint8_t delivered=0;
    waitFor([&]{y.Service();b.run();if(!(b.read(5)&1))return false;delivered=b.read(0);return true;});assert(delivered==value);
    // A rolled-back temporary sleep retains the live peer and queued input.
    auto beforeSleep=y.GetCounters().received;uint8_t sleepByte=0x73;
    assert(send(by,&sleepByte,1,MSG_NOSIGNAL)==1);
    waitFor([&]{return y.GetCounters().received==beforeSleep+1;});
    {ReadOnlyGuest guard(b.soc);b.sleep();assert(y.Describe().find(" connected")!=std::string::npos);}
    assert(!b.soc.PowerSleeping() && y.PendingReceive()==1);y.Service();assert(b.receive()==sleepByte);
    // Final checkpoint release must close a committed sleeping peer immediately,
    // before Service or an intervening wake can hide the sleep transition.
    a.configure(3);auto beforeCommit=x.GetCounters().received;
    assert(send(ax,&sleepByte,1,MSG_NOSIGNAL)==1);
    waitFor([&]{return x.GetCounters().received==beforeCommit+1;});assert(x.PendingReceive()==1);
    {ReadOnlyGuest guard(a.soc);a.sleep();assert(!x.Endpoint().empty());guard.Commit();assert(x.Endpoint().empty());}
    assert(x.Endpoint().empty() && !x.PendingReceive());
    waitFor([&]{uint8_t c;auto n=recv(ax,&c,1,0);return n==0 || (n<0 && errno==ECONNRESET);});
    y.Stop();close(by);b.soc.AttachKeyboard(true);auto keyboard=b.save();b.soc.AttachKeyboard(false);
    assert(y.Start(&b.soc,tcp,error));by=connectTo(y.Endpoint());
    waitFor([&]{return y.Describe().find(" connected")!=std::string::npos;});
    assert(b.soc.Load(keyboard.size(),keyboard.data()));y.Service();
    assert(b.soc.KeyboardAttached() && !b.soc.IrHostAttached() && y.Describe().find("off")==0);
    x.Stop();y.Stop();close(ax);close(by);
    assert(!a.soc.IrHostAttached() && !b.soc.IrHostAttached());
    puts("PASS live loopback SIR byte relay TX->E2, reconnect and checkpoint/refused-load peer retention");
}
int main(){models(deviceTypeTX);models(deviceTypeE2);endpoints();}
