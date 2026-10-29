// Real board GPIO/MMIO/transaction integration against an unnamed local H4 peer.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "CPU.h"
#include "ReadOnlyGuest.h"
#include "mem.h"
#include "pxa_UART.h"
#include "soc_PXA.h"
struct Server {
 int listener=-1,peer=-1;std::string dir,path;
 Server(){char name[]="/tmp/frond-bt-soc-XXXXXX";auto*p=mkdtemp(name);assert(p);dir=p;path=dir+"/hci";listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,path.c_str());assert(!bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a)));assert(!listen(listener,4));}
 void Accept(){if(peer>=0)close(peer);peer=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);assert(peer>=0);}
 ~Server(){if(peer>=0)close(peer);close(listener);unlink(path.c_str());rmdir(dir.c_str());}
 void Send(const std::vector<uint8_t>&v){assert(send(peer,v.data(),v.size(),MSG_NOSIGNAL)==ssize_t(v.size()));}
};
static uint32_t read(SocPXA& s,uint32_t a){uint32_t v=0;assert(memAccess(cpuGetMem(s.GetCpu()),a,4,false,&v));return v;}
static void write(SocPXA&s,uint32_t a,uint32_t v){assert(memAccess(cpuGetMem(s.GetCpu()),a,4,true,&v));}
static void run(SocPXA&s,unsigned ms=5){assert(s.Run(uint64_t(ms)*100000,100000000)>=uint64_t(ms)*100000);}
static void pin(SocPXA&s,unsigned p,bool value){
 const unsigned bank=p/32,bit=p%32;const uint32_t mask=1u<<bit,af=0x40e00054+4*(p/16);
 write(s,af,read(s,af)&~(3u<<(2*(p%16))));
 write(s,0x40e0000c+4*bank,read(s,0x40e0000c+4*bank)|mask);
 write(s,(value?0x40e00018:0x40e00024)+4*bank,mask);
}
static std::vector<uint8_t> snapshot(SocPXA&s){assert(s.Save());auto v=s.GetSavestate();return {static_cast<uint8_t*>(v.data),static_cast<uint8_t*>(v.data)+v.size};}
static void alterLateFailure(std::vector<uint8_t>& bytes,unsigned power){
 auto*w=reinterpret_cast<uint32_t*>(bytes.data());const auto count=w[0];size_t pos=1+2*count;bool gpio=false,late=false;
 for(unsigned i=0;i<count;++i){const auto type=w[1+2*i],size=w[2+2*i];
    if(type==uint32_t(ChunkType::pxaGpio)){assert(size>13);w[pos+1+power/32]&=~(1u<<(power%32));w[pos+9+power/32]&=~(1u<<(power%32));gpio=true;}
    if(type==uint32_t(ChunkType::bcm2035)){w[pos]=0xffffffff;late=true;}
    pos+=size;
 }
 assert(gpio&&late&&pos*4==bytes.size());
}
int main(){
 for(auto device:{deviceTypeTX,deviceTypeE2}){
    Server server;const bool tx=device==deviceTypeTX;const unsigned power=tx?17:32,reset=tx?83:80;const uint32_t uart=tx?PXA_BTUART_BASE:PXA_HWUART_BASE;
    std::vector<uint8_t> nor((tx?16:8)*1024*1024),nand(tx?132*1024*1024:34603008,0xff);
    SocPXA soc(device,32*1024*1024,nor.data(),nor.size(),nand.data(),nand.size(),-1,tx?2:0);
    write(soc,0xa0003000,0xeafffffe);cpuInvalidateCodeCache(soc.GetCpu());cpuSetReg(soc.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);cpuSetReg(soc.GetCpu(),15,0xa0003000);
    pin(soc,power,false);pin(soc,reset,true);std::string error;
    assert(soc.SetBluetooth(server.path,error));server.Accept();uint8_t buf[64];assert(recv(server.peer,buf,sizeof(buf),0)==0);
    pin(soc,power,true);
    write(soc,uart+12,0x83);write(soc,uart,8);write(soc,uart+4,0);write(soc,uart+12,3);write(soc,uart+16,0xb);write(soc,uart+8,7);write(soc,uart+4,0x41);
    for(uint8_t c:{1,3,12,0})write(soc,uart,c);run(soc);server.Accept();run(soc);
    assert(recv(server.peer,buf,sizeof(buf),0)==4 && !memcmp(buf,"\1\3\14\0",4));assert(soc.BluetoothState()=="connected");
    const std::vector<uint8_t> reply{4,14,4,1,3,12,0};
    auto consume=[&](){std::vector<uint8_t> v;while(read(soc,uart+20)&1)v.push_back(read(soc,uart));return v;};
    server.Send(reply);run(soc);assert(consume()==reply);
    {
        ReadOnlyGuest checkpoint(soc);pin(soc,power,false);pin(soc,power,true);for(uint8_t c:{1,3,12,0})write(soc,uart,c);run(soc);
        assert(soc.BluetoothState()=="connected");assert(recv(server.peer,buf,sizeof(buf),MSG_DONTWAIT)<0);
    }
    assert(soc.BluetoothState()=="connected");
    // Real board RTS and automatic FIFO backpressure, including HWUART on E2.
    write(soc,uart+16,9);server.Send(reply);run(soc,20);assert(!(read(soc,uart+20)&3));
    write(soc,uart+16,0xb);run(soc);assert(consume()==reply);
    write(soc,uart+8,0xc7);write(soc,uart+16,0x2b);
    std::vector<uint8_t> longEvent(258);longEvent[0]=4;longEvent[1]=0xff;longEvent[2]=255;
    for(unsigned i=3;i<longEvent.size();++i)longEvent[i]=uint8_t(i*73);
    server.Send(longEvent);run(soc,40);assert(!(read(soc,uart+20)&2));
    for(uint8_t c:{1,0x27,0xfc,0})write(soc,uart,c);run(soc);
    auto expected=longEvent;const std::vector<uint8_t> unknown{4,14,4,1,0x27,0xfc,uint8_t(tx?0x12:1)};expected.insert(expected.end(),unknown.begin(),unknown.end());
    std::vector<uint8_t> received;
    for(unsigned i=0;i<32 && received.size()<expected.size();++i){auto part=consume();received.insert(received.end(),part.begin(),part.end());run(soc);assert(!(read(soc,uart+20)&2));}
    assert(received==expected);write(soc,uart+8,7);write(soc,uart+16,0xb);
    auto good=snapshot(soc),bad=good;alterLateFailure(bad,power);
    server.Send(reply);
    assert(!soc.Load(bad.size(),bad.data())); // GPIO load succeeds first; controller version fails later.
    assert(soc.BluetoothState()=="connected");run(soc);assert(consume()==reply);
    assert(recv(server.peer,buf,sizeof(buf),MSG_DONTWAIT)<0); // Same FD, no speculative HCI.
    assert(soc.Load(good.size(),good.data()));assert(recv(server.peer,buf,sizeof(buf),0)==0);
    assert(soc.BluetoothState()!="connected");
    pin(soc,power,false);pin(soc,power,true);for(uint8_t c:{1,3,12,0})write(soc,uart,c);run(soc);server.Accept();run(soc);
    assert(recv(server.peer,buf,sizeof(buf),0)==4);server.Send(reply);run(soc);assert(consume()==reply);
    {ReadOnlyGuest temporary(soc);pin(soc,power,false);pin(soc,power,true);temporary.Commit();}
    assert(recv(server.peer,buf,sizeof(buf),0)==0); // Same final levels, new controller epoch.
    for(uint8_t c:{1,3,12,0})write(soc,uart,c);run(soc);server.Accept();run(soc);
    assert(recv(server.peer,buf,sizeof(buf),0)==4);server.Send(reply);run(soc);assert(consume()==reply);
    const uint32_t af=0x40e00054+4*(power/16),original=read(soc,af),alternate=original|(1u<<(2*(power%16)));
    server.Send(reply);
    {ReadOnlyGuest temporary(soc);write(soc,af,alternate);write(soc,af,original);}
    assert(soc.BluetoothState()=="connected");run(soc);assert(consume()==reply);
    {ReadOnlyGuest temporary(soc);write(soc,af,alternate);write(soc,af,original);temporary.Commit();}
    assert(recv(server.peer,buf,sizeof(buf),0)==0);
    for(uint8_t c:{1,3,12,0})write(soc,uart,c);run(soc);server.Accept();run(soc);assert(recv(server.peer,buf,sizeof(buf),0)==4);
    write(soc,af,alternate);assert(recv(server.peer,buf,sizeof(buf),0)==0); // No UART tick needed.
    write(soc,af,original);for(uint8_t c:{1,3,12,0})write(soc,uart,c);run(soc);server.Accept();run(soc);assert(recv(server.peer,buf,sizeof(buf),0)==4);
    soc.Reset();assert(recv(server.peer,buf,sizeof(buf),0)==0);
    printf("PASS %s board: real GPIO/UART, checkpoint power fence, late-invalid load rollback retains peer/exact pending bytes, accepted load/reset close and power-cycle recovery\n",tx?"TX":"E2");
 }
}
