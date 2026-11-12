#include "EmRegsVZDana.h"
#include <cstdio>
#include "EmSession.h"
#include "EmUARTDragonball.h"
#include "EmCPU68K.h"
#include <cstring>
#include "dana_keyboard5.h"
#include <stdexcept>
#include "ExternalStorage.h"
#include "DanaUnqualified.h"
#include "EmLowMem.h"
#include "EmBankSRAM.h"
#include "EmMemory.h"

Bool EmRegsVZDana::GetLCDScreenOn() { return (StdRead(0xfffff441,1)&0x80)!=0; }
bool EmRegsVZDana::CopyLCDFrame(Frame& frame,bool fullRefresh) {
    return GetLCDScreenOn() && EmRegsVZ::CopyLCDFrame(frame,fullRefresh);
}
Bool EmRegsVZDana::GetLCDBacklightOn() { return (StdRead(0xfffff441,1)&0x10)!=0; }

void EmRegsVZDana::GetKeyInfo(int* rows, int* cols, uint16*, Bool*) {
    *rows = *cols = 0;
}

uint8 EmRegsVZDana::GetPortInternalValue(int port) {
    // The small ROM's reset path requires D7 high to leave its power-down branch.
    uint8 result=EmRegsVZ::GetPortInternalValue(port) | (port == 'D' ? 0x9f : port == 'F' || port == 'J' ? 2 : 0);
    if(port=='D') {
        if(gExternalStorage.IsMounted(EmHAL::Slot::sdcard)) result &= ~4;
        if(gExternalStorage.IsMounted(EmHAL::Slot::sdcard2)) result &= ~8;
    }
    if(port=='D' && !resetAsserted && StRequest()) result &= ~0x10;
    return result;
}

uint8 EmRegsVZDana::GetPortInputValue(int port) {
    if(port=='D') return (EmRegsVZ::GetPortInputValue(port)&~0x60) | (dcIrqHigh?0x20:0) | (hcIrqHigh?0x40:0);
    if(port=='J') return EmRegsVZ::GetPortInputValue(port) | 2; // MISO is ready between SPI transfers.
    if (port=='G') return (EmRegsVZ::GetPortInputValue(port)&~2) | (adc.packDetectHigh ? 2 : 0);
    if(port=='F') {
        auto* first=gExternalStorage.GetImageInSlot(EmHAL::Slot::sdcard);
        auto* second=gExternalStorage.GetImageInSlot(EmHAL::Slot::sdcard2);
        return 2 | (!first || !first->IsReadOnly() ? 0x80 : 0) | (!second || !second->IsReadOnly() ? 1 : 0);
    }
    return EmRegsVZ::GetPortInputValue(port) | (port=='K' && !resetAsserted ? 4 : port=='F' ? 2 : 0);
}
namespace {
    // SPI2 with neither chip select low: MISO floats high.
    class FloatingSpi : public EmSPISlave {
      public:
        uint16 DoExchange(uint16 control, uint16) override {
            DanaUnqualified("SPI2 exchange with no chip selected", "control=0x%04x", unsigned(control));
            return 0xffff;
        }
    };
}
EmSPISlave* EmRegsVZDana::GetSPI2Slave() {
    // E3 selects the ST controller; G2 selects the ADC.
    if (!(StdRead(0xfffff421,1) & 8)) return this;
    if (!(StdRead(0xfffff431,1) & 4)) return &adc;
    static FloatingSpi floating;
    return &floating;
}
uint16 EmRegsVZDana::DoExchange(uint16 control, uint16 data) {
    if (resetAsserted) return 0;
    if ((control & 15)==7) {
        // The reset helper dispatches EF after shifting its existing AA reply.
        if(data==0xeb || data==0xed || data==0xee || data==0xef) {
            if(sleepCommand || versionCommand || versionReply) {
                DanaUnqualified("ST power command during a partial request", "command=0x%02x", unsigned(data));
                sleepCommand=versionCommand=versionReply=0; return 0xffff;
            }
            bool ack=resetReply; resetReply=false;
            if(data==0xeb) { ebHold=1; ebReported=0; }
            if(data==0xed) pa3Option=0;
            EnterHalt(data==0xee);
            return ack ? 0xaa : 0;
        }
        if(sleepCommand || (!versionCommand && data=='1')) {
            constexpr char halt[]="123 Halt";
            if(sleepCommand<8) {
                if(data!=uint8(halt[sleepCommand])) {
                    DanaUnqualified("ST sleep framing", "byte %u is 0x%02x", sleepCommand, unsigned(data));
                    sleepCommand=0; return 0xffff;
                }
            } else wakeMask[sleepCommand-8]=data;
            if(++sleepCommand==10) {
                sleepCommand=0; EnterHalt();
            }
            return 0;
        }
        const char* request="Get Ver!";
        if (versionCommand || data=='G') {
            if (versionCommand==8) {
                if (data || versionReply>=4) {
                    DanaUnqualified("ST version framing", "reply byte %u clocked with 0x%02x", versionReply, unsigned(data));
                    versionCommand=versionReply=0; return 0xffff;
                }
                uint8 reply=uint8("07.4"[versionReply++]);
                if (versionReply==4) versionCommand=versionReply=0;
                return reply;
            }
            if (data!=uint8(request[versionCommand])) {
                DanaUnqualified("ST version request", "byte %u is 0x%02x", versionCommand, unsigned(data));
                versionCommand=0; return 0xffff;
            }
            ++versionCommand; return 0;
        }
        if (data==0) { if(resetReply) { resetReply=false; return 0xaa; } return stAsleep && !packetIndex && !pollIndex ? 0 : ReadKeyPacket(); }
    }
    // Unknown ST traffic shifts in a floating MISO.
    DanaUnqualified("ST unqualified SPI command", "control=0x%04x data=0x%04x", unsigned(control), unsigned(data));
    return 0xffff;
}

void EmRegsVZDana::SetSubBankHandlers() {
    EmRegsVZ::SetSubBankHandlers();
    SetHandler((ReadFunction)&EmRegsVZDana::StdRead,
               (WriteFunction)&EmRegsVZDana::StIntControlWrite,0xfffff41b,1);
    for(emuptr a : {0xfffff408u,0xfffff40bu,0xfffff428u,0xfffff42bu,
                    0xfffff438u,0xfffff43bu,0xfffff440u,0xfffff443u})
        SetHandler((ReadFunction)&EmRegsVZDana::StdRead,
                   (WriteFunction)&EmRegsVZDana::CardControlWrite,a,1);
    SetHandler((ReadFunction)&EmRegsVZDana::StdRead,
               (WriteFunction)&EmRegsVZDana::SoundControlWrite,0xfffff500,2);
    SetHandler((ReadFunction)&EmRegsVZDana::StdRead,
               (WriteFunction)&EmRegsVZDana::SoundClockWrite,0xfffff200,2);
    SetHandler((ReadFunction)&EmRegsVZDana::pllFreqSelRead,
               (WriteFunction)&EmRegsVZDana::SoundClockWrite,0xfffff202,2);
    SetHandler((ReadFunction)&EmRegsVZDana::StdRead,
               (WriteFunction)&EmRegsVZDana::SoundMuxWrite,0xfffff40b,1);
    SetHandler((ReadFunction)&EmRegsVZDana::StdRead,
               (WriteFunction)&EmRegsVZDana::StIntControlWrite,0xfffff302,2);
    SetHandler((ReadFunction)&EmRegsVZDana::StdRead,
               (WriteFunction)&EmRegsVZDana::StIntStatusWrite,0xfffff30c,2);
    SetHandler((ReadFunction)&EmRegsVZDana::StdRead,
               (WriteFunction)&EmRegsVZDana::KDirWrite,0xfffff440,1);
}
void EmRegsVZDana::KDirWrite(emuptr a, int size, uint32 value) {
    uint8 old=StdRead(a,size);
    StdWrite(a,size,value);
    SyncCards();
    if ((old&4) && !(value&4) && !(StdRead(0xfffff441,1)&4)) {
        ResetPowerProtocol();
        resetReply=true; versionCommand=versionReply=0; packetIndex=pollIndex=0; std::memset(presentedScan,0,8); ClearRegap(); queueHead=queueCount=0; keysDirty=false; if(heldCount) QueueKeys(); UpdateStIrq();
    }
}
void EmRegsVZDana::PortDataChanged(int p, uint8 old, uint8 value) {
    EmRegsVZ::PortDataChanged(p,old,value);
    if(p=='B' || p=='K' || p=='J' || p=='F') SyncCards();
    if(p=='K' || p=='M') SyncUsbSupply();
    if (p=='B' && ((old^value)&0x40)) {
        ResetPowerProtocol();
        resetAsserted=(value&0x40)!=0;
        resetReply=!resetAsserted; versionCommand=versionReply=0; packetIndex=pollIndex=0; std::memset(presentedScan,0,8); ClearRegap(); queueHead=queueCount=0; keysDirty=false; if(heldCount) QueueKeys(); UpdateStIrq();
    }
}

#include "savestate/ChunkHelper.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"


void EmRegsVZDana::Save(Savestate<ChunkType>& s) { SaveState(s); }
void EmRegsVZDana::Save(SavestateProbe<ChunkType>& s) { SaveState(s); }
template <typename T> void EmRegsVZDana::SaveState(T& s) {
    EmRegsVZ::Save(s);
    adc.Save(s);
    sd0.Save(s); sd1.Save(s);
    if(auto* uart=s.GetChunk(ChunkType::uartDana)) {
        auto pending=fUART[1]->CapturePending();
        uart->Put32(1); uart->Put32(pending.rxCount); uart->Put32(pending.txCount);
        uart->Put32(pending.receiving);
        for(auto byte:pending.rx) uart->Put32(byte);
        for(auto byte:pending.tx) uart->Put32(byte);
    }
    SaveStChunk(s);
}
template <typename T> void EmRegsVZDana::SaveStChunk(T& s) {
    auto* c=s.GetChunk(ChunkType::regsVZDana); if(!c) return;
    c->Put32(kStateVersion); SaveChunkHelper h(*c); StateFields(h); InputStateFields(h); PowerStateFields(h); PowerModeFields(h); RegapFields(h); BacklogFields(h);
}
template void EmRegsVZDana::SaveStChunk(Savestate<ChunkType>&);
template void EmRegsVZDana::SaveStChunk(SavestateProbe<ChunkType>&);
void EmRegsVZDana::Load(SavestateLoader<ChunkType>& l) {
    EmUARTDragonball::PendingState pending;
    if(auto* uart=l.GetChunk(ChunkType::uartDana)) {
        if(uart->Get32()!=1) { l.NotifyError(); return; }
        pending.rxCount=uart->Get32(); pending.txCount=uart->Get32();
        unsigned receiving=uart->Get32(); pending.receiving=receiving!=0;
        bool valid=receiving<=1;
        for(auto& byte:pending.rx) { unsigned value=uart->Get32(); if(value>255) valid=false; byte=value; }
        for(auto& byte:pending.tx) { unsigned value=uart->Get32(); if(value>255) valid=false; byte=value; }
        if(!valid || !uart->IsAtEnd()) { l.NotifyError(); return; }
    }
    if(!fUART[1]->RestorePending(pending)) { l.NotifyError(); return; }
    EmRegsVZ::Load(l);
    SyncUsbSupply();
    adc.Load(l);
    if(!l.GetChunk(ChunkType::spiSlaveSD)) sd0.Reset();
    sd0.Load(l); sd1.Load(l);
    LoadStChunk(l);
}
void EmRegsVZDana::LoadStChunk(SavestateLoader<ChunkType>& l) {
    auto* c=l.GetChunk(ChunkType::regsVZDana);
    if(!c) { l.NotifyError(); return; }
    unsigned version=c->Get32();
    if(version>kStateVersion) EmHAL::loadedNewerState=true;
    if(version<5 || version>kStateVersion) { l.NotifyError(); return; }
    LoadChunkHelper h(*c);
    if(version>=6) StateFields(h);
    else {
        LegacyStateFields(h);
        if(heldKeys>3 || packetKeys>3 || packetIndex>2 ||
           (packetIndex && !packetKeys) || (packetIndex==2 && packetKeys!=3)) {
            l.NotifyError(); return;
        }
        MigrateLegacyInput();
    }
    hostPaced=false; inputLastCycle=inputDelay=inputClockHz=0; pollIndex=0; std::memset(presentedScan,0,8);
    if(version>=7) InputStateFields(h);
    ResetPowerProtocol(); wakeMask[0]=wakeMask[1]=0;
    if(version>=8) PowerStateFields(h);
    if(version>=9) PowerModeFields(h);
    else { rowGroups=stAsleep ? SelectedRowGroups() : 0; fallingOnly=stAsleep ? 1 : 0; }
    ClearRegap();
    if(version>=10) RegapFields(h);
    backlog.clear(); backlogDamaged=false;
    if(version>=11) BacklogFields(h);
    if(backlogDamaged) { backlog.clear(); l.NotifyError(); }
    noWakeValid=false;
    for(unsigned i=0;i<8;i++)
        if(regapLeft[i]>inputClockHz*kRegapMs/1000 || (!regapScan[i])!=(!regapLeft[i]) || (!hostPaced && regapScan[i])) l.NotifyError();
    if(repeatReply && (!packetIndex || !presentedScan[packetIndex-1])) l.NotifyError();
    if(sleepCommand>9 || wakeMask[0]>255 || wakeMask[1]>255 || stAsleep>2 || (version<9 && stAsleep>1) ||
       ebHold>1 || ebReported>1 || (ebReported && !ebHold) || rowGroups>7 || pa3Option>1 || fallingOnly>1 ||
       (!pa3Option && !stAsleep) || (stAsleep==1 && !fallingOnly) || (stAsleep==2 && !fallingOnly && rowGroups) ||
       (stAsleep && (sleepCommand || rowGroups!=SelectedRowGroups() || (queueCount && !packetIndex))) ||
       (!stAsleep && rowGroups) ||
       ((sleepCommand || stAsleep) && (versionCommand || versionReply || resetAsserted))) l.NotifyError();
    if(inputClockHz>0xffffffffu || inputDelay>inputClockHz/50 || (!hostPaced && inputDelay)) l.NotifyError();
    if(pollIndex>8 || (pollIndex && (!hostPaced || !presentedScan[pollIndex-1] || packetIndex))) l.NotifyError();
    // From v11 the backlog makes the chunk variable length. A save into a layout probed with a
    // longer backlog leaves unused words at the end, so only older fixed-size chunks must end here.
    if(version<11 && !c->IsAtEnd()) l.NotifyError();
    if(adc.mainBattery>4095 || adc.supply>4095 || heldKeys || packetKeys || heldCount>8 || queueHead>=32 || queueCount>32 || packetIndex>8 ||
       (packetIndex && !repeatReply && !queueCount) || versionCommand>8 || versionReply>3 ||
       (versionCommand!=8 && versionReply)) { l.NotifyError(); return; }
    if(keysDirty!=(queueCount!=0) || (queueCount && !stAsleep && !ebReported && std::memcmp(heldScan,packets[(queueHead+queueCount-1)%32],8))) l.NotifyError();
    auto valid=[](const uint8* codes) {
        bool empty=false;
        for(unsigned i=0;i<8;i++) {
            if(!codes[i]) { empty=true; continue; }
            if(empty || !danaScanValid5(codes[i])) return false;
            for(unsigned j=0;j<i;j++) if(codes[j]==codes[i]) return false;
        }
        return true;
    };
    UpdateIRQ2(dcIrqHigh?0x20:0); UpdateIRQ3(hcIrqHigh?0x40:0);
    if(!valid(presentedScan)) l.NotifyError();
    if(!valid(heldScan)) l.NotifyError();
    for(auto& packet:packets) if(!valid(packet)) l.NotifyError();
    for(unsigned i=0;i<8;i++) if((i<heldCount)!=(heldScan[i]!=0)) l.NotifyError();
    if(packetIndex && !repeatReply && !packets[queueHead][packetIndex-1]) l.NotifyError();
    if(!valid(regapScan)) l.NotifyError();
    if(!BacklogValid()) l.NotifyError();

}

void EmRegsVZDana::SyncUsbSupply() {
    if(!usbSupplySink) return;
    usbSupplySink(0,(StdRead(0xfffff449,1)&0x20)!=0);
    usbSupplySink(1,(StdRead(0xfffff441,1)&1)!=0);
}

void EmRegsVZDana::Reset(Bool hardware) {
    EmRegsVZ::Reset(hardware);
    DispatchPwmChange();
    SyncUsbSupply();
    sd0.Reset(); sd1.Reset();
    if(!hardware) return;
    ResetPowerProtocol();
    hostPaced=false; inputLastCycle=inputDelay=inputClockHz=0; pollIndex=0; std::memset(presentedScan,0,8); ClearRegap();
    backlog.clear(); noWakeValid=false;
    resetReply=true; resetAsserted=false; versionCommand=versionReply=0;
    heldKeys=packetKeys=0; heldCount=queueHead=queueCount=0;
    std::memset(heldScan,0,sizeof heldScan); std::memset(packets,0,sizeof packets); packetIndex=0; keysDirty=false; irqPinHigh=true; irqLatched=false; UpdateStIrq();
    adc=AnalogueInputs();
    SetDcIrqPin(true);
    SetHcIrqPin(true);
}

bool EmRegsVZDana::DispatchDanaInput(const std::vector<DanaTransition5>& batch) {
    if(batch.size()>24) return false;
    std::vector<std::pair<uint8,bool>> changes;
    for(auto event:batch) changes.push_back({event.scan.Code(),event.down});
    return AcceptTransitions(changes,true);
}
// The held set once every waiting transition has gone into the FIFO.
std::vector<uint8> EmRegsVZDana::BacklogHeld() const {
    std::vector<uint8> held(heldScan,heldScan+std::min(heldCount,8u));
    for(auto entry:backlog) {
        uint8 code=entry&0x7f;
        auto it=std::find(held.begin(),held.end(),code);
        if(entry&0x80) { if(it==held.end()) held.push_back(code); }
        else if(it!=held.end()) held.erase(it);
    }
    return held;
}
// Each waiting transition changes the held set, valid codes only and never more than eight down.
bool EmRegsVZDana::BacklogValid() const {
    if(backlog.size()>kBacklogLimit+8) return false;
    std::vector<uint8> held(heldScan,heldScan+std::min(heldCount,8u));
    for(auto entry:backlog) {
        uint8 code=entry&0x7f;
        if(!danaScanValid5(code)) return false;
        auto it=std::find(held.begin(),held.end(),code);
        if((entry&0x80)!=0 ? it!=held.end() : it==held.end()) return false;
        if(entry&0x80) { held.push_back(code); if(held.size()>8) return false; }
        else held.erase(it);
    }
    return true;
}
// Appends the changes that alter the held set, all or none. Feeds straight away only when
// nothing was waiting, which keeps a short burst going into the FIFO at once as before.
bool EmRegsVZDana::AcceptTransitions(const std::vector<std::pair<uint8,bool>>& changes,bool paced) {
    auto next=BacklogHeld();
    std::vector<uint8> add; bool presses=false;
    for(auto [code,down]:changes) {
        if(!danaScanValid5(code)) return false;
        auto it=std::find(next.begin(),next.end(),code);
        if(down ? it!=next.end() : it==next.end()) continue;
        if(down) { next.push_back(code); if(next.size()>8) return false; presses=true; }
        else next.erase(it);
        add.push_back(uint8(code|(down?0x80:0)));
    }
    if(presses && backlog.size()+add.size()>kBacklogLimit) return false;
    if(paced) hostPaced=true;
    bool idle=backlog.empty();
    backlog.insert(backlog.end(),add.begin(),add.end()); noWakeValid=false;
    if(idle) FeedBacklog(true);
    return true;
}
// A press keeps a FIFO slot for its own release, so a release always fits once it is applied.
// While the ST sleeps, a direct dispatch applies as on hardware: a key that doesn't wake it is
// lost. Keys that were already waiting stay for after the wake; only a press that wakes the ST
// goes in, and a later waking key goes first with its release (PullWakingKey).
void EmRegsVZDana::FeedBacklog(bool direct) {
    bool fed=false;
    while(!backlog.empty()) {
        uint8 code=backlog.front()&0x7f; bool down=(backlog.front()&0x80)!=0;
        if(stAsleep && down && !direct && !Wakes(code)) {
            // CycleSlowly runs on every interrupt while the CPU sleeps, so scan the backlog again
            // only once it or the wake inputs change.
            auto inputs=WakeInputs();
            if(noWakeValid && inputs==noWakeAt) break;
            if(!PullWakingKey()) { noWakeAt=inputs; noWakeValid=true; break; }
            fed=true; continue;
        }
        if(!stAsleep && (down ? queueCount+1+heldCount+1>32 || heldCount==8 : queueCount>=32)) break;
        if(!stAsleep && down && KeyQueueBlocked()) {
            if(!PullRelease()) break;
            fed=true; continue;
        }
        backlog.pop_front(); noWakeValid=false;
        ApplyScanCode(code,down); fed=true;
    }
    if(fed) UpdateStIrq();
}
// Everything Wakes reads besides the backlog.
std::array<uint32,14> EmRegsVZDana::WakeInputs() const {
    std::array<uint32,14> inputs{heldCount,rowGroups,wakeMask[0],wakeMask[1],stAsleep,ebHold};
    for(unsigned i=0;i<8;i++) inputs[6+i]=heldScan[i];
    return inputs;
}
// True when pressing code now would give the falling row edge that wakes the sleeping ST.
bool EmRegsVZDana::Wakes(uint8 code) const {
    return heldCount<8 && (SelectedRowGroups(code) & ~rowGroups)!=0;
}
// Takes the first waiting press that wakes the ST and whose key isn't used earlier in the
// backlog, with its release, and applies both. The rest keep their order.
bool EmRegsVZDana::PullWakingKey() {
    ++wakeScans;
    bool seen[128]{};
    for(size_t i=0;i<backlog.size();i++) {
        uint8 code=backlog[i]&0x7f;
        if((backlog[i]&0x80) && !seen[code] && Wakes(code)) {
            size_t release=i+1;
            while(release<backlog.size() && (backlog[release]&0x7f)!=code) ++release;
            bool released=release<backlog.size();
            if(released) backlog.erase(backlog.begin()+release);
            backlog.erase(backlog.begin()+i); noWakeValid=false;
            ApplyScanCode(code,true);
            if(released) ApplyScanCode(code,false);
            return true;
        }
        seen[code]=true;
    }
    return false;
}
void EmRegsVZDana::ReleaseDanaInput(bool cancel) {
    if(!cancel && !backlog.empty()) {
        // Release after the waiting keys, keeping their order.
        for(auto code:BacklogHeld()) backlog.push_back(code);
        noWakeValid=false;
        return;
    }
    backlog.clear(); noWakeValid=false;
    if(stAsleep) { heldCount=0; rowGroups=0; std::memset(heldScan,0,8); return; }
    ebHold=ebReported=0;
    if(!cancel) {
        if(!heldCount) return;
        heldCount=0; std::memset(heldScan,0,8);
        QueueKeys();
        UpdateStIrq(); return;
    }
    // Preserve a reply already being clocked; replace all later input with release.
    if(packetIndex && !repeatReply) { queueCount=1; }
    else { queueHead=queueCount=0; }
    heldCount=0; std::memset(heldScan,0,8);
    QueueKeys(); UpdateStIrq();
}

void EmRegsVZDana::DispatchButtonEvent(ButtonEvent e) {
    uint8 code;
    switch(e.GetButton()) {
        case ButtonEvent::Button::power: code=0x0f; break;
        case ButtonEvent::Button::rockerUp: code=0x70; break;
        default: throw std::runtime_error("Dana key is not qualified");
    }
    // Runs from CycleSlowly, so a refusal is logged and the event dropped, never thrown.
    if(!DispatchScanCode(code,e.GetType()==ButtonEvent::Type::press))
        logPrintf("Dana keyboard busy, %s dropped\n",code==0x0f ? "power" : "rocker");
}

void EmRegsVZDana::QueueKeys() {
    // Every press keeps a slot for its release, so the FIFO is only full here in a state saved by
    // an older core. Then replace the newest report, never the head that may be being clocked:
    // a lost press is better than a key left down.
    if(queueCount==32) { std::memcpy(packets[(queueHead+31)%32],heldScan,8); keysDirty=true; return; }
    unsigned tail=(queueHead+queueCount)%32;
    std::memcpy(packets[tail],heldScan,8); ++queueCount;
    keysDirty=true;
}

bool EmRegsVZDana::DispatchScanCode(uint8 code,bool down) {
    return AcceptTransitions({{code,down}});
}
void EmRegsVZDana::ApplyScanCode(uint8 code,bool down) {
    unsigned index=0;
    while(index<heldCount && heldScan[index]!=code) ++index;
    if(down ? index<heldCount || heldCount==8 : index==heldCount) return;
    if(down) {
        heldScan[heldCount++]=code;
        std::sort(heldScan,heldScan+heldCount,[](uint8 a,uint8 b) {
            return (a&15)==(b&15) ? a<b : (a&15)<(b&15);
        });
    }
    else {
        for(unsigned i=index+1;i<heldCount;i++) heldScan[i-1]=heldScan[i];
        heldScan[--heldCount]=0;
    }
    if(stAsleep) {
        unsigned groups=SelectedRowGroups();
        bool falling=(groups & ~rowGroups)!=0;
        rowGroups=groups;
        if(falling) {
            stAsleep=0; rowGroups=0; pa3Option=1; fallingOnly=0;
            QueueKeys(); if(ebHold) ebReported=1;
        }
    } else if(!ebReported || !heldCount) {
        QueueKeys(); if(!heldCount) ebHold=ebReported=0;
    }
    UpdateStIrq();
}

uint8 EmRegsVZDana::ReadKeyPacket() {
    if(ebReported && !queueCount && !pollIndex && !packetIndex) return 0;
    bool ready=!pollIndex && (packetIndex || (queueCount && InputReady()));
    if(ready && !packetIndex && RegapBlocked()) {
        // Hold back a quick press of a just released key; report the held set again meanwhile.
        if(presentedScan[0]) repeatReply=true; else ready=false;
    }
    if(!ready) {
        if(!hostPaced) return 0;
        // The ROM polls between IRQs. No new packet still means the last held set.
        if(pollIndex) {
            uint8 reply=presentedScan[pollIndex++-1];
            if(pollIndex>8 || !presentedScan[pollIndex-1]) pollIndex=0;
            return reply;
        }
        unsigned count=0; while(count<8 && presentedScan[count]) ++count;
        if(count) pollIndex=1;
        return 0x40|count;
    }
    auto* packet=repeatReply ? presentedScan : packets[queueHead];
    uint8 reply;
    if(packetIndex) reply=packet[packetIndex++-1];
    else {
        unsigned count=0; while(count<8 && packet[count]) ++count;
        reply=0x40|count;
        packetIndex=1;
    }
    if(packetIndex>8 || !packet[packetIndex-1]) {
        if(hostPaced) InputReady();
        if(repeatReply) repeatReply=false;
        else {
            if(hostPaced) NoteRelease(packet);
            std::memcpy(presentedScan,packet,8);
            std::memset(packet,0,8); queueHead=(queueHead+1)%32; --queueCount;
            FeedBacklog();
        }
        // Space reports 10 ms apart. The ROM releases every key when no report comes for three
        // of its 10 ms ticks (20-30 ms), so a held key is reported again well inside that.
        if(hostPaced) { inputLastCycle=InputCycle(); inputClockHz=InputHz(); inputDelay=inputClockHz/100; }
        packetIndex=0;
    }
    keysDirty=queueCount!=0;
    UpdateStIrq(); return reply;
}

void EmRegsVZDana::UpdateStIrq() {
    bool high=resetAsserted || !StRequest();
    uint16 control=StdRead(0xfffff302,2);
    bool positive=(control&0x8000)!=0;
    if(control&0x0800) {
        if(high!=irqPinHigh && high==positive) irqLatched=true;
    } else irqLatched=high==positive;
    irqPinHigh=high;
    uint16 pending=StdRead(0xfffff310,2);
    StdWrite(0xfffff310,2,(pending&~1)|(irqLatched?1:0));
    UpdateInterrupts();
}
void EmRegsVZDana::StIntControlWrite(emuptr a,int size,uint32 value) {
    // Both ISP routes support the level mode used by the ROM.
    if(a<=0xfffff302 && a+size>0xfffff302 &&
       ((value >> (8*(a+size-0xfffff303)))&6)) {
        DanaUnqualified("ISP edge interrupt mode", "ICR write 0x%x at 0x%08x", unsigned(value), unsigned(a));
        return;
    }
    StdWrite(a,size,value); UpdateStIrq(); UpdateIRQ2(dcIrqHigh?0x20:0); UpdateIRQ3(hcIrqHigh?0x40:0);
}
void EmRegsVZDana::StIntStatusWrite(emuptr a,int size,uint32 value) {
    uint32 clear=size==4 ? value>>16 : value;
    if(size==1 && a==0xfffff30c)clear=0;
    if((StdRead(0xfffff302,2)&0x0800) && (clear&1))irqLatched=false;
    intStatusHiWrite(a,size,value); UpdateStIrq(); UpdateIRQ2(dcIrqHigh?0x20:0); UpdateIRQ3(hcIrqHigh?0x40:0);
}

uint64 EmRegsVZDana::InputCycle() const { return gCPU68K ? gCPU68K->CycleNow() : 0; }
uint64 EmRegsVZDana::InputHz() const { return gSession ? uint64(gSession->GetClocksPerSecond()) : 0; }
bool EmRegsVZDana::InputReady() {
    uint64 now=InputCycle(),hz=InputHz();
    if(inputClockHz && hz!=inputClockHz) {
        inputDelay=inputDelay*hz/inputClockHz;
        for(auto& left:regapLeft) left=left*hz/inputClockHz;
    }
    inputClockHz=hz;
    if(now>=inputLastCycle) {
        uint64 elapsed=now-inputLastCycle;
        inputDelay=elapsed>=inputDelay?0:inputDelay-elapsed;
        AgeRegap(elapsed);
    } else AgeRegap(0);
    inputLastCycle=now;
    return !inputDelay;
}
// True when the queue head presses a key the guest saw released less than kRegapMs ago.
bool EmRegsVZDana::RegapBlocked() const {
    if(!queueCount) return false;
    for(auto code:packets[queueHead]) {
        if(!code) break;
        if(std::find(regapScan,regapScan+8,code)!=regapScan+8 &&
           std::find(presentedScan,presentedScan+8,code)==presentedScan+8) return true;
    }
    return false;
}
// True when a press now could overflow Palm OS's key queue. The ROM puts each press in that
// queue as its report comes, so presses faster than the app takes them would be dropped there.
// They wait in the backlog instead, counting the presses already in the FIFO, and only while the
// ST is awake, so a waking press always goes in. Host input only. Unblocking is seen when
// FeedBacklog next runs, from a report read or CycleSlowly. An app that never reads keys lets
// presses build up to kBacklogLimit instead of being dropped.
bool EmRegsVZDana::KeyQueueBlocked() const {
    if(!hostPaced) return false;
    uint32 presses=0;
    const uint8* before=presentedScan;
    for(unsigned i=0;i<queueCount;i++) {
        const uint8* packet=packets[(queueHead+i)%32];
        for(unsigned k=0;k<8 && packet[k];k++)
            if(std::find(before,before+8,packet[k])==before+8) ++presses;
        before=packet;
    }
    const uint32 room=KeyQueueRoom();
    return room!=~0u && room<=kKeyQueueReserve+presses*kKeyQueueEntry;
}
// While a press waits, a release of a key that is down goes ahead of it, so a key held across
// the next press (a down, b down, a up) isn't left down. Only up to the first release of a key
// pressed in the backlog, so a chord's order stays (shift up after a down and a up), and never a
// modifier (caps, shifts, ctrl, function, command), whose release changes what the press types.
bool EmRegsVZDana::PullRelease() {
    if(queueCount>=32) return false;
    bool seen[128]{},pressed[128]{};
    for(size_t i=0;i<backlog.size();i++) {
        uint8 code=backlog[i]&0x7f;
        if(backlog[i]&0x80) { pressed[code]=seen[code]=true; continue; }
        if(pressed[code]) return false;
        bool modifier=code==0x0c || code==0x09 || code==0x69 || code==0x7b || code==0x46 || code==0x13;
        if(!seen[code] && !modifier && std::find(heldScan,heldScan+heldCount,code)!=heldScan+heldCount) {
            backlog.erase(backlog.begin()+i); noWakeValid=false;
            ApplyScanCode(code,false);
            return true;
        }
        seen[code]=true;
    }
    return false;
}
uint32 EmRegsVZDana::KeyQueueRoom() const {
    CEnableFullAccess munge;
    return KeyQueueRoomIn(EmLowMem_GetGlobal(sysEvtMgrGlobalsP),EmBankSRAM::GetMemoryStart(),
        EmMemory::GetRegionSize(MemoryRegion::ram),
        [](emuptr a) { return uint32(EmMemGet16(a)); },[](emuptr a) { return uint32(EmMemGet32(a)); });
}
// Counts each entry down and drops the expired ones, keeping regapScan packed.
void EmRegsVZDana::AgeRegap(uint64 elapsed) {
    unsigned n=0;
    for(unsigned i=0;i<8;i++) {
        if(!regapScan[i]) continue;
        uint64 left=elapsed>=regapLeft[i]?0:regapLeft[i]-elapsed;
        if(!left) continue;
        regapScan[n]=regapScan[i]; regapLeft[n++]=left;
    }
    for(unsigned i=n;i<8;i++) { regapScan[i]=0; regapLeft[i]=0; }
}
// Called before next replaces presentedScan: each key it releases gets its own full window,
// evicting the entry closest to expiry when all eight are in use; keys it presses leave.
void EmRegsVZDana::NoteRelease(const uint8* next) {
    auto in=[](const uint8* codes,uint8 code) { return code && std::find(codes,codes+8,code)!=codes+8; };
    uint8 codes[8]{}; uint64 left[8]{}; unsigned n=0;
    for(unsigned i=0;i<8;i++) if(regapScan[i] && !in(next,regapScan[i])) { codes[n]=regapScan[i]; left[n++]=regapLeft[i]; }
    for(auto code:presentedScan) if(code && !in(next,code)) {
        unsigned slot=0;
        while(slot<n && codes[slot]!=code) ++slot;
        if(slot==n) {
            if(n<8) ++n;
            else { slot=0; for(unsigned i=1;i<8;i++) if(left[i]<left[slot]) slot=i; }
        }
        codes[slot]=code; left[slot]=inputClockHz*kRegapMs/1000;
    }
    std::memcpy(regapScan,codes,8); std::memcpy(regapLeft,left,sizeof left);
}
void EmRegsVZDana::CycleSlowly(Bool sleeping) {
    EmRegsVZ::CycleSlowly(sleeping);
    ReportHeldKeys();
}
void EmRegsVZDana::ReportHeldKeys() {
    FeedBacklog();
    if(!stAsleep && !ebReported && hostPaced && heldCount && !queueCount && !pollIndex && InputReady()) QueueKeys();
    UpdateStIrq();
}
void EmRegsVZDana::ScaleCycles(uint32 factor) {
    inputLastCycle*=factor;
    EmRegsVZ::ScaleCycles(factor);
    if(cycleScaleSink) cycleScaleSink(factor);
    UpdateStIrq();
}

void EmRegsVZDana::MigrateLegacyInput() {
    heldCount=queueHead=queueCount=0;
    std::memset(heldScan,0,sizeof heldScan); std::memset(packets,0,sizeof packets);
    if(heldKeys&1) heldScan[heldCount++]=0x0f;
    if(heldKeys&2) heldScan[heldCount++]=0x70;
    if(packetIndex) {
        unsigned n=0;
        if(packetKeys&1) packets[0][n++]=0x0f;
        if(packetKeys&2) packets[0][n++]=0x70;
        queueCount=1;
    }
    if(keysDirty) QueueKeys();
    heldKeys=packetKeys=0; keysDirty=queueCount!=0;
}

// Common HALT always selects column F; DA/DB select the other columns.
bool EmRegsVZDana::StRequest() {
    return packetIndex || (!stAsleep && keysDirty && InputReady() && (presentedScan[0] || !RegapBlocked()));
}
void EmRegsVZDana::ResetPowerProtocol() {
    sleepCommand=0; stAsleep=0; ebHold=ebReported=rowGroups=0; pa3Option=1; fallingOnly=0;
}

// extra counts one more key as held, for Wakes.
unsigned EmRegsVZDana::SelectedRowGroups(uint8 extra) const {
    unsigned groups=0;
    unsigned count=std::min(heldCount,8u);
    for(unsigned i=0;i<=count;i++) {
        uint8 code=i<count ? heldScan[i] : extra;
        if(!code) continue;
        unsigned column=code&15, row=code>>4;
        if(column==15 || (stAsleep!=2 && !ebHold &&
           (wakeMask[column/8] & (1u<<(column%8)))))
            groups |= row<4 ? 1 : row==4 ? 2 : 4;
    }
    return groups;
}
void EmRegsVZDana::EnterHalt(bool shortPath) {
    stAsleep=shortPath ? 2 : 1;
    if(!shortPath) fallingOnly=1;
    rowGroups=SelectedRowGroups();
    // Keep an already presented header/payload. Discard only unstarted reports.
    // A repeat being clocked reads presentedScan, so its queue head is unstarted too.
    queueCount=packetIndex && !repeatReply ? 1 : 0;
    if(!queueCount) queueHead=0;
    for(unsigned i=0;i<32;i++) if(!queueCount || i!=queueHead) std::memset(packets[i],0,8);
    keysDirty=queueCount!=0;
    if(shortPath && !fallingOnly && rowGroups) {
        stAsleep=0; rowGroups=0; pa3Option=1;
        QueueKeys(); if(ebHold) ebReported=1;
    }
    if(!pollIndex && !(packetIndex && repeatReply)) std::memset(presentedScan,0,8);
    UpdateStIrq();
}

// Dana ROM HwrSoundOn uses PWM1 single samples on PB7/PWMO1. PWM2 is idle.
// MC68VZ328UM 15.4: SYSCLK/CLK32, prescaler+1, 2<<CLKSEL, PERIOD+2.
void EmRegsVZDana::DispatchPwmChange() {
    const uint16 control=StdRead(0xfffff500,2);
    if(!pwmActive || !(control&0x10) || GetAsleep() || (StdRead(0xfffff40b,1)&0x80)) {
        EmHAL::onPwmChange.Dispatch(-1,-1);
        return;
    }
    const unsigned period=std::min(256u,StdRead(0xfffff504,1)+2);
    const unsigned sample=StdRead(0xfffff503,1);
    const uint32 clock=(control&0x8000) ? 32768 : GetSystemClockFrequency();
    const double frequency=double(clock)/(((control>>8)&127)+1)/(2u<<(control&3))/period;
    EmHAL::onPwmChange.Dispatch(frequency<=20000 ? frequency : -1,
                                 double(std::min(sample,period))/period);
}
void EmRegsVZDana::SoundControlWrite(emuptr a,int size,uint32 value) {
    StdWrite(a,size,value); if(!(StdRead(0xfffff500,2)&0x10)) pwmActive=false;
    DispatchPwmChange();
}
void EmRegsVZDana::SoundClockWrite(emuptr a,int size,uint32 value) {
    pllRegisterWrite(a,size,value);
    DispatchPwmChange();
}
void EmRegsVZDana::SoundMuxWrite(emuptr a,int size,uint32 value) {
    StdWrite(a,size,value);
    SyncCards();
    DispatchPwmChange();
}

void EmRegsVZDana::TurnSoundOff() {
    EmRegsVZ::TurnSoundOff();
    pwmActive=false;
    DispatchPwmChange();
}

// Dana DrvControl 102e028c enables UART2 SIR and drives PG4 low.
Bool EmRegsVZDana::GetLineDriverState(EmUARTDeviceType type) {
    return type==kUARTIR && (StdRead(0xfffff430,1)&0x10) &&
        (StdRead(0xfffff433,1)&0x10) && !(StdRead(0xfffff431,1)&0x10) &&
        !(StdRead(0xfffff43b,1)&0x30) && (StdRead(0xfffff918,2)&0x20);
}
EmUARTDeviceType EmRegsVZDana::GetUARTDevice(int uart) {
    return uart==1 && GetLineDriverState(kUARTIR) ? kUARTIR : kUARTNone;
}


bool EmRegsVZDana::CardPowered(unsigned slot) {
    emuptr base=slot ? 0xfffff440 : 0xfffff408;
    return (StdRead(base,1)&StdRead(base+1,1)&StdRead(base+3,1)&0x20)!=0;
}
bool EmRegsVZDana::CardSelected(unsigned slot) {
    emuptr base=slot ? 0xfffff428 : 0xfffff438;
    uint8 mask=slot ? 4 : 8;
    return CardPowered(slot) && (StdRead(base,1)&StdRead(base+3,1)&mask) &&
           !(StdRead(base+1,1)&mask);
}
void EmRegsVZDana::SyncCards() {
    for(unsigned slot=0;slot<2;slot++) {
        auto& sd=slot ? sd1 : sd0;
        if(!CardPowered(slot)) sd.Reset();
        else if(CardSelected(slot)) sd.Enable();
        else sd.Disable();
    }
}
void EmRegsVZDana::CardControlWrite(emuptr a,int size,uint32 value) {
    StdWrite(a,size,value);
    SyncCards();
}
uint16 EmRegsVZDana::CardBus::DoExchange(uint16 control,uint16 data) {
    board.SyncCards();
    // PJ0/1/2 must be assigned to SPI1. Both selected cards receive the clocks.
    unsigned bits=(control&15)+1;
    uint16 result=bits==16 ? 0xffff : (1u<<bits)-1;
    if(board.StdRead(0xfffff43b,1)&7) return result;
    if(bits!=8 && bits!=16) return result;
    if(board.CardSelected(0)) result &= board.sd0.DoExchange(control,data);
    if(board.CardSelected(1)) result &= board.sd1.DoExchange(control,data);
    return result;
}
bool EmRegsVZDana::SupportsImageInSlot(EmHAL::Slot slot,uint32 blocks) {
    return (slot==EmHAL::Slot::sdcard || slot==EmHAL::Slot::sdcard2) &&
           EmSPISlaveSD::IsSizeRepresentable(blocks);
}
void EmRegsVZDana::Mount(EmHAL::Slot slot,CardImage&) {
    if(slot!=EmHAL::Slot::sdcard && slot!=EmHAL::Slot::sdcard2) return;
    (slot==EmHAL::Slot::sdcard ? sd0 : sd1).Reset();
    fPortDEdge |= slot==EmHAL::Slot::sdcard ? 4 : 8;
    UpdatePortDInterrupts();
}
void EmRegsVZDana::Unmount(EmHAL::Slot slot) {
    if(slot!=EmHAL::Slot::sdcard && slot!=EmHAL::Slot::sdcard2) return;
    (slot==EmHAL::Slot::sdcard ? sd0 : sd1).Reset();
    uint8 mask=slot==EmHAL::Slot::sdcard ? 4 : 8;
    fPortDEdge |= mask;
    UpdatePortDInterrupts();
}
void EmRegsVZDana::Remount(EmHAL::Slot slot,CardImage&) {
    // Restore preserves protocol cursors; explicit insertion uses Mount.
    if(slot==EmHAL::Slot::sdcard || slot==EmHAL::Slot::sdcard2) UpdatePortDInterrupts();
}
