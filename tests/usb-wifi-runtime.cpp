// UsbWifiRuntime on an ISP1161 model with no firmware: plugging in on the external port, port
// supply, port reset, the swap with the printer, machine reset, load and a rejected load, and
// one ARP exchange with slirp through the adapter.
#include <cassert>
#include <cstring>
#include <iostream>
#include "EmRegsISPDana.h"
#include "EmBankRegs.h"
#include "UsbPrinterRuntime.h"
#include "UsbWifiRuntime.h"
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
};
using Reply=EmRegsISPDana::HostReply;
using Packet=EmRegsISPDana::HostPacket;
using Result=EmRegsISPDana::PacketResult;
using Bytes=std::vector<uint8_t>;
static void w16(Bytes& b,size_t o,unsigned v) { b[o]=uint8_t(v); b[o+1]=uint8_t(v>>8); }
static unsigned le(const Bytes& b,size_t o) { return b[o]|(b[o+1]<<8); }

// The ROM's side of the adapter, packet by packet on the target the runtime plugged in.
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
        control({0,5,2,0,0,0,0,0}); addr=2;
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
    // An Ethernet II payload sent the ROM's way: 802.3 header and SNAP.
    void transmit(const uint8_t* dest,uint16_t type,const Bytes& payload) {
        Bytes m(60,0);
        std::memcpy(&m[46],dest,6); auto mac=w.mac(); std::memcpy(&m[52],mac.data(),6);
        const unsigned n=8+payload.size(); m[58]=uint8_t(n>>8); m[59]=uint8_t(n);
        const uint8_t snap[8]={0xaa,0xaa,3,0,0,0,uint8_t(type>>8),uint8_t(type)};
        m.insert(m.end(),snap,snap+8); m.insert(m.end(),payload.begin(),payload.end());
        out(m);
    }
};

int main() {
    Isp i; UsbWifiRuntime w; uint64_t t=0; w.clock=[&] { return t; };
    assert(w.status()=="off" && !w.hasMac());
    w.attach(i);
    assert(i.HasHostTarget(0) && !i.HasHostTarget(1));
    assert(w.hasMac() && (w.mac()[0]&3)==2);
    const auto mac=w.mac();
    assert(w.status().rfind("powered link=down ip=none mac="+UsbWifiRuntime::macText(mac)+" tx=0 rx=0 dropped=0",0)==0);

    // Through the real executor: the device descriptor at address 0 is the MN-510's.
    i.ready();
    auto setup=i.transfer(0,0,false,0,{0x80,6,0,1,0,0,18,0},0,64); assert((setup[1]>>4)==0);
    auto desc=i.transfer(2,0,true,0,{},18,64);
    assert(desc[0]==18 && desc[8+1]==1 && desc[8+8]==0x5e && desc[8+9]==0x04 && desc[8+10]==0x6e && desc[8+11]==0);
    i.transfer(1,0,true,0,{},0,64);
    i.transfer(0,0,false,0,{0,5,1,0,0,0,0,0}); i.transfer(2,0,true,0,{},0);
    assert(w.adapter.busAddress()==1);
    // A root port reset is a bus reset for the adapter.
    i.reg(0x95,0x10); assert(w.adapter.busAddress()==0 && i.HasHostTarget(0));

    // No port power, no adapter; power back is a fresh one.
    Host h{w}; h.connect();
    i.SetPortSupply(0,false);
    assert(!i.HasHostTarget(0) && !w.adapter.linkUp() && w.status().rfind("unpowered link=down",0)==0);
    i.SetPortSupply(0,true);
    assert(i.HasHostTarget(0) && w.status().rfind("powered link=down",0)==0);
    i.SetPortSupply(1,false); assert(i.HasHostTarget(0)); i.SetPortSupply(1,true);

    // ARP for the router at 10.0.2.2, answered by slirp through poll().
    h.connect();
    const uint8_t broadcast[6]={255,255,255,255,255,255};
    Bytes arp={0,1,8,0,6,4,0,1}; arp.insert(arp.end(),mac.begin(),mac.end());
    for(uint8_t b:{10,0,2,15,0,0,0,0,0,0,10,0,2,2}) arp.push_back(b);
    h.transmit(broadcast,0x0806,arp);
    t+=2000000; w.poll();
    Bytes rx=h.in();
    assert(rx.size()>=60+8+28 && le(rx,0)==0x2000);
    assert(rx[66]==8 && rx[67]==6 && rx[68+6]==0 && rx[68+7]==2);
    assert(!std::memcmp(&rx[46],mac.data(),6));
    assert(rx[68+14]==10 && rx[68+15]==0 && rx[68+16]==2 && rx[68+17]==2);
    // An IPv4 frame from the guest names its address in the readout.
    Bytes ip(28,0); ip[0]=0x45; ip[3]=28; ip[8]=64; ip[9]=17;
    ip[12]=10; ip[13]=0; ip[14]=2; ip[15]=15; ip[16]=10; ip[17]=0; ip[18]=2; ip[19]=2;
    h.transmit(&rx[52],0x0800,ip);
    assert(w.status().find(" link=up ip=10.0.2.15 ")!=std::string::npos);
    assert(w.status().find(" tx=2 rx=1 ")!=std::string::npos);

    // The printer takes the port's place and gives it back; the MAC stays.
    {
        UsbPrinterRuntime printer;
        w.detach(); assert(!i.HasHostTarget(0) && !i.hostPortReset && !i.hostSupplyChanged && w.status()=="off");
        printer.attach(i); assert(i.HasHostTarget(0) && printer.status().rfind("attached",0)==0);
        printer.detach(); assert(!i.HasHostTarget(0));
    }
    w.attach(i); assert(i.HasHostTarget(0) && w.mac()==mac);

    // A machine reset powers the adapter off and on.
    h.connect();
    i.Reset(true);
    assert(i.HasHostTarget(0) && !w.adapter.linkUp());

    // Load with the adapter coming back before the guest runs: no connect change, and the guest's
    // address, configuration and link carry on through the real executor.
    i.ready(); i.reg(0x95,0x10); i.now+=20000; i.get(0x0f); i.reg(0x95,0x1f0000);
    h.connect();
    assert((i.get(0x15)&0x1f0003)==3);
    const Bytes state=w.adapter.save();
    const auto bus=w.adapter.bus(); assert(bus.address==2 && bus.configuration==1);
    Savestate<ChunkType> saved; assert(saved.Save(i));
    {
        SavestateLoader<ChunkType> loader; assert(loader.Load(saved.GetBuffer(),saved.GetSize(),i));
        assert(!i.HasHostTarget(0));
        w.attach(i); assert(w.adapter.load(state.data(),state.size()) && w.adapter.setBus(bus));
        w.resetRouter();
        assert(i.HasHostTarget(0) && w.adapter.linkUp() && w.mac()==mac);
        assert((i.get(0x15)&0x1f0003)==3);
        auto get=i.transfer(0,0,false,2,{0x80,8,0,0,0,0,1,0}); assert((get[1]>>4)==0);
        auto config=i.transfer(2,0,true,2,{},1); assert((config[1]>>4)==0 && config[8]==1);
        i.transfer(1,0,true,2,{},0);
    }
    // Nothing plugged back in: the guest's next access finds the port disconnected.
    {
        SavestateLoader<ChunkType> loader; assert(loader.Load(saved.GetBuffer(),saved.GetSize(),i));
        w.detach();
        assert((i.get(0x15)&0x10003)==0x10000);
        w.attach(i); assert((i.get(0x15)&1) && w.adapter.busAddress()==0);
    }
    // A different device: the owner drops the kept port before plugging in its own.
    {
        SavestateLoader<ChunkType> loader; assert(loader.Load(saved.GetBuffer(),saved.GetSize(),i));
        i.DropLoadedPort(0); assert(!i.HasHostTarget(0) && (i.get(0x15)&0x10003)==0x10000);
        w.attach(i); assert(i.HasHostTarget(0) && (i.get(0x15)&1));
    }
    w.attach(i); i.reg(0x95,0x10); i.now+=20000; i.get(0x0f); i.reg(0x95,0x1f0000);
    h.connect();
    // A rejected load leaves the adapter plugged in and connected.
    {
        SavestateLoader<ChunkType> loader; assert(!loader.Load(saved.GetBuffer(),saved.GetSize()-4,i));
        assert(i.HasHostTarget(0) && w.adapter.linkUp());
        assert(w.adapter.busAddress()==2 && (h.command(0)>>8)==0);
    }
    Bytes bad=state; bad[4]=9; assert(!w.adapter.load(bad.data(),bad.size()));
    UsbPrism2Adapter::Bus badBus; badBus.configuration=1; assert(!w.adapter.setBus(badBus));
    badBus={0,0,false,false,true}; assert(!w.adapter.setBus(badBus));

    // MAC text: locally administered unicast only.
    UsbPrism2Adapter::Mac m;
    assert(UsbWifiRuntime::parseMac(UsbWifiRuntime::macText(mac),m) && m==mac);
    assert(!UsbWifiRuntime::parseMac("03:00:00:00:00:01",m));
    assert(!UsbWifiRuntime::parseMac("00:11:22:33:44:55",m));
    assert(!UsbWifiRuntime::parseMac("02:00:00:00:00:0",m));
    assert(!UsbWifiRuntime::parseMac("02:00:00:00:00:01x",m));

    // Bank disposal unplugs it.
    EmBankRegs::onBeforeDispose.Dispatch();
    assert(!i.HasHostTarget(0) && w.status()=="off");
    std::cout<<"PASS Wi-Fi runtime: plug, supply, port reset, printer swap, reset, load kept and dropped, rejected load, ARP\n";
}
