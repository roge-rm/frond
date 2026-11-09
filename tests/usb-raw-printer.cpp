#include <cassert>
#include <iostream>
#include "EmRegsISPDana.h"
#include "UsbRawPrinter.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
class Isp: public EmRegsISPDana {
 public:
 uint64 now=0;
 Isp(){SetSubBankHandlers();Reset(true);}
 uint64 CurrentCycle()const override{return now;}
 uint64 ClockHz()const override{return 1000000;}
 void reg(uint16 c,uint32 v){SetWord(0x1f000002,c);SetWord(0x1f000000,v);if(c<0xa0)SetWord(0x1f000000,v>>16);}
 uint32 get(uint16 c){SetWord(0x1f000002,c);uint32 v=GetWord(0x1f000000);if(c<0x20)v|=uint32(GetWord(0x1f000000))<<16;return v;}
 void ready(UsbRawPrinter& p){SetHostTarget(0,[&p](const auto& b){return p.packet<HostReply>(b);});reg(0x94,0x10000);reg(0x95,2);reg(0x81,0x80);reg(0xab,4096);}
 std::vector<uint8> transfer(unsigned dir,unsigned ep,bool toggle,unsigned addr,std::vector<uint8> payload,unsigned count=0,unsigned mps=8){
 unsigned n=dir==2?count:payload.size();std::vector<uint8>b(8+n);b[1]=8|(toggle?4:0);b[2]=mps;b[3]=8|(ep<<4);b[4]=n;b[5]=(n>>8)|(dir<<2);b[6]=addr;
 if(dir!=2)std::copy(payload.begin(),payload.end(),b.begin()+8);
 reg(0xa2,b.size());SetWord(0x1f000002,0xc1);for(unsigned x=0;x<b.size();x+=2)SetWord(0x1f000000,b[x]|(x+1<b.size()?b[x+1]<<8:0));
 now+=1000;get(0x0f);reg(0xa2,b.size());SetWord(0x1f000002,0x41);for(unsigned x=0;x<b.size();x+=2){auto w=GetWord(0x1f000000);b[x]=w;if(x+1<b.size())b[x+1]=w>>8;}return b;
 }
};
using R=EmRegsISPDana::PacketResult;
using P=EmRegsISPDana::HostPacket;
using Reply=EmRegsISPDana::HostReply;
static unsigned outputs=0;
static RawPrintJob::Publication output(void*c,const uint8_t*b,size_t n){++outputs;*static_cast<std::vector<uint8_t>*>(c)=std::vector<uint8_t>(b,b+n);return RawPrintJob::Publication::published;}
int main(){
 UsbRawPrinter p(128);Isp i;i.ready(p);unsigned addr=0;
 auto setup=[&](unsigned t,unsigned r,unsigned v,unsigned index,unsigned len){auto b=i.transfer(0,0,false,addr,{uint8(t),uint8(r),uint8(v),uint8(v>>8),uint8(index),uint8(index>>8),uint8(len),uint8(len>>8)});assert((b[1]>>4)==0);};
 auto status=[&]{auto b=i.transfer(2,0,true,addr,{},0);assert((b[1]>>4)==0);};
 auto read=[&](unsigned n){return i.transfer(2,0,true,addr,{},n);};
 auto endRead=[&]{auto b=i.transfer(1,0,true,addr,{});assert((b[1]>>4)==0);};
 setup(0x80,6,0x100,0,18);auto b=read(18);assert(b[0]==18&&b[8]==18&&b[15]==8);endRead();
 setup(0,5,7,0,0);auto wrong=i.transfer(2,0,true,7,{},0);assert(wrong[1]>>4==5);status();addr=7;
 setup(0x80,6,0x200,0,25);b=read(25);assert(b[0]==25&&b[8+14]==7&&b[8+16]==1);endRead();
 setup(0,9,1,0,0);status();assert(p.begin());
 // Segment at full-speed MPS, retain zeros, ACK duplicate without append, ZLP is data only.
 std::vector<uint8> bytes(64);for(unsigned n=0;n<bytes.size();++n)bytes[n]=n;
 b=i.transfer(1,1,false,addr,bytes,0,64);assert(b[0]==64&&!(b[1]&0xf8)&&p.size()==64);
 b=i.transfer(1,1,false,addr,bytes,0,64);assert(b[0]==64&&p.size()==64);
 b=i.transfer(1,1,true,addr,{},0,64);assert(p.size()==64&&outputs==0);
 b=i.transfer(1,1,false,addr,{0,255,0},0,64);assert(p.size()==67);
 std::vector<uint8> captured;assert(p.finish(output,&captured)==RawPrintJob::State::published);assert(outputs==1&&captured.size()==67&&captured[64]==0&&captured[65]==255);
 p.finish(output,&captured);assert(outputs==1);
 setup(0xa1,0,0,0,255);b=read(255);assert(b[8]==0&&b[9]==40&&b[0]==40);endRead();
 // Invalid requests ACK setup then protocol STALL; next SETUP recovers.
 for(auto request:std::vector<std::vector<unsigned>>{{0xa1,1,1,0,1},{0xa1,1,0,1,1},{0xa1,1,0,0,2},{0x80,6,0x101,0,18},{0x81,10,0,1,1},{0,5,128,0,0},{0xa1,0,1,0,8}}){setup(request[0],request[1],request[2],request[3],request[4]);b=read(8);assert(b[1]>>4==4);}
 setup(0xa1,1,0,0,1);b=read(1);assert(b[8]==0x10);endRead();
 assert(p.begin());b=i.transfer(1,1,true,addr,bytes,0,64);assert(p.size()==64);
 b=i.transfer(1,1,false,addr,bytes,0,64);assert(p.size()==128);
 b=i.transfer(1,1,true,addr,{1},0,64);assert(b[0]==0&&b[1]>>4==4&&p.state()==RawPrintJob::State::overflow&&p.size()==0);
 p.finish(output,&captured);assert(outputs==1);
 setup(0xa1,1,0,0,1);b=read(1);assert(b[8]==0x10);endRead();
 setup(2,1,0,1,0);status();assert(p.begin());b=i.transfer(1,1,false,addr,{9},0,64);assert(p.size()==1);
 setup(0x21,2,0,0,0);status();assert(p.state()==RawPrintJob::State::aborted);p.finish(output,&captured);assert(outputs==1);
 setup(2,3,0,1,0);status();setup(0x82,0,0,1,2);b=read(2);assert(b[8]==1);endRead();
 setup(2,1,0,1,0);status();setup(0x82,0,0,1,2);b=read(2);assert(b[8]==0);endRead();
 assert(p.begin());b=i.transfer(1,1,false,addr,{0,1},0,64);assert(p.size()==2);
 setup(1,11,0,0,0);status();assert(p.state()==RawPrintJob::State::aborted);
 setup(0x81,10,0,0,1);b=read(1);assert(b[8]==0);endRead();
 // Owner cancellation is required independently of dropping the controller callback.
 assert(p.begin());i.SetHostTarget(0,{});assert(p.state()==RawPrintJob::State::collecting);p.cancel();assert(p.state()==RawPrintJob::State::aborted);p.finish(output,&captured);assert(outputs==1);
 // Reset/detach/load owners abort separately; saved controller state never contains spool bytes.
 for(unsigned boundary=0;boundary<3;++boundary){
 p.cancel();i.Reset(true);i.ready(p);assert(p.begin());
 Savestate<ChunkType> saved;assert(saved.Save(i));
 if(boundary==0)i.Reset(true);
 if(boundary==1)i.SetHostConnection(0,false);
 if(boundary==2){SavestateLoader<ChunkType> loader;assert(loader.Load(saved.GetBuffer(),saved.GetSize(),i));}
 assert(p.state()==RawPrintJob::State::collecting);p.cancel();assert(p.state()==RawPrintJob::State::aborted);
 p.finish(output,&captured);assert(outputs==1);
 }
 // Direct seam checks for retransmitted control IN and invalid packet sizes/fields.
 p.cancel();auto packet=[&](P q){return p.packet<Reply>(q);};
 assert(packet({0,0,0,false,false,8,{0x80,6,0,1,0,0,64,0}}).result==R::ack);
 auto first=packet({0,0,2,true,false,8,{}});auto duplicate=packet({0,0,2,true,false,8,{}});assert(first.bytes==duplicate.bytes);
 auto second=packet({0,0,2,false,false,8,{}});assert(second.bytes.size()==8);
 auto third=packet({0,0,2,true,false,8,{}});assert(third.bytes.size()==2);
 assert(packet({0,0,1,true,false,1,{0}}).result==R::stall);
 // Forty-byte ID followed by a required terminating control ZLP.
 assert(packet({0,0,0,false,false,8,{0,5,1,0,0,0,0,0}}).result==R::ack);
 assert(packet({0,0,2,true,false,0,{}}).result==R::ack);
 assert(packet({1,0,0,false,false,8,{0,9,1,0,0,0,0,0}}).result==R::ack);
 assert(packet({1,0,2,true,false,0,{}}).result==R::ack);
 assert(packet({1,0,0,false,false,8,{0xa1,0,0,0,0,0,255,0}}).result==R::ack);
 for(unsigned k=0;k<5;++k)assert(packet({1,0,2,bool(!(k%2)),false,8,{}}).bytes.size()==8);
 assert(packet({1,0,2,false,false,8,{}}).bytes.empty());
 assert(packet({1,0,1,true,false,0,{}}).result==R::ack);
 p.cancel();
 assert(packet({0,0,0,false,false,8,{0,5,3,0,0,0,0,0}}).result==R::ack);
 p.cancel();assert(packet({3,0,2,true,false,0,{}}).result==R::noAnswer);
 std::cout<<"USB printer HC/control/raw adapter PASS\n";
}
