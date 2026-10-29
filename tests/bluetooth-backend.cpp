#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "BluetoothBackend.h"
struct Server {
    int listener=-1,peer=-1;std::string directory,path;
    Server(){char name[]="/tmp/frond-bt-test-XXXXXX";auto*p=mkdtemp(name);assert(p);directory=p;path=directory+"/hci";listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(listener>=0);sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,path.c_str());assert(!bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a)));assert(!listen(listener,2));}
    void Accept(){if(peer>=0)close(peer);peer=accept4(listener,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);assert(peer>=0);}
    ~Server(){if(peer>=0)close(peer);close(listener);unlink(path.c_str());rmdir(directory.c_str());}
    void Send(const std::vector<uint8_t>&v){assert(send(peer,v.data(),v.size(),MSG_NOSIGNAL)==ssize_t(v.size()));}
};
static std::vector<uint8_t> drain(BluetoothBackend&b){std::vector<uint8_t>v;while(b.Pending())v.push_back(b.Read());assert(b.Read()==0x100);return v;}
int main(){
 Server s;BluetoothBackend b;std::string error;
 assert(!b.Open("relative",error) && !error.empty());assert(b.Open(s.path,error));s.Accept();
 assert(!b.Open(s.path+"-missing",error) && b.Connected()); // Failed replacement preserves live peer.
 const std::vector<uint8_t> complete{4,14,4,1,3,12,0};
 s.Send({4,14});b.Poll();assert(!b.Pending());s.Send({4,1,3,12,0});b.Poll();assert(drain(b)==complete);
 s.Send({4,1,1,0x7f,3,0x20,0,2,0xaa,0x55});b.Poll();assert((drain(b)==std::vector<uint8_t>{4,1,1,0x7f,3,0x20,0,2,0xaa,0x55}));
 // A partly clocked frame retains ownership when new frames compact the
 // receive buffer; the boundary is cleared exactly after its final byte.
 s.Send(complete);b.Poll();assert(!b.FrameInProgress());assert(b.Read()==4 && b.FrameInProgress());
 const std::vector<uint8_t> later{2,1,0x20,0,0,3,1,0,0};s.Send(later);b.Poll();
 auto tail=std::vector<uint8_t>(complete.begin()+1,complete.end());tail.insert(tail.end(),later.begin(),later.end());
 assert(drain(b)==tail && !b.FrameInProgress());
 // Fill the socket enough to require short writes, then drain both directions.
 std::vector<uint8_t> frame(32768),expected,actual;frame[0]=2;frame[1]=0x20;frame[3]=(frame.size()-5)&255;frame[4]=(frame.size()-5)>>8;
 for(size_t i=5;i<frame.size();++i)frame[i]=uint8_t(i*37+11);
 for(unsigned i=0;i<16;++i){assert(b.Write(frame.data(),frame.size()));expected.insert(expected.end(),frame.begin(),frame.end());}
 b.Poll();assert(b.Connected() && b.Queued());
 for(unsigned tries=0;tries<10000 && (b.Queued() || actual.size()<expected.size());++tries){
    uint8_t buf[16384];const auto n=recv(s.peer,buf,sizeof(buf),MSG_DONTWAIT);if(n>0)actual.insert(actual.end(),buf,buf+n);b.Poll();
 }
 assert(actual==expected && !b.Queued());
 // Partial incoming events belong to their connection and cannot escape after reconnect.
 s.Send({4,14});b.Poll();b.Close();assert(!b.Pending());assert(b.Open(s.path,error));s.Accept();s.Send(complete);b.Poll();assert(drain(b)==complete);
 s.Send({1,3,12,0});b.Poll();assert(!b.Connected() && !b.Pending()); // Controller must never emit an HCI command.
 assert(b.Open(s.path,error));s.Accept();close(s.peer);s.peer=-1;b.Poll();assert(!b.Connected() && !b.Pending());
 assert(b.Open(s.path,error));s.Accept();assert(!b.Write(frame.data(),BluetoothBackend::maxFrame+1) && !b.Connected());
 puts("PASS H4 Unix endpoint: framing, coalescing, short writes, exact 512KiB output, failed replacement, disconnect, generation and bounds");
}
