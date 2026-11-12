#ifndef __EMSCRIPTEN__
#include "HostInfrared.h"
#include "soc_PXA.h"
#include "pxa_UART.h"
#include <deque>
#include <algorithm>
#include "ir/IrPhone.h"
#include "ir/IrPrinter.h"
#include "ir/IrPrinterControl.h"
#include "modem/PppServer.h"
#include "net/SlirpIp.h"
struct HostInfrared::Impl {
    struct Phone {
        frondNet::SlirpIp network;
        frondModem::PppServer ppp{network};
        frondIr::IrPhone ir{ppp};
    };
    SocPXA* soc{};
    std::unique_ptr<Phone> phone;
    std::unique_ptr<frondIr::IrPrinter> printer;
    frondIr::IrPrinterControl printerControl;
    // The IR transceiver was on at the last Service in phone or printer mode.
    bool phoneHeard{false};
    // Guest nanoseconds for the phone, carried on across a load that moves the clock back.
    uint64_t clock{},clockMark{};
    uint64_t Now() {
        const uint64_t t=soc?soc->GetTime():0;
        if(t>clockMark)clock+=t-clockMark;
        clockMark=t;
        return clock;
    }
    HostByteLink link;
    HostSerial::Config config;
    uint64_t dropped{},discarded{};
    bool restart{};
    std::string error;
    std::deque<uint8_t> transmit;
    uint64_t transmitPeer{};
    void Flush() {
        if(transmit.empty() || !soc)return;
        if(transmitPeer!=link.PeerGeneration()){dropped+=transmit.size();transmit.clear();return;}
        // UART callbacks have already paced these bytes. Publish each frontend
        // slice, without adding another complete frame's transmission delay.
        uint8_t data[1024];size_t budget=8192;
        while(!transmit.empty() && budget){
            const size_t count=std::min({sizeof(data),transmit.size(),budget,link.SendSpace()});
            if(!count)break;
            std::copy_n(transmit.begin(),count,data);
            const size_t sent=link.SendForPeer(data,count,transmitPeer);if(!sent)break;
            for(size_t i=0;i<sent;++i)transmit.pop_front();budget-=sent;
        }
    }
    static uint_fast16_t Read(void* context) {
        auto& s=*static_cast<Impl*>(context);
        uint8_t value;
        if(s.phone) return s.phone->ir.Read(&value,1,s.Now())==1 ? value : UART_CHAR_NONE;
        if(s.printer) return s.printer->Read(&value,1,s.Now())==1 ? value : UART_CHAR_NONE;
        return s.link.Receive(&value,1)==1 ? value : UART_CHAR_NONE;
    }
    static void Write(uint_fast16_t value,void* context) {
        auto& s=*static_cast<Impl*>(context);
        if (value&~0xffu) { ++s.dropped; return; }
        // The UART paced this byte, so now is when its last bit went out.
        if (s.phone) { const uint8_t byte=uint8_t(value); s.phone->ir.Write(&byte,1,s.Now()); return; }
        if (s.printer) { const uint8_t byte=uint8_t(value); s.printer->Write(&byte,1,s.Now()); return; }
        const auto generation=s.link.PeerGeneration();
        if(!s.transmit.empty() && s.transmitPeer!=generation){s.dropped+=s.transmit.size();s.transmit.clear();}
        if(!s.link.Connected()){++s.dropped;return;}
        if(s.transmit.size()>=8192){++s.dropped;return;}
        if(s.transmit.empty())s.transmitPeer=generation;
        s.transmit.push_back(uint8_t(value));
        // Publish from Service after this guest slice. Sending synchronously
        // from the last UART character can let another frontend reply before
        // the source guest has finished its transmit-to-receive turnaround.
    }
    static bool Pending(void* context) {
        auto& s=*static_cast<Impl*>(context);
        if (s.phone) return s.phone->ir.Available(s.Now())!=0;
        if (s.printer) return s.printer->Available(s.Now())!=0;
        return s.link.PendingReceive()!=0;
    }
    static void ModeChanged(void* context) {
        auto& s=*static_cast<Impl*>(context);if (!s.soc || s.phone || s.printer) return;
        const auto state=pxaUartIrState(s.soc->GetIrUart());
        const uint64_t tag=(uint64_t(state.baud)<<8)|(state.lcr&0x3f);
        s.link.SetReceiveWindow(s.soc->IrPowerEnabled() && state.receive,tag);
    }
    static void Replaced(void* context) {
        auto& s=*static_cast<Impl*>(context);
        // The phone loses its link and call, as after a reset, sleep or load.
        if (s.phone) { s.phone->ir.Drop(s.Now()); return; }
        // The printer throws away a job that was coming in.
        if (s.printer) { s.printer->Drop(s.Now()); return; }
        // Closing the peer, rather than just clearing the userspace queues,
        // prevents old kernel-buffered bytes entering the new guest timeline.
        s.link.Close();s.transmit.clear();s.restart=true;
    }
};
HostInfrared::HostInfrared():impl(std::make_unique<Impl>()){}
HostInfrared::~HostInfrared(){Stop();}
bool HostInfrared::Start(SocPXA* soc,const HostSerial::Config& config,std::string& error) {
    // Another endpoint while one is on keeps the UART hooks: bytes the UART holds came from the
    // old peer and stay the guest's, as when a load replaces the peer. Taking the hooks again
    // would be refused until the guest had read them.
    if (impl->soc && impl->soc==soc && config.kind!=HostSerial::Kind::off) {
        impl->phone.reset();impl->printer.reset();
        impl->link.Close();impl->transmit.clear();impl->restart=false;impl->error.clear();
        error.clear();impl->dropped=impl->discarded=0;impl->config={};
        if (!impl->link.Open(config,error,true)) {Stop();return false;}
        Impl::ModeChanged(impl.get());
        impl->config=impl->link.GetConfig();
        return true;
    }
    Stop();error.clear();impl->dropped=impl->discarded=0;
    if (config.kind==HostSerial::Kind::off) return true;
    if (!soc) {error="Infrared needs a Palm TX or Tungsten E2.";return false;}
    IrHostHooks hooks{impl.get(),Impl::Read,Impl::Write,Impl::Pending,Impl::Replaced,Impl::ModeChanged};
    if (!soc->SetIrHostHooks(&hooks)) {
        error="Infrared UART is busy; detach the keyboard, or wait for the Palm to finish sending.";return false;
    }
    impl->soc=soc;
    if (!impl->link.Open(config,error,true)) {Stop();return false;}
    Impl::ModeChanged(impl.get());
    impl->config=impl->link.GetConfig();
    return true;
}
bool HostInfrared::StartPhone(SocPXA* soc,std::string& error) {
    error.clear();
    if (!soc) {error="Infrared needs a Palm TX or Tungsten E2.";return false;}
    auto phone=std::make_unique<Impl::Phone>();
    if (!phone->network.Valid()) {error="slirp did not start";return false;}
    if (impl->phone && impl->soc==soc) return true;
    impl->printer.reset();
    // A host endpoint on the same machine hands its hooks over, as Start does between endpoints.
    if (impl->soc==soc) {
        impl->link.Close();impl->transmit.clear();impl->restart=false;impl->error.clear();
        impl->dropped=impl->discarded=0;impl->config={};
    } else {
        Stop();impl->dropped=impl->discarded=0;
        IrHostHooks hooks{impl.get(),Impl::Read,Impl::Write,Impl::Pending,Impl::Replaced,Impl::ModeChanged};
        if (!soc->SetIrHostHooks(&hooks)) {
            error="Infrared UART is busy; detach the keyboard, or wait for the Palm to finish sending.";return false;
        }
        impl->soc=soc;
    }
    impl->clockMark=soc->GetTime();
    impl->phoneHeard=false;
    impl->phone=std::move(phone);
    return true;
}
bool HostInfrared::StartPrinter(SocPXA* soc,std::string& error) {
    error.clear();
    if (!soc) {error="Infrared needs a Palm TX or Tungsten E2.";return false;}
    if (impl->printer && impl->soc==soc) return true;
    impl->phone.reset();
    if (impl->soc==soc) {
        impl->link.Close();impl->transmit.clear();impl->restart=false;impl->error.clear();
        impl->dropped=impl->discarded=0;impl->config={};
    } else {
        Stop();impl->dropped=impl->discarded=0;
        IrHostHooks hooks{impl.get(),Impl::Read,Impl::Write,Impl::Pending,Impl::Replaced,Impl::ModeChanged};
        if (!soc->SetIrHostHooks(&hooks)) {
            error="Infrared UART is busy; detach the keyboard, or wait for the Palm to finish sending.";return false;
        }
        impl->soc=soc;
    }
    impl->clockMark=soc->GetTime();
    impl->phoneHeard=false;
    impl->printer=std::make_unique<frondIr::IrPrinter>();
    impl->printerControl.Reset();
    return true;
}
bool HostInfrared::PrinterMode() const {return bool(impl->printer);}
bool HostInfrared::PrinterNeedsGuest() {return impl->printer && impl->printer->NeedsGuest(impl->Now());}
bool HostInfrared::PrinterCommand(const std::string& action,const std::string& argument,std::string& result) {
    return impl->printerControl.Command(impl->printer.get(),action,argument,result);
}
bool HostInfrared::PhoneMode() const {return bool(impl->phone);}
bool HostInfrared::Active() const {return impl->phone || impl->printer || impl->config.kind!=HostSerial::Kind::off;}
std::string HostInfrared::PhoneStats() const {
    if (!impl->phone && !impl->printer) return "off";
    const auto& lap=impl->phone ? impl->phone->ir.Lap() : impl->printer->Lap();const auto& s=lap.GetStats();
    return "frames_in="+std::to_string(s.framesIn)+" frames_out="+std::to_string(s.framesOut)+
        " bad="+std::to_string(lap.BadFrames())+" connects="+std::to_string(s.connects)+
        " resent="+std::to_string(s.resent)+" timeouts="+std::to_string(s.timeouts);
}
void HostInfrared::Stop() {
    if (impl->soc) impl->soc->SetIrHostHooks(nullptr);
    impl->phone.reset();impl->printer.reset();
    impl->link.Close();impl->transmit.clear();impl->soc=nullptr;impl->config={};impl->restart=false;impl->error.clear();
}
void HostInfrared::Service() {
    auto* soc=impl->soc;
    if (!soc || soc->HostIoPaused()) return;
    if (soc->PowerSleeping()) {
        if (!impl->phone && !impl->printer && impl->link.IsOpen()) Impl::Replaced(impl.get());
        return;
    }
    if (impl->printer) {
        const uint64_t now=impl->Now();
        // A transceiver turned off leaves the link to the guest's disconnect time, as on frond-68k.
        impl->printer->Service(now);
        impl->printerControl.Observe(impl->printer.get());
        if (soc->IrPowerEnabled() && pxaUartIrReceiveEnabled(soc->GetIrUart()) && impl->printer->Available(now))
            pxaUartKick(soc->GetIrUart());
        return;
    }
    if (impl->phone) {
        const uint64_t now=impl->Now();
        // A transceiver turned off (IR closed, or a reset) ends the link and drops unread answers.
        const bool heard=soc->IrPowerEnabled();
        if (impl->phoneHeard && !heard) impl->phone->ir.Drop(now);
        impl->phoneHeard=heard;
        impl->phone->ir.Service(now);
        // Wake the UART once an answer's turnaround time has passed.
        if (soc->IrPowerEnabled() && pxaUartIrReceiveEnabled(soc->GetIrUart()) && impl->phone->ir.Available(now))
            pxaUartKick(soc->GetIrUart());
        return;
    }
    if (impl->restart) {
        impl->restart=false;
        if (soc->KeyboardAttached()) {
            soc->SetIrHostHooks(nullptr);impl->soc=nullptr;impl->config={};
            impl->error="Restored keyboard owns the infrared UART.";
            return;
        }
        if (!impl->link.Open(impl->config,impl->error,true)) return;
        Impl::ModeChanged(impl.get());
    }
    impl->Flush();
    // Register transitions update the receive generation immediately. The
    // worker samples arrivals; OS-buffered unsampled bytes have no optical time.
    Impl::ModeChanged(impl.get());
    if (soc->IrPowerEnabled() && pxaUartIrReceiveEnabled(soc->GetIrUart()) && impl->link.PendingReceive())
        pxaUartKick(soc->GetIrUart());
}
HostSerial::Config HostInfrared::GetConfig() const {return impl->config;}
std::string HostInfrared::Endpoint() const {return impl->link.Endpoint();}
uint64_t HostInfrared::DroppedTransmit() const {return impl->dropped;}
uint64_t HostInfrared::DroppedReceive() const {return impl->discarded+impl->link.GetCounters().rxDiscarded;}
size_t HostInfrared::PendingReceive() const {return impl->link.PendingReceive();}
HostSerial::Counters HostInfrared::GetCounters() const {return impl->link.GetCounters();}
std::string HostInfrared::Describe() const {
    if (impl->phone) return impl->phone->ir.Describe();
    if (impl->printer) return impl->printer->Describe();
    if (impl->config.kind==HostSerial::Kind::off) return impl->error.empty()?"off":"off error: "+impl->error;
    return HostSerial::Format(impl->config)+" "+impl->link.Endpoint()+
        (impl->link.Connected()?" connected":" waiting")+
        (impl->restart?" suspended":"")+
        (impl->error.empty()?"":" error: "+impl->error);
}
#endif
