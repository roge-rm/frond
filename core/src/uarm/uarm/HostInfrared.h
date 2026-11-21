#ifndef FROND_HOST_INFRARED_H
#define FROND_HOST_INFRARED_H
#include <cstddef>
#include <memory>
#include <string>
#include "HostSerial.h"
#include "HostUartPort.h"
class SocPXA;
// A SIR byte endpoint. The guest owns IrLAP, IAS, TinyTP and OBEX.
// Or, in phone mode, the core's mobile phone (frondIr::IrPhone, the IrLAP secondary) with PPP
// and slirp behind its modem, on guest time; or in printer mode the core's IrDA printer
// (frondIr::IrPrinter).
// All calls run on the emulation thread; HostByteLink owns the host worker.
class HostInfrared {
 public:
    HostInfrared();
    ~HostInfrared();
    HostInfrared(const HostInfrared&)=delete;
    HostInfrared& operator=(const HostInfrared&)=delete;
    // The port outlives the peer or Stop runs first. The SocPXA forms use its IR UART.
    bool Start(HostInfraredPort* port,const HostSerial::Config& config,std::string& error);
    bool Start(SocPXA* soc,const HostSerial::Config& config,std::string& error);
    bool Start(std::nullptr_t,const HostSerial::Config& config,std::string& error) {
        return Start(static_cast<HostInfraredPort*>(nullptr),config,error);
    }
    // The phone in place of a host endpoint; Start or Stop ends it.
    bool StartPhone(HostInfraredPort* port,std::string& error);
    bool StartPhone(SocPXA* soc,std::string& error);
    bool StartPhone(std::nullptr_t,std::string& error) {return StartPhone(static_cast<HostInfraredPort*>(nullptr),error);}
    void Stop();
    bool PhoneMode() const;
    // The printer in place of a host endpoint or the phone; Start, StartPhone or Stop ends it, and
    // any held output that wasn't saved goes with it.
    bool StartPrinter(HostInfraredPort* port,std::string& error);
    bool StartPrinter(SocPXA* soc,std::string& error);
    bool StartPrinter(std::nullptr_t,std::string& error) {return StartPrinter(static_cast<HostInfraredPort*>(nullptr),error);}
    // The port in use, null when off.
    HostInfraredPort* Port() const;
    bool PrinterMode() const;
    // The printer has a job coming in or a link up (frondIr::IrPrinter::NeedsGuest).
    bool PrinterNeedsGuest();
    // irprinter status|save PATH|clear (frondIr::IrPrinterControl).
    bool PrinterCommand(const std::string& action,const std::string& argument,std::string& result);
    // A host endpoint or the phone is on the IR port.
    bool Active() const;
    // The phone's or printer's IrLAP counters.
    std::string PhoneStats() const;
    void Service();
    HostSerial::Config GetConfig() const;
    std::string Describe() const;
    std::string Endpoint() const;
    uint64_t DroppedTransmit() const;
    uint64_t DroppedReceive() const;
    size_t PendingReceive() const;
    HostSerial::Counters GetCounters() const;
 private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif
