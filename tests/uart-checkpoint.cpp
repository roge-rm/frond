// Exercise the production export guard with synthetic TX firmware and callback peers.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <vector>
#include "CPU.h"
#include "ReadOnlyGuest.h"
#include "pxa_UART.h"
#include "soc_PXA.h"

class TestSoc : public SocPXA {
 public:
    using SocPXA::SocPXA;
    bool failSave=false;
    bool Save() override {
        if(failSave){failSave=false;return false;}
        return SocPXA::Save();
    }
};
struct Peer {
    unsigned reads=0,ticks=0,active=0;
    std::deque<uint16_t> input;
    std::vector<uint16_t> output;
    void attach(PxaUart* uart) {
        pxaUartSetFuncs(uart,[](void* p)->uint_fast16_t {
            auto& peer=*static_cast<Peer*>(p);++peer.reads;
            if(peer.input.empty())return UART_CHAR_NONE;
            auto c=peer.input.front();peer.input.pop_front();return c;
        },[](uint_fast16_t c,void* p){static_cast<Peer*>(p)->output.push_back(c);},
        [](void* p){auto& peer=*static_cast<Peer*>(p);++peer.active;return !peer.input.empty();},
        [](void* p){++static_cast<Peer*>(p)->ticks;},this);
    }
};
int main() {
    std::vector<uint8_t> rom(16*1024*1024),nand(132*1024*1024,0xff);
    TestSoc soc(deviceTypeTX,32*1024*1024,rom.data(),rom.size(),nand.data(),nand.size(),-1,2);
    auto* cpu=soc.GetCpu();auto* uart=soc.GetSerialUart();assert(uart);
    const uint32_t branch=0xeafffffe;
    std::memcpy(static_cast<uint8_t*>(soc.GetMemoryData().data)+0x3000,&branch,4);
    cpuSetReg(cpu,15,0xa0003000);
    auto write=[&](unsigned reg,uint32_t value){assert(cpuMemOpExternal(cpu,&value,PXA_FFUART_BASE+4*reg,4,true));};
    auto read=[&](unsigned reg){uint32_t value=0;assert(cpuMemOpExternal(cpu,&value,PXA_FFUART_BASE+4*reg,4,false));return value;};
    auto run=[&](unsigned ms){for(unsigned i=0;i<ms;++i)assert(soc.Run(100000,100000000)>=100000);};
    write(3,0x83);write(0,96);write(1,0);write(3,3);write(4,8);write(1,0x41);write(2,7);
    Peer peer;peer.attach(uart);
    for(unsigned round=0;round<4;++round){
        write(0,0x40+round);peer.input={uint16_t(0x80+round)};
        const auto reads=peer.reads,ticks=peer.ticks,active=peer.active;
        const auto outputs=peer.output.size();
        const auto before=soc.GetTime();
        try {
            ReadOnlyGuest outer(soc);
            assert(!pxaUartTaskRequired(uart));run(10);
            write(0,0xee);write(2,7); // speculative FIFO mutations must be rolled back
            {
                SoC::SerialCheckpoint inner(soc);run(10);write(0,0xdd);
                assert(!pxaUartTaskRequired(uart));
            }
            run(10);
            assert(peer.reads==reads && peer.ticks==ticks && peer.active==active && peer.output.size()==outputs);
            if(round&1)throw std::runtime_error("simulated injected-call timeout");
        } catch(const std::runtime_error&) {assert(round&1);}
        assert(soc.GetTime()==before && peer.input.size()==1);
        assert(peer.reads==reads && peer.ticks==ticks && peer.active==active && peer.output.size()==outputs);
        run(20);
        assert(peer.output.size()==outputs+1 && peer.output.back()==0x40+round);
        assert(peer.input.empty() && read(0)==0x80+round);
        run(5);assert(peer.output.size()==outputs+1);
    }
    puts("PASS: real export guard with nested serial guards suppresses RX/TX/tick/activity callbacks on success and exception; resumes exactly once");
    write(0,0x55);soc.failSave=true;
    bool rejected=false;
    try{ReadOnlyGuest failed(soc);}catch(const std::runtime_error&){rejected=true;}
    assert(rejected && pxaUartTaskRequired(uart));
    const auto count=peer.output.size();run(20);
    assert(peer.output.size()==count+1 && peer.output.back()==0x55);
    puts("PASS: checkpoint construction failure releases UART guard and pending TX progresses");
    // Commit releases exactly its own freezes, retaining any outer owners.
    write(0,0x5a); peer.input={0x90};
    const auto committedAt=peer.output.size();
    {
        SoC::SerialCheckpoint serial(soc);
        NetworkSimulation::Checkpoint network(soc.networkSimulation);
        {
            ReadOnlyGuest checkpoint(soc);
            run(10);
            checkpoint.Commit(); checkpoint.Commit();
            assert(soc.networkSimulation.Frozen() && !pxaUartTaskRequired(uart));
        }
        run(10);
        assert(soc.networkSimulation.Frozen() && peer.output.size()==committedAt);
    }
    assert(!soc.networkSimulation.Frozen()); run(20);
    assert(peer.output.size()==committedAt+1 && peer.output.back()==0x5a);
    assert(peer.input.empty() && read(0)==0x90);
    { ReadOnlyGuest next(soc); next.Commit(); }
    puts("PASS: commit releases only its own UART/network freezes and resumes real peer I/O once");
    // Internal accessory callbacks intentionally survive restore. External live
    // ownership uses AttachExternal instead (covered by uart-dma-peer.cpp).
    write(0,0x7e);assert(soc.Save());auto state=soc.GetSavestate();
    std::vector<uint8_t> saved(static_cast<uint8_t*>(state.data),static_cast<uint8_t*>(state.data)+state.size);
    const auto sent=peer.output.size();run(20);assert(peer.output.size()==sent+1);
    assert(soc.Load(saved.size(),saved.data()));run(20);
    assert(peer.output.size()==sent+2 && peer.output[sent]==0x7e && peer.output[sent+1]==0x7e);
    // A rejected restore rolls its speculative state back without executing I/O.
    write(0,0x44);const auto rejectedAt=peer.output.size();
    assert(!soc.Load(saved.size()-4,saved.data()));assert(peer.output.size()==rejectedAt);
    run(20);assert(peer.output.size()==rejectedAt+1 && peer.output.back()==0x44);
    // Reset clears guest pending TX and timing, but still retains callback bindings.
    write(0,0x99);const auto resetAt=peer.output.size();soc.Reset();
    cpuSetReg(cpu,15,0xa0003000);run(5);assert(peer.output.size()==resetAt);
    write(3,0x83);write(0,96);write(1,0);write(3,3);write(1,0x41);write(2,7);
    write(0,0x66);run(20);assert(peer.output.size()==resetAt+1 && peer.output.back()==0x66);
    puts("PASS: rejected restore executes no TX; reset clears pending guest bytes and timing");
    puts("PASS: internal accessory bindings survive restore/reset; normal restore replays their saved guest TX");
}
