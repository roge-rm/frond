// The Dana Wireless's built-in Wi-Fi module (UsbWifiRuntime on root port 1) on an ISP1161 model
// with no firmware: port supply (PK0) on and off, the printer on port 0 at the same time, port
// resets kept to their own port, machine reset, load kept and dropped, a rejected load, one ARP
// exchange with slirp, the power-up delay before it connects, and the serial number token a Dana
// ROM gets.
#include <cassert>
#include <cstring>
#include <iostream>
#include "EmRegsISPDana.h"
#include "EmBankRegs.h"
#include "UsbPrinterRuntime.h"
#include "UsbWifiRuntime.h"
#include "dana_wireless5.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"

class Isp: public EmRegsISPDana {
 public:
    uint64 now=0;
    Isp() { SetSubBankHandlers(); Reset(true); }
    uint64 CurrentCycle() const override { return now; }
    uint64 ClockHz() const override { return 1000000; }
    void reg(uint16 c,uint32 v) { SetWord(0x1f000002,c); SetWord(0x1f000000,v); if(c<0xa0) SetWord(0x1f000000,v>>16); }
    uint32 get(uint16 c) { SetWord(0x1f000002,c); uint32 v=GetWord(0x1f000000); if(c<0x20) v|=uint32(GetWord(0x1f000000))<<16; return v; }
    void ready() { reg(0x94,0x10000); reg(0x95,2); reg(0x81,0x80); reg(0xab,4096); }
    // One PTD through the real ATL executor; returns it with its data.
    std::vector<uint8> transfer(unsigned dir,unsigned ep,bool toggle,unsigned addr,std::vector<uint8> payload,unsigned count=0,unsigned mps=64) {
        unsigned n=dir==2?count:payload.size(); std::vector<uint8> b(8+n);
        b[1]=8|(toggle?4:0); b[2]=mps; b[3]=8|(ep<<4); b[4]=n; b[5]=(n>>8)|(dir<<2); b[6]=addr;
        if(dir!=2) std::copy(payload.begin(),payload.end(),b.begin()+8);
        reg(0xa2,b.size()); SetWord(0x1f000002,0xc1);
        for(unsigned x=0;x<b.size();x+=2) SetWord(0x1f000000,b[x]|(x+1<b.size()?b[x+1]<<8:0));
        now+=1000; get(0x0f); reg(0xa2,b.size()); SetWord(0x1f000002,0x41);
        for(unsigned x=0;x<b.size();x+=2) { auto w=GetWord(0x1f000000); b[x]=w; if(x+1<b.size()) b[x+1]=w>>8; }
        return b;
    }
    // Arms a list without running a frame, and reads it back after one.
    void submit(const std::vector<uint8>& b) {
        reg(0xa2,b.size()); SetWord(0x1f000002,0xc1);
        for(unsigned x=0;x<b.size();x+=2) SetWord(0x1f000000,b[x]|(x+1<b.size()?b[x+1]<<8:0));
    }
    std::vector<uint8> frame(unsigned size) {
        now+=1000; get(0x0f); reg(0xa2,size); SetWord(0x1f000002,0x41); std::vector<uint8> b(size);
        for(unsigned x=0;x<size;x+=2) { auto w=GetWord(0x1f000000); b[x]=w; if(x+1<size) b[x+1]=w>>8; }
        return b;
    }
    // Resets a root port (0x95 or 0x96) and clears its change bits.
    void resetPort(unsigned port) { reg(0x95+port,0x10); now+=20000; get(0x0f); reg(0x95+port,0x1f0000); }
};
using Reply=EmRegsISPDana::HostReply;
using Packet=EmRegsISPDana::HostPacket;
using Result=EmRegsISPDana::PacketResult;
using Bytes=std::vector<uint8_t>;
static void w16(Bytes& b,size_t o,unsigned v) { b[o]=uint8_t(v); b[o+1]=uint8_t(v>>8); }
static unsigned le(const Bytes& b,size_t o) { return b[o]|(b[o+1]<<8); }

// The ROM's side of the module, packet by packet on the target the runtime plugged in.
struct Host {
    UsbWifiRuntime& w;
    unsigned addr=0;
    bool outT=false, inT=false;
    Reply send(unsigned ep,unsigned dir,bool t,unsigned req,Bytes b={}) {
        return w.adapter.packet<Reply>(Packet{uint8_t(addr),uint8_t(ep),uint8_t(dir),t,false,req,std::move(b)});
    }
    void control(Bytes setup) {
        assert(send(0,0,false,8,setup).result==Result::ack);
        assert(send(0,2,true,0).result==Result::ack);
    }
    void configure() {
        addr=0; outT=inT=false;
        control({0,5,3,0,0,0,0,0}); addr=3;
        control({0,9,1,0,0,0,0,0});
    }
    void out(Bytes m) {
        m.resize((m.size()+63)&~size_t(63));
        for(size_t o=0;o<m.size();o+=64) {
            assert(send(2,1,outT,64,Bytes(m.begin()+o,m.begin()+o+64)).result==Result::ack);
            outT=!outT;
        }
    }
    Bytes in() {
        Bytes got;
        for(;;) {
            auto r=send(1,2,inT,64);
            if(r.result!=Result::ack) return got;
            inT=!inT; got.insert(got.end(),r.bytes.begin(),r.bytes.end());
            if(r.bytes.size()<64) return got;
        }
    }
    unsigned command(unsigned c,unsigned p0=0) {
        Bytes m(10); w16(m,0,1); w16(m,2,c); w16(m,4,p0); out(m);
        Bytes r=in(); assert(le(r,0)==0x8001); return le(r,2);
    }
    void connect() {
        configure();
        assert((command(0)>>8)==0);
        Bytes m(8); w16(m,0,2); w16(m,2,2); w16(m,4,0xfc00); w16(m,6,1); out(m);
        assert(le(in(),0)==0x8002);
        assert((command(1)>>8)==0);
        Bytes link=in(); assert(le(link,0)==0x8000 && le(link,4)==0xf200 && le(link,6)==1);
        assert(w.adapter.linkUp());
    }
    void transmit(const uint8_t* dest,uint16_t type,const Bytes& payload) {
        Bytes m(60,0);
        std::memcpy(&m[46],dest,6); auto mac=w.mac(); std::memcpy(&m[52],mac.data(),6);
        const unsigned n=8+payload.size(); m[58]=uint8_t(n>>8); m[59]=uint8_t(n);
        const uint8_t snap[8]={0xaa,0xaa,3,0,0,0,uint8_t(type>>8),uint8_t(type)};
        m.insert(m.end(),snap,snap+8); m.insert(m.end(),payload.begin(),payload.end());
        out(m);
    }
};

// A ROM with a card header pointing its token store at 0x6000, and the tokens given.
static Bytes rom(const Bytes& tokens) {
    Bytes r(0x8000,0xff);
    r[0x64]=0x10; r[0x65]=0; r[0x66]=0x60; r[0x67]=0;
    std::copy(tokens.begin(),tokens.end(),r.begin()+0x6000);
    return r;
}
static Bytes token(const char* id,const std::string& data) {
    Bytes t(id,id+4); t.push_back(uint8_t(data.size()>>8)); t.push_back(uint8_t(data.size()));
    t.insert(t.end(),data.begin(),data.end());
    if(data.size()&1) t.push_back(0xff);
    return t;
}

int main() {
    // The serial number: digits 1 to 3 become 040, the rest stays; no token, no change.
    {
        Bytes r=rom(token("snum","A010040453722"));
        assert(danaSerialToken5(r.data(),r.size())==0x6006 && danaWirelessSerial5(r.data(),r.size()));
        assert(!std::memcmp(&r[0x6006],"A040040453722",13) && r[0x6013]==0xff);
        assert(danaWirelessSerial5(r.data(),r.size()) && !std::memcmp(&r[0x6006],"A040040453722",13));
        Bytes after=token("HWID","abc"); Bytes s=token("snum","B0109");
        after.insert(after.end(),s.begin(),s.end());
        r=rom(after); assert(danaWirelessSerial5(r.data(),r.size()) && !std::memcmp(&r[0x6010],"B0409",5));
        r=rom({}); Bytes before=r; assert(!danaWirelessSerial5(r.data(),r.size()) && r==before);
        r=rom(token("snum","A01")); before=r; assert(!danaWirelessSerial5(r.data(),r.size()) && r==before);
        r=rom(token("snum","\xff""010040")); assert(!danaSerialToken5(r.data(),r.size()));
        r=rom(token("snum","A010040")); r[0x6004]=0x20; assert(!danaSerialToken5(r.data(),r.size()));
        r=rom(token("snum","A010040")); r[0x65]=1; assert(!danaSerialToken5(r.data(),r.size()));
        r=rom(token("snum","A010040")); r[0x64]=0x0f; assert(!danaSerialToken5(r.data(),r.size()));
    }

    Isp i; UsbWifiRuntime m{1}; uint64_t t=0; m.clock=[&] { return t; };
    assert(m.rootPort()==1 && m.status()=="off");
    m.attach(i);
    assert(i.HasHostTarget(1) && !i.HasHostTarget(0) && m.hasMac());
    assert(i.hostOwner[1].supplyChanged && i.hostOwner[1].portReset && !i.hostOwner[0].supplyChanged);
    const auto mac=m.mac();

    // PK0 low takes the module off the bus; port 0's supply doesn't touch it.
    i.SetPortSupply(1,false);
    assert(!i.HasHostTarget(1) && m.status().rfind("unpowered link=down",0)==0);
    i.SetPortSupply(0,false); i.SetPortSupply(0,true); assert(!i.HasHostTarget(1));
    i.SetPortSupply(1,true);
    assert(i.HasHostTarget(1) && m.status().rfind("powered link=down",0)==0);
    i.SetPortSupply(0,false); assert(i.HasHostTarget(1)); i.SetPortSupply(0,true);

    // The printer on port 0 at the same time: both enumerate through the real executor, each at its
    // own address, and each port's reset only resets its own device.
    UsbPrinterRuntime printer;
    printer.attach(i);
    assert(i.HasHostTarget(0) && i.HasHostTarget(1) && i.hostOwner[0].reattach && i.hostOwner[1].supplyChanged);
    i.ready(); i.reg(0x96,2);
    i.resetPort(0);
    auto setup=i.transfer(0,0,false,0,{0,5,2,0,0,0,0,0}); assert((setup[1]>>4)==0); i.transfer(2,0,true,0,{},0);
    assert(printer.printer.bus().address==2 && m.adapter.busAddress()==0);
    i.resetPort(1);
    setup=i.transfer(0,0,false,0,{0x80,6,0,1,0,0,18,0}); assert((setup[1]>>4)==0);
    auto desc=i.transfer(2,0,true,0,{},18);
    assert(desc[0]==18 && desc[8+1]==1 && desc[8+8]==0x5e && desc[8+9]==0x04 && desc[8+10]==0x6e);
    i.transfer(1,0,true,0,{},0);
    i.transfer(0,0,false,0,{0,5,3,0,0,0,0,0}); i.transfer(2,0,true,0,{},0);
    assert(m.adapter.busAddress()==3 && printer.printer.bus().address==2);
    auto get=i.transfer(0,0,false,2,{0x80,6,0,1,0,0,18,0},0,8); assert((get[1]>>4)==0);
    desc=i.transfer(2,0,true,2,{},18,8); assert((desc[1]>>4)==0 && desc[0]==18 && desc[8+8]!=0x5e);
    i.transfer(1,0,true,2,{},0,8);
    // A list with a SETUP for the printer and one for the module: PK0 dropping or a reset of port 1
    // fails only the module's PTD, and the printer's owner isn't told.
    {
        unsigned cancels=0; auto keep=i.hostOwner[0].cancelled;
        i.hostOwner[0].cancelled=[&] { ++cancels; };
        for(unsigned event=0;event<2;++event) {
            Bytes list(32,0);
            const uint8 get8[8]={0x80,6,0,1,0,0,8,0};
            for(unsigned n=0;n<2;++n) {
                uint8* p=&list[n*16]; p[1]=8; p[2]=8; p[3]=n?8:0; p[4]=8; p[6]=n?3:2;
                std::memcpy(p+8,get8,8);
            }
            i.submit(list);
            if(event==0) i.resetPort(1); else i.SetPortSupply(1,false);
            auto r=i.frame(32);
            assert(r[0]==8 && (r[1]>>4)==0 && r[16]==0 && (r[17]>>4)==5 && cancels==0);
            auto in=i.transfer(2,0,true,2,{},8,8); assert((in[1]>>4)==0 && in[8]==18);
            i.transfer(1,0,true,2,{},0,8);
            assert(printer.printer.bus().address==2);
        }
        i.hostOwner[0].cancelled=keep;
    }
    // PK0 off and on drops and brings back the module only.
    i.SetPortSupply(1,false); assert(!i.HasHostTarget(1) && printer.printer.bus().address==2);
    i.SetPortSupply(1,true); i.reg(0x96,2); i.resetPort(1);
    i.transfer(0,0,false,0,{0,5,3,0,0,0,0,0}); i.transfer(2,0,true,0,{},0);
    assert(m.adapter.busAddress()==3 && printer.printer.bus().address==2);
    i.resetPort(0);
    assert(printer.printer.bus().address==0 && m.adapter.busAddress()==3);
    i.resetPort(1);
    assert(m.adapter.busAddress()==0);
    printer.detach();
    assert(!i.HasHostTarget(0) && i.HasHostTarget(1) && !i.hostOwner[0].reattach && i.hostOwner[1].supplyChanged);

    // ARP for 10.0.2.2, answered by slirp through poll().
    Host h{m}; h.connect();
    const uint8_t broadcast[6]={255,255,255,255,255,255};
    Bytes arp={0,1,8,0,6,4,0,1}; arp.insert(arp.end(),mac.begin(),mac.end());
    for(uint8_t b:{10,0,2,15,0,0,0,0,0,0,10,0,2,2}) arp.push_back(b);
    h.transmit(broadcast,0x0806,arp);
    t+=2000000; m.poll();
    Bytes rx=h.in();
    assert(rx.size()>=60+8+28 && le(rx,0)==0x2000 && rx[66]==8 && rx[67]==6 && rx[68+7]==2);
    assert(!std::memcmp(&rx[46],mac.data(),6));

    // A machine reset powers the module off and on with a fresh state.
    i.Reset(true);
    assert(i.HasHostTarget(1) && !i.HasHostTarget(0) && !m.adapter.linkUp() && m.mac()==mac);

    // Load with the module coming back before the guest runs: no connect change on port 1.
    i.ready(); i.reg(0x96,2); i.resetPort(1);
    h.connect();
    assert((i.get(0x16)&0x1f0003)==3);
    const Bytes state=m.adapter.save();
    const auto bus=m.adapter.bus(); assert(bus.address==3 && bus.configuration==1);
    Savestate<ChunkType> saved; assert(saved.Save(i));
    {
        SavestateLoader<ChunkType> loader; assert(loader.Load(saved.GetBuffer(),saved.GetSize(),i));
        assert(!i.HasHostTarget(1));
        m.attach(i); assert(m.adapter.load(state.data(),state.size()) && m.adapter.setBus(bus));
        m.sessionLoaded();
        assert(i.HasHostTarget(1) && m.adapter.linkUp() && (i.get(0x16)&0x1f0003)==3);
        auto config=i.transfer(0,0,false,3,{0x80,8,0,0,0,0,1,0}); assert((config[1]>>4)==0);
        config=i.transfer(2,0,true,3,{},1); assert((config[1]>>4)==0 && config[8]==1);
        i.transfer(1,0,true,3,{},0);
    }
    // Radio off in the loaded machine: the owner drops port 1 and the guest finds it gone.
    {
        SavestateLoader<ChunkType> loader; assert(loader.Load(saved.GetBuffer(),saved.GetSize(),i));
        m.detach(); i.DropLoadedPort(1);
        assert(!i.HasHostTarget(1) && (i.get(0x16)&0x10003)==0x10000);
        m.attach(i); assert(i.HasHostTarget(1) && (i.get(0x16)&1) && m.adapter.busAddress()==0);
    }
    // A loaded port 1 not taken back is dropped at the guest's next access; port 0 is untouched.
    i.ready(); i.reg(0x96,2); i.resetPort(1); h.connect();
    {
        Savestate<ChunkType> again; assert(again.Save(i));
        SavestateLoader<ChunkType> loader; assert(loader.Load(again.GetBuffer(),again.GetSize(),i));
        m.detach();
        assert((i.get(0x16)&0x10003)==0x10000 && !(i.get(0x15)&1));
        m.attach(i);
    }
    i.ready(); i.reg(0x96,2); i.resetPort(1); h.connect();
    // A rejected load leaves the module plugged in and connected.
    {
        SavestateLoader<ChunkType> loader; assert(!loader.Load(saved.GetBuffer(),saved.GetSize()-4,i));
        assert(i.HasHostTarget(1) && m.adapter.linkUp() && (h.command(0)>>8)==0);
    }

    // The external adapter's runtime and the module's are separate, with separate MACs.
    {
        UsbWifiRuntime external;
        external.attach(i);
        assert(i.HasHostTarget(0) && i.HasHostTarget(1) && external.mac()!=mac);
        external.detach(); assert(!i.HasHostTarget(0) && i.HasHostTarget(1));
    }

    // With a power-up delay, PK0 rising, attaching and a machine reset connect the module only once
    // that much guest time has passed (in poll); PK0 falling first cancels it. A port connected in a
    // loaded session gets the module back at once, without enumerating again.
    {
        Isp d; UsbWifiRuntime late{1,50000000}; uint64_t lt=0; late.clock=[&] { return lt; };
        late.attach(d); assert(!d.HasHostTarget(1));
        lt=50000000; late.poll(); assert(d.HasHostTarget(1));
        d.Reset(true); assert(!d.HasHostTarget(1));
        lt+=50000000; late.poll(); assert(d.HasHostTarget(1));
        d.ready(); d.reg(0x96,2); d.resetPort(1);
        Host lh{late}; lh.connect(); assert((d.get(0x16)&0x1f0003)==3);
        const Bytes ls=late.adapter.save(); const auto lb=late.adapter.bus();
        Savestate<ChunkType> connected; assert(connected.Save(d));
        {
            SavestateLoader<ChunkType> loader; assert(loader.Load(connected.GetBuffer(),connected.GetSize(),d));
            late.attach(d); assert(late.adapter.load(ls.data(),ls.size()) && late.adapter.setBus(lb));
            assert(d.HasHostTarget(1) && late.adapter.linkUp() && (d.get(0x16)&0x1f0003)==3);
            assert(late.adapter.busAddress()==3 && (lh.command(0)>>8)==0);
        }
        // Saved inside the delay: the port wasn't connected, so the load waits for the delay again.
        d.SetPortSupply(1,false); d.SetPortSupply(1,true); lt+=10000000;
        Savestate<ChunkType> early; assert(early.Save(d));
        {
            SavestateLoader<ChunkType> loader; assert(loader.Load(early.GetBuffer(),early.GetSize(),d));
            late.attach(d); assert(!d.HasHostTarget(1));
            lt+=49999999; late.poll(); assert(!d.HasHostTarget(1));
            lt+=1; late.poll(); assert(d.HasHostTarget(1) && (d.get(0x16)&0x10001)==0x10001);
        }
        d.SetPortSupply(1,false); assert(!d.HasHostTarget(1));
        d.SetPortSupply(1,true); assert(!d.HasHostTarget(1) && late.status().rfind("powered",0)==0);
        lt+=49999999; late.poll(); assert(!d.HasHostTarget(1));
        lt+=1; late.poll(); assert(d.HasHostTarget(1));
        d.SetPortSupply(1,false); d.SetPortSupply(1,true); lt+=10000000;
        d.SetPortSupply(1,false); lt+=60000000; late.poll(); assert(!d.HasHostTarget(1));
        d.SetPortSupply(1,true); late.detach(); lt+=60000000; late.poll(); assert(!d.HasHostTarget(1));
    }

    // Bank disposal unplugs it.
    EmBankRegs::onBeforeDispose.Dispatch();
    assert(!i.HasHostTarget(1) && m.status()=="off" && !i.hostOwner[1].supplyChanged);
    std::cout<<"PASS Dana Wireless module: serial token, PK0 supply, printer beside it, port resets, reset, load kept and dropped, rejected load, ARP, power-up delay, another port's PTDs kept\n";
}
