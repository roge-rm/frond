#pragma once
#include "EmRegsISPDana.h"
#include "EmBankRegs.h"
#include "UsbRawPrinter.h"
#include "RawPrintFile.h"

// Runner-owned, single controller thread. Dispose before register banks are freed.
// connectDelayNs is guest time from the board powering the external port (PM5) to the printer
// connecting. With none, a wake that powers the port connects the printer in the same scan that
// finds the module port's sleep disconnect on the Dana Wireless; that disconnect suspends the HC
// and the ROM then waits forever on the printer's enumeration.
class UsbPrinterRuntime {
 public:
    UsbRawPrinter printer{16*1024*1024};
    // Guest time in nanoseconds. Set by the owner before attach when there's a connect delay.
    std::function<uint64_t()> clock;
    ~UsbPrinterRuntime() { EmBankRegs::onBeforeDispose.RemoveHandler(disposeHandler); detach(); }
    explicit UsbPrinterRuntime(uint64_t connectDelayNs=0): connectDelay(connectDelayNs) {}
    UsbPrinterRuntime(const UsbPrinterRuntime&) = delete;
    UsbPrinterRuntime& operator=(const UsbPrinterRuntime&) = delete;
    void attach(EmRegsISPDana& controller) {
        detach(); dc=&controller; lastSave="none";
        printer.keepCollecting(true);
        dc->hostOwner[0]={};
        dc->hostOwner[0].cancelled=[this] { printer.cancel(); };
        dc->hostOwner[0].reattach=[this] { supplyChanged(); };
        dc->hostOwner[0].supplyChanged=[this] { supplyChanged(); };
        supplyChanged();
    }
    void detach() {
        printer.keepCollecting(false);
        printer.cancel();
        if(dc) { dc->SetHostTarget(0,{});dc->hostOwner[0]={};dc=nullptr; }
        connecting=false;
    }
    // Connects the printer once its connect delay has passed. Called often.
    void poll() { if(dc && connecting && now()>=connectAt) { connecting=false; plug(); } }
    bool command(const std::string& action,const std::string& argument,std::string& result) {
        bool ok=true;
        if(action=="begin") {ok=dc && dc->HasHostTarget(0) && ((printer.keeping() && printer.state()==RawPrintJob::State::collecting) || printer.begin());}
        else if(action=="abort") printer.cancel();
        else if(action=="clear") {if(!dc) ok=false; else printer.clear();}
        else if(action=="save" || action=="finish") {
            if(!dc || printer.state()!=RawPrintJob::State::collecting) ok=false;
            else {
                RawPrintFile output(argument);
                auto state=printer.finish(RawPrintFile::Output,&output);
                ok=state==RawPrintJob::State::published;
                result=output.error;
                lastSave=stateName(state);
            }
        } else if(action!="status") {result="Unknown printer action.";return false;}
        if(!result.empty()) result+=" ";
        result+=status();return ok;
    }
    std::string status() const {
        return std::string(dc?"attached ":"off ")+stateName(printer.state())+" bytes="+std::to_string(printer.size())+
            " last="+lastSave+" idle="+std::to_string(printer.idleMs());
    }
 private:
    void plug() { dc->SetHostTarget(0,[this](const auto& p) { return printer.packet<EmRegsISPDana::HostReply>(p); }); }
    uint64_t now() const { return clock ? clock() : 0; }
    // The printer is only connected while the board powers the external port (PM5), so a sleep
    // unplugs it and the wake plugs it in again for the ROM to enumerate. A port connected in a
    // loaded session gets its printer back at once, so it doesn't enumerate again.
    void supplyChanged() {
        if(!dc) return;
        const bool on=dc->PortSupply(0);
        if(on==(dc->HasHostTarget(0) || connecting)) return;
        connecting=false;
        if(!on) { dc->SetHostTarget(0,{}); return; }
        if(!connectDelay || dc->PortLoaded(0)) { plug(); return; }
        dc->SetHostTarget(0,{});
        connecting=true; connectAt=now()+connectDelay;
    }
    static const char* stateName(RawPrintJob::State s) {
        static const char* names[]={"idle","collecting","publishing","published","rejected","uncertain","aborted","overflow","invalid","allocationFailed"};
        return names[static_cast<unsigned>(s)];
    }
    EmRegsISPDana* dc=nullptr;
    std::string lastSave="none";
    const uint64_t connectDelay;
    bool connecting=false;
    uint64_t connectAt=0;
    int disposeHandler=EmBankRegs::onBeforeDispose.AddHandler([this] { detach(); });
};
