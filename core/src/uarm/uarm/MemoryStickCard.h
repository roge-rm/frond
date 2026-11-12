#ifndef FROND_MEMORY_STICK_CARD_H
#define FROND_MEMORY_STICK_CARD_H
#include "MemoryStickMedia.h"
#include <algorithm>
#include <array>
#include <stdexcept>

// Classic serial Memory Stick registers and flash commands. The guest owns the FTL.
// Block and page bounds come from the attached media (16 or 32 pages per block).
class MemoryStickCard {
 public:
    enum : uint32_t { Cached=1, WriteReady=2 };
    struct State {
        std::array<uint8_t, 32> reg{};
        std::array<uint8_t, 512> page{};
        uint32_t readStart{}, readSize{30}, writeStart{16}, writeSize{15};
        uint32_t operation{}, bufferFlags{};
    } state;
    MemoryStickMedia media;
    void Reset() {
        state = {};
        state.readSize=30; state.writeStart=16;state.writeSize=15;
        state.reg[4]=0xff; state.reg[5]=0; state.reg[6]=0xff;state.reg[7]=0xff;
        state.reg[16]=0x80;
        state.reg[2]=media.WriteProtected()?1:0;
        state.reg[1]=0x80;
    }
    uint32_t Block() const { return (uint32_t(state.reg[17])<<16)|(uint32_t(state.reg[18])<<8)|state.reg[19]; }
    uint8_t Interrupt() const { return state.reg[1]; }
    void Interrupt(uint8_t value) { state.reg[1]=value; }
    bool PageRead(bool extraOnly) {
        MemoryStickMedia::Page page;
        if(media.Read(Block(),state.reg[21],page)!=MemoryStickMedia::Result::ok) return false;
        std::copy(page.begin()+512,page.end(),state.reg.begin()+22);
        if(!extraOnly){std::copy_n(page.begin(),512,state.page.begin());state.bufferFlags=Cached|WriteReady;}
        return true;
    }
    bool PageWrite(bool data, bool extra) {
        MemoryStickMedia::Page page;page.fill(0xff);
        if(data)std::copy(state.page.begin(),state.page.end(),page.begin());
        if(extra)std::copy_n(state.reg.begin()+22,9,page.begin()+512);
        return media.Program(Block(),state.reg[21],page,data,extra)==MemoryStickMedia::Result::ok;
    }
    bool Command(uint8_t command) {
        if(!media.Present())return false;
        const auto operation=state.operation;state.operation=0;
        switch(command) {
         case 0x3c:{
            const auto old=state;Reset();state.readStart=old.readStart;state.readSize=old.readSize;
            state.writeStart=old.writeStart;state.writeSize=old.writeSize;return true;
         }
         case 0xcc:Interrupt(0x80);return true;
         case 0xc3:state.bufferFlags=0;state.page.fill(0xff);Interrupt(0x80);return true;
         case 0x33:state.operation=operation==2?1:operation==4?3:0;Interrupt(0x80);return true;
         case 0x99:Interrupt(media.Erase(Block())==MemoryStickMedia::Result::ok?0x80:0xc0);return true;
         case 0xaa:
            if((state.reg[20]&0xc0)==0xc0){Interrupt(0xc0);return true;}
            if(!PageRead(state.reg[20]&0xc0)){Interrupt(0xc0);return true;}
            if(state.reg[20]&0xc0)Interrupt(0x80);
            else {state.operation=(state.reg[20]&0x20)?1:2;Interrupt(state.operation==1?0xa0:0x20);}
            return true;
         case 0x55:
            if(media.WriteProtected()||Block()>=media.Blocks()||state.reg[21]>=media.PagesPerBlock()){Interrupt(0xc0);return true;}
            if((state.reg[20]&0xc0)==0xc0){Interrupt(0xc0);return true;}
            if(state.reg[20]&0x80){
                MemoryStickMedia::Page page;page.fill(0xff);page[512]=state.reg[22];
                Interrupt(media.Program(Block(),state.reg[21],page,false,true)==MemoryStickMedia::Result::ok?0x80:0xc0);
            } else if(state.reg[20]&0x40)Interrupt(PageWrite(false,true)?0x80:0xc0);
            else if((state.reg[20]&0x20)&&(state.bufferFlags&WriteReady)) {
                Interrupt(PageWrite(true,true)?0x80:0xc0);state.bufferFlags&=~WriteReady;
            } else {state.operation=(state.reg[20]&0x20)?3:4;state.bufferFlags&=~WriteReady;Interrupt(0x20);}
            return true;
         default:Interrupt(0x81);return true;
        }
    }
    bool TpcWrite(uint8_t tpc,const uint8_t* in,size_t size) {
        switch(tpc) {
         case 8:
            if(size!=4||in[0]+unsigned(in[1])>32||in[2]+unsigned(in[3])>32)return false;
            state.readStart=in[0];state.readSize=in[1];state.writeStart=in[2];state.writeSize=in[3];return true;
         case 0xb:
            if(size!=state.writeSize||state.writeStart>32||size>32-state.writeStart)return false;
            // Identification/status registers ignore writes; the transport still completes.
            for(size_t i=0;i<size;++i)if(state.writeStart+i>=16&&state.writeStart+i<31)state.reg[state.writeStart+i]=in[i];
            return true;
         case 0xe:return size==1&&Command(*in);
         case 0xd:
            if(size!=512)return false;
            std::copy_n(in,512,state.page.begin());state.bufferFlags=Cached|WriteReady;
            if(state.operation==3||state.operation==4) {
                if(!PageWrite(true,true)){Interrupt(0xc0);state.operation=0;return true;}
                state.bufferFlags&=~WriteReady;
                if(state.operation==4&&++state.reg[21]<media.PagesPerBlock())Interrupt(0x20);
                else {state.operation=0;Interrupt(0x80);}
            } else {state.bufferFlags=Cached|WriteReady;Interrupt(0x80);}
            return true;
         default:return false;
        }
    }
    bool TpcRead(uint8_t tpc,uint8_t* out,size_t size) {
        switch(tpc) {
         case 4:
            if(size!=state.readSize||state.readStart>32||size>32-state.readStart)return false;
            state.reg[2]=(state.reg[2]&~0x31)|(media.WriteProtected()?1:0)|((state.bufferFlags&WriteReady)?0x10:0x20);
            std::copy_n(state.reg.begin()+state.readStart,size,out);return true;
         case 7:if(size!=1)return false;*out=Interrupt();return true;
         case 2:
            if(size!=512||(state.operation!=1&&state.operation!=2))return false;
            std::copy(state.page.begin(),state.page.end(),out);state.bufferFlags&=~WriteReady;return true;
         default:return false;
        }
    }
    void FinishRead(uint8_t tpc) {
        if(tpc!=2)return;
        if(state.operation==2&&++state.reg[21]<media.PagesPerBlock()&&PageRead(false))Interrupt(0x20);
        else {state.operation=0;Interrupt(0x80);}
    }
    // A pending page operation must address the attached geometry.
    static bool Valid(const State& s, const MemoryStickMedia::Geometry& g) {
        return s.readStart<=32&&s.readSize<=32-s.readStart&&
            s.writeStart<=32&&s.writeSize<=32-s.writeStart&&
            s.operation<=4&&s.bufferFlags<=3&&s.reg[4]==0xff&&s.reg[5]==0&&s.reg[6]==0xff&&s.reg[7]==0xff&&
            (!s.operation||(s.reg[21]<g.pagesPerBlock&&((uint32_t(s.reg[17])<<16)|(uint32_t(s.reg[18])<<8)|s.reg[19])<g.blocks));
    }
};
#endif
