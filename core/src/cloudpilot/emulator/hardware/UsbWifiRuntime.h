#pragma once
#include <cstdio>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include "EmRegsISPDana.h"
#include "EmBankRegs.h"
#include "UsbPrism2Adapter.h"
#include "net/WifiRouter.h"

// A Prism 2.5 Wi-Fi adapter on one of the ISP1161's root ports, its Ethernet frames through a
// frondNet::WifiRouter (libslirp): the MN-510 on the Dana's external USB port (root port 0,
// powered by PM5), or the Dana Wireless's built-in module (root port 1, powered by PK0). It is only
// connected while the board powers its port. Runner-owned, single emulation thread. Dispose before
// register banks are freed.
// connectDelayNs is guest time from the board powering the port to the device connecting, the
// module's power-up. The Dana Wireless ROM polls for its module every 100 ms after raising PK0 and
// gives up if a poll finds it half enumerated, so the module can't connect at once.
class UsbWifiRuntime {
 public:
    UsbPrism2Adapter adapter;
    // Guest time in nanoseconds, for slirp's timers. Set by the owner before attach.
    std::function<uint64_t()> clock;

    explicit UsbWifiRuntime(unsigned rootPort=0,uint64_t connectDelayNs=0): port(rootPort),connectDelay(connectDelayNs) {
        adapter.setTxSink([this](const std::vector<uint8_t>& frame) { transmit(frame); });
    }
    ~UsbWifiRuntime() { EmBankRegs::onBeforeDispose.RemoveHandler(disposeHandler); detach(); }
    UsbWifiRuntime(const UsbWifiRuntime&) = delete;
    UsbWifiRuntime& operator=(const UsbWifiRuntime&) = delete;

    void attach(EmRegsISPDana& controller) {
        detach(); dc=&controller;
        if(!haveMac) newMac();
        if(!router) resetRouter();
        auto& owner=dc->hostOwner.at(port);
        owner={};
        owner.reattach=[this] { adapter.detach(); powered=false; supplyChanged(); };
        owner.portReset=[this] { adapter.busReset(); };
        owner.supplyChanged=[this] { supplyChanged(); };
        powered=false;
        supplyChanged();
    }
    void detach() {
        if(dc) {
            dc->SetHostTarget(port,{});
            dc->hostOwner[port]={};
            dc=nullptr;
        }
        adapter.detach(); powered=false; connecting=false;
    }
    bool attached() const { return dc!=nullptr; }
    unsigned rootPort() const { return port; }

    // Runs slirp and hands its frames to the adapter. A frame the adapter can't queue is dropped.
    // Called often; slirp runs at most once per guest millisecond unless the guest sent something.
    void poll() {
        if(dc && connecting && now()>=connectAt) { connecting=false; plug(); }
        if(!dc || !router) return;
        const uint64_t t=now();
        if(!sent && t>=lastPoll && t-lastPoll<1000000) return;
        sent=false; lastPoll=t;
        router->Poll(t);
        std::vector<uint8_t> frame;
        while(router->Receive(frame)) {
            if(adapter.receive(frame.data(),frame.size())) ++rxFrames; else ++dropped;
        }
    }
    // A fresh slirp: host connections drop, the guest keeps its address and the link.
    void resetRouter() { router=std::make_unique<frondNet::WifiRouter>(); }
    // After a session load: a router kept from earlier Wi-Fi use starts again too, since its
    // timers ran on the old guest clock.
    void sessionLoaded() { if(router) resetRouter(); }

    // The adapter's MAC, made once per machine and saved with its session.
    bool hasMac() const { return haveMac; }
    UsbPrism2Adapter::Mac mac() const { return mac_; }
    void setMac(const UsbPrism2Adapter::Mac& m) { mac_=m; haveMac=true; adapter.setMac(m); }
    static std::string macText(const UsbPrism2Adapter::Mac& m) {
        char text[18];
        snprintf(text,sizeof text,"%02x:%02x:%02x:%02x:%02x:%02x",m[0],m[1],m[2],m[3],m[4],m[5]);
        return text;
    }
    // Locally administered unicast only, as a made-up MAC must be.
    static bool parseMac(const std::string& text,UsbPrism2Adapter::Mac& m) {
        unsigned b[6]; char end;
        if(text.size()!=17 || sscanf(text.c_str(),"%2x:%2x:%2x:%2x:%2x:%2x%c",&b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&end)!=6) return false;
        if((b[0]&3)!=2) return false;
        for(unsigned i=0;i<6;++i) m[i]=uint8_t(b[i]);
        return true;
    }

    // Lower-case hex, and back; false on an odd length or another character.
    static std::string hexText(const std::vector<uint8_t>& bytes) {
        static const char digits[]="0123456789abcdef";
        std::string text;
        for(const auto b:bytes) { text+=digits[b>>4]; text+=digits[b&15]; }
        return text;
    }
    static bool parseHex(const std::string& text,std::vector<uint8_t>& bytes) {
        auto nibble=[](char c) { return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1; };
        if(text.size()%2) return false;
        std::vector<uint8_t> out;
        for(size_t i=0;i<text.size();i+=2) {
            const int high=nibble(text[i]), low=nibble(text[i+1]);
            if(high<0 || low<0) return false;
            out.push_back(uint8_t(high<<4|low));
        }
        bytes.swap(out);
        return true;
    }
    // The adapter's bus state as a session line: address, configuration, IN halt, OUT halt, OUT
    // toggle and, when something is in flight, its bytes in hex.
    static std::string busText(const UsbPrism2Adapter::Bus& b) {
        std::string text=std::to_string(b.address)+" "+std::to_string(b.configuration)+" "+std::to_string(int(b.haltIn))+" "+
            std::to_string(int(b.haltOut))+" "+std::to_string(int(b.outToggle));
        if(!b.flight.empty()) text+=" "+hexText(b.flight);
        return text;
    }
    // False unless the adapter would take it.
    static bool parseBus(const std::string& text,UsbPrism2Adapter::Bus& b) {
        std::istringstream fields(text);
        unsigned address, configuration, haltIn, haltOut, toggle; std::string flight, extra;
        if(!(fields>>address>>configuration>>haltIn>>haltOut>>toggle) || haltIn>1 || haltOut>1 || toggle>1) return false;
        UsbPrism2Adapter::Bus bus{address,configuration,bool(haltIn),bool(haltOut),bool(toggle),{}};
        if(fields>>flight && (!parseHex(flight,bus.flight) || bus.flight.empty() || fields>>extra)) return false;
        UsbPrism2Adapter check;
        if(!check.setBus(bus)) return false;
        b=std::move(bus);
        return true;
    }

    // "off", or the port power, link, the guest's IPv4 address, MAC and frame counts.
    std::string status() const {
        if(!dc) return "off";
        std::string s=std::string(powered?"powered":"unpowered")+" link="+(adapter.linkUp()?"up":"down")+" ip="+ip+
            " mac="+macText(mac_)+" tx="+std::to_string(txFrames)+" rx="+std::to_string(rxFrames)+
            " dropped="+std::to_string(dropped+adapter.rxDropped());
        if(router && !router->Valid()) s+=" router=failed";
        return s;
    }

 private:
    uint64_t now() const { return clock ? clock() : 0; }
    void newMac() {
        std::random_device random;
        UsbPrism2Adapter::Mac m;
        for(auto& b:m) b=uint8_t(random());
        m[0]=uint8_t((m[0]&0xfc)|2);
        setMac(m);
    }
    void plug() { dc->SetHostTarget(port,[this](const auto& p) { return adapter.packet<EmRegsISPDana::HostReply>(p); }); }
    // Power on or off is a fresh adapter; no power is an unplugged port. A powered port connects the
    // device after connectDelay (in poll), whether the board just powered it, it was attached or a
    // reset plugged it in again. A port connected in a loaded session gets its device back at once,
    // so it doesn't enumerate again.
    void supplyChanged() {
        if(!dc) return;
        const bool on=dc->PortSupply(port);
        if(on==powered && (dc->HasHostTarget(port) || connecting)==on) return;
        powered=on; adapter.detach(); connecting=false;
        if(!on) { dc->SetHostTarget(port,{}); return; }
        if(!connectDelay || dc->PortLoaded(port)) { plug(); return; }
        dc->SetHostTarget(port,{});
        connecting=true; connectAt=now()+connectDelay;
    }
    void transmit(const std::vector<uint8_t>& frame) {
        ++txFrames;
        // The guest's address, from the IPv4 source of what it sends.
        if(frame.size()>=34 && frame[12]==8 && frame[13]==0 && (frame[26]|frame[27]|frame[28]|frame[29]))
            ip=std::to_string(frame[26])+"."+std::to_string(frame[27])+"."+std::to_string(frame[28])+"."+std::to_string(frame[29]);
        if(router) router->Input(frame.data(),frame.size(),now());
        sent=true;
    }
    const unsigned port;
    EmRegsISPDana* dc=nullptr;
    std::unique_ptr<frondNet::WifiRouter> router;
    UsbPrism2Adapter::Mac mac_{};
    const uint64_t connectDelay;
    bool haveMac=false, powered=false, sent=false, connecting=false;
    uint64_t lastPoll=0, connectAt=0;
    std::string ip="none";
    unsigned txFrames=0, rxFrames=0, dropped=0;
    int disposeHandler=EmBankRegs::onBeforeDispose.AddHandler([this] { detach(); });
};
