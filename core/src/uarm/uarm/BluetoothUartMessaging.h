#ifndef FROND_BLUETOOTH_UART_MESSAGING_H
#define FROND_BLUETOOTH_UART_MESSAGING_H
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Legacy TX controller transport, inferred from the original driver's FC27/29
// producer and EF-event consumer. This models UART availability, not electrical
// wake polarity, radio execution or physical idle/power timing. The extra mode-
// activation EF04 is a virtual-controller notification; its physical BCM2035
// bytes were not captured (the original driver only counts those four bytes).
class BluetoothUartMessaging {
 public:
    enum class Result { unhandled, handled, failed };
    void Reset(){state=0;parameters={};}
    bool CanReadController()const{return state==0 || state==1;}
    template<class Send> Result Command(const uint8_t* b,size_t n,Send send){
        if(n==4 && b[0]==1 && b[1]==3 && b[2]==12){Reset();return Result::unhandled;}
        if(n<4 || b[0]!=1 || b[2]!=0xfc || (b[1]!=0x27 && b[1]!=0x28 && b[1]!=0x29))return Result::unhandled;
        auto complete=[&](uint8_t status){const uint8_t r[]={4,14,4,1,b[1],0xfc,status};return send(r,sizeof(r));};
        auto done=[](bool ok){return ok?Result::handled:Result::failed;};
        if(b[1]==0x27){
            if(n!=14 || b[3]!=10)return done(complete(0x12));
            if(b[4]!=0 && b[4]!=2)return done(complete(0x11));
            for(size_t i=7;i<13;++i)if(b[i]>1)return done(complete(0x12));
            if(b[4]==0 && b[10])return done(complete(0x12));
            std::copy(b+4,b+14,parameters.begin());state=b[4]==2?1:0;
            if(!complete(0))return Result::failed;
            return done(state==0 || Event(4,send));
        }
        if(b[1]==0x28){
            if(n!=4 || b[3])return done(complete(0x12));
            std::array<uint8_t,17> r{4,14,14,1,0x28,0xfc,0};
            std::copy(parameters.begin(),parameters.end(),r.begin()+7);
            return done(send(r.data(),r.size()));
        }
        if(n!=5 || b[3]!=1)return done(complete(0x12));
        if(!state)return done(complete(0x0c));
        switch(b[4]){
            case 1: state=1;return done(Event(4,send)); // Host wake request -> wake acknowledgment.
            case 2: if(state==3)state=1;return Result::handled; // Host acknowledges controller wake.
            case 3: state=2;return done(Event(6,send)); // Host sleep request -> sleep acknowledgment.
            case 4: return Result::handled; // No autonomous sleep request is outstanding.
            default:return done(complete(0x12));
        }
    }
    template<class Send>bool PendingControllerInput(bool pending,Send send){
        if(state!=2 || !pending)return true;
        if(!Event(3,send))return false;
        state=3;return true;
    }
    template<class T>void Transfer(T& h){h.Do32(state).DoBuffer(parameters.data(),parameters.size());}
    bool Valid()const{
        if(state>3 || parameters[0]!=(state?2:0))return false;
        for(size_t i=3;i<9;++i)if(parameters[i]>1)return false;
        return parameters[0]!=0 || !parameters[6];
    }
 private:
    uint32_t state{}; // 0 disabled, 1 awake, 2 asleep, 3 controller wake awaiting host acknowledgment.
    std::array<uint8_t,10>parameters{};
    template<class Send>static bool Event(uint8_t token,Send send){const uint8_t r[]={4,0xef,1,token};return send(r,sizeof(r));}
};
#endif
