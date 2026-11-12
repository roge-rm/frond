#include "EmRegsISPDana.h"
#include <cstdio>
#include "EmCPU68K.h"
#include "EmHAL.h"
#include <algorithm>
#include <stdexcept>
#include <tuple>
#include "DanaUnqualified.h"

// Floating bus value for a refused read of this width.
static uint32 Floating(int size) { return size==1 ? 0xff : size==2 ? 0xffff : 0xffffffff; }

void EmRegsISPDana::CycleThunk(void* context, uint64, bool) {
    static_cast<EmRegsISPDana*>(context)->SettleLoadedPorts();
    static_cast<EmRegsISPDana*>(context)->Tick();
}
void EmRegsISPDana::Initialize() {
    EmRegs::Initialize();
    EmHAL::AddCycleConsumer(CycleThunk,this);
}
void EmRegsISPDana::Dispose() {
    EmHAL::RemoveCycleConsumer(CycleThunk,this);
    EmRegs::Dispose();
}

static bool KnownDeviceCommand(uint16 c) {
    return (c>=1 && c<=0x0f) || c==0x10 || (c>=0x12 && c<=0x5f) ||
        (c>=0x61 && c<=0x70) || (c>=0x72 && c<=0x8f) || (c>=0xa0 && c<=0xaf) ||
        c==0xb0 || c==0xb2 || c==0xb3 || (c>=0xb6 && c<=0xbb) ||
        c==0xc0 || c==0xc2 || c==0xc3 || (c>=0xd0 && c<=0xdf) || c==0xf4;
}

void EmRegsISPDana::Reset(Bool hardware) {
    HostCancelled();
    EmRegs::Reset(hardware);
    if (!hardware) return;
    lastCycle = CurrentCycle(); fractional=0; resetCycles=0;
    regs.fill(0);
    regs[0] = 0x10; // OHCI revision, not chip revision.
    regs[0x0d] = 0x2edf;
    regs[0x11] = 0x628;
    regs[0x12]=2; regs[0x20]=0x28;
    regs[0x27] = 0x6122; // Modelled datasheet revision; firmware checks only 0x61xx.
    command = 0; word = 0; latch = 0;
    hostArmed=false; hostTargets={}; loadedPorts={};
    hostAttached={}; hostLowSpeed={}; portResetCycles={};
    hostRam.fill(0); hostCursor=0; hostWritten=0;
    ResetDevice();
    RefreshHostInterrupt();
    for(auto& o:hostOwner) if(o.reattach) o.reattach();
}
void EmRegsISPDana::RefreshHostInterrupt() {
    if ((regs[4]&0x80000000) && (regs[3]&regs[4]&0x7f)) regs[0x24]|=0x10;
    bool active=(regs[0x20]&1) && (regs[0x24]&regs[0x25]&0x77);
    if(hostIrqSink) hostIrqSink(active ? bool(regs[0x20]&4) : !bool(regs[0x20]&4));
}
bool EmRegsISPDana::HostInterruptPending() {
    Tick(); RefreshHostInterrupt();
    return (regs[0x20]&1) && (regs[0x24]&regs[0x25]&0x77);
}
void EmRegsISPDana::SetHostConnection(unsigned index, bool attached, bool lowSpeed) {
    if(index>=2) throw std::runtime_error("ISP invalid root port");
    if(!attached) CancelHostWork(int(index));
    Tick(); hostAttached[index]=attached; hostLowSpeed[index]=attached && lowSpeed;
    auto& p=regs[0x15+index];
    auto before=p;
    bool connected=attached && (p&0x100);
    if(bool(p&1)!=connected) p|=0x10000;
    if(!connected) {
        CancelHostWork(int(index));
        if(p&2) p|=0x20000;
        p&=~0x217u; portResetCycles[index]=0;
    } else p=(p&~0x200u)|1|(lowSpeed?0x200:0);
    if(p!=before) regs[3]|=0x40;
    RefreshHostInterrupt();
}
void EmRegsISPDana::HostPortPower(unsigned index, bool on) {
    if(regs[0x12]&0x200) on=true;
    if(!on) CancelHostWork(int(index));
    auto& p=regs[0x15+index];
    auto before=p;
    if(on) p|=0x100; else p&=~0x100u;
    SetHostConnection(index,hostAttached[index],hostLowSpeed[index]);
    if(p!=before) regs[3]|=0x40;
    RefreshHostInterrupt();
}
void EmRegsISPDana::SetSubBankHandlers() {
    EmRegs::SetSubBankHandlers();
    SetHandler((ReadFunction)&EmRegsISPDana::Read,
               (WriteFunction)&EmRegsISPDana::Write, GetAddressStart(), 8);
}
uint64 EmRegsISPDana::CurrentCycle() const { return gCPU68K ? gCPU68K->CycleNow() : 0; }
uint64 EmRegsISPDana::ClockHz() const { return EmHAL::GetSystemClockFrequency(); }
void EmRegsISPDana::Tick() {
    uint64 now = CurrentCycle();
    if (now < lastCycle) { lastCycle=now; fractional=0; }
    uint64 delta=now-lastCycle; lastCycle=now;
    if (resetCycles) {
        if (delta>=resetCycles) { resetCycles=0; regs[2]&=~1u; }
        else resetCycles-=delta;
    }
    for(unsigned i=0;i<2;++i) if(portResetCycles[i]) {
        if(delta>=portResetCycles[i]) {
            portResetCycles[i]=0;
            regs[3]|=0x40; // Reset completion is a new event even with CSC still set.
            regs[0x15+i]=(regs[0x15+i]&~0x40014u)|0x100002;
        } else portResetCycles[i]-=delta;
    }
    RefreshHostInterrupt();
    if ((regs[1] & 0xc0)!=0x80) { fractional=0; return; }
    uint64 hz=ClockHz();
    if (!hz) throw std::runtime_error("ISP missing system clock");
    // 12 MHz USB bit times, with sub-bit phase in system-clock units.
    // Divide before multiplying so a long idle cannot overflow the conversion.
    uint64 bits=(delta/hz)*12000000;
    uint64 phase=(delta%hz)*12000000+fractional;
    bits+=phase/hz; fractional=phase%hz;
    uint64 remaining=regs[0x0e]&0x3fff;
    uint64 frames=0;
    if(remaining && bits>=remaining) regs[0x0e]=(regs[0x0e]&0x3fff)|(regs[0x0d]&0x80000000);
    if(bits>remaining) {
        bits-=remaining+1;
        uint64 interval=regs[0x0d]&0x3fff;
        frames=1+bits/(interval+1);
        remaining=interval-bits%(interval+1);
        if(bits>=interval) regs[0x0e]=(regs[0x0e]&0x3fff)|(regs[0x0d]&0x80000000);
    } else remaining-=bits;
    regs[0x0e]=(regs[0x0e]&0x80000000)|remaining;
    if (frames) {
        uint64 number=regs[0x0f]&0xffff;
        if(frames>=0x8000 || ((number&0x7fff)+frames)>=0x8000) regs[3]|=0x20;
        regs[0x0f]=(number+frames)&0xffff;
        regs[3]|=4; regs[0x24]|=1;
        // One bounded service pass, without replaying a backlog of external work.
        ExecuteHostFrame();
        RefreshHostInterrupt();
    }
    // A frame that can raise an interrupt (an armed ATL list, or SOF) wakes a sleeping CPU at its
    // boundary, so the interrupt isn't late. A late one leaves the guest's handler running into the
    // next frame, which completes the list it just armed, and its clear then loses that interrupt.
    if((regs[0x20]&1) && ((hostArmed && (regs[0x2c]&4) && (regs[0x25]&2)) || (regs[0x25]&1))) {
        const uint64 phase=(remaining+1)*hz;
        const uint64 needed=phase>fractional ? phase-fractional : 1;
        EmHAL::WakeAt(now+(needed+11999999)/12000000,now);
    }
}
uint32 EmRegsISPDana::Read(emuptr address, int size) {
    SettleLoadedPorts();
    Tick();
    if (size==2 && address==GetAddressStart()+4) return DeviceRead();
    if (size != 2 || address != GetAddressStart() || (command & 0x80)) {
        DanaUnqualified("ISP unsupported bus read", "%d bytes at 0x%08x, command 0x%02x",
                        size, unsigned(address), unsigned(command));
        return Floating(size);
    }
    if(command==0x41) return HostWord(false);
    unsigned index = command;
    // Only disconnected-controller registers are implemented.
    switch (index) {
        case 0: case 1: case 2: case 3: case 4: case 5: case 0x0d: case 0x0e:
        case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x0f: case 0x27: case 0x11: case 0x20: case 0x24: case 0x25: case 0x28:
        case 0x22: case 0x2a: case 0x2b: case 0x2c: break;
        default:
            DanaUnqualified("ISP unqualified register read", "register 0x%02x", index);
            return 0xffff;
    }
    uint32 value = regs[index == 5 ? 4 : index];
    if(index<0x20) {
        if(!word) latch=value;
        value=latch;
    }
    uint32 result = (value >> (16 * word)) & 0xffff;
    word = index < 0x20 ? (word + 1) % 2 : 0;
    if(!word) latch=0;
    return result;
}
void EmRegsISPDana::Write(emuptr address, int size, uint32 value) {
    SettleLoadedPorts();
    if (size==2 && address==GetAddressStart()+6) {
        DeviceCommand(value & 0xff);
        return;
    }
    if (size==2 && address==GetAddressStart()+4) { DeviceWrite(value); return; }
    if (size != 2 || (address != GetAddressStart() && address != GetAddressStart()+2)) {
        DanaUnqualified("ISP unsupported bus write", "%d bytes at 0x%08x, value 0x%x",
                        size, unsigned(address), unsigned(value));
        return;
    }
    if (address == GetAddressStart()+2) {
        if(value==0x41 || value==0xc1) {
            // The command is taken so its data words reach the ATL port, which drops them.
            if(!regs[0x22] || regs[0x22]>regs[0x2b])
                DanaUnqualified("ISP ATL count outside allocation", "count %u, ATL buffer %u",
                                unsigned(regs[0x22]), unsigned(regs[0x2b]));
            hostCursor=0;
            if(value==0xc1) { hostRam.fill(0); hostArmed=false; hostWritten=0; }
        }
        command = value; word = 0; latch = 0;
        return;
    }
    if (!(command & 0x80)) {
        DanaUnqualified("ISP write to read command", "command 0x%02x, value 0x%04x", unsigned(command), unsigned(value));
        return;
    }
    if(command==0xc1) { HostWord(true,value); return; }
    latch |= (value & 0xffff) << (16 * word);
    if (command < 0xa0 && word++ == 0) return;
    uint32 complete=latch; word=0; latch=0; Commit(complete);
}
void EmRegsISPDana::Commit(uint32 value) {
    Tick();
    switch (command) {
        case 0x81:
            if((regs[1]&0xc0)!=0x80 && (value&0xc0)==0x80) {
                fractional=0;
                regs[0x0e]=regs[0x0d]&0x80003fff;
                regs[0x0f]=(regs[0x0f]+1)&0xffff;
                regs[3]|=4;
                if(!(regs[0x0f]&0x7fff)) regs[3]|=0x20;
            }
            regs[1] = value & 0x7ff; break;
        case 0x82:
            if (value&1) {
                CancelHostWork();
                for(unsigned i=1;i<0x12;++i) regs[i]=0;
                regs[0x0d]=0x2edf; regs[0x11]=0x628; regs[2]=1;
                portResetCycles={};
                for(unsigned i=0;i<2;++i) regs[0x15+i]&=~0x16u;
                fractional=0; resetCycles=(ClockHz()+99999)/100000;
            }
            break;
        case 0x83: regs[3] &= ~value; break;
        case 0x84: regs[4] |= value & 0xc000007f; break;
        case 0x85: regs[4] &= ~value; break;
        case 0x8d: regs[0x0d] = value & 0xffff3fff; break;
        case 0x91: regs[0x11] = value & 0x7ff; break;
        case 0x92:
            regs[0x12]=(value & 0xff001b00)|2;
            if(regs[0x12]&0x200) for(unsigned i=0;i<2;++i) HostPortPower(i,true);
            break;
        case 0x93: regs[0x13]=value & 0xffff0000; break;
        case 0x94:
            for (unsigned port=0; port<2; ++port) {
                bool perPort=(regs[0x12]&0x100)!=0;
                bool masked=(regs[0x13]&(1u<<(17+port)))!=0;
                if (!perPort || !masked) {
                    if(value&1) HostPortPower(port,false);
                    if(value&0x10000) HostPortPower(port,true);
                }
            }
            if(value&0x8000) regs[0x14]|=0x8000;
            if(value&0x80000000) regs[0x14]&=~0x8000;
            regs[0x14]&=~(value&0x20000);
            break;
        case 0x95: case 0x96: {
            unsigned index=(command&0x7f)-0x15;
            auto& port=regs[0x15+index];
            // SetPortSuspend on a disconnected port only sets CSC (handled below).
            if(((value&4) && (port&1)) || ((value&8) && (port&4))) {
                DanaUnqualified("ISP host suspend/resume", "port %u status write 0x%08x", index, unsigned(value));
                return;
            }
            port &= ~(value & 0x1f0000);
            auto before=port;
            bool controlled=(regs[0x12]&0x100) && (regs[0x13]&(1u<<(17+index)));
            if(controlled) {
                if(value&0x200) HostPortPower(index,false);
                if(value&0x100) HostPortPower(index,true);
            }
            if(value&1) { CancelHostWork(int(index)); port&=~2u; }
            if(!(port&1)) { if(value&0x16) port|=0x10000; }
            else {
                if(value&2) port|=2;
                if(value&0x10) {
                    CancelHostWork(int(index));
                    port|=0x10;
                    portResetCycles[index]=(ClockHz()+99)/100;
                    if(hostOwner[index].portReset) hostOwner[index].portReset();
                }
            }
            if(port!=before) regs[3]|=0x40;
            break;
        }
        case 0xa0:
            if(value&2) {
                DanaUnqualified("ISP HC edge interrupt mode", "HcHardwareConfiguration 0x%04x", unsigned(value));
                return;
            }
            regs[0x20] = (value & 0x1fe7) | 8; break;
        case 0xa2:
            if(value>4096) {
                DanaUnqualified("ISP invalid transfer count", "count %u", unsigned(value));
                return;
            }
            regs[0x22]=value; hostCursor=0; break;
        case 0xa4: regs[0x24] &= ~(value & 0x77); break;
        case 0xa5: regs[0x25] = value & 0x77; break;
        case 0xa8: regs[0x28] = value; break;
        case 0xa9:
            if (value==0xf6) {
                // HC reset cancels transfers without unplugging external devices.
                CancelHostWork();
                hostCursor=0; hostWritten=0; portResetCycles={};
                fractional=0; resetCycles=0;
                regs.fill(0); regs[0]=0x10; regs[0x0d]=0x2edf;
                regs[0x11]=0x628; regs[0x27]=0x6122; regs[0x12]=2; regs[0x20]=0x28;
            }
            break;
        case 0xaa: case 0xab:
            if (value > 4096 || (value & 3) ||
                (command==0xaa ? 2*value+regs[0x2b] : 2*regs[0x2a]+value)>4096) {
                DanaUnqualified("ISP invalid buffer length", "command 0x%02x, length %u", unsigned(command), unsigned(value));
                return;
            }
            regs[command & 0x7f] = value; hostArmed=false; hostCursor=0; hostWritten=0; regs[0x2c]=0; break;
        default:
            DanaUnqualified("ISP unqualified register write", "command 0x%02x, value 0x%08x", unsigned(command), unsigned(value));
            return;
    }
    RefreshHostInterrupt();
}

void EmRegsISPDana::ResetDevice() {
    dcCommand=dcMode=dcHardware=dcScratch=dcAddress=0;
    dcUnlocked=true; dcSuspended=false;
    dcInterrupt=dcInterruptEnable=dcLatch=0; dcWord=0;
    dcEndpoints.fill(0);
    // Control FIFOs are fixed 64-byte single buffers, enabled after reset.
    dcEndpoints[0]=0x83; dcEndpoints[1]=0xc3;
    fifo={}; allocated=false; configNext=cursor=0; setupPending=false; effectiveAddress=0; deviceEnabled=false;
    flushed=0; accessCancelled=false;
    RefreshIrq();
}

uint32 EmRegsISPDana::DeviceRead() {
    if(!dcUnlocked) {
        DanaUnqualified("ISP DC locked read", "command 0x%02x", unsigned(dcCommand));
        return 0xffff;
    }
    unsigned e=dcCommand&15;
    if(dcCommand>=0x10 && dcCommand<=0x1f) {
        if(accessCancelled) return 0;
        auto& ep=fifo[e]; auto& b=ep.packet[ep.cpu];
        if(!b.full || cursor> (b.length+1)/2) {
            DanaUnqualified("ISP DC FIFO read boundary", "endpoint %u, word %u", e, cursor);
            return 0xffff;
        }
        if(cursor++==0) return b.length;
        return b.data[cursor-2];
    }
    if((dcCommand>=0x50 && dcCommand<=0x5f) || (dcCommand>=0xd0 && dcCommand<=0xdf))
        return EndpointStatus(e,dcCommand<0x60);
    if(dcCommand>=0xa0 && dcCommand<=0xaf) {
        if(allocated && (fifo[e].packet[fifo[e].cpu].full || fifo[e].statusPending))
            flushed &= ~(1u<<e);
        auto result=fifo[e].error; fifo[e].error &= ~0x80; fifo[e].statusPending=false; return result;
    }
    if (dcCommand>=0x30 && dcCommand<=0x3f) return dcEndpoints[dcCommand-0x30];
    switch (dcCommand) {
        case 0xc0: {
            uint32 result = (dcInterrupt >> (16*dcWord)) & 0xffff;
            flushed &= ~((result << (16*dcWord)) >> 8);
            // Read-to-clear applies to the returned word, never unseen endpoint bits.
            dcInterrupt &= ~(uint32(0x5b) & (0xffffu << (16*dcWord)));
            dcWord=(dcWord+1)%2;
            RefreshIrq(); return result;
        }
        case 0xc3: { uint32 value=(dcInterruptEnable >> (16*dcWord)) & 0xffff;
            dcWord=(dcWord+1)%2; return value; }
        case 0xb3: return dcScratch;
        case 0xb7: return dcAddress;
        case 0xb9: return dcMode;
        case 0xbb: return dcHardware;
        default:
            DanaUnqualified("ISP DC unqualified read", "command 0x%02x", unsigned(dcCommand));
            return 0xffff;
    }
}
void EmRegsISPDana::DeviceWrite(uint16 value) {
    if (dcCommand==0xb0) { dcUnlocked=(value==0xaa37); return; }
    if (!dcUnlocked) {
        DanaUnqualified("ISP DC locked write", "command 0x%02x, value 0x%04x", unsigned(dcCommand), unsigned(value));
        return;
    }
    if (dcCommand>=0x20 && dcCommand<=0x2f) {
        unsigned endpoint=dcCommand-0x20;
        value &= 0xff;
        if((endpoint<2 && value!=(endpoint ? 0xc3 : 0x83)) ||
           (endpoint>=2 && ((value&0x10) || (value&15)>3))) {
            DanaUnqualified("ISP DC unsupported ECR", "endpoint %u, value 0x%02x", endpoint, unsigned(value));
            return;
        }
        auto old=dcEndpoints[endpoint]; dcEndpoints[endpoint]=value;
        unsigned next=endpoint==configNext ? configNext+1 : (endpoint==0 ? 1 : 0);
        if(next==16) {
            try { ValidateConfiguration(); }
            catch(const std::runtime_error& error) {
                dcEndpoints[endpoint]=old;
                DanaUnqualified("ISP DC FIFO configuration", "%s", error.what());
                return;
            }
        }
        flushed=0;
        for(auto& epoch:packetResetEpoch) ++epoch;
        fifo={}; allocated=false; setupPending=false; cursor=0; dcInterrupt&=0xff;
        RefreshIrq();
        configNext=next;
        if(configNext==16) { allocated=true; configNext=0; }
        return;
    }
    if(dcCommand>=1 && dcCommand<=15) {
        if(accessCancelled) return;
        auto& ep=fifo[dcCommand]; auto& b=ep.packet[ep.cpu];
        if(b.full) {
            DanaUnqualified("ISP DC full IN FIFO", "endpoint %u", unsigned(dcCommand));
            return;
        }
        if(cursor==0) {
            if(value>Capacity(dcCommand)) {
                DanaUnqualified("ISP DC packet length", "endpoint %u, length %u", unsigned(dcCommand), unsigned(value));
                return;
            }
            b={}; b.writing=true; b.length=value; cursor=1; return;
        }
        if(cursor>(b.length+1)/2) {
            DanaUnqualified("ISP DC FIFO write boundary", "endpoint %u, word %u", unsigned(dcCommand), cursor);
            return;
        }
        b.data[cursor-1]=value; ++cursor; b.words=cursor-1; return;
    }
    switch (dcCommand) {
        case 0xc2:
            dcLatch |= uint32(value) << (16*dcWord);
            if(dcWord++==0) return;
            dcInterruptEnable=dcLatch & 0x00ffff7f;
            dcWord=0; dcLatch=0;
            break;
        case 0xb2: dcScratch=value; break;
        case 0xb6: dcAddress=value & 0xff; deviceEnabled=(value&128)!=0; break;
        case 0xb8:
            if ((dcMode & 0x20) && !(value & 0x20)) dcSuspended=true;
            dcMode=value & 0xad; break;
        case 0xba:
            if(value&(2|0x4000)) {
                DanaUnqualified("ISP DC unsupported IRQ/pull-up mode", "hardware configuration 0x%04x", unsigned(value));
                return;
            }
            dcHardware=value; break;
        default:
            DanaUnqualified("ISP DC unqualified write", "command 0x%02x, value 0x%04x", unsigned(dcCommand), unsigned(value));
            return;
    }
    RefreshIrq();
}

#include "savestate/ChunkHelper.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"

template <typename T> void EmRegsISPDana::StateFields(T& h) { h.Do64(lastCycle).Do64(fractional).Do16(command).Do32(word).Do32(latch)
        .Do16(dcCommand).Do16(dcMode).Do16(dcHardware).Do16(dcScratch)
        .DoBool(dcUnlocked).DoBool(dcSuspended).Do32(dcInterrupt)
        .Do32(dcInterruptEnable).Do32(dcLatch).Do32(dcWord).Do64(resetCycles);
    for (auto& value : regs) h.Do32(value); }
void EmRegsISPDana::Save(Savestate<ChunkType>& s) { SaveState(s); }
void EmRegsISPDana::Save(SavestateProbe<ChunkType>& s) { SaveState(s); }
template <typename T> void EmRegsISPDana::SaveState(T& s) {

    auto* c=s.GetChunk(ChunkType::regsISPDana); if(!c) return;
    c->Put32(10); SaveChunkHelper h(*c); StateFields(h);
    h.Do16(dcAddress);
    for (auto& value : dcEndpoints) h.Do16(value);
    PacketFields(h); CancellationFields(h); HostFields(h);
    for(auto& cycles:portResetCycles) h.Do64(cycles);
    h.Do32(hostWritten);
}
void EmRegsISPDana::Load(SavestateLoader<ChunkType>& l) {

    auto* c=l.GetChunk(ChunkType::regsISPDana);
    if(!c) { l.NotifyError(); return; }
    uint32 version=c->Get32();
    if(version!=2 && version!=3 && version!=4 && version!=5 && version!=6 && version!=7 && version!=8 && version!=9 && version!=10) { l.NotifyError(); return; }
    auto fields=std::tie(lastCycle,fractional,resetCycles,regs,command,word,latch,dcEndpoints,dcAddress,dcCommand,dcMode,dcHardware,dcScratch,dcUnlocked,dcSuspended,dcInterrupt,dcInterruptEnable,dcLatch,dcWord,fifo,configNext,cursor,allocated,setupPending,effectiveAddress,vbus,deviceEnabled,flushed,accessCancelled,hostRam,hostCursor,portResetCycles,hostWritten);
    auto before=std::make_tuple(lastCycle,fractional,resetCycles,regs,command,word,latch,dcEndpoints,dcAddress,dcCommand,dcMode,dcHardware,dcScratch,dcUnlocked,dcSuspended,dcInterrupt,dcInterruptEnable,dcLatch,dcWord,fifo,configNext,cursor,allocated,setupPending,effectiveAddress,vbus,deviceEnabled,flushed,accessCancelled,hostRam,hostCursor,portResetCycles,hostWritten);
    bool invalid=false;
    LoadChunkHelper h(*c); StateFields(h);
    if(version<9) {
        uint64 hz=ClockHz();
        if(!hz || fractional>=hz) invalid=true;
        else {
            uint64 phase=fractional*12000;
            uint64 elapsed=phase/hz;
            fractional=phase%hz;
            regs[0x0e]=(regs[1]&0xc0)==0x80 ? 11999-elapsed : 0;
        }
        if(word && !(command&0x80) && command<0x20)
            latch=regs[command==5?4:command];
    }
    if(regs[0x0e]&~0x80003fffu) invalid=true;
    if(regs[0x20]&2) invalid=true;
    dcAddress=0; dcEndpoints.fill(0); dcEndpoints[0]=0x83; dcEndpoints[1]=0xc3;
    fifo={}; configNext=cursor=0; allocated=setupPending=false; effectiveAddress=0; deviceEnabled=false;
    if(version>=3) {
        h.Do16(dcAddress);
        for (auto& value : dcEndpoints) h.Do16(value);
    }
    flushed=0; accessCancelled=false;
    if(version>=4) PacketFields(h);
    hostRam.fill(0); hostCursor=0; hostWritten=0;

    if(cursor>33) invalid=true;
    if(version>=5) CancellationFields(h);
    else {
        // v2/v3 have no packet fields; v4 has no cancellation or write ownership.
        for(auto& ep:fifo) { ep.statusPending=bool(ep.error&0x80); ep.error &= ~0x80; }
        // Old control accesses cannot distinguish a fresh SETUP cursor from a stale one.
        // Cancel either partial access and leave the packet for a fresh command.
        if(version==4 && ((dcCommand>=1 && dcCommand<=0x10) || (dcCommand>=0x12 && dcCommand<=0x1f))) {
            unsigned e=dcCommand&15;
            if(fifo[e].cpu<=1 && (!allocated || !(dcEndpoints[e]&128) ||
               (dcCommand>=0x10 && !fifo[e].packet[fifo[e].cpu].full) ||
               (e<2 && setupPending && cursor) ||
               (dcCommand<0x10 && cursor && !fifo[e].packet[fifo[e].cpu].length &&
                !fifo[e].packet[fifo[e].cpu].words && !fifo[e].packet[fifo[e].cpu].full))) {
                flushed |= 1u<<e; accessCancelled=true; cursor=0;
            }
        }
        if(version==4 && !accessCancelled && dcCommand>=1 && dcCommand<=15 && cursor && fifo[dcCommand].cpu<=1)
            fifo[dcCommand].packet[fifo[dcCommand].cpu].writing=true;
    }
    if(version>=6) HostFields(h);
    portResetCycles={};
    if(version>=7) for(auto& cycles:portResetCycles) h.Do64(cycles);
    // Legacy images used the transfer counter as the submitted extent.
    hostWritten=version>=6 ? (command==0xc1 ? std::min<unsigned>(hostCursor,regs[0x22]) : std::min<unsigned>(regs[0x22],regs[0x2b])) : 0;
    if(version>=10) h.Do32(hostWritten);
    if(hostWritten>regs[0x2b] ||
       (version>=10 && command==0xc1 && hostWritten!=std::min<unsigned>(hostCursor,regs[0x22]))) invalid=true;
    for(unsigned i=0;i<2;++i) {
        auto p=regs[0x15+i];
        if(portResetCycles[i]>(ClockHz()+99)/100 ||
           bool(portResetCycles[i])!=bool(p&0x10) ||
           ((p&0x217) && !(p&0x100)) || ((p&0x216) && !(p&1))) invalid=true;
    }
    if(regs[0x22]>4096 || hostCursor>((regs[0x22]+1)&~1u) || (hostCursor&1) ||
       (hostCursor && ((regs[0x22]+1)&~1u)>regs[0x2b]) || (regs[0x2c]&~36u)) invalid=true;
    if(flushed>0xffff || (accessCancelled &&
       (!((dcCommand>=1 && dcCommand<=0x10) || (dcCommand>=0x12 && dcCommand<=0x1f)) ||
        !(flushed&(1u<<(dcCommand&15))) || cursor))) invalid=true;
    if(setupPending && !fifo[0].setup) invalid=true;
    if(dcCommand && !KnownDeviceCommand(dcCommand)) invalid=true;
    bool packetCommand=(dcCommand>=1 && dcCommand<=0x10) || (dcCommand>=0x12 && dcCommand<=0x1f);
    if(version>=4 && packetCommand) {
        unsigned e=dcCommand&15;
        if(accessCancelled!=bool(flushed&(1u<<e)) ||
           ((dcCommand<0x10)!=bool(dcEndpoints[e]&64))) invalid=true;
        if(!accessCancelled && (!allocated || !(dcEndpoints[e]&128) ||
           ((dcCommand<0x10)!=bool(dcEndpoints[e]&64)) ||
           (dcCommand>=0x10 && fifo[e].cpu<=1 && !fifo[e].packet[fifo[e].cpu].full))) invalid=true;
    }
    if(cursor && !((dcCommand>=1 && dcCommand<=15) || dcCommand==0x10 || (dcCommand>=0x12 && dcCommand<=0x1f))) invalid=true;
    if(configNext>15 || cursor>33 || effectiveAddress>127 || (allocated && configNext)) invalid=true;
    try { if(allocated) ValidateConfiguration(); } catch(...) { invalid=true; }
    for(auto value:dcEndpoints) if((value&0x10) || (value&15)>3) invalid=true;
    for(unsigned i=0;i<16;++i) {
        auto& ep=fifo[i];
        if(ep.cpu>1 || ep.bus>1 || (!(dcEndpoints[i]&32) && (ep.cpu || ep.bus)) ||
           (i>0 && (ep.setup || ep.overwrite)) ||
           ep.error>255 || (ep.offered && (!(dcEndpoints[i]&64) || !ep.packet[ep.bus].full))) invalid=true;
        for(auto& b:ep.packet) if((b.writing && (b.full || !allocated || !(dcEndpoints[i]&128) || !(dcEndpoints[i]&64) || (flushed&(1u<<i)))) || b.length>Capacity(i) || b.words>32 ||
            b.words>(b.length+1)/2 || (b.full && b.words!=(b.length+1)/2) ||
            ((!allocated || !(dcEndpoints[i]&128)) && (b.full || b.length || b.words))) invalid=true;
    }
    // A saved controller never restores an external host connection.
    vbus=false; for(auto& ep:fifo) ep.offered=false;
    if(dcAddress>0xff || dcEndpoints[0]!=0x83 || dcEndpoints[1]!=0xc3)
        invalid=true;
    for(auto value : dcEndpoints) if(value>0xff) invalid=true;
    if((regs[0x15]&~0x1f031fu) || (regs[0x16]&~0x1f031fu) || resetCycles > (ClockHz()+99999)/100000 || command > 0xff || dcCommand > 0xff || word > 1 || dcWord > 1 ||
       (command >= 0xa0 && word) || (word == 0 && latch) ||
       (word == 1 && (command & 0x80) && (latch & 0xffff0000)) ||
       regs[0x2a] > 4096 || regs[0x2b] > 4096 ||
       (regs[0x2a] & 3) || (regs[0x2b] & 3) ||
       2*regs[0x2a] + regs[0x2b] > 4096 ||
       (dcLatch && (dcCommand != 0xc2 || dcWord != 1 || (dcLatch & 0xffff0000))) ||
       (dcWord && dcCommand != 0xc0 && dcCommand != 0xc3 && dcCommand != 0xc2) ||
       (ClockHz() && fractional >= ClockHz())) invalid=true;
    if(cursor && dcCommand<=0x1f) {
        unsigned e=dcCommand&15; auto& ep=fifo[e];
        if(ep.cpu<=1) {
            auto& b=ep.packet[ep.cpu];
            if(!allocated || !(dcEndpoints[e]&128) ||
               ((dcCommand<0x10) != bool(dcEndpoints[e]&64)) ||
               (dcCommand>=0x10 ? !b.full : !b.writing) ||
               cursor>1+(b.length+1)/2 ||
               (dcCommand<0x10 && b.words!=cursor-1)) invalid=true;
        }
    }
    if(c->HasError() || !c->IsAtEnd()) invalid=true;
    if(invalid) { fields=before; l.NotifyError(); }
    if(!invalid) {
        HostCancelled();
        // An armed list still runs at the next frame, or completes with errors if its port goes.
        hostArmed=(regs[0x2c]&36)==4; hostTargets={};
        hostAttached={}; hostLowSpeed={};
        for(unsigned i=0;i<2;++i) {
            auto& p=regs[0x15+i];
            loadedPorts[i]=p&1;
            if(!loadedPorts[i]) {
                portResetCycles[i]=0;
                if((regs[0x12]&0x200) && !(p&0x100)) { p|=0x100; regs[3]|=0x40; }
            }
        }
        RefreshHostInterrupt();
    }
    RefreshIrq();
}

unsigned EmRegsISPDana::Capacity(unsigned e) const { return 8u << (dcEndpoints[e]&3); }
void EmRegsISPDana::ValidateConfiguration() const {
    unsigned total=0;
    for(unsigned i=0;i<16;++i) {
        auto c=dcEndpoints[i];
        if((c&0x10) || (c&15)>3) throw std::runtime_error("ISP DC unsupported FIFO profile");
        if(c&0x80) total+=Capacity(i)*((c&32)?2:1);
    }
    if(total>512) throw std::runtime_error("ISP DC noniso FIFO budget");
}
void EmRegsISPDana::RefreshIrq() {
    bool active=(dcMode&8) && (dcInterrupt&dcInterruptEnable);
    if(irqSink) irqSink(active ? bool(dcHardware&1) : !bool(dcHardware&1));
}
void EmRegsISPDana::DeviceCommand(uint16 c) {
    if(c==0xf6) { ResetDevice(); return; }
    bool known=KnownDeviceCommand(c);
    // A refused command leaves the previous one selected.
    if(!known) { DanaUnqualified("ISP DC unsupported command", "command 0x%02x", unsigned(c)); return; }
    if(!dcUnlocked && c!=0xb0) { DanaUnqualified("ISP DC locked command", "command 0x%02x", unsigned(c)); return; }
    unsigned e=c&15;
    bool writePacket=c>=1 && c<=15, readPacket=c>=0x10 && c<=0x1f;
    bool validate=c>=0x61 && c<=0x6f, clear=c>=0x70 && c<=0x7f;
    bool ignored=false;
    if(writePacket || readPacket || validate || clear) {
        bool in=dcEndpoints[e]&64;
        if(((writePacket || validate)!=in)) {
            DanaUnqualified("ISP DC wrong FIFO direction", "command 0x%02x", unsigned(c));
            return;
        }
        if(!allocated || !(dcEndpoints[e]&128)) {
            if(!(flushed&(1u<<e))) {
                DanaUnqualified("ISP DC unavailable FIFO command", "command 0x%02x", unsigned(c));
                return;
            }
            ignored=true;
        }
        if(readPacket && (flushed&(1u<<e)) && !fifo[e].packet[fifo[e].cpu].full) ignored=true;
        if(readPacket && !ignored && !fifo[e].packet[fifo[e].cpu].full) {
            DanaUnqualified("ISP DC empty OUT FIFO", "command 0x%02x", unsigned(c));
            return;
        }
        if((validate || clear) && ((flushed&(1u<<e)) || (e<2 && setupPending))) ignored=true;
    }
    if((validate || clear) && !ignored) {
        auto& ep=fifo[e]; auto& b=ep.packet[ep.cpu];
        if(validate) {
            if(b.full || !b.writing || b.words!=(b.length+1)/2) {
                DanaUnqualified("ISP DC incomplete IN packet", "command 0x%02x", unsigned(c));
                return;
            }
            b.full=true; b.writing=false;
        } else { b={}; ep.setup=false; }
        if(dcEndpoints[e]&32) ep.cpu^=1;
    } else if(c>=0x40 && c<=0x4f) fifo[e].stalled=true;
    else if(c>=0x80 && c<=0x8f) { flushed &= ~(1u<<e); fifo[e]={}; ++packetResetEpoch[e]; }
    else if(c==0xf4) setupPending=false;
    dcCommand=c; dcWord=0; dcLatch=0; cursor=0; accessCancelled=false;
    if(writePacket || readPacket) {
        accessCancelled=ignored;
        if(!ignored) flushed &= ~(1u<<e);
    }
    RefreshIrq();
}
uint16 EmRegsISPDana::EndpointStatus(unsigned e,bool clear) {
    auto& ep=fifo[e];
    // An empty flushed endpoint's status may still be read by a stale ISR.
    if(allocated && (ep.packet[ep.cpu].full || ep.statusPending)) flushed &= ~(1u<<e);
    uint16 result=(ep.stalled?128:0)|(ep.packet[1].full?64:0)|(ep.packet[0].full?32:0)|
        (ep.toggle?16:0)|(ep.overwrite?8:0)|(ep.setup?4:0)|(ep.cpu?2:0);
    if(clear) { dcInterrupt &= ~(1u<<(8+e)); ep.overwrite=false; RefreshIrq(); }
    return result;
}
void EmRegsISPDana::SetVbus(bool connected) {
    if(vbus==connected) return;
    vbus=connected;
    // External transactions are discarded at a cable boundary.
    for(unsigned e=0;e<16;++e) CancelAccess(e);
    fifo={}; setupPending=false; cursor=0; effectiveAddress=0;
    dcInterrupt=0; RefreshIrq();
}
void EmRegsISPDana::BusReset() {
    if(!DeviceAttached()) return;
    for(unsigned e=0;e<16;++e) CancelAccess(e);
    fifo={}; allocated=false; configNext=cursor=0; setupPending=false; effectiveAddress=0; deviceEnabled=false;
    for(unsigned i=2;i<16;++i) dcEndpoints[i]&=~128;
    deviceEnabled=true; dcUnlocked=true; dcInterrupt=1; RefreshIrq();
}
EmRegsISPDana::PacketResult EmRegsISPDana::HostOut(unsigned e,const std::vector<uint8>& bytes,bool data1,bool setup) {
    if(e>=16 || (dcEndpoints[e]&64) || bytes.size()>Capacity(e) || (setup && (e || bytes.size()!=8 || data1)))
        throw std::runtime_error("ISP DC invalid host OUT");
    if(!DeviceAttached() || !deviceEnabled || !allocated || !(dcEndpoints[e]&128)) return PacketResult::noAnswer;
    auto& ep=fifo[e];
    if(setup) {
        bool overwritten=setupPending; CancelAccess(0); CancelAccess(1); fifo[0]={}; fifo[1]={}; ep.overwrite=overwritten;
        ep.setup=true; setupPending=true; ep.toggle=false; fifo[1].toggle=true;
    } else if(ep.stalled) { Transaction(e,20); return PacketResult::stall; }
    if(!setup && e==0 && setupPending) { Transaction(e,18); return PacketResult::nak; }
    auto& b=ep.packet[ep.bus];
    if(data1!=ep.toggle) { Transaction(e,30); return PacketResult::ack; }
    if(b.full) { Transaction(e,18); return PacketResult::nak; }
    b={}; b.full=true; b.length=bytes.size(); b.words=(bytes.size()+1)/2;
    for(unsigned i=0;i<bytes.size();++i) b.data[i/2]|=uint16(bytes[i])<<((i&1)*8);
    Transaction(e,1|(data1?64:0)); ep.toggle=!ep.toggle;
    if(dcEndpoints[e]&32) ep.bus^=1;
    dcInterrupt|=1u<<(8+e); RefreshIrq(); return PacketResult::ack;
}
EmRegsISPDana::PacketResult EmRegsISPDana::HostIn(unsigned e,std::vector<uint8>& bytes,bool& data1) {
    if(e>=16 || !(dcEndpoints[e]&64)) throw std::runtime_error("ISP DC invalid host IN");
    auto& ep=fifo[e];
    if(!DeviceAttached() || !deviceEnabled || !allocated || !(dcEndpoints[e]&128)) return PacketResult::noAnswer;
    if(ep.stalled) { Transaction(e,20); return PacketResult::stall; }
    auto& b=ep.packet[ep.bus];
    if(!b.full || (e==1 && setupPending)) { Transaction(e,18); return PacketResult::nak; }
    bytes.resize(b.length);
    for(unsigned i=0;i<b.length;++i) bytes[i]=b.data[i/2]>>((i&1)*8);
    data1=ep.toggle; ep.offered=true; return PacketResult::ack;
}
EmRegsISPDana::PacketResult EmRegsISPDana::PeekHostIn(unsigned e,std::vector<uint8>& bytes,bool& data1) const {
    if(!PacketIsIn(e) || !DeviceAttached() || !deviceEnabled || !allocated || !(dcEndpoints[e]&128))
        return PacketResult::noAnswer;
    const auto& ep=fifo[e]; const auto& b=ep.packet[ep.bus];
    if(ep.stalled) return PacketResult::stall;
    if(!b.full || (e==1 && setupPending)) return PacketResult::nak;
    bytes.resize(b.length);
    for(unsigned i=0;i<b.length;++i) bytes[i]=b.data[i/2]>>((i&1)*8);
    data1=ep.toggle; return PacketResult::ack;
}
bool EmRegsISPDana::TryHostAckIn(unsigned e) {
    if(e>=16 || !fifo[e].offered) return false;
    HostAckIn(e); return true;
}
void EmRegsISPDana::HostAckIn(unsigned e) {
    if(e>=16 || !fifo[e].offered) throw std::runtime_error("ISP DC unoffered host ACK");
    auto& ep=fifo[e]; auto& b=ep.packet[ep.bus];
    if(e==1 && b.length==0) effectiveAddress=dcAddress&127;
    Transaction(e,1|(ep.toggle?64:0)); ep.toggle=!ep.toggle; ep.offered=false; b={};
    if(dcEndpoints[e]&32) ep.bus^=1;
    dcInterrupt|=1u<<(8+e); RefreshIrq();
}
template <typename T> void EmRegsISPDana::PacketFields(T& h) {
    h.Do32(configNext).Do32(cursor).DoBool(allocated).DoBool(setupPending).Do16(effectiveAddress);
    for(auto& ep:fifo) {
        h.Do16(ep.cpu).Do16(ep.bus).Do16(ep.error).DoBool(ep.stalled).DoBool(ep.toggle)
            .DoBool(ep.setup).DoBool(ep.overwrite).DoBool(ep.offered);
        for(auto& b:ep.packet) {
            h.Do16(b.length).Do16(b.words).DoBool(b.full);
            for(auto& value:b.data) h.Do16(value);
        }
    }
    h.DoBool(deviceEnabled);
}

void EmRegsISPDana::Transaction(unsigned e,uint16 status) {
    auto& ep=fifo[e];
    uint16 data01=(status&1) ? (status&64) : (ep.error&64);
    ep.error=(ep.statusPending?0x80:0)|data01|(status&~64);
    ep.statusPending=true;
    if((status&1) || (dcMode&4)) dcInterrupt|=1u<<(8+e);
    RefreshIrq();
}

// The datasheet leaves FIFO overruns unpredictable. Host flushes cancel the CPU
// access deterministically: zero reads and dropped writes until a new command.
// Stale ISR permission lasts until this endpoint's status is returned or a fresh
// FIFO access starts. Guest reconfiguration/unstall explicitly discards it.
void EmRegsISPDana::CancelAccess(unsigned e,int buffer) {
    if(buffer>=0 && fifo[e].cpu!=unsigned(buffer)) return;
    flushed |= 1u<<e;
    bool packet=(dcCommand>=1 && dcCommand<=0x10) || (dcCommand>=0x12 && dcCommand<=0x1f);
    if(packet && (dcCommand&15)==e) { accessCancelled=true; cursor=0; }
}
template <typename T> void EmRegsISPDana::CancellationFields(T& h) {
    h.Do32(flushed).DoBool(accessCancelled);
    for(auto& ep:fifo) {
        h.DoBool(ep.statusPending);
        for(auto& b:ep.packet) h.DoBool(b.writing);
    }
}

// Philips ISP1161A rev.03, sections 9.3 (tables 4/5), 9.4.2/3,
// 10.4.3 (22/A2), 10.6.3 and 10.6.7 (41/C1). Low byte is even RAM.
uint16 EmRegsISPDana::HostWord(bool writing, uint16 value) {
    unsigned count=regs[0x22];
    if(!count || count>regs[0x2b] || hostCursor>=count || hostCursor+2>regs[0x2b]) {
        DanaUnqualified("ISP ATL access outside transfer", "%s at byte %u, count %u, ATL buffer %u",
                        writing ? "write" : "read", hostCursor, count, unsigned(regs[0x2b]));
        return 0xffff;
    }
    auto& slot=hostRam[hostCursor/2];
    uint16 result=slot;
    if(writing) slot=(hostCursor+1==count) ? value&255 : value;
    hostCursor+=2;
    if(writing) hostWritten=std::min<unsigned>(hostCursor,count);
    if(hostCursor>=count) {
        regs[0x24]|=4;
        if(writing) { regs[0x2c]=(regs[0x2c]&~32u)|4; hostArmed=true; }
        else { regs[0x2c]&=~36u; hostArmed=false; }
    }
    return hostCursor>count ? result&255 : result;
}
template <typename T> void EmRegsISPDana::HostFields(T& h) {
    h.Do32(hostCursor);
    for(auto& value:hostRam) h.Do16(value);
}
bool EmRegsISPDana::DecodeHostPtd(const uint8* data, unsigned size, unsigned offset, HostPtd& out) {
    if(!data || size>4096 || (offset&3) || offset>size || size-offset<8) return false;
    auto p=data+offset;
    HostPtd v;
    v.actual=p[0]|((p[1]&3)<<8); v.toggle=p[1]&4; v.active=p[1]&8;
    v.completion=p[1]>>4; v.maximum=p[2]|((p[3]&3)<<8);
    v.lowSpeed=p[3]&4; v.last=p[3]&8; v.endpoint=p[3]>>4;
    v.total=p[4]|((p[5]&3)<<8); v.direction=(p[5]>>2)&3;
    v.interrupt=p[5]&32; v.address=p[6]&127;
    if((p[5]&0xd0) || (p[6]&128) || p[7] || v.direction==3 ||
       !v.maximum || v.actual>v.total || v.total>size-offset-8 ||
       v.completion==10 || v.completion==11) return false;
    out=v; return true;
}
bool EmRegsISPDana::ValidateHostList(const uint8* data, unsigned size, unsigned allocation) {
    if(!allocation) allocation=size;
    if(size>allocation || allocation>4096) return false;
    unsigned offset=0;
    for(;;) {
        HostPtd v;
        if(offset>size || size-offset<8 ||
           !DecodeHostPtd(data,allocation,offset,v)) return false;
        unsigned end=offset+8+v.total;
        // IN destinations occupy RAM, but need no input bytes after Last.
        if(v.direction!=2 && end>size) return false;
        if(v.last) return true;
        offset=(end+3)&~3u;
    }
}
bool EmRegsISPDana::CompleteHostPtd(uint8* data, unsigned size, unsigned offset,
                                   uint16 actual, uint8 completion, bool toggle) {
    HostPtd v;
    if(!DecodeHostPtd(data,size,offset,v) || actual>v.total || completion>15 ||
       completion==10 || completion==11 || completion>=14) return false;
    data[offset]=actual&255;
    data[offset+1]=(actual>>8)|(toggle?4:0)|(completion<<4);
    return true;
}

// A port's own events tell only that port's owner; controller resets and loads tell both.
void EmRegsISPDana::HostCancelled(int port) {
    for(unsigned i=0;i<2;++i) if((port<0 || unsigned(port)==i) && hostOwner[i].cancelled) hostOwner[i].cancelled();
}
void EmRegsISPDana::SettleLoadedPorts() {
    for(unsigned i=0;i<2;++i) if(loadedPorts[i] && !hostTargets[i]) DropLoadedPort(i);
}
void EmRegsISPDana::DropLoadedPort(unsigned port) {
    if(port>=2) throw std::runtime_error("ISP invalid target port");
    if(!loadedPorts[port]) return;
    loadedPorts[port]=false; hostTargets[port]={};
    auto& p=regs[0x15+port];
    p|=0x10000;
    if(p&2) p|=0x20000;
    p&=~0x217u; portResetCycles[port]=0;
    if(regs[0x12]&0x200) p|=0x100;
    regs[3]|=0x40;
    CancelHostWork(int(port));
    RefreshHostInterrupt();
}
void EmRegsISPDana::SetPortSupply(unsigned port, bool on) {
    if(port>=2) throw std::runtime_error("ISP invalid supply port");
    if(portSupply[port]==on) return;
    portSupply[port]=on;
    if(hostOwner[port].supplyChanged) hostOwner[port].supplyChanged();
}
void EmRegsISPDana::SetHostTarget(unsigned port, HostTarget target) {
    if(port>=2) throw std::runtime_error("ISP invalid target port");
    if(loadedPorts[port]) {
        // The saved connection: the device comes back without a connect change.
        hostTargets[port]=std::move(target);
        if(!hostTargets[port]) return;
        loadedPorts[port]=false; hostAttached[port]=true; hostLowSpeed[port]=regs[0x15+port]&0x200;
        return;
    }
    SetHostConnection(port,false);
    hostTargets[port]=std::move(target);
    SetHostConnection(port,bool(hostTargets[port]));
}
void EmRegsISPDana::ExecuteHostFrame() {
    if(!hostArmed || !(regs[0x2c]&4)) return;
    // rev.03 sections 9.3-9.5: DWORD PTDs, round-robin packets, frame readback.
    // Bounds are model policies, not cycle-accurate USB wire timing.
    std::array<uint8,4096> data{};
    const unsigned written=hostWritten, size=regs[0x2b];
    for(unsigned i=0;i<written;++i) data[i]=hostRam[i/2]>>((i&1)*8);
    if(!ValidateHostList(data.data(),written,size)) {
        DanaUnqualified("ISP malformed ATL submission", "%u bytes written of a %u-byte ATL buffer", written, size);
        FailHostList(data,written,size);
        return;
    }
    std::vector<unsigned> offsets;
    for(unsigned o=0;;) {
        HostPtd p; DecodeHostPtd(data.data(),size,o,p);
        offsets.push_back(o);
        if(p.last) break;
        o=(o+8+p.total+3)&~3u;
    }
    hostArmed=false;
    std::array<unsigned,512> errors{}, attempts{};
    unsigned budget=128;
    bool progress=true;
    while(budget && progress) {
        progress=false;
        for(unsigned n=0;n<offsets.size() && budget;++n) {
            unsigned o=offsets[n]; HostPtd p;
            DecodeHostPtd(data.data(),size,o,p);
            if(!p.active || (p.interrupt && attempts[n]) || attempts[n]>=32) continue;
            progress=true; --budget; ++attempts[n];
            unsigned count=std::min<unsigned>(p.maximum,p.total-p.actual);
            HostPacket packet{p.address,p.endpoint,p.direction,p.toggle,p.lowSpeed,count,{}};
            if(p.direction!=2) packet.bytes.assign(data.begin()+o+8+p.actual,data.begin()+o+8+p.actual+count);
            HostReply reply;
            // Every enabled port sees the packet; a device at another address doesn't answer.
            for(unsigned port=0;port<2;++port) {
                if(hostTargets[port] && (regs[0x15+port]&0x117)==0x103 &&
                   bool(regs[0x15+port]&0x200)==p.lowSpeed) {
                    reply=hostTargets[port](packet);
                    if(reply.result!=PacketResult::noAnswer) break;
                }
            }
            unsigned cc=0; bool done=false;
            if(reply.result==PacketResult::nak) { attempts[n]=32; }
            else if(reply.result==PacketResult::stall) { cc=4; done=true; }
            else if(reply.result==PacketResult::noAnswer) {
                cc=5; done=++errors[n]>=3;
            } else if(p.direction==2 && reply.bytes.size()>count) { cc=8; done=true; }
            else if(p.direction==2 && reply.toggle!=p.toggle) {
                cc=3; done=++errors[n]>=3;
            } else {
                errors[n]=0;
                unsigned accepted=p.direction==2 ? reply.bytes.size() : count;
                if(p.direction==2) std::copy(reply.bytes.begin(),reply.bytes.end(),data.begin()+o+8+p.actual);
                p.actual+=accepted; p.toggle=!p.toggle;
                done=p.actual==p.total;
                if(p.direction==2 && accepted<p.maximum && !done) { cc=9; done=true; }
            }
            data[o]=p.actual&255;
            data[o+1]=(p.actual>>8)|(p.toggle?4:0)|(done?0:8)|(cc<<4);
            // Commit each accepted packet before the next target call.
            for(unsigned i=0;i<size;++i)
                hostRam[i/2]=(hostRam[i/2]&~(255u<<((i&1)*8)))|(uint16(data[i])<<((i&1)*8));
        }
    }
    regs[0x2c]|=32; regs[0x24]|=2;
}

// A list the model can't run completes at once: each readable active PTD before the first malformed
// one gets cc 5 (device not responding), the malformed one and any after it stay as the guest wrote
// them, and the ATL done interrupt is raised.
void EmRegsISPDana::FailHostList(std::array<uint8,4096>& data, unsigned written, unsigned size) {
    hostArmed=false;
    for(unsigned o=0; o<=written && written-o>=8;) {
        HostPtd p;
        // The same per-PTD checks as ValidateHostList: an OUT needs all its bytes written.
        if(!DecodeHostPtd(data.data(),size,o,p) || (p.direction!=2 && o+8+p.total>written)) break;
        if(p.active) {
            CompleteHostPtd(data.data(),size,o,p.actual,5,p.toggle);
            hostRam[o/2]=data[o]|(uint16(data[o+1])<<8);
        }
        if(p.last) break;
        o=(o+8+p.total+3)&~3u;
    }
    regs[0x2c]|=32; regs[0x24]|=2;
}

// Cycle stamps follow the session's new cycle unit. The remaining reset times grow with it, but never
// past a fresh reset's length, so a later load still accepts them.
void EmRegsISPDana::ScaleCycles(uint32 factor) {
    lastCycle*=factor;
    uint64 hz=ClockHz();
    resetCycles*=factor;
    for(auto& cycles:portResetCycles) cycles*=factor;
    if(!hz) return;
    resetCycles=std::min<uint64>(resetCycles,(hz+99999)/100000);
    for(auto& cycles:portResetCycles) cycles=std::min<uint64>(cycles,(hz+99)/100);
}

// A port's reset, disable, power or disconnect leaves the ATL list armed, as on the ISP1161: the
// next frame completes that port's PTDs with cc 5 as their device no longer answers, and the
// other port's run on. A controller reset or load completes every active PTD with cc 5.
void EmRegsISPDana::CancelHostWork(int port) {
    HostCancelled(port);
    if(port>=0 || !hostArmed) return;
    hostArmed=false;
    std::array<uint8,4096> data{};
    unsigned written=hostWritten, size=regs[0x2b];
    for(unsigned i=0;i<written;++i) data[i]=hostRam[i/2]>>((i&1)*8);
    if(!ValidateHostList(data.data(),written,size)) return;
    for(unsigned o=0;;) {
        HostPtd p; DecodeHostPtd(data.data(),size,o,p);
        if(p.active) {
            CompleteHostPtd(data.data(),size,o,p.actual,5,p.toggle);
            hostRam[o/2]=data[o]|(uint16(data[o+1])<<8);
        }
        if(p.last) break;
        o=(o+8+p.total+3)&~3u;
    }
    regs[0x2c]|=32; regs[0x24]|=2;
}
