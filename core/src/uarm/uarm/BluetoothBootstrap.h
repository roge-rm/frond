#ifndef FROND_BLUETOOTH_BOOTSTRAP_H
#define FROND_BLUETOOTH_BOOTSTRAP_H
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>

// The Palm drivers upload their own unchanged image over the UART, then use
// ordinary H4. This models that wire contract, not execution of the 8051 image.
class BluetoothBootstrap {
 public:
    static constexpr uint32_t maxFrame=65540;
    using HciSink=std::function<bool(const uint8_t*,size_t)>;
    enum Stage : uint32_t { hciBeforeUpload, replyDelay, bootAckDelay, mini, patch, bootReadyDelay, hci, failed };
    explicit BluetoothBootstrap(bool e2=true):e2(e2){}
    void Reset() {stage=hciBeforeUpload;rxCount=wanted=head=count=0;due=0;rx={};out={};}
    bool Failed() const {return stage==failed;}
    bool InHci() const {return stage==hci || stage==hciBeforeUpload;}
    bool Ready() const {return stage==hci;}
    bool Respond(const uint8_t* bytes,size_t size){return Emit(bytes,size);}
    bool Pending() const {return count || stage==replyDelay || stage==bootAckDelay || stage==bootReadyDelay;}
    Stage State() const {return Stage(stage);}
    void Tick(uint64_t now) {
        if(count || now<due)return;
        if(stage==replyDelay){const uint8_t v[]={4,14,4,1,0x2e,0xfc,0};Emit(v,sizeof(v));stage=bootAckDelay;due=0;}
        else if(stage==bootAckDelay && due){const uint8_t v[]={'4','1'};Emit(v,sizeof(v));stage=mini;due=0;}
        else if(stage==bootReadyDelay){
            // E2 explicitly consumes four bytes before HCI init; their contents
            // are ignored. Use a valid vendor event for this virtual controller.
            const uint8_t v[]={4,0xff,1,0};Emit(v,sizeof(v));stage=hci;due=0;
        }
    }
    uint16_t Read(uint64_t now) {
        Tick(now);if(!count)return 0x100;
        const auto c=out[head];out[head]=0;head=(head+1)%out.size();--count;
        if(!count && stage==bootAckDelay)due=now+150000000;
        return c;
    }
    bool Write(uint8_t c,uint64_t now,const HciSink& sink) {
        if(Failed())return false;
        if(stage==replyDelay || stage==bootAckDelay || stage==bootReadyDelay)return Reject();
        if(stage==patch && !rxCount && c=='Z') {
            if(e2){stage=bootReadyDelay;due=now+100000000;}else stage=hci;
            return true;
        }
        if(!rxCount) {
            if(stage==mini && c!=':')return Reject();
            if(stage==patch && c!='@')return Reject();
            if(InHci() && c!=1 && c!=2 && c!=3)return Reject();
        }
        if(rxCount==rx.size())return Reject();
        rx[rxCount++]=c;
        if(InHci()) {
            if(rx[0]==1 && rxCount==4)wanted=4+rx[3];
            else if(rx[0]==2 && rxCount==5)wanted=5+rx[3]+(uint32_t(rx[4])<<8);
            else if(rx[0]==3 && rxCount==4)wanted=4+rx[3];
            if(wanted>rx.size())return Reject();
            if(wanted && rxCount==wanted) {
                if(rx[0]==1 && wanted==4 && rx[1]==0x2e && rx[2]==0xfc) {
                    stage=replyDelay;due=now+10000000;
                } else if(!sink || !sink(rx.data(),rxCount))return Reject();
                ClearRx();
            }
            return true;
        }
        const auto digits=stage==mini?2u:4u;
        if(rxCount==digits+1) {
            uint32_t n=0;
            for(unsigned i=1;i<=digits;++i){const int d=Hex(rx[i]);if(d<0)return Reject();n=n*16+unsigned(d);}
            wanted=2*n+11;
            if(wanted>rx.size())return Reject();
        }
        if(wanted && rxCount==wanted) {
            unsigned sum=0;
            for(unsigned i=1;i+1<rxCount;i+=2) {
                const int hi=Hex(rx[i]),lo=Hex(rx[i+1]);if(hi<0 || lo<0)return Reject();
                sum+=16*hi+lo;
            }
            const bool valid=!(sum&255);
            if(stage==mini) {
                const uint8_t ack=valid?'1':'0';if(!Emit(&ack,1))return false;
                if(valid && wanted==11 && !std::memcmp(rx.data(),":00000001FF",11))stage=patch;
            } else {
                const uint8_t ack[]={'0','0',uint8_t(valid?'1':'0')};if(!Emit(ack,sizeof(ack)))return false;
            }
            ClearRx();
        }
        return true;
    }
    template<class H> void Transfer(H& h) {
        h.Do32(stage).Do32(rxCount).Do32(wanted).Do32(head).Do32(count).Do64(due)
            .DoBuffer(rx.data(),rx.size()).DoBuffer(out.data(),out.size());
    }
    bool Valid() const {
        if(stage>failed || rxCount>rx.size() || wanted>rx.size() || head>=out.size() || count>out.size())return false;
        if(!rxCount)return !wanted;
        uint32_t header=0,expected=0;
        if(InHci()){
            if(rx[0]!=1 && rx[0]!=2 && rx[0]!=3)return false;
            header=rx[0]==2?5:4;
            if(rxCount>=header)expected=header+(rx[0]==2?uint32_t(rx[3])+(uint32_t(rx[4])<<8):rx[3]);
        }else if(stage==mini || stage==patch){
            if(rx[0]!=(stage==mini?':':'@'))return false;
            header=stage==mini?3:5;
            if(rxCount>=header){
                uint32_t length=0;for(uint32_t i=1;i<header;++i){const auto digit=Hex(rx[i]);if(digit<0)return false;length=16*length+uint32_t(digit);}
                expected=2*length+11;
            }
        }else return false;
        // A completed frame is consumed synchronously by Write; only a partial
        // header/frame can be present at a serialization boundary.
        return rxCount<header?!wanted:expected<=rx.size() && wanted==expected && rxCount<expected;
    }
 private:
    bool e2;
    uint32_t stage{hciBeforeUpload},rxCount{},wanted{},head{},count{};
    uint64_t due{};
    std::array<uint8_t,maxFrame> rx{};
    std::array<uint8_t,64> out{};
    static int Hex(uint8_t c){if(c>='0' && c<='9')return c-'0';if(c>='A' && c<='F')return c-'A'+10;return -1;}
    void ClearRx(){rx={};rxCount=wanted=0;}
    bool Reject(){stage=failed;ClearRx();out={};head=count=0;return false;}
    bool Emit(const uint8_t* v,size_t n){if(n>out.size()-count)return Reject();for(size_t i=0;i<n;++i)out[(head+count++)%out.size()]=v[i];return true;}
};
#endif
