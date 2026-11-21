#include "PosixCompat.h"
#include "BluetoothBackend.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <afunix.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {
#ifdef _WIN32
using HostSocket = SOCKET;
constexpr HostSocket invalidSocket = INVALID_SOCKET;
constexpr int sendFlags = 0, receiveFlags = 0;
int socketError() { return WSAGetLastError(); }
bool interrupted(int error) { return error == WSAEINTR; }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
void closeSocket(HostSocket socket) { closesocket(socket); }

bool startWinsock() {
    struct Winsock {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        ~Winsock() { if (!error) WSACleanup(); }
    };
    static const Winsock winsock;
    return !winsock.error;
}

bool connectSocket(HostSocket socket, const sockaddr_un& address, int& error) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
        error = socketError();
        return false;
    }
    if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return true;
    error = socketError();
    if (!wouldBlock(error)) return false;
    fd_set ready, failed;
    FD_ZERO(&ready);
    FD_ZERO(&failed);
    FD_SET(socket, &ready);
    FD_SET(socket, &failed);
    // A local controller has a short deadline. Keep the previous connection on failure.
    timeval timeout{0, 50000};
    const int result = select(0, nullptr, &ready, &failed, &timeout);
    if (result > 0 && FD_ISSET(socket, &ready) && !FD_ISSET(socket, &failed)) return true;
    if (result == 0) error = WSAETIMEDOUT;
    else if (result < 0) error = socketError();
    else {
        int length = sizeof(error);
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0)
            error = socketError();
    }
    return false;
}
#else
using HostSocket = int;
constexpr HostSocket invalidSocket = -1;
constexpr int sendFlags = MSG_DONTWAIT | MSG_NOSIGNAL, receiveFlags = MSG_DONTWAIT;
int socketError() { return errno; }
bool interrupted(int error) { return error == EINTR; }
bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
void closeSocket(HostSocket socket) { close(socket); }

bool connectSocket(HostSocket socket, const sockaddr_un& address, int& error) {
    if (fcntl(socket, F_SETFL, O_NONBLOCK) == 0 &&
        connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return true;
    error = socketError();
    return false;
}
#endif

std::string errorText(int error) {
#ifdef _WIN32
    return "Windows Bluetooth socket error " + std::to_string(error);
#else
    return std::strerror(error);
#endif
}
}  // namespace

bool BluetoothBackend::Open(const std::string& path,std::string& error) {
    std::string endpoint = path;
#ifdef _WIN32
    std::replace(endpoint.begin(), endpoint.end(), '\\', '/');
#endif
    sockaddr_un address{};
    if (!frondPosix::IsAbsolutePath(endpoint) || endpoint.find('\0') != std::string::npos ||
        endpoint.size() >= sizeof(address.sun_path)) {
        error = "Choose an absolute Unix socket path shorter than 108 bytes.";
        return false;
    }
#ifdef _WIN32
    if (!startWinsock()) {
        error = "Could not start Windows Sockets.";
        return false;
    }
    const HostSocket candidate = WSASocketW(AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
#else
    const HostSocket candidate = frondPosix::Socket(AF_UNIX, SOCK_STREAM | frondPosix::kCloexec, 0);
#endif
    if (candidate == invalidSocket) {
        error = errorText(socketError());
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    int failure = 0;
    if (!connectSocket(candidate, address, failure)) {
        error = errorText(failure);
        closeSocket(candidate);
        return false;
    }
    Close();
    fd = static_cast<intptr_t>(candidate);
    error.clear();
    if(hasAddress){
        const uint8_t command[]={1,0x01,0xfc,6};
        outgoing.assign(command,command+sizeof(command));outgoing.insert(outgoing.end(),localAddress.begin(),localAddress.end());addressReplyDue=true;
    }
    return true;
}
void BluetoothBackend::Close() {
    if (Connected()) closeSocket(static_cast<HostSocket>(fd));
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
    if(!Connected())return;
    const auto socket = static_cast<HostSocket>(fd);
    // Bounded work per guest callback. A short write keeps its exact suffix.
    for(unsigned tries=0;tries<4 && Queued();++tries) {
        const auto n=send(socket,reinterpret_cast<const char*>(outgoing.data()+sentAt),int(Queued()),sendFlags);
        const int error = n < 0 ? socketError() : 0;
        if(n<0 && interrupted(error))continue;
        if(n<0 && wouldBlock(error))break;
        if(n<=0){Close();return;}
        sentAt+=size_t(n);
    }
    if(sentAt==outgoing.size()){outgoing.clear();sentAt=0;}
    if (!receive) return;
    std::array<uint8_t,4096> bytes;
    for(unsigned tries=0;tries<4;++tries) {
        const auto n=recv(socket,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),receiveFlags);
        const int error = n < 0 ? socketError() : 0;
        if(n<0 && interrupted(error))continue;
        if(n<0 && wouldBlock(error))break;
        if(n<=0 || !Consume(bytes.data(),size_t(n))){Close();return;}
    }
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
