#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include "CPU.h"
#include "soc_PXA.h"
#include "pxa_UART.h"
#include "HostSerial.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

static void io(SocPXA& s,uint32_t address,uint32_t value) {
    assert(cpuMemOpExternal(s.GetCpu(),&value,address,4,true));
}
static uint32_t get(SocPXA& s,unsigned reg) {
    uint32_t value=0;assert(cpuMemOpExternal(s.GetCpu(),&value,PXA_FFUART_BASE+4*reg,4,false));return value;
}
static void put(SocPXA& s,unsigned reg,uint32_t value){io(s,PXA_FFUART_BASE+4*reg,value);}
static void run(SocPXA& s,unsigned ms=5){s.Run(ms*100000,100000000);}
struct Peer {
    std::deque<uint8_t> rx;
    std::vector<uint8_t> tx;
    unsigned replaced{},mode{};
    SerialHostHooks Hooks(){return {this,[](void* p)->uint_fast16_t{
        auto& q=static_cast<Peer*>(p)->rx;if(q.empty())return UART_CHAR_NONE;
        auto v=q.front();q.pop_front();return v;
    },[](uint_fast16_t v,void* p){static_cast<Peer*>(p)->tx.push_back(v);},
    [](void* p){return !static_cast<Peer*>(p)->rx.empty();},
    [](void* p){++static_cast<Peer*>(p)->replaced;},
    [](void* p){++static_cast<Peer*>(p)->mode;}};}
};
static std::vector<uint8_t> save(SocPXA& s){assert(s.Save());auto b=s.GetSavestate();return {(uint8_t*)b.data,(uint8_t*)b.data+b.size};}
struct TestSoc : SocPXA {using SocPXA::SocPXA;using SocPXA::BeginSerialCheckpoint;using SocPXA::EndSerialCheckpoint;};
int main(){
    std::vector<uint8_t> rom(4096);
    TestSoc s(deviceTypeTG50,16*1024*1024,rom.data(),rom.size(),nullptr,0,-1,0);
    const uint32_t loop=0xeafffffe;std::memcpy((uint8_t*)s.GetMemoryData().data+0x3000,&loop,4);
    cpuSetReg(s.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);cpuSetReg(s.GetCpu(),15,0xa0003000);
    Peer serial,ir;auto raw=serial.Hooks(),infrared=ir.Hooks();
    assert(s.SetIrHostHooks(&infrared));assert(!s.SetSerialHostHooks(&raw));assert(s.SetIrHostHooks(nullptr));
    assert(s.SetSerialHostHooks(&raw));assert(!s.SetIrHostHooks(&infrared));
    s.AttachKeyboard(true);assert(!s.KeyboardAttached());
    put(s,3,0x83);put(s,0,96);put(s,1,0);put(s,3,3);put(s,4,3);put(s,1,0x41);put(s,2,7);
    io(s,0x40e0000c,1u<<27);io(s,0x40e00018,1u<<27);
    put(s,8,0);assert(s.SerialEnabled());
    serial.rx={0x00,0x80,0xff};pxaUartKick(s.GetSerialUart());run(s);
    for(auto b:{0,128,255}){assert(get(s,5)&1);assert(get(s,0)==unsigned(b));}
    put(s,0,0x5a);run(s);assert(serial.tx==std::vector<uint8_t>{0x5a});
    io(s,0x40e00024,1u<<27);assert(!s.SerialEnabled());
    put(s,0,0xc1);run(s);assert(serial.tx.size()==1);
    io(s,0x40e00018,1u<<27);put(s,8,3);assert(!s.SerialEnabled());
    serial.rx={0x33};pxaUartKick(s.GetSerialUart());run(s);assert(!(get(s,5)&1));
    put(s,8,0);pxaUartKick(s.GetSerialUart());run(s);assert(get(s,0)==0x33);
    auto state=save(s);auto bad=state;auto* words=(uint32_t*)bad.data();size_t offset=1+2*words[0];bool found=false;
    for(unsigned i=0;i<words[0];++i){if(words[1+2*i]==uint32_t(ChunkType::irKeyboard)){words[offset+2]=IrKeyboard::capacity;found=true;break;}offset+=words[2+2*i];}
    assert(found);const auto replaced=serial.replaced;assert(!s.Load(bad.size(),bad.data()));assert(serial.replaced==replaced);
    s.BeginSerialCheckpoint();assert(!s.SerialEnabled());s.EndSerialCheckpoint();assert(s.SerialEnabled());
    assert(s.Load(state.size(),state.data()));assert(serial.replaced==replaced+1);
    assert(s.SetSerialHostHooks(nullptr));assert(s.SetIrHostHooks(&infrared));
    put(s,8,1);io(s,0x08000020,1u<<10);io(s,0x08000040,1u<<10);io(s,0x40e00024,1u<<27);
    put(s,0,0x7e);run(s);assert(ir.tx==std::vector<uint8_t>{0x7e});
    assert(s.SetIrHostHooks(nullptr));s.AttachKeyboard(true);assert(s.KeyboardAttached());
    auto keyboardState=save(s);
    put(s,8,3);assert(s.KeyboardKey(0x1a,true));s.AttachKeyboard(false);
    auto pendingKeyboardState=save(s);
    assert(s.Load(keyboardState.size(),keyboardState.data()));
    s.AttachKeyboard(false);assert(!s.KeyboardAttached());
    HostSerial host;std::string error;const auto route=s.GetSerialRoute();
    assert(host.Start(s.GetSerialUart(),{HostSerial::Kind::tcp,0},error,route));
    put(s,8,0);io(s,0x40e00018,1u<<27);host.Service();
    const auto port=std::stoi(host.Endpoint().substr(host.Endpoint().find(':')+1));
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);sockaddr_in address{};
    address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    assert(connect(fd,(sockaddr*)&address,sizeof(address))==0);
    for(unsigned n=0;n<500 && !host.Connected();++n){host.Service();usleep(1000);}assert(host.Connected());
    const uint8_t bytes[]={0,0x80,0xff};assert(send(fd,bytes,3,0)==3);
    for(unsigned n=0;n<500 && host.PendingReceive()!=3;++n){host.Service();usleep(1000);}assert(host.PendingReceive()==3);
    host.Service();run(s);assert(get(s,5)&1);
    auto pendingState=save(s);
    assert(s.Load(pendingState.size(),pendingState.data()));
    assert(!host.Connected() && !(get(s,6)&0xf0));host.Service();
    for(auto b:bytes){assert(get(s,5)&1);assert(get(s,0)==b);}
    TestSoc fresh(deviceTypeTG50,16*1024*1024,rom.data(),rom.size(),nullptr,0,-1,0);
    assert(fresh.Load(pendingState.size(),pendingState.data()));
    for(auto b:bytes){assert(get(fresh,5)&1);assert(get(fresh,0)==b);}
    close(fd);fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    assert(connect(fd,(sockaddr*)&address,sizeof(address))==0);
    for(unsigned n=0;n<500 && !host.Connected();++n){host.Service();usleep(1000);}assert(host.Connected());
    // Bytes sampled with the wired mux disabled cannot enter a later wired window.
    io(s,0x40e00024,1u<<27);host.Service();assert(send(fd,bytes,3,0)==3);
    for(unsigned n=0;n<100;++n){host.Service();usleep(1000);}assert(!host.PendingReceive());
    auto disabledState=save(s);
    assert(!s.Load(bad.size(),bad.data()));assert(save(s)==disabledState);
    io(s,0x40e00018,1u<<27);host.Service();run(s);assert(!(get(s,5)&1));
    io(s,0xa0003000,0xee070e10);cpuInvalidateCodeCache(s.GetCpu());
    cpuSetReg(s.GetCpu(),15,0xa0003000);cpuSetReg(s.GetCpu(),0,3);cpuCyclePure(s.GetCpu());
    assert(s.PowerSleeping());run(s);host.Service();
    assert(!host.Connected() && host.Endpoint().empty() && host.Describe().find("suspended")!=std::string::npos);
    assert(!(get(s,6)&0xf0));host.Service();assert(host.Endpoint().empty());
    close(fd);
    assert(s.Load(keyboardState.size(),keyboardState.data()));host.Service();
    assert(s.KeyboardAttached() && host.GetConfig().kind==HostSerial::Kind::off);
    assert(host.Describe().find("Restored accessory")!=std::string::npos);
    s.AttachKeyboard(false);host.Stop();
    assert(host.Start(s.GetSerialUart(),{HostSerial::Kind::tcp,0},error,route));
    assert(s.Load(pendingKeyboardState.size(),pendingKeyboardState.data()));host.Service();
    assert(!s.KeyboardAttached() && host.GetConfig().kind==HostSerial::Kind::off);
    assert(host.Describe().find("Restored accessory")!=std::string::npos);
    assert(s.Load(state.size(),state.data()));
    assert(s.SetIrHostHooks(&infrared));assert(s.SetIrHostHooks(nullptr));
    int busy=socket(AF_INET,SOCK_STREAM,0);assert(busy>=0);address.sin_port=0;
    assert(bind(busy,(sockaddr*)&address,sizeof(address))==0);assert(listen(busy,1)==0);
    socklen_t length=sizeof(address);assert(getsockname(busy,(sockaddr*)&address,&length)==0);
    assert(!host.Start(s.GetSerialUart(),{HostSerial::Kind::tcp,ntohs(address.sin_port)},error,route));
    assert(s.SetIrHostHooks(&infrared));assert(s.SetIrHostHooks(nullptr));close(busy);
    assert(host.Start(s.GetSerialUart(),{HostSerial::Kind::tcp,0},error,route));
    address.sin_port=htons(std::stoi(host.Endpoint().substr(host.Endpoint().find(':')+1)));
    assert(s.Load(state.size(),state.data()));
    busy=socket(AF_INET,SOCK_STREAM,0);assert(busy>=0);
    const int on=1;setsockopt(busy,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
    assert(bind(busy,(sockaddr*)&address,sizeof(address))==0);assert(listen(busy,1)==0);
    host.Service();assert(host.GetConfig().kind==HostSerial::Kind::off);
    assert(host.Describe().find("error:")!=std::string::npos);
    const auto failure=host.Describe();host.Service();assert(host.Describe()==failure);
    assert(s.SetIrHostHooks(&infrared));assert(s.SetIrHostHooks(nullptr));close(busy);
    puts("PASS: TG50 permanent FFUART owner, plain/IR GPIO mux, exact binary bytes, owner exclusion, malformed rollback and replacement fences");
}
