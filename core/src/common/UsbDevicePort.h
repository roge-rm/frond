#pragma once
#include <cstdint>
#include "PxaUsbOperations.h"

class UsbDevicePort {
 public:
    struct Operations {
        bool (*Reset)(void*);
        bool (*Frame)(void*);
        bool (*Enabled)(void*);
        bool (*Listening)(void*);
        bool (*SofWanted)(void*);
        uint32_t (*EnableCount)(void*);
        uint8_t (*Address)(void*);
        void (*SetNotify)(void*, void (*notify)(void*), void* context);
        UsbPacketResult (*Setup)(void*, const uint8_t* data);
        UsbPacketResult (*Out)(void*, const uint8_t* data, unsigned size);
        UsbPacketResult (*In)(void*, uint8_t* data, unsigned* size);
        UsbPacketResult (*EpOut)(void*, uint8_t ep, const uint8_t* data, unsigned size);
        UsbPacketResult (*EpIn)(void*, uint8_t ep, uint8_t* data, unsigned* size);
        UsbPacketResult (*PeekIn)(void*, uint8_t ep, unsigned* size);
        // Optional cooperative host credit; this query never issues a USB token.
        bool (*OutReady)(void*, uint8_t ep, unsigned size){nullptr};
    };
    UsbDevicePort() = default;
    UsbDevicePort(void* context, const Operations& operations): context(context), ops(&operations) {}
    UsbDevicePort(Pxa255Udc* p) {
        static const Operations operations{
            [](void* p) -> bool { return pxa255UdcHostReset(static_cast<Pxa255Udc*>(p)); },
            [](void* p) -> bool { return pxa255UdcHostFrame(static_cast<Pxa255Udc*>(p)); },
            [](void* p) -> bool { return pxa255UdcHostEnabled(static_cast<Pxa255Udc*>(p)); },
            [](void* p) -> bool { return pxa255UdcHostListening(static_cast<Pxa255Udc*>(p)); },
            [](void* p) -> bool { return pxa255UdcHostSofWanted(static_cast<Pxa255Udc*>(p)); },
            [](void* p) -> uint32_t { return pxa255UdcHostEnableCount(static_cast<Pxa255Udc*>(p)); },
            [](void* p) -> uint8_t { return pxa255UdcHostAddress(static_cast<Pxa255Udc*>(p)); },
            [](void* p, void (*notify)(void*), void* context) -> void { return pxa255UdcHostSetNotify(static_cast<Pxa255Udc*>(p), notify, context); },
            [](void* p, const uint8_t* data) -> UsbPacketResult { return pxa255UdcHostSetup(static_cast<Pxa255Udc*>(p), data); },
            [](void* p, const uint8_t* data, unsigned size) -> UsbPacketResult { return pxa255UdcHostOut(static_cast<Pxa255Udc*>(p), data, size); },
            [](void* p, uint8_t* data, unsigned* size) -> UsbPacketResult { return pxa255UdcHostIn(static_cast<Pxa255Udc*>(p), data, size); },
            [](void* p, uint8_t ep, const uint8_t* data, unsigned size) -> UsbPacketResult { return pxa255UdcHostEpOut(static_cast<Pxa255Udc*>(p), ep, data, size); },
            [](void* p, uint8_t ep, uint8_t* data, unsigned* size) -> UsbPacketResult { return pxa255UdcHostEpIn(static_cast<Pxa255Udc*>(p), ep, data, size); },
            [](void* p, uint8_t ep, unsigned* size) -> UsbPacketResult { return pxa255UdcHostPeekIn(static_cast<Pxa255Udc*>(p), ep, size); },
        };
        if(p) { context=p; ops=&operations; }
    }
    UsbDevicePort(Pxa270Udc* p) {
        static const Operations operations{
            [](void* p) -> bool { return pxa270UdcHostReset(static_cast<Pxa270Udc*>(p)); },
            [](void* p) -> bool { return pxa270UdcHostFrame(static_cast<Pxa270Udc*>(p)); },
            [](void* p) -> bool { return pxa270UdcHostEnabled(static_cast<Pxa270Udc*>(p)); },
            [](void* p) -> bool { return pxa270UdcHostListening(static_cast<Pxa270Udc*>(p)); },
            [](void* p) -> bool { return pxa270UdcHostSofWanted(static_cast<Pxa270Udc*>(p)); },
            [](void* p) -> uint32_t { return pxa270UdcHostEnableCount(static_cast<Pxa270Udc*>(p)); },
            [](void* p) -> uint8_t { return pxa270UdcHostAddress(static_cast<Pxa270Udc*>(p)); },
            [](void* p, void (*notify)(void*), void* context) -> void { return pxa270UdcHostSetNotify(static_cast<Pxa270Udc*>(p), notify, context); },
            [](void* p, const uint8_t* data) -> UsbPacketResult { return pxa270UdcHostSetup(static_cast<Pxa270Udc*>(p), data); },
            [](void* p, const uint8_t* data, unsigned size) -> UsbPacketResult { return pxa270UdcHostOut(static_cast<Pxa270Udc*>(p), data, size); },
            [](void* p, uint8_t* data, unsigned* size) -> UsbPacketResult { return pxa270UdcHostIn(static_cast<Pxa270Udc*>(p), data, size); },
            [](void* p, uint8_t ep, const uint8_t* data, unsigned size) -> UsbPacketResult { return pxa270UdcHostEpOut(static_cast<Pxa270Udc*>(p), ep, data, size); },
            [](void* p, uint8_t ep, uint8_t* data, unsigned* size) -> UsbPacketResult { return pxa270UdcHostEpIn(static_cast<Pxa270Udc*>(p), ep, data, size); },
            [](void* p, uint8_t ep, unsigned* size) -> UsbPacketResult { return pxa270UdcHostPeekIn(static_cast<Pxa270Udc*>(p), ep, size); },
        };
        if(p) { context=p; ops=&operations; }
    }
    UsbDevicePort(Pxa255Udc* p, bool (*gate)(void*), void* gateContext): UsbDevicePort(p) { listeningGate=gate; this->gateContext=gateContext; }
    explicit operator bool() const { return ops && context; }
    bool Reset() const { return *this ? ops->Reset(context) : false; }
    bool Frame() const { return *this ? ops->Frame(context) : false; }
    bool Enabled() const { return *this ? ops->Enabled(context) : false; }
    bool Listening() const { return *this ? (!listeningGate || listeningGate(gateContext)) && ops->Listening(context) : false; }
    bool SofWanted() const { return *this ? ops->SofWanted(context) : false; }
    uint32_t EnableCount() const { return *this ? ops->EnableCount(context) : 0; }
    uint8_t Address() const { return *this ? ops->Address(context) : 0; }
    void SetNotify(void (*notify)(void*), void* context) const { if (*this) ops->SetNotify(this->context, notify, context); }
    UsbPacketResult Setup(const uint8_t* data) const { return *this ? ops->Setup(context, data) : UsbPacketResult::Disabled; }
    UsbPacketResult Out(const uint8_t* data, unsigned size) const { return *this ? ops->Out(context, data, size) : UsbPacketResult::Disabled; }
    UsbPacketResult In(uint8_t* data, unsigned* size) const { return *this ? ops->In(context, data, size) : UsbPacketResult::Disabled; }
    UsbPacketResult EpOut(uint8_t ep, const uint8_t* data, unsigned size) const { return *this ? ops->EpOut(context, ep, data, size) : UsbPacketResult::Disabled; }
    UsbPacketResult EpIn(uint8_t ep, uint8_t* data, unsigned* size) const { return *this ? ops->EpIn(context, ep, data, size) : UsbPacketResult::Disabled; }
    UsbPacketResult PeekIn(uint8_t ep, unsigned* size) const { return *this ? ops->PeekIn(context, ep, size) : UsbPacketResult::Disabled; }
    bool OutReady(uint8_t ep, unsigned size) const { return !*this || !ops->OutReady || ops->OutReady(context, ep, size); }
 private:
    void* context{nullptr};
    const Operations* ops{nullptr};
    bool (*listeningGate)(void*){nullptr};
    void* gateContext{nullptr};
};
