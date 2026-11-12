#pragma once
#include "EmRegsISPDana.h"
#include "UsbDevicePort.h"
#include <algorithm>
#include <array>

// Borrowed controller, with an external cable and host packet sequencing.
class UsbDanaPort {
 public:
    explicit UsbDanaPort(EmRegsISPDana& controller): dc(controller) {}
    void SetCable(bool connected) {
        dc.SetVbus(connected);
        resetRequired=true;
        if(connected) ++enableCount;
        outToggle={};
    }
    UsbDevicePort Port() {
        static const UsbDevicePort::Operations ops{
            [](void* p) { return Self(p).Reset(); },
            [](void*) { return false; }, // SOF registers are not implemented.
            [](void* p) { return Self(p).dc.DeviceAttached(); },
            [](void* p) { return Self(p).dc.DeviceAttached(); },
            [](void*) { return false; },
            [](void* p) { return Self(p).enableCount; },
            [](void* p) { return uint8_t(Self(p).dc.EffectiveAddress()); },
            [](void*, void (*)(void*), void*) {},
            [](void* p,const uint8_t* data) { return Self(p).Setup(data); },
            [](void* p,const uint8_t* data,unsigned size) { return Self(p).Out(0,data,size); },
            [](void* p,uint8_t* data,unsigned* size) { return Self(p).In(0,data,size,true); },
            [](void* p,uint8_t ep,const uint8_t* data,unsigned size) { return Self(p).Out(ep,data,size); },
            [](void* p,uint8_t ep,uint8_t* data,unsigned* size) { return Self(p).In(ep,data,size,true); },
            [](void* p,uint8_t ep,unsigned* size) { return Self(p).In(ep,nullptr,size,false); },
            [](void* p,uint8_t ep,unsigned size) {
                return ep>14 || Self(p).resetRequired || !Self(p).dc.DeviceAttached() ||
                    Self(p).dc.HostOutReady(ep ? ep+1 : 0,size);
            }
        };
        return {this,ops};
    }
 private:
    using R=UsbPacketResult;
    EmRegsISPDana& dc;
    bool resetRequired=true;
    uint32_t enableCount=0;
    std::array<bool,15> outToggle{};
    std::array<uint64,15> outResetEpoch{};
    static UsbDanaPort& Self(void* p) { return *static_cast<UsbDanaPort*>(p); }
    static R Result(EmRegsISPDana::PacketResult r) {
        using D=EmRegsISPDana::PacketResult;
        switch(r) { case D::ack:return R::Ack; case D::nak:return R::Nak;
            case D::stall:return R::Stall; case D::noAnswer:return R::Timeout; }
        return R::Unsupported;
    }
    bool Ready() {
        if(!dc.DeviceAttached()) resetRequired=true;
        return dc.DeviceAttached() && !resetRequired;
    }
    bool Reset() {
        if(!dc.DeviceAttached()) return false;
        dc.BusReset(); resetRequired=false; outToggle={}; return true;
    }
    R Setup(const uint8_t* data) {
        if(!Ready()) return R::Timeout;
        auto r=Result(dc.HostOut(0,{data,data+8},false,true));
        if(r==R::Ack) { outToggle[0]=true; outResetEpoch[0]=dc.PacketResetEpoch(0); }
        return r;
    }
    R Out(uint8_t ep,const uint8_t* data,unsigned size) {
        if(!Ready()) return R::Timeout;
        if(ep>14 || size>64) return R::Unsupported;
        const unsigned fifo=ep ? ep+1 : 0;
        if(dc.PacketIsIn(fifo) || size>dc.PacketCapacity(fifo)) return R::Timeout;
        // ECR writes and clear-halt restart the controller's DATA0 sequence.
        const auto epoch=dc.PacketResetEpoch(fifo);
        if(outResetEpoch[ep]!=epoch) { outResetEpoch[ep]=epoch; outToggle[ep]=false; }
        std::vector<uint8> bytes;
        if(size) bytes.assign(data,data+size);
        auto r=Result(dc.HostOut(fifo,bytes,outToggle[ep]));
        if(r==R::Ack) outToggle[ep]=!outToggle[ep];
        return r;
    }
    R In(uint8_t ep,uint8_t* data,unsigned* size,bool complete) {
        if(!Ready()) return R::Timeout;
        ep&=0x7f;
        if(ep>14) return R::Unsupported;
        const unsigned fifo=ep ? ep+1 : 1;
        std::vector<uint8> bytes; bool data1=false;
        if(!dc.PacketIsIn(fifo)) return R::Timeout;
        auto r=Result(complete ? dc.HostIn(fifo,bytes,data1) : dc.PeekHostIn(fifo,bytes,data1));
        if(r==R::Ack) {
            *size=bytes.size();
            if(complete) {
                std::copy(bytes.begin(),bytes.end(),data);
                // No guest slice runs between this offer and its explicit ACK.
                if(!dc.TryHostAckIn(fifo)) return R::Timeout;
            }
        }
        return r;
    }
};
