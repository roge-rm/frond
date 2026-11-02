#include "BcspH4Bridge.h"
#include "BluetoothBackend.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
using csr::BcspTransport;
struct Server {
    int listener=-1,peer=-1;std::string directory,path;
    Server(){char name[]="/tmp/frond-csr-owned-XXXXXX";auto* p=mkdtemp(name);assert(p);directory=p;path=directory+"/hci";
        listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(listener>=0);
        sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,path.c_str());assert(!bind(listener,(sockaddr*)&a,sizeof(a)));assert(!listen(listener,1));}
    void Accept(){peer=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);assert(peer>=0);}
    ~Server(){if(peer>=0)close(peer);close(listener);unlink(path.c_str());rmdir(directory.c_str());}
    void Send(BluetoothBackend& b,const std::vector<uint8_t>& bytes){
        size_t at=0;for(unsigned tries=0;at<bytes.size() && tries<100000;++tries){
            auto n=send(peer,bytes.data()+at,std::min(size_t(4096),bytes.size()-at),MSG_DONTWAIT|MSG_NOSIGNAL);
            if(n>0)at+=n;
            b.Poll();assert(b.Connected());
        }assert(at==bytes.size());for(unsigned i=0;i<20;++i)b.Poll();
    }
};
static void feed(BcspTransport& t,const std::array<uint8_t,4>& p){
    for(auto c:BcspTransport::FrameBytes(1,{p.begin(),p.end()}))t.Write(c,0);
}
static void drain(BcspTransport& t){for(unsigned i=0;i<10000;++i)if(t.Read(0)==0x100)return;assert(false);}
int main(){
    Server server;BluetoothBackend backend;std::string error;assert(backend.Open(server.path,error));server.Accept();
    BcspTransport transport({100,200,12});transport.Start(0);drain(transport);
    feed(transport,BcspTransport::syncResponse);drain(transport);feed(transport,BcspTransport::confResponse);
    const uint8_t event[]={14,4,1,1,16,0};
    for(unsigned i=0;i<6;++i)assert(transport.Queue(5,event,sizeof(event)));
    csr::BcspH4Bridge<BluetoothBackend> bridge(transport,backend);
    server.Send(backend,{4,14,4,1,1,16,0});bridge.Pump(0);assert(!backend.Pending());
    std::vector<uint8_t> large(65540);large[0]=2;large[1]=1;large[2]=0x20;large[3]=large[4]=255;
    for(size_t i=5;i<large.size();++i)large[i]=uint8_t(i*71+19);
    for(unsigned i=0;i<15;++i)server.Send(backend,large);
    assert(backend.Pending()==15*large.size());
    server.Send(backend,{large.begin(),large.end()-1});assert(backend.Pending()==15*large.size());
    assert(send(server.peer,large.data()+large.size()-1,1,MSG_NOSIGNAL)==1);
    const uint8_t command[]={1,3,12,0};assert(backend.Write(command,sizeof(command)));
    bridge.Pump(1);assert(backend.Connected() && backend.Pending()==15*large.size() && !backend.Queued());
    uint8_t sent[4];assert(recv(server.peer,sent,sizeof(sent),MSG_DONTWAIT)==4 && !memcmp(sent,command,4));
    for(auto c:large)assert(backend.Read()==c);
    bridge.Pump(2);assert(backend.Connected() && backend.Pending()==15*large.size());
    for(unsigned i=0;i<15;++i)for(auto c:large)assert(backend.Read()==c);
    assert(!backend.Pending() && bridge.Error().empty());
    puts("PASS real H4 partial65539-to65540 backpressure boundary, write flush while RX paused, exact983100 bytes");
}
