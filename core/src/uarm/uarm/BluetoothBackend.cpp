#include "BluetoothBackend.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

bool BluetoothBackend::Open(const std::string& path,std::string& error) {
#ifdef __EMSCRIPTEN__
    error="Bluetooth controller endpoints require the native runner.";return false;
#else
    if(path.empty() || path[0]!='/' || path.find('\0')!=std::string::npos || path.size()>=sizeof(sockaddr_un::sun_path)) {
        error="Choose an absolute Unix socket path shorter than 108 bytes.";return false;
    }
    const int candidate=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(candidate<0){error=std::strerror(errno);return false;}
    sockaddr_un address{};address.sun_family=AF_UNIX;
    std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    // A local Unix connect has no network route/timeout. Set nonblocking before
    // connection so a full listener queue cannot stall the emulation thread.
    if(fcntl(candidate,F_SETFL,O_NONBLOCK)<0 || connect(candidate,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0) {
        error=std::strerror(errno);close(candidate);return false;
    }
    Close();fd=candidate;error.clear();
    if(hasAddress){
        const uint8_t command[]={1,0x01,0xfc,6};
        outgoing.assign(command,command+sizeof(command));outgoing.insert(outgoing.end(),localAddress.begin(),localAddress.end());addressReplyDue=true;
    }
    return true;
#endif
}
void BluetoothBackend::Close() {
#ifndef __EMSCRIPTEN__
    if(fd>=0)close(fd);
#endif
    fd=-1;incoming.clear();outgoing.clear();received.clear();expected=sentAt=readAt=frameRemaining=0;addressReplyDue=false;
}
// Command Complete or Command Status for Write_BD_ADDR. A controller without it answers
// Unknown Command and keeps its own address.
bool BluetoothBackend::AddressReply() const {
    if(incoming[0]!=4)return false;
    if(incoming[1]==0x0e && incoming.size()>=6)return incoming[4]==0x01 && incoming[5]==0xfc;
    if(incoming[1]==0x0f && incoming.size()>=7)return incoming[5]==0x01 && incoming[6]==0xfc;
    return false;
}
bool BluetoothBackend::Write(const uint8_t* bytes,size_t size) {
    if(!Connected() || !bytes || !size || size>maxFrame || size>capacity-Queued()) {Close();return false;}
    if(size==10 && bytes[0]==1 && bytes[1]==0x01 && bytes[2]==0xfc && bytes[3]==6){
        std::copy(bytes+4,bytes+10,localAddress.begin());hasAddress=true;
    }
    if(sentAt){outgoing.erase(outgoing.begin(),outgoing.begin()+sentAt);sentAt=0;}
    outgoing.insert(outgoing.end(),bytes,bytes+size);return true;
}
bool BluetoothBackend::Consume(const uint8_t* bytes,size_t size) {
    for(size_t i=0;i<size;++i) {
        if(incoming.size()==maxFrame)return false;
        incoming.push_back(bytes[i]);
        if(incoming.size()==1 && incoming[0]!=2 && incoming[0]!=3 && incoming[0]!=4)return false;
        if(incoming[0]==4 && incoming.size()==3)expected=3+incoming[2];
        else if(incoming[0]==2 && incoming.size()==5)expected=5+incoming[3]+(size_t(incoming[4])<<8);
        else if(incoming[0]==3 && incoming.size()==4)expected=4+incoming[3];
        if(expected>maxFrame)return false;
        if(expected && incoming.size()==expected) {
            if(addressReplyDue && AddressReply()){addressReplyDue=false;incoming.clear();expected=0;continue;}
            if(expected>capacity-Pending())return false;
            if(readAt){received.erase(received.begin(),received.begin()+readAt);readAt=0;}
            received.insert(received.end(),incoming.begin(),incoming.end());
            incoming.clear();expected=0;
        }
    }
    return true;
}
void BluetoothBackend::Poll(bool receive) {
#ifndef __EMSCRIPTEN__
    if(!Connected())return;
    // Bounded work per guest callback. A short write keeps its exact suffix.
    for(unsigned tries=0;tries<4 && Queued();++tries) {
        const auto n=send(fd,outgoing.data()+sentAt,Queued(),MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n<0 && errno==EINTR)continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))break;
        if(n<=0){Close();return;}
        sentAt+=size_t(n);
    }
    if(sentAt==outgoing.size()){outgoing.clear();sentAt=0;}
    if (!receive) return;
    std::array<uint8_t,4096> bytes;
    for(unsigned tries=0;tries<4;++tries) {
        const auto n=recv(fd,bytes.data(),bytes.size(),MSG_DONTWAIT);
        if(n<0 && errno==EINTR)continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))break;
        if(n<=0 || !Consume(bytes.data(),size_t(n))){Close();return;}
    }
#endif
}
uint16_t BluetoothBackend::Read() {
    if(!Pending())return 0x100;
    if(!frameRemaining){
        // Consume exposes complete validated frames only. Retain this frame's
        // ownership across byte callbacks and receive-buffer compaction.
        const auto* p=received.data()+readAt;
        frameRemaining=p[0]==4?3+p[2]:p[0]==2?5+p[3]+(size_t(p[4])<<8):4+p[3];
    }
    --frameRemaining;
    const auto c=received[readAt++];
    if(readAt==received.size()){received.clear();readAt=0;}
    return c;
}
