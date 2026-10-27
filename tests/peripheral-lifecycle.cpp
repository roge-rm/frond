// Firmware-free lifecycle tests using production scheduling and unnamed local peers.
// No host serial endpoint, external network service or physical timing assertions.
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include "CPU.h"
#include "HostNetwork.h"
#include "ReadOnlyGuest.h"
#include "mem.h"
#include "pace.h"
#include "pxa_UART.h"
#include "soc_PXA.h"

class Machine : public SocPXA {
 public:
    using SocPXA::SocPXA;
    bool failSave = false;
    bool Save() override {
        if (failSave) { failSave = false; return false; }
        return SocPXA::Save();
    }
};
static void write(Machine& soc, uint32_t a, uint32_t v) {
    assert(memAccess(cpuGetMem(soc.GetCpu()), a, 4, true, &v));
}
static uint32_t read(Machine& soc, uint32_t a) {
    uint32_t v = 0;
    assert(memAccess(cpuGetMem(soc.GetCpu()), a, 4, false, &v));
    return v;
}
static void run(Machine& soc, unsigned ms) {
    assert(soc.Run(uint64_t(ms) * 100000, 100000000) >= uint64_t(ms) * 100000);
}
static std::vector<uint8_t> snapshot(Machine& soc) {
    assert(soc.Save()); auto b = soc.GetSavestate();
    return {static_cast<uint8_t*>(b.data), static_cast<uint8_t*>(b.data) + b.size};
}
static void enterSleep(Machine& soc) {
    write(soc, 0x40f0000c, 1); // GPIO0 wake enabled, falling edge
    write(soc, 0x40f00010, 0);
    write(soc, 0x40f00014, 1);
    write(soc, 0xa0001000, 0xee070e10); // MCR p14,0,r0,c7,c0,0
    write(soc, 0xa0001004, 0xeafffffe);
    auto* cpu = soc.GetCpu(); cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(cpu, 15, 0xa0001000); cpuSetReg(cpu, 0, 3); cpuCyclePure(cpu);
    assert(soc.PowerSleeping());
}
static void wake(Machine& soc) {
    soc.KeyDown(keyIdPower); run(soc, 40);
    assert(!soc.PowerSleeping());
    soc.KeyUp(keyIdPower); run(soc, 40);
}
static void configure(Machine& soc) {
    const uint32_t u = PXA_FFUART_BASE;
    write(soc,u+12,0x83); write(soc,u,96); write(soc,u+4,0);
    write(soc,u+12,3); write(soc,u+16,8); write(soc,u+4,0x41); write(soc,u+8,7);
}
struct Peer {
    int fd, other;
    unsigned reads = 0, ticks = 0, active = 0, sent = 0, closes = 0;
    Peer() {
        int pair[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair));
        fd=pair[0]; other=pair[1];
    }
    ~Peer() { if(fd>=0)close(fd); close(other); }
    bool attach(PxaUart* uart) {
        return pxaUartAttachExternal(uart, [](void* p)->uint_fast16_t {
            auto& x=*static_cast<Peer*>(p); ++x.reads; uint8_t c;
            auto n=recv(x.fd,&c,1,0); if(n==1)return c;
            assert(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)); return UART_CHAR_NONE;
        }, [](uint_fast16_t c,void* p) {
            auto& x=*static_cast<Peer*>(p); uint8_t b=c;
            assert(send(x.fd,&b,1,MSG_NOSIGNAL)==1); ++x.sent;
        }, [](void* p) { ++static_cast<Peer*>(p)->active; return true; },
        [](void* p) { ++static_cast<Peer*>(p)->ticks; },
        [](void* p) { auto& x=*static_cast<Peer*>(p); ++x.closes; assert(x.fd>=0); close(x.fd); x.fd=-1; },this);
    }
    void input(uint8_t c) { assert(send(other,&c,1,MSG_NOSIGNAL)==1); }
    int output() { uint8_t c; auto n=recv(other,&c,1,0); return n==1 ? c : n==0 ? -1 : -2; }
    bool closed() {
        uint8_t c; auto n=recv(other,&c,1,0);
        // Closing a socket with unread peer data can report reset instead of EOF.
        return n==0 || (n<0 && errno==ECONNRESET);
    }
};
static void startRing(Machine& soc) {
    // A legal cyclic memory descriptor. No peripheral request or host I/O.
    write(soc,0xa0002000,0xa0002000);
    write(soc,0xa0002004,0xa0004000);
    write(soc,0xa0002008,0xa0005000);
    write(soc,0xa000200c,0x00200004);
    write(soc,0xa0004000,0x89abcdef);
    write(soc,0x40000000,0x00000017);
    write(soc,0x40000200,0xa0002000);
    write(soc,0x40000000,0x80000000);
}
// Exercise the real synthetic NetLib ABI against mapped RAM and the real CPU.
// The guest never executes these calls; the test prepares their packed arguments.
struct NetCalls {
    Machine& soc;
    uint8_t* ram;
    static constexpr uint32_t sp=0xa0007000, error=sp+0x100, endpoint=sp+0x200, data=sp+0x300;
    explicit NetCalls(Machine& soc) : soc(soc),ram(static_cast<uint8_t*>(soc.GetMemoryData().data)) {}
    void put(uint32_t a,uint32_t v,unsigned size=4) {
        for(unsigned i=0;i<size;++i)ram[a-0xa0000000+size-1-i]=v>>(8*i);
    }
    uint32_t get(uint32_t a,unsigned size=4) {
        uint32_t v=0; for(unsigned i=0;i<size;++i)v=(v<<8)|ram[a-0xa0000000+i]; return v;
    }
    void start() {
        memset(ram+sp-0xa0000000,0,48); put(sp,NetworkSimulation::refnum,2); put(error,0,2);
        paceSetAreg(7,sp); paceSetPC(0xa0007400);
    }
    void arg(unsigned offset,uint32_t v,unsigned size=4) { put(sp+offset,v,size); }
    uint32_t call(uint16_t trap) {
        assert(soc.networkSimulation.Handle(soc.GetCpu(),trap)); return paceGetDreg(0);
    }
    uint16_t openUdp() {
        start(); arg(2,error); assert(call(0xa801)==0 && !get(error,2));
        return socket();
    }
    uint16_t socket() {
        start(); arg(2,2,1); arg(4,2,1); arg(6,17,2); arg(12,error);
        assert(call(0xa807)==4 && !get(error,2));
        put(endpoint,2,2); put(endpoint+2,0,2); put(endpoint+4,0x7f000001);
        start(); arg(2,4,2); arg(4,endpoint); arg(8,8,2); arg(14,error);
        assert(!call(0xa80b) && !get(error,2));
        start(); arg(2,4,2); arg(4,endpoint); arg(8,endpoint+16); arg(24,error); put(endpoint+16,8,2);
        assert(!call(0xa829) && !get(error,2)); return get(endpoint+2,2);
    }
    uint16_t receive(int expected=-1) {
        start(); arg(2,4,2); arg(4,data); arg(8,1,2); arg(24,error);
        const auto result=call(0xa813), err=get(error,2);
        if(expected>=0)assert(!err && result==1 && get(data,1)==unsigned(expected));
        else assert(result==UINT32_MAX && err);
        return err;
    }
    void close() { start(); arg(2,4,2); arg(8,error); assert(!call(0xa808) && !get(error,2)); }
};
int main() {
    // A broken descriptor loop must fail this process within a bounded time.
    alarm(45);
    std::vector<uint8_t> rom(16*1024*1024), nand(132*1024*1024,0xff);
    const uint32_t branch=0xeafffffe; memcpy(rom.data(),&branch,4);
    Machine soc(deviceTypeTX,32*1024*1024,rom.data(),rom.size(),nand.data(),nand.size(),-1,2);
    auto* ram=static_cast<uint8_t*>(soc.GetMemoryData().data);
    // Maximum DCMD length at the smallest burst size still completes exactly,
    // including its final byte lanes and an ordinary terminating descriptor.
    for(unsigned mode : {0u,0x40000000u}) {
        for(unsigned i=0;i<8192;++i) { ram[0x8000+i]=uint8_t(i*37); ram[0x10000+i]=0xa5; }
        write(soc,0x40000000,mode|0x17);
        write(soc,0xa0002000,1); write(soc,0xa0002004,0xa0008000);
        write(soc,0xa0002008,0xa0010000); write(soc,0xa000200c,0xc0201fff);
        write(soc,0x40000200,0xa0002000);
        write(soc,0x40000204,0xa0008000); write(soc,0x40000208,0xa0010000);
        write(soc,0x4000020c,0xc0201fff); write(soc,0x40000000,mode|0x80000000);
        run(soc,1); assert((read(soc,0x40000000)&0x8000000du)==12);
        assert(!memcmp(ram+0x8000,ram+0x10000,8191) && ram[0x11fff]==0xa5);
    }
    puts("PASS: 8191-byte minimum-burst DMA completes exact tails in direct and terminating-descriptor modes");
    fflush(stdout); // Preserve the finite-transfer control before a broken ring can hang.
    startRing(soc);
    // A second channel must also get service while channel zero loops.
    write(soc,0x40000004,0x40000017);
    write(soc,0x40000214,0xa0004000); write(soc,0x40000218,0xa0006000);
    write(soc,0x4000021c,0xc0210004); write(soc,0x40000004,0xc0000000);
    run(soc,1);
    assert(read(soc,0xa0005000)==0x89abcdef && read(soc,0xa0006000)==0x89abcdef);
    assert(read(soc,0x40000000)&0x80000000);
    auto saved=snapshot(soc); const auto time=soc.GetTime();
    {
        ReadOnlyGuest checkpoint(soc);
        run(soc,1); write(soc,0xa0004000,0x10203040); run(soc,1);
        assert(read(soc,0xa0005000)==0x10203040);
    }
    assert(soc.GetTime()==time && read(soc,0xa0005000)==0x89abcdef);
    assert(read(soc,0x40000000)&0x80000000); run(soc,1);
    assert(!soc.Load(saved.size()-4,saved.data())); run(soc,1);
    assert(read(soc,0x40000000)&0x80000000);
    write(soc,0x40000000,0); assert(!(read(soc,0x40000000)&0x80000000));
    assert(soc.Load(saved.size(),saved.data())); run(soc,1);
    assert(read(soc,0x40000000)&0x80000000);
    enterSleep(soc); write(soc,0xa0005000,0); run(soc,100);
    assert(!read(soc,0xa0005000)); // peripheral clocks stopped
    wake(soc); assert(!(read(soc,0x40000000)&0x80000000));
    assert(!read(soc,0xa0005000)); // reset domain must not replay ring
    startRing(soc); run(soc,1); soc.Reset(); run(soc,1);
    assert(!(read(soc,0x40000000)&0x80000000));
    puts("PASS: cyclic DMA yields to another channel/guest, survives checkpoint and accepted/rejected loads, stops across sleep/wake/reset");

    configure(soc); auto* uart=soc.GetSerialUart();
    {
        Peer peer; assert(peer.attach(uart));
        write(soc,PXA_FFUART_BASE,0x51); peer.input(0x81);
        const auto before=soc.GetTime();
        const auto initialReads=peer.reads, initialTicks=peer.ticks, initialActive=peer.active;
        {
            ReadOnlyGuest checkpoint(soc); run(soc,10);
            assert(peer.reads==initialReads && !peer.sent && peer.ticks==initialTicks && peer.active==initialActive);
        }
        assert(soc.GetTime()==before && !peer.closes);
        run(soc,5); assert(peer.output()==0x51 && read(soc,PXA_FFUART_BASE)==0x81);
        write(soc,PXA_FFUART_BASE,0x52); peer.input(0x82);
        {
            ReadOnlyGuest checkpoint(soc); run(soc,10); checkpoint.Commit();
        }
        run(soc,5); assert(peer.output()==0x52 && read(soc,PXA_FFUART_BASE)==0x82);
        soc.failSave=true; bool rejected=false;
        try { ReadOnlyGuest checkpoint(soc); } catch(const std::runtime_error&) { rejected=true; }
        assert(rejected && !peer.closes);
        // Partial-frame state at power sleep cannot escape while clocks stop.
        write(soc,PXA_FFUART_BASE,0x53); enterSleep(soc);
        const auto reads=peer.reads,ticks=peer.ticks,active=peer.active,sent=peer.sent;
        peer.input(0x83); auto asleep=snapshot(soc); run(soc,100);
        assert(peer.reads==reads && peer.ticks==ticks && peer.active==active && peer.sent==sent && !peer.closes);
        {
            ReadOnlyGuest checkpoint(soc); run(soc,100);
        }
        assert(soc.PowerSleeping() && !peer.closes);
        wake(soc); assert(peer.closes==1 && peer.closed() && peer.sent==sent);
        assert(!(read(soc,PXA_FFUART_BASE+20)&1));
        assert(soc.Load(asleep.size(),asleep.data())); assert(soc.PowerSleeping());
        wake(soc); assert(peer.closes==1); // saved CTS/ownership never resurrects
    }
    configure(soc);
    {
        Peer fresh; assert(fresh.attach(uart)); run(soc,5); assert(fresh.output()==-2);
        write(soc,PXA_FFUART_BASE,0x54); run(soc,5); assert(fresh.output()==0x54);
        auto state=snapshot(soc); write(soc,PXA_FFUART_BASE,0x55);
        assert(!soc.Load(state.size()-4,state.data()));
        assert(fresh.closes==1 && fresh.output()==-1); run(soc,5); assert(fresh.sent==1);
        soc.Reset(); assert(fresh.closes==1);
    }
    puts("PASS: scheduled socket peer I/O freezes/rolls back/commits, sleep stops callbacks, wake resets UART and closes ownership once, failed restore fails closed");

    NetCalls calls(soc); assert(soc.networkSimulation.SetMode("internet"));
    auto livePort=calls.openUdp();
    int udpPeer=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0); assert(udpPeer>=0);
    auto sendUdp=[&](char c) {
        sockaddr_in target{}; target.sin_family=AF_INET;
        target.sin_addr.s_addr=htonl(INADDR_LOOPBACK); target.sin_port=htons(livePort);
        assert(sendto(udpPeer,&c,1,0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==1);
    };
    for(bool commit : {false,true}) {
        {
            ReadOnlyGuest checkpoint(soc); sendUdp(commit?'C':'R');
            assert(calls.receive()==0x122f); run(soc,1);
            if(commit)checkpoint.Commit();
        }
        calls.receive(commit?'C':'R'); assert(calls.receive()==0x122f);
    }
    // Library Sleep/Wake are currently no-op acknowledgements, separate from
    // PXA true power sleep. Host kernels can still queue packets during either.
    calls.start(); assert(!calls.call(0xa803));
    calls.start(); assert(!calls.call(0xa804));
    enterSleep(soc); sendUdp('S'); auto networkAsleep=snapshot(soc); run(soc,100);
    assert(soc.PowerSleeping()); wake(soc); calls.receive('S');
    assert(soc.Load(networkAsleep.size(),networkAsleep.data()) && soc.PowerSleeping());
    wake(soc); assert(calls.receive()==0x1214); // occupied Palm reference, closed host socket
    calls.close(); livePort=calls.socket(); sendUdp('N'); calls.receive('N');
    auto networkSaved=snapshot(soc); sendUdp('X');
    assert(!soc.Load(networkSaved.size()-4,networkSaved.data()));
    assert(calls.receive()==0x1214); // rejected load also fails closed, including queued X
    calls.close(); livePort=calls.socket(); sendUdp('F'); calls.receive('F');
    soc.Reset(); assert(soc.networkSimulation.GetMode()==NetworkSimulation::internet);
    assert(calls.receive()==0x1202); // reset drops library/socket ownership, retaining mode
    livePort=calls.openUdp(); sendUdp('Z'); calls.receive('Z'); close(udpPeer);
    soc.networkSimulation.Reset();
    puts("PASS: real NetLib local UDP survives checkpoint rollback/commit and true sleep, closes across sleeping restore/rejected load, resets ownership and reconnects");

    // Host waits use monotonic host time, not the guest scheduler's time.
    // Reset must cancel the old operation and release its socket even if the
    // same Palm slot and call key are immediately reused.
    HostNetwork net; const HostNetwork::Key key{0x1000,0x2000,0xa813};
    char byte=0; uint32_t address=0; uint16_t port=0;
    for(unsigned i=0;i<8;++i) {
        assert(!net.Open(0,2,17).error);
        assert(!net.Bind(0,0x7f000001,0).error);
        auto waiting=net.Receive(0,&byte,1,0,address,port,-1,key); assert(waiting.waiting);
        net.Reset();
        auto closed=net.Receive(0,&byte,1,0,address,port,0,key);
        assert(closed.error==0x1214 && !closed.waiting);
        net.Complete(key); // late completion after cancellation is harmless
    }
    assert(!net.Open(0,2,17).error); assert(!net.Bind(0,0x7f000001,0).error);
    assert(!net.Address(0,false,address,port).error);
    int sender=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0); assert(sender>=0);
    sockaddr_in target{}; target.sin_family=AF_INET; target.sin_addr.s_addr=htonl(address); target.sin_port=htons(port);
    assert(sendto(sender,"N",1,0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==1);
    auto got=net.Receive(0,&byte,1,0,address,port,0,key); assert(!got.error && got.value==1 && byte=='N');
    close(sender); net.Reset();
    puts("PASS: pending local UDP receive/reset cancellation and fresh I/O after repeated socket-slot reuse");
}
