// Real core bus/restore paths; synthetic memory and a private, unnamed socketpair.
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include "CPU.h"
#include "ReadOnlyGuest.h"
#include "mem.h"
#include "pxa_UART.h"
#include "soc_PXA.h"
class Machine : public SocPXA {
 public:
    using SocPXA::SocPXA;
    bool failSave=false;
    bool Save() override{if(failSave){failSave=false;return false;}return SocPXA::Save();}
    bool buffer(uint32_t a,uint32_t n,bool w){return memIsBufferRange(mem,a,n,w);}
};
struct Peer {
    int fd=-1,other=-1;
    PxaUart* uart=nullptr;
    unsigned rejectedReentry=0;
    unsigned closes=0,reads=0,ticks=0,active=0,sent=0;
    Peer(){int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair));fd=pair[0];other=pair[1];}
    ~Peer(){if(fd>=0)close(fd);close(other);}
    bool attach(PxaUart* u){uart=u;return pxaUartAttachExternal(u,[](void* p)->uint_fast16_t{
        auto& x=*static_cast<Peer*>(p);++x.reads;uint8_t c;
        const auto n=recv(x.fd,&c,1,0);if(n==1)return c;assert(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK));return UART_CHAR_NONE;
    },[](uint_fast16_t c,void* p){auto& x=*static_cast<Peer*>(p);uint8_t b=c;assert(send(x.fd,&b,1,MSG_NOSIGNAL)==1);++x.sent;},
    [](void* p){++static_cast<Peer*>(p)->active;return false;},
    [](void* p){++static_cast<Peer*>(p)->ticks;},
    [](void* p){auto& x=*static_cast<Peer*>(p);++x.closes;assert(x.fd>=0);close(x.fd);x.fd=-1;assert(!x.attach(x.uart));++x.rejectedReentry;},this);}
    int receive(){uint8_t c=0;const auto n=recv(other,&c,1,0);return n==1?c:n==0?-1:-2;}
};
int main(){
    std::vector<uint8_t> rom(16*1024*1024),nand(132*1024*1024,0xff);
    Machine soc(deviceTypeTX,32*1024*1024,rom.data(),rom.size(),nand.data(),nand.size(),-1,2);
    auto* cpu=soc.GetCpu();auto* uart=soc.GetSerialUart();assert(uart);
    auto write=[&](uint32_t a,uint32_t v){assert(cpuMemOpExternal(cpu,&v,a,4,true));};
    auto read=[&](uint32_t a){uint32_t v=0;assert(cpuMemOpExternal(cpu,&v,a,4,false));return v;};
    auto uw=[&](unsigned r,uint32_t v){write(PXA_FFUART_BASE+4*r,v);};
    auto ur=[&](unsigned r){return read(PXA_FFUART_BASE+4*r);};
    auto tx=[&](uint8_t value){assert(cpuMemOpExternal(cpu,&value,PXA_FFUART_BASE,1,true));};
    auto config=[&]{uw(3,0x83);uw(0,96);uw(1,0);uw(3,3);uw(1,0x49);uw(4,0x2b);uw(2,0x27);};
    auto ticks=[&](unsigned n){for(unsigned i=0;i<n;++i)pxaUartProcess(uart);};
    auto snapshot=[&]{assert(soc.Save());auto b=soc.GetSavestate();
        const auto* words=static_cast<const uint32_t*>(b.data);const auto count=le32toh(words[0]);
        size_t offset=1+2*count;bool found=false;
        for(unsigned i=0;i<count;++i){const auto size=le32toh(words[2+2*i]);
            if(le32toh(words[1+2*i])==static_cast<uint32_t>(ChunkType::pxaUart)){
                assert(size==77 && le32toh(words[offset])==3);found=true; // host ownership/pins add no serialized fields
            }offset+=size;
        }assert(found);return std::vector<uint8_t>(static_cast<uint8_t*>(b.data),static_cast<uint8_t*>(b.data)+b.size);};
    assert(soc.buffer(0xa0002000,65,true)&&soc.buffer(0,4,false));
    assert(!soc.buffer(0,4,true)&&!soc.buffer(PXA_FFUART_BASE,4,false));
    assert(!soc.buffer(0xffffffff,4,false)&&!soc.buffer(0xa1ffffff,2,true));
    auto* ram=static_cast<uint8_t*>(soc.GetMemoryData().data);
    const uint32_t branch=0xeafffffe;memcpy(ram+0x1000,&branch,4);cpuSetReg(cpu,15,0xa0001000);
    auto dma=[&](uint32_t src,uint32_t dst,uint32_t command){
        write(0x40000000,0x40000017);write(0x40000204,src);write(0x40000208,dst);
        write(0x4000020c,command);write(0x40000000,0xc0000000);
        assert(soc.Run(20000,100000000)>=20000);
    };
    for(unsigned width=0;width<=3;++width)for(unsigned length=1;length<=65;++length){
        for(unsigned i=0;i<80;++i){ram[0x2000+i]=uint8_t(i^0x96);ram[0x3000+i]=0xa5;}
        dma(0xa0002000,0xa0003000,0xc0210000|(width<<14)|length);
        assert((read(0x40000000)&0x8000000du)==12 && !(read(0x4000020c)&0x1fff));
        for(unsigned i=0;i<80;++i)assert(ram[0x3000+i]==(i<length?uint8_t(i^0x96):0xa5));
    }
    memset(ram+0x3000,0xa5,8);dma(0xfffffffc,0xa0003000,0xc021c004);
    assert((read(0x40000000)&0x80000009u)==9);
    for(unsigned i=0;i<8;++i)assert(ram[0x3000+i]==0xa5);
    puts("PASS: real mapped RAM/ROM classification and exact 1..65-byte DMA copies with documented WIDTH=00 and preserved legacy width encodings");
    // The existing version-0 DMA chunk resumes a queued, documented WIDTH=00 copy.
    write(0x40000000,0x40000017);write(0x40000204,0xa0002000);write(0x40000208,0xa0003000);
    write(0x4000020c,0xc0210023);write(0x40000000,0xc0000000);auto dmaSaved=snapshot();
    assert(soc.Run(20000,100000000)>=20000);memset(ram+0x3000,0xa5,40);
    assert(soc.Load(dmaSaved.size(),dmaSaved.data()));assert(soc.Run(20000,100000000)>=20000);
    for(unsigned i=0;i<40;++i)assert(ram[0x3000+i]==(i<35?uint8_t(i^0x96):0xa5));
    puts("PASS: queued WIDTH=00 DMA resumes from unchanged version-0 state");
    config();std::deque<uint16_t> input{0x11,0x22,0x33,0x44};
    pxaUartSetFuncs(uart,[](void* p)->uint_fast16_t{auto& q=*static_cast<std::deque<uint16_t>*>(p);if(q.empty())return UART_CHAR_NONE;auto v=q.front();q.pop_front();return v;},nullptr,nullptr,nullptr,&input);
    ticks(400);assert(ur(9)==4);memset(ram+0x3000,0xa5,8); // RX paced at 9600 8N1
    dma(PXA_FFUART_BASE,0xa0003000,0x4021c003); // fixed MMIO source, increment RAM target
    assert((read(0x40000000)&0x80000009u)==9 && ur(9)==4);
    for(unsigned i=0;i<8;++i)assert(ram[0x3000+i]==0xa5);
    assert(ur(0)==0x44332211);
    puts("PASS: unsupported UART partial DMA beat reports BUSERR without reading FIFO or touching RAM");
    Peer first;assert(first.attach(uart));assert((ur(6)&0xf0)==0);
    assert(pxaUartSetModemInputs(uart,PXA_UART_CTS|PXA_UART_DSR|PXA_UART_DCD));ur(6);
    tx(0x41);ticks(107);assert(first.receive()==0x41);
    tx(0x42);auto saved=snapshot();ticks(107);const int got=first.receive();
    if(got!=0x42)std::fprintf(stderr,"peer got=%d sent=%u closes=%u fd=%d lsr=%x msr=%x\n",got,first.sent,first.closes,first.fd,ur(5),ur(6));
    assert(got==0x42);
    assert(soc.Load(saved.size(),saved.data()));assert(first.closes==1 && first.rejectedReentry==1 && first.fd==-1 && first.receive()==-1);
    assert(read(0x40d00010)&(1u<<22)); // reconcile physical modem IRQ after the IC snapshot loads
    assert((ur(6)&0xf0)==0);assert(!(read(0x40d00010)&(1u<<22)));ticks(1000);assert(first.sent==2);
    Peer rejected;assert(!rejected.attach(uart)); // no replay to a new peer while old guest TX remains pending
    int spare[2];assert(!pipe(spare));
    assert(soc.Load(saved.size(),saved.data()));assert(first.closes==1 && fcntl(spare[0],F_GETFD)>=0);close(spare[0]);close(spare[1]);
    soc.Reset();config();assert(first.closes==1);
    puts("PASS: normal restore closes owned peer once, prevents saved-pin resurrection/replay and rejects busy reattach");
    {
        Peer failed;assert(failed.attach(uart));assert(pxaUartSetModemInputs(uart,PXA_UART_CTS));tx(0x43);
        assert(!soc.Load(saved.size()-4,saved.data()));assert(failed.closes==1 && failed.receive()==-1);
        ticks(1000);assert(!failed.sent);soc.Reset();config();assert(failed.closes==1);
    }
    puts("PASS: rejected restore and reset do not retain or close an owned descriptor twice");
    {
        Peer live;assert(live.attach(uart));assert(soc.Load(0,nullptr) && !live.closes);assert(pxaUartSetModemInputs(uart,PXA_UART_CTS));
        for(unsigned attempt=0;attempt<2;++attempt){
            tx(0x50+attempt);uint8_t incoming=0x80+attempt;assert(send(live.other,&incoming,1,MSG_NOSIGNAL)==1);
            const auto reads=live.reads,tickCount=live.ticks,active=live.active,sent=live.sent;
            try{
                ReadOnlyGuest checkpoint(soc);SoC::SerialCheckpoint nested(soc);
                pxaUartDetachExternal(uart);pxaUartSetFuncs(uart,nullptr,nullptr,nullptr,nullptr,nullptr);
                assert(!pxaUartSetModemInputs(uart,0));assert(!live.attach(uart));ticks(1000);
                assert(live.reads==reads && live.ticks==tickCount && live.active==active && live.sent==sent && !live.closes);
                if(attempt)throw std::runtime_error("injected timeout");
            }catch(const std::runtime_error&){assert(attempt);}
            assert(!live.closes && fcntl(live.fd,F_GETFD)>=0);ticks(107);
            assert(live.receive()==0x50+attempt && ur(0)==0x80+attempt && live.sent==sent+1);
        }
        soc.failSave=true;bool threw=false;try{ReadOnlyGuest checkpoint(soc);}catch(const std::runtime_error&){threw=true;}
        assert(threw && !live.closes);pxaUartDetachExternal(uart);pxaUartDetachExternal(uart);
        assert(live.closes==1 && live.receive()==-1);
    }
    puts("PASS: export success/error/nesting/construction failure preserve the live peer and defer RX/TX exactly once");
    soc.Reset();config();
    {Peer reset;assert(reset.attach(uart));soc.Reset();assert(reset.closes==1 && reset.receive()==-1);pxaUartDetachExternal(uart);assert(reset.closes==1);}
    puts("PASS: reset notifies and closes explicitly external callbacks; internal callbacks remain a separate model contract");

    // UART DMA, IER[DMAE] (PXA27x 10.4.2.3-10.4.2.6), through the real DMA controller.
    struct Line { std::deque<uint16_t> in; std::vector<uint16_t> out; } line;
    pxaUartSetFuncs(uart,[](void* p)->uint_fast16_t{auto& l=*static_cast<Line*>(p);if(l.in.empty())return UART_CHAR_NONE;auto v=l.in.front();l.in.pop_front();return v;},
        [](uint_fast16_t c,void* p){static_cast<Line*>(p)->out.push_back(c);},
        [](void* p){return !static_cast<Line*>(p)->in.empty();},nullptr,&line);
    auto runFor=[&](unsigned batches){soc.Run(uint64_t(batches)*977,100000000);}; // ~1 batch per 977 cycles
    const uint32_t dcsr2=0x40000008,ddadr2=0x40000220;
    auto channel=[&](unsigned ch,uint32_t src,uint32_t dst,uint32_t cmd,uint32_t csr){
        write(0x40000000+4*ch,0x40000217); // no-descriptor-fetch, clear status
        write(0x40000200+16*ch+4,src);write(0x40000200+16*ch+8,dst);write(0x40000200+16*ch+12,cmd);
        write(0x40000000+4*ch,0xc0000000|csr);
    };
    // 115200 8N1, FIFO + BUS + TRAIL, ITL 8; DMAE|UUE|RTOIE; DRCMR6 (FFUART RX) -> channel 2.
    auto dmaConfig=[&](uint32_t fcr){
        soc.Reset();cpuSetReg(cpu,15,0xa0001000);
        uw(3,0x83);uw(0,8);uw(1,0);uw(3,3);uw(4,8);uw(2,fcr);uw(1,0xd0);
        write(0x40000118,0x82);line.in.clear();line.out.clear();
    };
    const uint32_t rxCmd=0x6001c000; // INCTRGADDR|FLOWSRC, burst 8, width 4
    auto fill=[&](uint32_t offset,unsigned n){memset(ram+offset,0xa5,n);};
    auto feed=[&](unsigned n,unsigned base){for(unsigned i=0;i<n;++i)line.in.push_back(base+i);};
    {
        dmaConfig(0x77);fill(0x4000,32);channel(2,PXA_FFUART_BASE,0xa0004000,rxCmd|24,0x10000000);
        feed(19,0x80);
        while(line.in.size()>7)runFor(1);
        assert(ur(9)<8 && (read(ddadr2+12)&0x1fff)==16); // one 8-byte burst at the trigger level
        runFor(400);
        for(unsigned i=0;i<32;++i)assert(ram[0x4000+i]==(i<19?0x80+i:0xa5)); // 3 valid bytes in the last word
        assert(read(ddadr2+8)==0xa0004013 && (read(ddadr2+12)&0x1fff)==5 && ur(9)==0);
        const auto csr=read(dcsr2);assert((csr&0x80000208u)==0x80000200u); // EOR, still running
        assert(read(0x400000f0)&4);                                         // EORIRQEN interrupt
        assert((ur(2)&0x2f)==1);                                           // no CPU timeout while DMA drains
    }
    puts("PASS: RX DMA bursts at the trigger level, trailing bytes after the character timeout with a valid-byte count and EOR");
    {
        // EORSTOPEN acts only in descriptor-fetch mode (table 5-17).
        dmaConfig(0x77);fill(0x4000,32);
        const uint32_t d[4]={1,PXA_FFUART_BASE,0xa0004000,rxCmd|24};memcpy(ram+0x6000,d,sizeof(d));
        write(dcsr2,0x217);write(ddadr2,0xa0006000);write(dcsr2,0x84000000);feed(5,0x40);
        runFor(400);
        for(unsigned i=0;i<8;++i)assert(ram[0x4000+i]==(i<5?0x40+i:0xa5));
        assert((read(dcsr2)&0x80000208u)==0x208); // EORSTOPEN stopped the channel
    }
    {
        // EORJMPEN: descriptor fetch mode jumps to the next descriptor on EOR.
        dmaConfig(0x77);fill(0x4000,32);fill(0x4100,32);
        const uint32_t d[8]={0xa0006010,PXA_FFUART_BASE,0xa0004000,rxCmd|24,1,PXA_FFUART_BASE,0xa0004100,rxCmd|24};
        memcpy(ram+0x6000,d,sizeof(d));
        write(0x40000008,0x217);write(ddadr2,0xa0006000);write(dcsr2,0x88000000);
        feed(3,0x50);runFor(300);feed(2,0x60);runFor(300);
        assert(ram[0x4000]==0x50 && ram[0x4002]==0x52 && ram[0x4003]==0xa5);
        assert(ram[0x4100]==0x60 && ram[0x4101]==0x61 && ram[0x4102]==0xa5);
        assert((read(dcsr2)&0x80000208u)==0x208 && (read(ddadr2+12)&0x1fff)==22); // last descriptor: EOR stops
    }
    puts("PASS: EOR stops the channel with EORSTOPEN and moves to the next descriptor with EORJMPEN");
    {
        // The chain ends with bytes left: the UART raises IIR[EOC] and TOD; the CPU drains by PIO.
        dmaConfig(0x77);fill(0x4000,16);channel(2,PXA_FFUART_BASE,0xa0004000,rxCmd|10,0);feed(16,0x70);
        while(!line.in.empty())runFor(1);
        runFor(2);
        for(unsigned i=0;i<12;++i)assert(ram[0x4000+i]==(i<10?0x70+i:0xa5));
        assert((read(dcsr2)&0x80000008u)==8 && ur(9)==6);
        assert(read(0x40d00010)&(1u<<22));
        assert(ur(2)==0xec);                 // FIFO mode, EOC, character timeout
        assert(!(ur(2)&0x20));               // EOC clears on IIR read
        for(unsigned i=0;i<6;++i){uint8_t c=0;assert(cpuMemOpExternal(cpu,&c,PXA_FFUART_BASE,1,false) && c==0x7a+i);}
        assert(ur(9)==0 && !(read(0x40d00010)&(1u<<22)));
    }
    puts("PASS: end of descriptor chain with trailing bytes raises IIR[EOC] and TOD for PIO draining");
    {
        // An error byte in the FIFO stops RX DMA and interrupts without RLSE; PIO drains it.
        dmaConfig(0x77);fill(0x4000,16);channel(2,PXA_FFUART_BASE,0xa0004000,rxCmd|16,0);
        line.in={0x11,0x12,0x413};feed(9,0x14);
        while(!line.in.empty())runFor(1);
        runFor(5);
        assert(ur(9)==12 && ram[0x4000]==0xa5 && (read(ddadr2+12)&0x1fff)==16);
        assert((ur(2)&0x0f)==6 && (read(0x40d00010)&(1u<<22)));
        assert((ur(5)&0x88)==0x80); // FIFOE; the front byte is clean
        uint8_t c=0;
        assert(cpuMemOpExternal(cpu,&c,PXA_FFUART_BASE,1,false) && c==0x11);
        assert(cpuMemOpExternal(cpu,&c,PXA_FFUART_BASE,1,false) && c==0x12);
        assert((ur(5)&0x88)==0x88);  // framing error at the front
        assert(cpuMemOpExternal(cpu,&c,PXA_FFUART_BASE,1,false) && c==0x13);
        assert(!(ur(5)&0x80));
        runFor(400);                 // requests resume: one burst, then the trailing byte
        for(unsigned i=0;i<12;++i)assert(ram[0x4000+i]==(i<9?0x14+i:0xa5));
        assert(ur(9)==0 && (read(ddadr2+12)&0x1fff)==7 && (read(dcsr2)&0x200));
    }
    puts("PASS: receive errors stop DMA requests and raise the error interrupt until PIO drains the error byte");
    {
        // TX DMA: DRCMR7 -> channel 3, requests while the TX FIFO is under half (or empty with TIL).
        for(uint32_t fcr : {0x37u,0x3fu}){
            dmaConfig(fcr);write(0x4000011c,0x83);
            for(unsigned i=0;i<21;++i)ram[0x5000+i]=0x30+i;
            channel(3,0xa0005000,PXA_FFUART_BASE,0x9001c000|21,0);
            runFor(400);
            assert(line.out.size()==21);
            for(unsigned i=0;i<21;++i)assert(line.out[i]==0x30+i);
            assert((read(0x4000000c)&0x80000008u)==8);
        }
    }
    puts("PASS: TX DMA fills the FIFO in words and a byte-lane tail, at half empty or empty (TIL)");
    {
        // Save mid-transfer: the restored run moves the same bytes at the same times.
        dmaConfig(0x77);fill(0x4000,32);channel(2,PXA_FFUART_BASE,0xa0004000,rxCmd|24,0x10000000);feed(19,0x90);
        while(line.in.size()>7)runFor(1);
        runFor(3);
        auto saved=snapshot();const auto pending=line.in;
        auto finish=[&]{std::vector<uint32_t> trace;for(unsigned i=0;i<300;++i){runFor(1);trace.push_back(ur(9)|(read(ddadr2+12)&0x1fff)<<8);}
            trace.push_back(read(dcsr2));trace.push_back(read(ddadr2+8));
            for(unsigned i=0;i<32;++i)trace.push_back(ram[0x4000+i]);return trace;};
        const auto first=finish();
        assert(first[300]&0x200);
        assert(soc.Load(saved.size(),saved.data()));line.in=pending; // RAM is not in this snapshot; the rerun rewrites the same bytes
        const auto second=finish();
        for(size_t i=0;i<first.size();++i)if(first[i]!=second[i]){std::fprintf(stderr,"trace %zu: %x vs %x\n",i,first[i],second[i]);break;}
        assert(second==first);
    }
    puts("PASS: save/load mid DMA transfer resumes requests, trailing bytes and EOR identically");
    for(bool viaFcr : {false,true}){
        // Trailing request with no channel serving it: leaving DMAE or TRAIL hands the bytes to the CPU.
        dmaConfig(0x77);write(0x40000118,0);feed(3,0x20);
        while(!line.in.empty())runFor(1);
        runFor(60);
        assert(ur(9)==3 && (ur(2)&0x0f)==1); // trailing DMA request pending, no TOD
        if(viaFcr)uw(2,0x61);                // TRAIL off, FIFO kept
        else uw(1,0x51);                     // DMAE off, RAVIE|RTOIE
        assert((ur(2)&0x0f)==0x0c && ur(9)==3);
    }
    puts("PASS: clearing DMAE or FCR[TRAIL] mid-trail lets the character timeout interrupt the CPU");
    {
        // A stale UART mapping sharing the channel must not cap the burst at its valid bytes.
        dmaConfig(0x67);fill(0x4000,16);write(0x40000110,0x82); // DRCMR4 (BTUART RX) also -> channel 2
        channel(2,PXA_FFUART_BASE,0xa0004000,rxCmd|16,0);feed(8,0x30);
        while(!line.in.empty())runFor(1);
        runFor(5);
        for(unsigned i=0;i<8;++i)assert(ram[0x4000+i]==0x30+i);
        assert((read(ddadr2+12)&0x1fff)==8 && ur(9)==0);
    }
    puts("PASS: a channel shared with a stale UART request mapping still moves data");
}
