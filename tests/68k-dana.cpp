#include <cassert>
#include <cmath>
#include "EmHAL.h"
#include "ExternalStorage.h"
#include "CardVolume.h"
#include <cstring>
#include <cstdio>
#include <vector>
#include <stdexcept>
#include "EmRegsISPDana.h"
#include "UsbDanaPort.h"
#include "UsbHostBridge.h"
#include "EmBankRegs.h"
#include "PeripheralGuards.h"
#include "EmRegsVZDana.h"
#include "EmUARTDragonball.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"
#include "savestate/ChunkHelper.h"
class Isp : public EmRegsISPDana {
 public:
    uint64 now=0;
    Isp() { SetSubBankHandlers(); Reset(true); }
    uint64 CurrentCycle() const override { return now; }
    uint64 ClockHz() const override { return 1000000; }
    void Command(uint16 c) { SetWord(0x1f000002,c); }
    void Data(uint16 d) { SetWord(0x1f000000,d); }
    uint16 Data() { return GetWord(0x1f000000); }
    void DC(uint16 c) { SetWord(0x1f000006,c); }
    void DCData(uint16 d) { SetWord(0x1f000004,d); }
    uint16 DCData() { return GetWord(0x1f000004); }
};
template<class T> static std::vector<uint8> Save(T& m) {
    Savestate<ChunkType> s; assert(s.Save(m));
    auto* p=static_cast<uint8*>(s.GetBuffer());return {p,p+s.GetSize()};
}
template<class T> static bool Load(T& m,std::vector<uint8> s) {
    SavestateLoader<ChunkType> l;return l.Load(s.data(),s.size(),m);
}
static std::vector<uint8> LegacyIsp(std::vector<uint8> state) {
    state.resize(12+68*4);
    uint32 version=2, words=uint32((state.size()-12)/4);
    memcpy(state.data()+8,&words,4);memcpy(state.data()+12,&version,4);
    return state;
}
// Exercise the same board parser/state fields without VZ CPU/timer dependencies.
struct St : EmRegsVZDana {
    St() { SetSubBankHandlers(); }
    void Raw(emuptr address,int size,uint32 value) { StdWrite(address,size,value); }
    template<class T> void Save(T& s) {
        auto*c=s.GetChunk(ChunkType::regsVZDana); if(!c)return;
        SaveChunkHelper h(*c); StateFields(h); InputStateFields(h); PowerStateFields(h); PowerModeFields(h);
    }
    void Load(SavestateLoader<ChunkType>& s) {
        auto*c=s.GetChunk(ChunkType::regsVZDana);assert(c);
        LoadChunkHelper h(*c);StateFields(h);InputStateFields(h);PowerStateFields(h);PowerModeFields(h);
    }
    void LoadLegacy(SavestateLoader<ChunkType>& s) {
        auto*c=s.GetChunk(ChunkType::regsVZDana);assert(c);
        LoadChunkHelper h(*c);LegacyStateFields(h);MigrateLegacyInput();
    }
    uint16 Byte(uint8 b) { return DoExchange(7,b); }
    void ResetSt() { PortDataChanged('B',0,64);PortDataChanged('B',64,0); }
};
struct PowerSnapshot {
    std::vector<uint32> words;
    void Load(SavestateLoader<ChunkType>& s) {
        auto* c=s.GetChunk(ChunkType::regsVZDana); assert(c);
        while(!c->IsAtEnd()) words.push_back(c->Get32());
    }
};
struct LegacyInput {
    unsigned index;
    template<class T> void Save(T& s) {
        auto*c=s.GetChunk(ChunkType::regsVZDana);if(!c)return;
        for(unsigned value: {0u,0u,0u,0u,2u,3u,index,1u,0u,0u,3072u,50u,1u}) c->Put32(value);
    }
};
struct LegacyLoader : St {
    void Load(SavestateLoader<ChunkType>& s) { LoadLegacy(s); }
};
struct PacedSt : St {
    uint64 now=0;
    uint64 InputCycle() const override { return now; }
    uint64 InputHz() const override { return 1000; }
};
struct Sound : St {
    void Raw(emuptr a,int size,uint32 value) { StdWrite(a,size,value); }
    void Notify() { DispatchPwmChange(); }
};
struct Cards : St {
    uint64 clock=0;
    Cards() { systemCycles=0; nextTimerEventAfterCycle=~uint64(0); }
    uint16 Shift(uint16 value=255,unsigned bits=8) {
        SetWord(0xfffff704,0x600|unsigned(bits-1));
        SetWord(0xfffff702,value);
        SetWord(0xfffff704,0x700|unsigned(bits-1));
        assert(spi1TransferInProgress);
        assert(spi1Countdown==4*int(bits));
        clock+=4*bits; Cycle(clock,false);
        assert(!spi1TransferInProgress);
        return GetWord(0xfffff700);
    }
    void Control(emuptr a,uint8 v) { SetByte(a,v); }
    void Configure() {
        Raw(0xfffff408,1,0x20); Raw(0xfffff40b,1,0x20);
        Raw(0xfffff440,1,0x20); Raw(0xfffff443,1,0x20);
        Raw(0xfffff438,1,8); Raw(0xfffff43b,1,8);
        Raw(0xfffff428,1,4); Raw(0xfffff42b,1,4);
        Control(0xfffff409,0x20); Control(0xfffff441,0x20);
        Control(0xfffff439,8); Control(0xfffff429,4);
    }
    uint8 Command(unsigned cmd) {
        Shift(0x40|cmd);for(int i=0;i<4;i++) Shift(0);Shift(0x95);
        assert(Shift()==255);return Shift();
    }
};
static void CardBusTest() {
    Cards board;
    auto* first=new uint8[4096]();auto* second=new uint8[4096]();
    first[0]=0x31;second[0]=0x72;
    assert(gExternalStorage.AddImage("dana0",first,4096));
    assert(gExternalStorage.AddImage("dana1",second,4096,true));
    assert(gExternalStorage.Mount("dana0",EmHAL::Slot::sdcard));
    assert(gExternalStorage.Mount("dana1",EmHAL::Slot::sdcard2));
    board.Configure();
    assert((board.GetPortInputValue('D')&15)==3);
    assert((board.GetPortInputValue('F')&0x81)==0x80);
    assert(board.GetPortInputValue('J')&2);
    assert(board.GetPortInternalValue('J')&2);
    assert(board.Shift()==255); assert(board.Shift(65535,16)==65535);
    board.Control(0xfffff439,0);assert(board.Command(0)==1);assert(board.Command(1)==0);
    board.Control(0xfffff439,8);board.Control(0xfffff429,0);
    assert(board.Command(0)==1);assert(board.Command(1)==0);
    // Both selected cards see a valid broadcast reset, with identical R1 bits.
    board.Control(0xfffff439,0);assert(board.Command(0)==1);
    assert(board.Command(1)==0);
    board.Control(0xfffff439,8);
    assert(board.Command(17)==0);assert(board.Shift()==255);assert(board.Shift()==254);
    assert(board.Shift()==0x72);for(int i=1;i<514;i++) board.Shift();
    // A protected card consumes a write but rejects it before touching backing bytes.
    assert(board.Command(24)==0);board.Shift(254);
    for(int i=0;i<514;i++) board.Shift(0x99);
    assert((board.Shift()&31)==13);assert(second[0]==0x72);
    auto* image=gExternalStorage.GetImageInSlot(EmHAL::Slot::sdcard2);
    uint8 value=1;assert(image->Write(&value,0,0)==0);
    assert(!image->WriteByteRange(&value,0,1));assert(image->DirtyPages()[0]==0);
    assert(!image->ReadByteRange(&value,size_t(-1),2));
    CardVolume volume(*image);
    for(bool format: {true,false}) {
        bool refused=false;
        try { if(format) volume.Format(); else volume.FixupPartitionType(); }
        catch(const std::runtime_error&) { refused=true; }
        assert(refused);assert(second[0]==0x72);assert(image->DirtyPages()[0]==0);
    }
    board.Control(0xfffff429,4);board.Control(0xfffff439,0);
    assert(board.Command(17)==0);assert(board.Shift()==255);assert(board.Shift()==254);
    assert(board.Shift()==0x31);for(int i=1;i<514;i++) board.Shift();
    assert(board.Command(24)==0);board.Shift(254);
    for(int i=0;i<514;i++) board.Shift(0x53);
    assert((board.Shift()&31)==5);board.Shift();
    for(int i=0;i<512;i++) assert(first[i]==0x53);
    assert(second[0]==0x72);
    // Power loss affects only the first card's initialization.
    board.Control(0xfffff409,0);assert(board.Shift()==255);
    board.Control(0xfffff409,0x20);assert(board.Command(17)==1);
    board.Control(0xfffff439,8);board.Control(0xfffff429,0);
    assert(board.Command(17)==0);
    // GPIO assignment of any SPI pin disconnects the shared controller.
    board.Control(0xfffff43b,9);assert(board.Shift()==255);
    board.Control(0xfffff43b,8);
    assert(gExternalStorage.Unmount(EmHAL::Slot::sdcard));
    assert((board.GetPortInputValue('D')&12)==4);
    assert(gExternalStorage.IsMounted(EmHAL::Slot::sdcard2));
    gExternalStorage.Clear();
}
static void UsbCancellationTest() {
    using R=EmRegsISPDana::PacketResult;
    const std::vector<uint8> setup={0,5,9,0,0,0,0,0};
    auto reg=[](Isp& d,unsigned c,uint16 v) { d.DC(c); d.DCData(v); };
    auto read=[](Isp& d,unsigned c) { d.DC(c); return d.DCData(); };
    auto config=[&](Isp& d) {
        const uint16 c[]={0x83,0xc3,0x81,0xc1,0xa3,0xe3,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63};
        for(unsigned i=0;i<16;++i) reg(d,0x20+i,c[i]);
    };
    auto start=[&](Isp& d) { reg(d,0xb8,9); d.SetVbus(true); d.BusReset(); config(d); };
    auto refuses=[](auto op) { bool caught=false; try { op(); } catch(const std::runtime_error&) { caught=true; } assert(caught); };
    auto usable=[&](Isp& d) {
        assert(d.HostOut(0,setup,false,true)==R::ack);
        d.DC(0x10);assert(d.DCData()==8);assert(d.DCData()==0x500);
        for(unsigned i=0;i<3;++i) d.DCData();
        d.DC(0xf4);d.DC(0x70);
        d.DC(1);d.DCData(2);d.DCData(0x1234);d.DC(0x61);
        std::vector<uint8> bytes;bool toggle=false;
        assert(d.HostIn(1,bytes,toggle)==R::ack && toggle);
        assert((bytes==std::vector<uint8>{0x34,0x12}));d.HostAckIn(1);
    };
    // SETUP, bus reset and reconnect at each length/data instruction boundary.
    for(unsigned event=0;event<4;++event) for(bool writing:{false,true}) for(unsigned split=0;split<=3;++split) {
        Isp d;start(d);reg(d,0xb6,0x87);
        d.DC(1);d.DCData(0);d.DC(0x61);
        { std::vector<uint8> bytes;bool toggle=false;assert(d.HostIn(1,bytes,toggle)==R::ack);d.HostAckIn(1); }
        assert(d.EffectiveAddress()==7);reg(d,0xb6,0x89);
        unsigned e=event==0 ? (writing?1:0) : (writing?3:2);
        if(!writing) assert(d.HostOut(e,writing?std::vector<uint8>{}:std::vector<uint8>{1,2,3,4},false)==R::ack);
        // A peer transaction and its interrupt survive a control SETUP flush.
        assert(d.HostOut(4,{0x77},false)==R::ack);
        d.DC(writing?e:0x10+e);
        for(unsigned n=0;n<split;++n) { if(writing)d.DCData(n?0x2211:4);else d.DCData(); }
        if(event==0) assert(d.HostOut(0,setup,false,true)==R::ack);
        if(event==1) d.BusReset();
        if(event==2) { auto saved=Save(d);Isp fresh;assert(Load(fresh,saved));assert(Load(d,Save(fresh)));d.SetVbus(true); }
        if(event==3) { d.SetVbus(false);d.SetVbus(true); }
        auto cancelled=Save(d);Isp copy;assert(Load(copy,cancelled));
        for(unsigned n=split;n<4;++n) { if(writing){ d.DCData(0xffff);copy.DCData(0xffff); }else{ assert(d.DCData()==0);assert(copy.DCData()==0); } }
        d.DC((writing?0x60:0x70)+e);
        if(event==0) {
            d.DC(0xf4);d.DC(0x61); // Stale Validate before observing fresh status.
            assert(read(d,0xd0)&4);assert(read(d,0xd4)&32);
            d.DC(0xc0);assert(d.DCData()&(1u<<12));
            assert(read(d,0xb7)==0x89);assert(d.EffectiveAddress()==7);

            std::vector<uint8> bytes;bool toggle=false;assert(d.HostIn(1,bytes,toggle)==R::nak);
        } else { config(d); }
        usable(d);
        // Malformed cancellation fields must retain even a live host connection.
        auto good=Save(d),bad=good;uint32 invalid=0x10000;
        const size_t cancelOffset=12+4*(85+5+16*(8+2*35)+1);
        memcpy(bad.data()+cancelOffset,&invalid,4);
        assert(!Load(d,bad));assert(Save(d)==good);assert(d.DeviceAttached());
        bad=cancelled;invalid=0;memcpy(bad.data()+cancelOffset+4,&invalid,4);
        assert(!Load(d,bad));assert(Save(d)==good);assert(d.DeviceAttached());
        bad=good;invalid=0xee;memcpy(bad.data()+12+4*8,&invalid,4);
        assert(!Load(d,bad));assert(Save(d)==good);assert(d.DeviceAttached());
    }
    // Impossible active read/write and SETUP combinations are rejected atomically.
    {
        Isp live;start(live);assert(live.HostOut(0,setup,false,true)==R::ack);
        live.DC(0x10);live.DCData();auto partial=Save(live);
        for(auto item:{std::pair<unsigned,uint32>{100,0},{95,0},{87,0}}) {
            auto bad=partial;memcpy(bad.data()+12+4*item.first,&item.second,4);
            assert(!Load(live,bad));assert(Save(live)==partial);assert(live.DeviceAttached());
        }
        live.DC(3);live.DCData(2);partial=Save(live);
        auto bad=partial;uint32 disabled=1;memcpy(bad.data()+12+4*72,&disabled,4);
        assert(!Load(live,bad));assert(Save(live)==partial);
    }
    // Reset after the ISR sees EP2, before its Read/Clear commands.
    Isp d;start(d);assert(d.HostOut(2,{1},false)==R::ack);
    d.DC(0xc0);assert(d.DCData()&0x400);d.BusReset();
    d.DC(0x12);assert(d.DCData()==0);d.DC(0x72);
    d.DC(3);d.DCData(4);d.DCData(1);d.DC(0x63);
    config(d);usable(d);
    // A stale ISR may read endpoint status between a host flush and FIFO access.
    for(bool reset:{false,true}) for(unsigned status:{0x50u,0xd0u,0xa0u}) for(bool in:{false,true}) {
        Isp x;start(x);unsigned e=in?3:2;
        if(in) {
            x.DC(e);x.DCData(2);x.DCData(0x1234);x.DC(0x60+e);
            std::vector<uint8> bytes;bool toggle=false;
            assert(x.HostIn(e,bytes,toggle)==R::ack);x.HostAckIn(e);
        } else assert(x.HostOut(e,{1},false)==R::ack);
        x.DC(0xc0);assert(x.DCData()&(1u<<(8+e)));
        if(reset) x.BusReset();else x.SetVbus(false);
        read(x,status+e);
        if(in) {
            x.DC(e);x.DCData(2);x.DCData(0xabcd);x.DC(0x60+e);
        } else {
            x.DC(0x10+e);assert(x.DCData()==0);x.DC(0x70+e);
        }
        auto saved=Save(x);Isp copy;assert(Load(copy,saved));
        x.SetVbus(true);config(x);usable(x);
    }
    // A new host event can retire the guard even without a full CPU buffer.
    for(unsigned status:{0x53u,0xd3u,0xa3u}) {
        Isp x;start(x);x.SetVbus(false);x.SetVbus(true);
        std::vector<uint8> bytes;bool toggle=false;
        assert(x.HostIn(3,bytes,toggle)==R::nak);read(x,status);
        auto before=Save(x);refuses([&]() { x.DC(0x63); });assert(Save(x)==before);
        x.DC(3);x.DCData(2);x.DCData(0x1234);x.DC(0x63);
        assert(x.HostIn(3,bytes,toggle)==R::ack && bytes==std::vector<uint8>({0x34,0x12}));
    }
    // Guest direction errors and unrelated endpoint validation still refuse.
    refuses([&]() { d.DC(2); });
    d.DC(3);d.DCData(2);d.DCData(1);refuses([&]() { d.DC(0x65); });
    { std::vector<uint8> bytes;bool toggle=false;assert(d.HostIn(5,bytes,toggle)==R::nak); }
    d.DC(0x63);
    // SETUP-gated commands record their command and discard the previous latch.
    assert(d.HostOut(0,setup,false,true)==R::ack);d.DC(1);d.DCData(0);d.DC(0x61);
    refuses([&]() { d.DCData(0); });d.DC(0xf4);d.DC(0x61);
    // Table 97: a first event is readable; only a second unread event sets bit7.
    d.DC(0x84);assert(d.HostOut(4,{1},false)==R::ack);assert(read(d,0xa4)==1);
    assert(d.HostOut(4,{2},true)==R::ack);assert(d.HostOut(4,{3},false)==R::nak);
    assert(read(d,0xa4)==0xd2);assert(read(d,0xa4)==0x52);
    d.DC(0x44);assert(d.HostOut(4,{3},false)==R::stall);assert(read(d,0xa4)==0x54);
    d.DC(0x84);assert(d.HostOut(4,{1},false)==R::ack);read(d,0xa4);
    assert(d.HostOut(4,{1},false)==R::ack);assert(read(d,0xa4)==30);
    assert(d.HostOut(4,{2},true)==R::ack);read(d,0xa4);
    assert(d.HostOut(4,{2},true)==R::ack);assert(read(d,0xa4)==0x5e);
    // v4 keeps its exact prefix, reconstructing the active write owner.
    d.DC(0x83);d.DC(3);d.DCData(2);
    auto legacy=Save(d);legacy.resize(12+4*(85+5+16*(8+2*35)+1));
    uint32 version=4,words=(legacy.size()-12)/4;
    memcpy(legacy.data()+12,&version,4);memcpy(legacy.data()+8,&words,4);
    Isp old;assert(Load(old,legacy));old.DCData(1);old.DC(0x63);
    // Fresh config ends the reset guard, including wrong-owner refusal.
    {
        Isp x;start(x);auto before=Save(x);
        refuses([&]() { x.DC(0x65); });assert(Save(x)==before);
        assert(x.HostOut(2,{},false)==R::ack);x.DC(0x72);
        assert(x.HostOut(2,{1},true)==R::ack);x.DC(0x12);
        assert(x.DCData()==1);assert(x.DCData()==1);x.DC(0x72);
        before=Save(x);refuses([&]() { x.DC(0x12); });assert(Save(x)==before);
    }
    // Each endpoint status path ends only the guard it actually observes.
    for(unsigned command:{0x50u,0xd0u,0xa0u,0xc0u}) {
        Isp x;start(x);assert(x.HostOut(0,setup,false,true)==R::ack);
        x.DC(0xf4);x.DC(0x70); // Pre-observation Clear must preserve SETUP.
        if(command==0xc0) { x.DC(command);assert(x.DCData()&0x100); }
        else read(x,command);
        x.DC(0x70);assert(!(read(x,0xd0)&32));
    }
    // EP8 is in the high C0 word: unrelated low bits cannot release its guard.
    {
        Isp x;start(x);
        const uint16 c[]={0x83,0xc3,0x81,0xc1,0xa3,0xe3,0x23,0x63,0x81,0x63,0x23,0x63,0x23,0x63,0x23,0x63};
        for(unsigned i=0;i<16;++i) reg(x,0x20+i,c[i]);
        x.SetVbus(false);x.SetVbus(true);
        assert(x.HostOut(8,{1},false)==R::ack);
        x.DC(0xc0);x.DCData();x.DC(0x78); // Low half did not observe EP8.
        assert(read(x,0xd8)&32); // Now observe and allow discard.
        x.DC(0x78);assert(x.HostOut(8,{2},true)==R::ack);
        x.SetVbus(false);x.SetVbus(true);assert(x.HostOut(8,{3},false)==R::ack);
        x.DC(0xc0);x.DCData();assert(x.DCData()&1);x.DC(0x78);
        assert(x.HostOut(8,{4},true)==R::ack);
    }
    // All accepted ordinary FIFO prefixes, including zero-length commands, reload.
    for(unsigned base:{0u,2u,4u}) for(unsigned length=0;length<=(base==2?8u:64u);++length) for(bool writing:{false,true}) {
        for(unsigned split=0;split<=1+(length+1)/2;++split) {
            Isp x;start(x);unsigned e=base+unsigned(writing);
            if(!writing) assert(x.HostOut(e,std::vector<uint8>(length,0x23),false)==R::ack);
            x.DC(writing?e:0x10+e);
            for(unsigned i=0;i<split;++i) { if(writing)x.DCData(i?0x2323:length);else x.DCData(); }
            Isp restored;assert(Load(restored,Save(x)));assert(Save(restored)==Save(x));
        }
    }
    // An IN command issued before ACK starts writing once the full buffer is freed.
    for(unsigned e:{3u,5u}) {
        Isp x;start(x);std::vector<uint8> bytes;bool toggle=false;
        x.DC(e);x.DCData(2);x.DCData(0x2211);x.DC(0x60+e);
        assert(x.HostIn(e,bytes,toggle)==R::ack);
        x.DC(e);
        if(e==5) { x.DCData(2);x.DCData(0x4433); }
        { Isp restored;assert(Load(restored,Save(x))); }
        x.HostAckIn(e);
        { Isp restored;assert(Load(restored,Save(x))); }
        if(e==3) { x.DCData(2);x.DCData(0x4433); }
        x.DC(0x60+e);assert(x.HostIn(e,bytes,toggle)==R::ack);
        assert((bytes==std::vector<uint8>{0x33,0x44}));
    }
    auto v4=[](Isp& x) {
        auto image=Save(x);image.resize(12+4*1339);uint32 v=4,n=1339;
        memcpy(image.data()+12,&v,4);memcpy(image.data()+8,&n,4);return image;
    };
    // v4's UNREAD bit alone carries pending status; remove it from the new image.
    for(bool unread:{false,true}) {
        Isp x;start(x);assert(x.HostOut(2,{1},false)==R::ack);
        assert(x.HostOut(2,{2},true)==R::nak);
        if(!unread) read(x,0xa2);
        Isp restored;assert(Load(restored,v4(x)));
        auto migrated=Save(restored);uint32 pending;
        memcpy(&pending,migrated.data()+12+4*1347,4);assert(pending==unsigned(unread));
        assert(read(restored,0xa2)==18);
        restored.SetVbus(true); // Fresh transaction has no inherited pending flag.
        assert(restored.HostOut(2,{3},false)==R::ack);assert(read(restored,0xa2)==1);
    }
    for(uint32 status:{1u,0x81u}) {
        Isp x;start(x);auto image=v4(x);
        memcpy(image.data()+12+4*(90+2*78+2),&status,4);
        Isp restored;assert(Load(restored,image));auto migrated=Save(restored);uint32 pending;
        memcpy(&pending,migrated.data()+12+4*1347,4);assert(pending==unsigned(status==0x81));
        assert(read(restored,0xa2)==1);
    }
    // Raw v4 host-flush images have neither cancellation nor write-owner fields.
    for(bool writing:{false,true}) for(unsigned event=0;event<3;++event) {
        Isp x;start(x);unsigned e=event ? (writing?3:2) : (writing?1:0);
        if(!writing) assert(x.HostOut(e,{1,2},false)==R::ack);
        x.DC(writing?e:0x10+e);if(writing)x.DCData(4);else x.DCData();
        if(event==1)x.BusReset();
        else if(event==2) { x.SetVbus(false);x.SetVbus(true); }
        else assert(x.HostOut(0,setup,false,true)==R::ack);
        auto image=v4(x);uint32 stale=2;memcpy(image.data()+12+4*86,&stale,4);
        Isp restored;assert(Load(restored,image));
        if(writing)restored.DCData(0xffff);else assert(restored.DCData()==0);
    }
    // Ordered ECR updates may temporarily exceed the old configuration's budget.
    const uint16 moved[]={0x83,0xc3,0xa3,0xe3,0x23,0x63,0x81,0xc1,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63};
    for(unsigned i=0;i<16;++i) reg(d,0x20+i,moved[i]);
    assert(d.HostOut(2,{1},false)==R::ack);
    // Dana rejects unsupported IRQ2 edge routes before modifying ICR.
    St board;uint16 icr=board.GetWord(0xfffff302);
    refuses([&]() { board.SetWord(0xfffff302,icr|0x400); });
    refuses([&]() { board.SetByte(0xfffff302,4); });
    assert(board.GetWord(0xfffff302)==icr);
    // No bus response is distinct from NAK on an enabled, empty endpoint.
    reg(d,0xb6,0);std::vector<uint8> bytes;bool toggle=false;
    assert(d.HostIn(3,bytes,toggle)==R::noAnswer);
    reg(d,0xb6,0x80);d.BusReset();
    assert(d.HostIn(3,bytes,toggle)==R::noAnswer);
    d.SetVbus(false);
    assert(d.HostIn(3,bytes,toggle)==R::noAnswer);
    assert(d.HostOut(0,setup,false,true)==R::noAnswer);
    puts("Dana USB wordwise host cancellation, stale ISR, write ownership and status cases passed");
}

static void UsbPacketTest() {
    using R=EmRegsISPDana::PacketResult;
    Isp dc; St board;
    board.Raw(0xfffff304,2,0xffff); board.Raw(0xfffff302,2,0);
    board.SetByte(0xfffff41b,0);
    dc.SetDeviceIrq([&](bool high) { board.SetDcIrqPin(high); });
    auto pending=[&]() { return board.GetWord(0xfffff310)&2; };
    auto reg=[&](unsigned c,uint16 v) { dc.DC(c); dc.DCData(v); };
    auto read=[&](unsigned c) { dc.DC(c); return dc.DCData(); };
    auto config=[&]() {
        const uint16 c[]={0x83,0xc3,0x81,0xc1,0xa3,0xe3,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63};
        for(unsigned i=0;i<16;++i) reg(0x20+i,c[i]);
    };
    auto refuses=[&](auto op) { bool caught=false; try { op(); } catch(const std::runtime_error&) { caught=true; } assert(caught); };
    assert(!pending()); reg(0xb8,9); dc.SetVbus(true);
    dc.DC(0xc2);dc.DCData(0x3f01);dc.DCData(0);
    dc.BusReset(); assert(pending()); assert(read(0xc0)==1); assert(!pending());
    config();
    // SETUP replaces control buffers, holds clear/validate until F4.
    std::vector<uint8> setup={0,5,7,0,0,0,0,0};
    assert(dc.HostOut(0,setup,false,true)==R::ack); assert(pending());
    assert(read(0xd0)&0x24); assert(pending());
    dc.DC(0xc2);dc.DCData(0);dc.DCData(0);assert(!pending());
    dc.DC(0xc2);dc.DCData(0x3f01);dc.DCData(0);assert(pending());
    dc.DC(0x70);assert(read(0x50)&0x24);assert(!pending());
    dc.DC(0x10);assert(dc.DCData()==8);assert(dc.DCData()==0x500);
    auto partial=Save(dc); Isp restored;assert(Load(restored,partial));
    assert(restored.DCData()==7);assert(restored.DCData()==0);assert(restored.DCData()==0);
    refuses([&]() { restored.DCData(); });
    // Malformed cursor rejects without changing any controller field.
    auto bad=partial; uint32 invalid=34; memcpy(bad.data()+12+4*86,&invalid,4);
    auto before=Save(restored);assert(!Load(restored,bad));assert(Save(restored)==before);
    for(auto item: {std::pair<unsigned,uint32>{85,16},{89,128},{90,2},{91,2},{98,65},{99,33}}) {
        bad=partial;memcpy(bad.data()+12+4*item.first,&item.second,4);
        assert(!Load(restored,bad));assert(Save(restored)==before);
    }
    auto v3=partial;v3.resize(12+85*4);uint32 ver=3,words=85;
    memcpy(v3.data()+12,&ver,4);memcpy(v3.data()+8,&words,4);
    Isp old;assert(Load(old,v3));old.DC(0x30);assert(old.DCData()==0x83);
    dc.DC(0xf4);dc.DC(0x70);reg(0xb6,0x87);
    dc.DC(1);dc.DCData(0);dc.DC(0x61);
    bool toggle=false;std::vector<uint8> bytes;
    assert(dc.EffectiveAddress()==0);assert(dc.HostIn(1,bytes,toggle)==R::ack);
    assert(bytes.empty() && toggle);assert(dc.EffectiveAddress()==0);
    assert(dc.HostIn(1,bytes,toggle)==R::ack);dc.HostAckIn(1);assert(dc.EffectiveAddress()==7);
    refuses([&]() { dc.HostAckIn(1); });read(0x51);
    // Odd data and incomplete writes resume in a fresh controller.
    dc.DC(3);dc.DCData(3);dc.DCData(0x2211);
    auto writePartial=Save(dc);Isp writer;assert(Load(writer,writePartial));
    writer.DCData(0xff33);writer.DC(0x63);
    dc.DCData(0xff33);dc.DC(0x63);
    assert(dc.HostIn(3,bytes,toggle)==R::ack);assert((bytes==std::vector<uint8>{0x11,0x22,0x33}));
    dc.HostAckIn(3);read(0x53);
    for(unsigned n=0;n<2;++n) { dc.DC(5);dc.DCData(1);dc.DCData(0x30+n);dc.DC(0x65); }
    refuses([&]() { dc.DC(5);dc.DCData(1); });
    for(unsigned n=0;n<2;++n) {
        assert(dc.HostIn(5,bytes,toggle)==R::ack);assert(bytes==std::vector<uint8>{uint8(0x30+n)});
        assert(toggle==bool(n));dc.HostAckIn(5);
    }
    read(0x55);
    // Two physical buffers accept two packets and NAK a third until CPU clear.
    assert(dc.HostOut(4,{1,2,3},false)==R::ack);
    assert(dc.HostOut(4,{4},true)==R::ack);
    assert(dc.HostOut(4,{5},false)==R::nak);
    dc.DC(0x14);assert(dc.DCData()==3);assert(dc.DCData()==0x201);assert(dc.DCData()==3);
    dc.DC(0x74);dc.DC(0x14);assert(dc.DCData()==1);assert(dc.DCData()==4);dc.DC(0x74);
    assert(dc.HostOut(4,{5},false)==R::ack);
    dc.DC(0x44);assert(dc.HostOut(4,{5},true)==R::stall);dc.DC(0x84);
    assert(dc.HostOut(4,{5},false)==R::ack);
    reg(0xb8,13);read(0x54);assert(dc.HostOut(4,{6},true)==R::ack);
    assert(dc.HostOut(4,{7},false)==R::nak);assert(read(0xa4)==0xd2);assert(read(0xa4)==0x52);
    // IRQ follows mask, read-clear, polarity and mux through the VZ path.
    assert(pending());board.SetByte(0xfffff41b,0x20);assert(!pending());
    board.SetByte(0xfffff41b,0);assert(pending());
    reg(0xb8,1);assert(!pending());reg(0xb8,9);assert(pending());read(0x54);assert(!pending());
    refuses([&]() { reg(0x20,0); });refuses([&]() { reg(0x22,0x84); });
    refuses([&]() { reg(0x22,0x93); });refuses([&]() { dc.DC(0xf0); });
    refuses([&]() { dc.DC(3);dc.DCData(17); });
    dc.DC(3);dc.DCData(3);dc.DCData(1);refuses([&]() { dc.DC(0x63); });
    // A configuration consuming more than 512 noniso bytes is refused.
    config();refuses([&]() { for(unsigned i=0;i<16;++i) reg(0x20+i,i<2 ? (i?0xc3:0x83) : 0xa3); });
    dc.SetVbus(false);assert(!pending());assert(!dc.DeviceAttached());
    dc.SetVbus(true);dc.BusReset();assert(dc.EffectiveAddress()==0);assert(read(0xb7)==0x87);
    dc.DC(0xb0);dc.DCData(0);refuses([&]() { dc.DCData(); });
    refuses([&]() { dc.DC(0xee); });dc.DC(0xf6);assert(!pending());
    reg(0xba,1);board.SetWord(0xfffff302,0x4000);assert(!pending());
    reg(0xb8,9);dc.DC(0xc2);dc.DCData(1);dc.DCData(0);
    dc.BusReset();assert(pending());read(0xc0);assert(!pending());
    board.SetWord(0xfffff302,0);reg(0xba,0);assert(!pending());
}

static void UsbAdapterTest() {
    using R=UsbPacketResult;
    Isp dc; UsbDanaPort adapter(dc); auto port=adapter.Port();
    auto reg=[&](unsigned c,uint16 v) { dc.DC(c);dc.DCData(v); };
    auto config=[&] { const uint16 c[]={0x83,0xc3,0x81,0xc1,0xa3,0xe3,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63};
        for(unsigned i=0;i<16;++i) reg(0x20+i,c[i]); };
    uint8_t setup[]={0,5,7,0,0,0,0,0},bytes[64]{};unsigned size=0;
    reg(0xb8,9);adapter.SetCable(true);config();
    assert(port.Setup(setup)==R::Timeout);assert(port.Reset());config();
    assert(port.Setup(setup)==R::Ack);dc.DC(0xf4);dc.DC(0x70);reg(0xb6,0x87);
    dc.DC(1);dc.DCData(0);dc.DC(0x61);
    assert(port.PeekIn(0,&size)==R::Ack && size==0 && port.Address()==0);
    assert(port.PeekIn(0,&size)==R::Ack && port.Address()==0);
    assert(port.In(bytes,&size)==R::Ack && port.Address()==7);
    assert(!dc.TryHostAckIn(1));
    dc.DC(3);dc.DCData(1);dc.DCData(42);dc.DC(0x63);
    assert(port.PeekIn(0x82,&size)==R::Ack && size==1);
    assert(!dc.TryHostAckIn(3)); // Peeking creates no ACK ownership.
    dc.DC(0x83); // Guest clears the IN endpoint.
    assert(Load(dc,Save(dc)));
    adapter.SetCable(true);assert(port.Reset());config();
    dc.DC(3);dc.DCData(1);dc.DCData(43);dc.DC(0x63);
    assert(port.PeekIn(2,&size)==R::Ack && size==1);
    assert(port.PeekIn(2,&size)==R::Ack && size==1);
    assert(port.EpIn(2,bytes,&size)==R::Ack && bytes[0]==43);
    assert(port.EpIn(2,bytes,&size)==R::Nak && !dc.TryHostAckIn(3));
    dc.DC(3);dc.DCData(1);dc.DCData(44);dc.DC(0x63);
    dc.DC(0x83);assert(!dc.TryHostAckIn(3));
    assert(port.EpIn(0x82,bytes,&size)==R::Nak);
    assert(port.EpOut(1,bytes,17)==R::Timeout);
    assert(port.EpOut(1,setup,1)==R::Ack);assert(port.EpOut(1,setup,1)==R::Nak);
    adapter.SetCable(false);assert(port.EpIn(0x82,bytes,&size)==R::Timeout);
    adapter.SetCable(true);assert(port.Setup(setup)==R::Timeout);
    assert(port.Reset());config();assert(port.Setup(setup)==R::Ack);
    auto state=Save(dc);assert(Load(dc,state));
    assert(port.Setup(setup)==R::Timeout);
    adapter.SetCable(true);assert(port.Setup(setup)==R::Timeout);
    assert(port.Reset());config();assert(port.Setup(setup)==R::Ack);
    // The guest descriptor's OUT3 uses physical FIFO4, followed by IN4 at FIFO5.
    for(unsigned n=0;n<64;++n) bytes[n]=n;
    assert(port.EpOut(3,bytes,64)==R::Ack);
    dc.DC(0x14);assert(dc.DCData()==64);
    for(unsigned n=0;n<32;++n) assert(dc.DCData()==(2*n | ((2*n+1)<<8)));
    dc.DC(0x74);
    // Both physical IN buffers can be peeked repeatedly and consumed once each.
    for(unsigned n=0;n<2;++n) { dc.DC(5);dc.DCData(1);dc.DCData(50+n);dc.DC(0x65); }
    for(unsigned n=0;n<2;++n) {
        assert(port.PeekIn(4,&size)==R::Ack && size==1);
        assert(port.PeekIn(4,&size)==R::Ack && !dc.TryHostAckIn(5));
        Isp restored;assert(Load(restored,Save(dc)));
        assert(port.EpIn(4,bytes,&size)==R::Ack && bytes[0]==50+n);
    }
    assert(port.EpIn(4,bytes,&size)==R::Nak);
    assert(port.Reset());
    const uint16 wrong[]={0x83,0xc3,0xc1,0x81,0xa3,0xe3,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63};
    for(unsigned i=0;i<16;++i) reg(0x20+i,wrong[i]);
    assert(port.EpOut(1,setup,1)==R::Timeout);
    assert(port.EpIn(2,bytes,&size)==R::Timeout);
    assert(port.EpIn(1,bytes,&size)==R::Nak);
    assert(port.EpOut(2,setup,1)==R::Ack);
    dc.DC(0x13);assert(dc.DCData()==1);dc.DCData();dc.DC(0x73);
    // Noncontrol endpoint14 is physical FIFO15 and can have either direction.
    for(unsigned i=0;i<16;++i) reg(0x20+i,i==15?0x81:wrong[i]);
    assert(port.EpOut(14,setup,1)==R::Ack);
    dc.DC(0x1f);assert(dc.DCData()==1);dc.DCData();dc.DC(0x7f);
    for(unsigned i=0;i<16;++i) reg(0x20+i,i==15?0xc1:wrong[i]);
    dc.DC(15);dc.DCData(1);dc.DCData(99);dc.DC(0x6f);
    assert(port.EpIn(14,bytes,&size)==R::Ack && size==1 && bytes[0]==99);
    assert(port.EpIn(15,bytes,&size)==R::Unsupported);
    assert(port.EpOut(15,setup,1)==R::Unsupported);
    puts("Dana shared USB adapter reset/reattach/address/lateACK tests passed");
}

static void UsbBankLifetimeTest() {
    UsbHostBridge bridge;
    std::unique_ptr<UsbDanaPort> adapter;
    UsbHostHooks hooks{};
    bool disposed=false;
    class Bank : public Isp {
     public:
        std::function<void()> check;
        void Dispose() override { check(); }
        ~Bank() override { check(); }
    };
    auto* bank=new Bank;
    bank->check=[&] { assert(!adapter && !hooks.context); disposed=true; };
    EmBankRegs::AddSubBank(bank);
    adapter=std::make_unique<UsbDanaPort>(*bank);
    std::string error;
    assert(bridge.Start({[&] { assert(!disposed);return adapter->Port(); },
        [&](bool b) { assert(!disposed);adapter->SetCable(b); },
        [] { return false; },[] { return false; },[] { return false; },
        [] { return uint64_t(0); },[] {},
        [&](const UsbHostHooks* h) { hooks=h?*h:UsbHostHooks{}; }}, {}, error));
    auto h=EmBankRegs::onBeforeDispose.AddHandler([&] { bridge.Stop();hooks={};adapter.reset(); });
    EmBankRegs::Dispose();
    assert(disposed);
    bridge.Service();bridge.Stop();
    EmBankRegs::onBeforeDispose.RemoveHandler(h);
    puts("Dana bank owner disposes bridge references before bank Dispose and delete");
}
int main() {
    UsbBankLifetimeTest();
    UsbAdapterTest();
    UsbPacketTest();
    UsbCancellationTest();
    CardBusTest();
    {
        St board;
        board.Raw(0xfffff430,1,0x10); board.Raw(0xfffff433,1,0x10);
        board.Raw(0xfffff431,1,0); board.Raw(0xfffff43b,1,0xcf);
        board.Raw(0xfffff918,2,0x20);
        assert(board.GetLineDriverState(kUARTIR));
        assert(board.GetUARTDevice(1)==kUARTIR && board.GetUARTDevice(0)==kUARTNone);
        assert(!board.GetLineDriverState(kUARTSerial));
        board.Raw(0xfffff431,1,0x10); assert(!board.GetLineDriverState(kUARTIR));
        board.Raw(0xfffff431,1,0); board.Raw(0xfffff918,2,0); assert(!board.GetLineDriverState(kUARTIR));
        board.Raw(0xfffff918,2,0x20); board.Raw(0xfffff43b,1,0xdf); assert(!board.GetLineDriverState(kUARTIR));
        board.Raw(0xfffff43b,1,0xcf); board.Raw(0xfffff430,1,0); assert(!board.GetLineDriverState(kUARTIR));
        EmUARTDragonball uart(EmUARTDragonball::kUART_DragonballVZ,1);
        EmUARTDragonball::PendingState pending;
        pending.rxCount=pending.txCount=64; pending.receiving=true;
        for(unsigned i=0;i<64;i++) { pending.rx[i]=i; pending.tx[i]=255-i; }
        assert(uart.RestorePending(pending));
        for(unsigned repeat=0;repeat<2;repeat++) {
            auto actual=uart.CapturePending();
            assert(actual.rxCount==64 && actual.txCount==64 && actual.receiving);
            assert(!std::memcmp(actual.rx,pending.rx,64) && !std::memcmp(actual.tx,pending.tx,64));
        }
        auto invalid=pending;invalid.rxCount=65;assert(!uart.RestorePending(invalid));
        invalid=pending;invalid.txCount=63;assert(!uart.RestorePending(invalid));
        assert(uart.CapturePending().rxCount==64);
        assert(uart.RestorePending({}));assert(!uart.CapturePending().rxCount);
    }

    {
        Sound sound;
        double hz=0,duty=0;
        auto handler=EmHAL::onPwmChange.AddHandler([&](double f,double d){ hz=f; duty=d; });
        sound.Raw(0xfffff200,2,0); sound.Raw(0xfffff202,2,0);
        sound.Raw(0xfffff40b,1,0x7f);
        const auto pll=sound.GetWord(0xfffff202);
        assert((sound.GetWord(0xfffff202)^pll)==0x8000);
        sound.SetWord(0xfffff500,0x10); sound.SetByte(0xfffff504,254);
        sound.SetByte(0xfffff503,64);
        assert(hz==960 && duty==.25); // SYSCLK=491520, divide by 2 and 256.
        sound.SetByte(0xfffff500,1); assert(hz==480 && duty==.25);
        sound.SetByte(0xfffff500,0); assert(hz==960 && duty==.25);
        sound.SetWord(0xfffff500,0x8010); assert(hz==64);
        sound.SetWord(0xfffff500,0xff13);
        assert(hz==.0625); // CLK32 / 128 / 16 / 256.
        sound.SetByte(0xfffff40b,0xff); assert(hz<0);
        sound.SetByte(0xfffff40b,0x7f); assert(hz==.0625);
        sound.Raw(0xfffff200,2,8); sound.Notify(); assert(hz<0);
        sound.Raw(0xfffff200,2,0); sound.Notify(); assert(hz==.0625);
        sound.SetWord(0xfffff500,0x8010);sound.SetByte(0xfffff504,0);
        sound.SetByte(0xfffff503,255); assert(hz==8192 && duty==1);
        sound.SetWord(0xfffff500,0x10); assert(hz<0); // Above audio band silences the old tone.
        sound.SetByte(0xfffff501,0); assert(hz<0);
        EmHAL::onPwmChange.RemoveHandler(handler);
    }

    // Each byte of the Halt request, including the two mask bytes, survives save/load.
    const uint8 halt[]={ '1','2','3',' ','H','a','l','t',0,0x80 };
    for(unsigned cut=0;cut<=10;cut++) {
        St power; assert(power.Byte(0)==0xaa);
        for(unsigned i=0;i<cut;i++) assert(power.Byte(halt[i])==0);
        auto partial=Save(power); St resumed; assert(Load(resumed,partial));
        for(unsigned i=cut;i<10;i++) assert(resumed.Byte(halt[i])==0);
        assert(resumed.DispatchScanCode(0x2b,true)); // A cannot wake mask1 bit7.
        assert(resumed.Byte(0)==0);
        assert(resumed.GetPortInternalValue('D')&0x10);
        assert(resumed.DispatchScanCode(0x2b,false));
        assert(resumed.DispatchScanCode(0x0f,true));
        assert(!(resumed.GetPortInternalValue('D')&0x10));
        assert(resumed.Byte(0)==0x41);assert(resumed.Byte(0)==0x0f);
        assert(resumed.DispatchScanCode(0x0f,false));assert(resumed.Byte(0)==0x40);
        resumed.ResetSt();assert(resumed.Byte(0xef)==0xaa);
    }
    for(uint8 mode: {0xeb,0xed,0xee,0xef}) {
        for(unsigned cut=1;cut<10;cut++) {
            St m; m.Byte(0);
            for(unsigned i=0;i<cut;i++) m.Byte(halt[i]);
            auto before=Save(m); bool refused=false;
            try { m.Byte(mode); } catch(const std::runtime_error&) { refused=true; }
            assert(refused && Save(m)==before);
        }
        for(unsigned cut=1;cut<12;cut++) {
            St m; m.Byte(0);
            const char* request="Get Ver!";
            for(unsigned i=0;i<cut;i++) m.Byte(i<8 ? request[i] : 0);
            auto before=Save(m); bool refused=false;
            try { m.Byte(mode); } catch(const std::runtime_error&) { refused=true; }
            assert(refused && Save(m)==before);
        }
    }
    // Byte-backed EB: column F wakes, full scans use column order and suppress
    // later held snapshots until every physical key has released.
    for(uint8 mode: {0xeb,0xed,0xee,0xef}) {
        St m; assert(m.Byte(0)==0xaa);
        for(auto b:halt) m.Byte(b);
        m.Byte(mode);
        auto asleep=Save(m); St n; assert(Load(n,asleep));
        n.DispatchScanCode(0x6e,true); assert(n.Byte(0)==0);
        n.DispatchScanCode(0x0f,true);
        assert(n.Byte(0)==0x42); assert(n.Byte(0)==0x6e);
        auto partial=Save(n); St q; assert(Load(q,partial));
        assert(q.Byte(0)==0x0f);
        q.DispatchScanCode(0x2b,true);
        if(mode==0xeb) assert(q.Byte(0)==0);
    }
    {
        St m; m.Byte(0); m.Byte(0xeb);
        m.DispatchScanCode(0x0f,true); assert(m.Byte(0)==0x41); assert(m.Byte(0)==0x0f);
        m.DispatchScanCode(0x6e,true); assert(m.Byte(0)==0);
        m.DispatchScanCode(0x0f,false); assert(m.Byte(0)==0);
        m.DispatchScanCode(0x6e,false); assert(m.Byte(0)==0x40);
        // Held rows are drained at entry. Another key in the same IRQ group
        // cannot wake until the group releases and falls again.
        m.DispatchScanCode(0x0f,true); m.Byte(0xeb);
        assert(m.Byte(0)==0); m.DispatchScanCode(0x0f,false);
        // Use selected column B to exercise two physical keys in group 0..3.
        m.Byte('1');m.Byte('2');m.Byte('3');m.Byte(' ');m.Byte('H');m.Byte('a');m.Byte('l');m.Byte('t');m.Byte(0);m.Byte(8);
        m.ResetSt(); m.Byte(0); // MCU reset clears D8 while retaining column B.
        m.DispatchScanCode(0x0b,true); m.Byte(0xef);
        m.DispatchScanCode(0x2b,true); assert(m.Byte(0)==0);
        m.DispatchScanCode(0x0b,false); assert(m.Byte(0)==0);
        m.DispatchScanCode(0x2b,false); m.DispatchScanCode(0x0f,true);
        assert(m.Byte(0)==0x41); assert(m.Byte(0)==0x0f);
    }
    // EE retains the low-level IRQ option after reset; common HALT drains it.
    {
        St m; m.Byte(0); m.DispatchScanCode(0x0f,true); m.Byte(0xee);
        assert(m.Byte(0)==0x41); assert(m.Byte(0)==0x0f);
        m.Byte(0xef); assert(m.Byte(0)==0);
        m.Byte(0xee); assert(m.Byte(0)==0); // Common HALT's falling edge survives EE.
        m.DispatchScanCode(0x0f,false);m.DispatchScanCode(0x0f,true);
        assert(m.Byte(0)==0x41); assert(m.Byte(0)==0x0f);
    }
    for(uint8 mode: {0xeb,0xed,0xee,0xef}) {
        for(unsigned cut: {1u,2u}) {
            St m; m.Byte(0); m.DispatchScanCode(0x6e,true);m.DispatchScanCode(0x0f,true);
            // Consume the Enter-only packet, then present the full chord.
            assert(m.Byte(0)==0x41);assert(m.Byte(0)==0x6e);
            assert(m.Byte(0)==0x42);
            if(cut==2) assert(m.Byte(0)==0x6e);
            m.Byte(mode);auto pending=Save(m);St n;assert(Load(n,pending));
            if(cut==1) assert(n.Byte(0)==0x6e);
            assert(n.Byte(0)==0x0f);
        }
        St ack;ack.ResetSt();assert(ack.Byte(mode)==0xaa);
        assert(ack.Byte(0)==0); // The command never invents another reset ack.
    }
    {
        St m; m.Byte(0); for(auto b:halt) m.Byte(b);
        m.Byte(0xeb); m.Byte(0xed); m.Byte(0xee);
        PowerSnapshot state; assert(Load(state,Save(m)));
        auto& w=state.words;
        assert(w[298]==0x80 && w[299]==2); // Retained mask and short HALT.
        assert(w[300]==1 && w[301]==0 && w[303]==0 && w[304]==1);
        m.DispatchScanCode(0x0f,true);
        PowerSnapshot awake; assert(Load(awake,Save(m)));
        assert(awake.words[303]==1 && awake.words[304]==0);
        m.ResetSt();PowerSnapshot reset;assert(Load(reset,Save(m)));
        assert(reset.words[298]==0x80 && reset.words[300]==0);
    }
    St dark; dark.SetByte(0xfffff441,0x80);assert(dark.GetLCDScreenOn());assert(!dark.GetLCDBacklightOn());
    dark.SetByte(0xfffff441,0x10);assert(!dark.GetLCDScreenOn());assert(dark.GetLCDBacklightOn());

    for(unsigned index: {1u,2u}) {
        LegacyInput fixture{index}; LegacyLoader migrated;
        auto data=Save(fixture);assert(Load(migrated,data));
        if(index==1) assert(migrated.Byte(0)==0x0f);
        assert(migrated.Byte(0)==0x70);
        assert(migrated.Byte(0)==0x41); assert(migrated.Byte(0)==0x70);
        assert(migrated.Byte(0)==0);
    }
    PacedSt paced; assert(paced.Byte(0)==0xaa);
    auto scan=*DanaScan5::Named("a");
    assert(paced.DispatchDanaInput({{scan,true},{scan,false}}));
    assert(paced.Byte(0)==0x41);assert(paced.Byte(0)==scan.Code());
    auto waiting=Save(paced);
    paced.now=19; assert(paced.Byte(0)==0x41); assert(paced.Byte(0)==scan.Code());
    paced.now=0; assert(Load(paced,waiting));
    paced.now=19; assert(paced.Byte(0)==0x41); assert(paced.Byte(0)==scan.Code());
    paced.now=20; assert(paced.Byte(0)==0x40);

    paced.now=40;
    assert(paced.DispatchDanaInput({{scan,true}}));
    assert(paced.Byte(0)==0x41);assert(paced.Byte(0)==scan.Code());
    paced.now=45; assert(paced.Byte(0)==0x41);
    assert(paced.DispatchDanaInput({{scan,false}}));
    auto idleReply=Save(paced);
    paced.now=80;assert(paced.Byte(0)==scan.Code());assert(paced.Byte(0)==0x40);
    paced.now=45;assert(Load(paced,idleReply));assert(paced.Byte(0)==scan.Code());
    paced.now=60;assert(paced.Byte(0)==0x40);
    paced.now=80;assert(paced.DispatchDanaInput({{scan,true}}));
    assert(paced.Byte(0)==0x41);assert(paced.Byte(0)==scan.Code());
    assert(paced.DispatchDanaInput({{scan,false}}));
    paced.ResetSt();assert(paced.Byte(0)==0xaa);assert(paced.Byte(0)==0x40);
    St reserved; assert(reserved.Byte(0)==0xaa);
    std::vector<DanaTransition5> fill;
    for(unsigned i=0;i<24;i++) fill.push_back({scan,(i%2)==0});
    assert(reserved.DispatchDanaInput(fill));
    for(const char* name: {"a","b","c","d"}) assert(reserved.DispatchDanaInput({{*DanaScan5::Named(name),true}}));
    auto full=Save(reserved);
    assert(!reserved.DispatchDanaInput({{*DanaScan5::Named("e"),true}}));assert(Save(reserved)==full);
    for(const char* name: {"a","b","c","d"}) assert(reserved.DispatchDanaInput({{*DanaScan5::Named(name),false}}));
    reserved.ReleaseDanaInput();assert(reserved.Byte(0)==0x40);
    const auto& dana=*deviceProfileNamed5("alphasmart-dana");
    const auto& iiix=*deviceProfileNamed5("palm-iiix");
    for(const auto* command: {"hotsync","serial-mode"})
        assert(!peripheralRefusal68k(dana,command,"on").empty());
    assert(peripheralRefusal68k(dana,"irda-mode","on").empty()==dana.capabilities.infrared);
    for(const auto* command: {"audio-on","sound-mode"})
        assert(peripheralRefusal68k(dana,command,"on").empty()==dana.capabilities.sound);
    for(const auto* command: {"audio-off","serial-mode","irda-mode","sound-mode"})
        assert(peripheralRefusal68k(dana,command,"off").empty());
    for(const auto* command: {"audio-on","hotsync","serial-mode","irda-mode","sound-mode"})
        assert(peripheralRefusal68k(iiix,command,"on").empty());
    auto silentDana=dana; silentDana.capabilities.sound=false;
    assert(peripheralRefusal68k(dana,"irda-mode","on").empty()==dana.capabilities.infrared);
    for(const auto* command: {"audio-on","sound-mode"})
        assert(!peripheralRefusal68k(silentDana,command,"on").empty());
    assert(dana.capabilities.sound && !dana.capabilities.fastAudio && !dana.capabilities.microphone);
    Isp isp;
    isp.Command(0x8d);isp.Data(0x3210);
    auto low=Save(isp);
    isp.Data(0x1234);isp.Command(0x0d);
    assert(isp.Data()==0x3210);assert(isp.Data()==0x1234);
    assert(Load(isp,low));isp.Data(0xabcd);isp.Command(0x0d);
    assert(isp.Data()==0x3210);
    auto readLow=Save(isp);
    assert(isp.Data()==0xabcd);assert(Load(isp,readLow));assert(isp.Data()==0xabcd);
    isp.Command(0x8d);isp.Data(0x1111);isp.Command(0x8d);isp.Data(0x2222);isp.Data(0x3333);
    isp.Command(0x0d);assert(isp.Data()==0x2222);assert(isp.Data()==0x3333);
    isp.Command(0x8d);isp.Data(0xffff);isp.Reset(true);isp.Command(0x0d);
    assert(isp.Data()==0x2edf);assert(isp.Data()==0);
    isp.DC(0x12b2);isp.DCData(0x9876);isp.DC(0x34b3);assert(isp.DCData()==0x9876);
    isp.DC(0xc0);assert(isp.DCData()==0);auto dcLow=Save(isp);
    isp.DC(0xb2);isp.DCData(1);assert(Load(isp,dcLow));assert(isp.DCData()==0);
    isp.DC(0xb3);assert(isp.DCData()==0x9876);
    isp.DC(0xc2);isp.DCData(0x1234);auto dcWrite=Save(isp);
    isp.DCData(0x56);isp.DC(0xc3);assert(isp.DCData()==0x1234);
    auto dcRead=Save(isp);assert(isp.DCData()==0x56);
    assert(Load(isp,dcRead));assert(isp.DCData()==0x56);
    assert(Load(isp,dcWrite));isp.DCData(0x78);isp.DC(0xc3);
    assert(isp.DCData()==0x1234);assert(isp.DCData()==0x78);
    isp.DC(0xc2);isp.DCData(0xffff);isp.Reset(true);isp.DC(0xc3);
    assert(isp.DCData()==0);assert(isp.DCData()==0);
    // Poll reads advance SOF from guest cycles, retaining fractional milliseconds.
    // DC register programming is separate from HC PTDs; no host is attached.
    isp.DC(0xb6);isp.DCData(0xffa5);isp.DC(0xb7);assert(isp.DCData()==0xa5);
    const uint16 startup[]={0x83,0xc3,0x81,0xc1,0xa3,0xe3,0x23,0x63,
                            0x23,0x63,0x23,0x63,0x23,0x63,0x23,0x63};
    for(unsigned i=0;i<16;i++) { isp.DC(0x20+i);isp.DCData(startup[i]); }
    for(unsigned i=0;i<16;i++) { isp.DC(0x30+i);assert(isp.DCData()==startup[i]); }
    // A command without its data word survives a fresh object restore.
    isp.DC(0x24);auto configPending=Save(isp);Isp fresh;
    assert(Load(fresh,configPending));fresh.DCData(0xffe3);
    fresh.DC(0x34);assert(fresh.DCData()==0xe3);
    isp.DCData(0xa3);isp.DC(0xb7);assert(isp.DCData()==0xa5);
    auto configured=Save(isp);
    for(auto item: {std::pair<unsigned,uint32>{68,256},{69,0x80},{70,0xc0},{71,256}}) {
        auto bad=configured;memcpy(bad.data()+12+4*item.first,&item.second,4);
        assert(!Load(isp,bad));assert(Load(isp,configured));assert(Save(isp)==configured);
    }
    // Public v2 chunks retain their partial HC/DC state and start with reset ECRs.
    auto legacy=LegacyIsp(configured);
    assert(Load(fresh,legacy));fresh.DC(0x30);assert(fresh.DCData()==0x83);
    fresh.DC(0x31);assert(fresh.DCData()==0xc3);
    fresh.DC(0x34);assert(fresh.DCData()==0);fresh.DC(0xb7);assert(fresh.DCData()==0);
    assert(Load(fresh,LegacyIsp(dcWrite)));fresh.DCData(0x78);fresh.DC(0xc3);
    assert(fresh.DCData()==0x1234);assert(fresh.DCData()==0x78);
    assert(Load(fresh,LegacyIsp(readLow)));assert(fresh.Data()==0xabcd);
    assert(Load(fresh,LegacyIsp(low)));fresh.Data(0xabcd);fresh.Command(0x0d);
    assert(fresh.Data()==0x3210);assert(fresh.Data()==0xabcd);
    {
    // Locked commands refuse immediately; hardware reset also works while locked.
    isp.DC(0xb0);isp.DCData(0);
    bool refused=false; try { isp.DC(0x24); } catch(const std::runtime_error&) { refused=true; }
    assert(refused); isp.DC(0xf6);isp.DC(0x34);assert(isp.DCData()==0);
    isp.Command(0xa8);isp.Data(0xbeef);isp.DC(0xf6);
    isp.Command(0x28);assert(isp.Data()==0xbeef);
    refused=false;try { isp.DC(0x10); } catch(const std::runtime_error&) { refused=true; }
    assert(refused);isp.Reset(true);
    }

    isp.Command(0x81);isp.Data(0x80);isp.Data(0);
    isp.now=1500;isp.Command(0x0f);assert(isp.Data()==1);assert(isp.Data()==0);
    auto fraction=Save(isp);
    isp.now=2000;isp.Command(0x0f);assert(isp.Data()==2);
    isp.now=1500;assert(Load(isp,fraction));isp.now=2000;isp.Command(0x0f);assert(isp.Data()==2);
    isp.Command(0x24);assert(isp.Data()==1);isp.Command(0xa4);isp.Data(1);isp.Command(0x24);assert(isp.Data()==0);
    // Malformed host command, latch counters, buffer sizes and fractional time.
    auto good=Save(isp);
    uint32 count=0,type=0;memcpy(&count,good.data(),4);memcpy(&type,good.data()+4,4);
    assert(count==1 && type==uint32(ChunkType::regsISPDana));
    for(auto item: {std::pair<unsigned,uint32>{0,6}, {3,1000000}, {5,256}, {6,2},
                   {7,1}, {8,256}, {16,1}, {17,2}, {20+0x2a,4097},
                   {20+0x2b,4095}, {20+0x15,1}, {20+0x16,2}}) {
        auto bad=good;
        memcpy(bad.data()+12+4*item.first,&item.second,4);
        assert(!Load(isp,bad));
        assert(Load(isp,good));assert(Save(isp)==good);
    }
    // A partial C0 read clears only read-to-clear bits in that word.
    auto pending=Save(isp);uint32 events=0x0056005b;
    memcpy(pending.data()+12+4*14,&events,4);assert(Load(isp,pending));
    isp.DC(0xc0);assert(isp.DCData()==0x5b);
    auto statusLow=Save(isp);assert(isp.DCData()==0x56);
    assert(Load(isp,statusLow));assert(isp.DCData()==0x56);
    isp.DC(0xc0);assert(isp.DCData()==0);assert(isp.DCData()==0x56);
    // Disconnected root ports report failed requests as CSC, never CCS/PES/PRS.
    isp.Reset(true);
    isp.Command(0x95);isp.Data(0x16);isp.Data(0);
    isp.Command(0x15);assert(isp.Data()==0);assert(isp.Data()==1);
    isp.Command(0x95);isp.Data(0);isp.Data(1);
    isp.Command(0x15);assert(isp.Data()==0);assert(isp.Data()==0);
    isp.Command(0xaa);isp.Data(1024);isp.Command(0xab);isp.Data(2048);
    auto lengths=Save(isp);
    bool refused=false;
    try { isp.Command(0xaa);isp.Data(1028); } catch(const std::runtime_error&) { refused=true; }
    assert(refused);auto afterRefusal=Save(isp);assert(Load(isp,afterRefusal));
    isp.Command(0x2a);assert(isp.Data()==1024);
    assert(Load(isp,lengths));
    isp.Command(0x82);isp.Data(1);isp.Data(0);
    isp.Command(2);assert(isp.Data()==1);assert(isp.Data()==0);
    auto resetting=Save(isp);
    isp.now+=9;isp.Command(2);assert(isp.Data()==1);
    isp.now++;isp.Command(2);assert(isp.Data()==0);
    isp.now-=10;assert(Load(isp,resetting));isp.now+=10;
    isp.Command(2);assert(isp.Data()==0);
    isp.Command(0xa9);isp.Data(0xf6);
    isp.Command(0x2a);assert(isp.Data()==0);
    St st;
    st.SetByte(0xfffff421,8);st.SetByte(0xfffff431,0);
    auto* defaults=st.GetSPI2Slave();
    for(auto sample: {std::pair<uint8,uint16>{0xa4,3072},{0xe4,50}}) {
        defaults->DoExchange(7,sample.first);
        uint16 hi=defaults->DoExchange(7,0),lo=defaults->DoExchange(7,0);
        assert(((hi<<8)|lo)==(sample.second<<3));
    }
    assert(st.Byte(0)==0xaa);assert(st.Byte(0)==0);
    // Polling K cannot create a second reset acknowledgement.
    for(int i=0;i<8;i++)st.GetPortInputValue('K');
    assert(st.Byte(0)==0);
    const char* request="Get Ver!";
    for(unsigned split=0;split<=8;split++) {
        st.ResetSt();assert(st.Byte(0)==0xaa);
        for(unsigned i=0;i<split;i++)assert(st.Byte(request[i])==0);
        auto partial=Save(st);
        st.ResetSt();assert(Load(st,partial));
        for(unsigned i=split;i<8;i++)assert(st.Byte(request[i])==0);
        for(unsigned i=0;i<4;i++) {
            auto reply=Save(st);auto expected=st.Byte(0);
            assert(expected==uint8("07.4"[i]));
            assert(Load(st,reply));assert(st.Byte(0)==expected);
        }
        assert(st.Byte(0)==0);
    }
    st.Byte('G');st.Byte('e');st.ResetSt();assert(st.Byte(0)==0xaa);assert(st.Byte(0)==0);
    st.PortDataChanged('B',0,64);assert(!(st.GetPortInputValue('K')&4));assert(st.Byte(0)==0);
    st.PortDataChanged('B',64,0);assert(st.GetPortInputValue('K')&4);assert(st.Byte(0)==0xaa);
    st.DispatchButtonEvent(ButtonEvent(ButtonEvent::Button::power,ButtonEvent::Type::press));
    assert(!(st.GetPortInternalValue('D')&16));assert(st.Byte(0)==0x41);
    auto keyReply=Save(st);assert(st.Byte(0)==0x0f);
    assert(st.GetPortInternalValue('D')&16);assert(Load(st,keyReply));assert(st.Byte(0)==0x0f);
    st.DispatchButtonEvent(ButtonEvent(ButtonEvent::Button::power,ButtonEvent::Type::release));
    assert(st.Byte(0)==0x40);assert(st.GetPortInternalValue('D')&16);
    st.DispatchButtonEvent(ButtonEvent(ButtonEvent::Button::rockerUp,ButtonEvent::Type::press));
    st.ResetSt();assert(st.Byte(0)==0xaa);assert(st.Byte(0)==0x41);assert(st.Byte(0)==0x70);
    // Preserve a complete chord and its releases while an earlier SPI reply is pending.
    st.DispatchScanCode(0x70,false); assert(st.Byte(0)==0x40);
    const uint8 chord[]={0x09,0x7b,0x13,0x46,0x2b,0x2c,0x2d,0x2a};
    for(auto code:chord) assert(st.DispatchScanCode(code,true));
    assert(!st.DispatchScanCode(0x2e,true));
    auto order=[](uint8 a,uint8 b) { return (a&15)==(b&15) ? a<b : (a&15)<(b&15); };
    for(unsigned n=1;n<=8;n++) {
        std::vector<uint8> expected(chord,chord+n); std::sort(expected.begin(),expected.end(),order);
        assert(st.Byte(0)==(0x40|n));
        for(unsigned i=0;i<n;i++) {
            auto partial=Save(st);
            assert(st.Byte(0)==expected[i]);
            assert(Load(st,partial)); assert(st.Byte(0)==expected[i]);
        }
    }
    assert(st.Byte(0)==0);
    for(auto code:chord) assert(st.DispatchScanCode(code,false));
    for(unsigned n=7;;n--) {
        assert(st.Byte(0)==(0x40|n));
        std::vector<uint8> expected(chord+8-n,chord+8); std::sort(expected.begin(),expected.end(),order);
        for(unsigned i=0;i<n;i++) assert(st.Byte(0)==expected[i]);
        if(!n) break;
    }
    // Full FIFO refuses before changing held state; retry keeps the transition.
    for(unsigned i=0;i<32;i++) assert(st.DispatchScanCode(0x2b,(i%2)==0));
    assert(!st.DispatchScanCode(0x2b,true));
    for(unsigned i=0;i<32;i++) {
        assert(st.Byte(0)==(i%2 ? 0x40 : 0x41));
        if(!(i%2)) assert(st.Byte(0)==0x2b);
    }
    assert(st.DispatchScanCode(0x2b,true)); assert(st.Byte(0)==0x41); assert(st.Byte(0)==0x2b);
    assert(st.DispatchScanCode(0x2b,false)); assert(st.Byte(0)==0x40);
    assert(!st.DispatchScanCode(0,true)); assert(!st.DispatchScanCode(0xff,true));
    // Typed batches refuse atomically, preserving all saved board state.
    auto a=*DanaScan5::Named("a"), shift=*DanaScan5::Named("lshift");
    std::vector<DanaTransition5> batch;
    for(unsigned i=0;i<24;i++) batch.push_back({a,(i%2)==0});
    assert(st.DispatchDanaInput(batch));
    auto before=Save(st);
    assert(!st.DispatchDanaInput(batch)); assert(Save(st)==before);
    for(unsigned i=0;i<24;i++) { assert(st.Byte(0)==(i%2?0x40:0x41)); if(!(i%2)) assert(st.Byte(0)==a.Code()); }
    assert(st.DispatchDanaInput({{shift,true},{a,true}}));
    assert(st.Byte(0)==0x41); // Preserve the partially clocked reply across focus loss.
    st.ReleaseDanaInput();
    auto released=Save(st); assert(Load(st,released));
    assert(st.Byte(0)==shift.Code()); assert(st.Byte(0)==0x40); assert(st.Byte(0)==0x40);
    assert(st.SetVirtualAnalogueInputs(0xabc,0x456,false));
    assert(!(st.GetPortInputValue('G')&2));
    assert(!st.SetVirtualAnalogueInputs(4096,0,true));
    // ST and ADC selection are independent. Three 8-bit transfers shift one 12-bit sample.
    st.SetByte(0xfffff421,8);st.SetByte(0xfffff431,0);
    auto* adc=st.GetSPI2Slave();
    adc->DoExchange(7,0xa4);
    uint16 mainHi=adc->DoExchange(7,0),mainLo=adc->DoExchange(7,0);
    assert(((mainHi<<8)|mainLo)==(0xabc<<3));
    adc->DoExchange(7,0xe4);
    uint16 supplyHi=adc->DoExchange(7,0),supplyLo=adc->DoExchange(7,0);
    assert(((supplyHi<<8)|supplyLo)==(0x456<<3));
    for(auto value: {0,2048,4095}) {
        assert(st.SetVirtualAnalogueInputs(value,4095-value,true));
        adc->DoExchange(7,0xa4);
        uint16 hi=adc->DoExchange(7,0),lo=adc->DoExchange(7,0);
        assert(((hi<<8)|lo)==(value<<3));
    }
    // ROM 1007b68c..1007b694 compares uint16((supply >> 1) - 25)
    // with main. Sample through the converter, retaining raw units.
    for (auto supply : {0, 49, 50, 51, 2048, 2049, 2050, 2051}) {
        assert(st.SetVirtualAnalogueInputs(1000,supply,true));
        adc->DoExchange(7,0xe4);
        uint16 hi=adc->DoExchange(7,0),lo=adc->DoExchange(7,0);
        uint16 raw=((hi<<8)|lo)>>3;
        assert(raw==supply);
        bool external=uint16((raw>>1)-25)>=1000;
        assert(external==(supply<50 || supply>=2050));
    }
    assert(st.DispatchScanCode(0x70,true)); assert(st.Byte(0)==0x41); assert(st.Byte(0)==0x70);
    // Real IRQ1 register behaviour for the ST D4 pin, in level and edge modes.
    st.SetWord(0xfffff304,0);st.SetWord(0xfffff302,0);
    st.DispatchButtonEvent(ButtonEvent(ButtonEvent::Button::rockerUp,ButtonEvent::Type::release));
    assert(st.GetWord(0xfffff310)&1);assert(st.GetWord(0xfffff30c)&1);
    assert(st.Byte(0)==0x40);assert(!(st.GetWord(0xfffff310)&1));
    st.SetWord(0xfffff302,0x0800);
    st.DispatchButtonEvent(ButtonEvent(ButtonEvent::Button::power,ButtonEvent::Type::press));
    assert(st.GetWord(0xfffff310)&1);
    st.SetWord(0xfffff30c,1);assert(!(st.GetWord(0xfffff310)&1));
    assert(st.Byte(0)==0x41);assert(st.Byte(0)==0x0f);assert(!(st.GetWord(0xfffff310)&1));
    st.SetWord(0xfffff302,0x8800);
    st.DispatchButtonEvent(ButtonEvent(ButtonEvent::Button::power,ButtonEvent::Type::release));
    assert(!(st.GetWord(0xfffff310)&1));assert(st.Byte(0)==0x40);
    assert(st.GetWord(0xfffff310)&1);st.SetWord(0xfffff30c,1);assert(!(st.GetWord(0xfffff310)&1));
    puts("Dana ST partial request/reply/reset, IRQ1, DC packet/FIFO/IRQ2 and disconnected HC cases passed");
}
