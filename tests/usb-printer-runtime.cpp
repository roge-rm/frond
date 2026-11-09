#include <cassert>
#include <iostream>
#include "EmRegsISPDana.h"
#include "UsbRawPrinter.h"
#include "UsbPrinterRuntime.h"
#include "EmBankRegs.h"
#include <fstream>
#include <cstring>
#include <sys/stat.h>
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
int main(int argc,char** argv) {
 assert(argc==2);std::string root=argv[1];
 Isp i;UsbPrinterRuntime owner;owner.attach(i);
 auto bind=[&] {owner.attach(i);i.reg(0x94,0x10000);i.reg(0x95,2);i.reg(0x81,0x80);i.reg(0xab,4096);
 i.transfer(0,0,false,0,{0,5,1,0,0,0,0,0});i.transfer(2,0,true,0,{},0);
 auto setup=i.transfer(0,0,false,1,{0,9,1,0,0,0,0,0});assert((setup[1]>>4)==0);
 auto status=i.transfer(2,0,true,1,{},0);assert((status[1]>>4)==0);};
 auto begin=[&] {std::string result;assert(owner.command("begin","",result));};
 auto bytes=[&] {auto b=i.transfer(1,1,false,1,{0,255,0,42},0,64);if(b[0]!=4) std::cerr<<"bad bulk actual="<<unsigned(b[0])<<" cc="<<unsigned(b[1])<<" "<<owner.status()<<"\n";assert(b[0]==4);assert(owner.printer.size()==4);};
 bind();begin();bytes();
 std::string result;assert(owner.command("finish",root+"/exact.raw",result));
 std::ifstream in(root+"/exact.raw",std::ios::binary);std::string data((std::istreambuf_iterator<char>(in)),{});assert(data==std::string("\0\xff\0*",4));
 struct stat st{};assert(stat((root+"/exact.raw").c_str(),&st)==0 && (st.st_mode&0777)==0600);
 for(auto fault:{RawPrintFile::Fault::write,RawPrintFile::Fault::flush,RawPrintFile::Fault::close,RawPrintFile::Fault::publish,RawPrintFile::Fault::directory}) {
   RawPrintJob job(8);job.begin();uint8_t b[]={0,1,0};job.append(b,3);
   std::string path=root+"/fault-"+std::to_string(int(fault));RawPrintFile file(path);file.fault=fault;
   auto state=job.finish(RawPrintFile::Output,&file);
   assert(state==(fault==RawPrintFile::Fault::directory?RawPrintJob::State::uncertain:RawPrintJob::State::rejected));
   assert((access(path.c_str(),F_OK)==0)==(fault==RawPrintFile::Fault::directory));
   job.finish(RawPrintFile::Output,&file);
 }
 assert(symlink(root.c_str(),(root+"/directory-link").c_str())==0);
 for(const auto& path:std::vector<std::string>{"relative.raw",root+"/../bad.raw",root+"/directory-link/bad.raw",root+"/"+std::string(256,'x')}) {
   RawPrintJob bad(8);bad.begin();RawPrintFile file(path);assert(bad.finish(RawPrintFile::Output,&file)==RawPrintJob::State::rejected);
 }
 RawPrintJob job(8);job.begin();RawPrintFile exists(root+"/exact.raw");assert(job.finish(RawPrintFile::Output,&exists)==RawPrintJob::State::rejected);
 for(unsigned boundary=0;boundary<7;++boundary) {
   std::cerr<<"boundary "<<boundary<<"\n";bind();begin();bytes();Savestate<ChunkType> saved;assert(saved.Save(i));
   if(boundary==0)i.Reset(true);
   if(boundary==1)i.Reset(false);
   if(boundary==2)i.SetHostConnection(0,false);
   if(boundary==3) {
     i.reg(0xa9,0xf6);assert(i.HasHostTarget(0));
     i.reg(0x92,0x20000202);i.reg(0x94,0x8001);
     assert((i.get(0x15)&0x101)==0x101);
   }
   if(boundary==4){SavestateLoader<ChunkType> loader;assert(loader.Load(saved.GetBuffer(),saved.GetSize(),i));}
   if(boundary==5)owner.detach();
   if(boundary==6){EmBankRegs::onBeforeDispose.Dispatch();}
   assert(owner.printer.state()==RawPrintJob::State::aborted);
   result.clear();assert(!owner.command("finish",root+"/aborted",result));assert(access((root+"/aborted").c_str(),F_OK)!=0);
 }
 bind();begin();bytes();Savestate<ChunkType> saved;assert(saved.Save(i));
 SavestateLoader<ChunkType> loader;assert(!loader.Load(saved.GetBuffer(),saved.GetSize()-4,i));
 assert(owner.printer.state()==RawPrintJob::State::collecting && owner.printer.size()==4);
 const auto* raw=static_cast<const uint8_t*>(saved.GetBuffer());std::vector<uint8_t> malformed(raw,raw+saved.GetSize());
 uint64 invalidCycles=10001;memcpy(malformed.data()+malformed.size()-16,&invalidCycles,8);
 SavestateLoader<ChunkType> late;assert(!late.Load(malformed.data(),malformed.size(),i));
 Savestate<ChunkType> after;assert(after.Save(i));assert(after.GetSize()==saved.GetSize());assert(memcmp(after.GetBuffer(),saved.GetBuffer(),saved.GetSize())==0);
 assert(owner.printer.state()==RawPrintJob::State::collecting && owner.printer.size()==4);
 auto more=i.transfer(1,1,true,1,{0,1,0,2},0,64);assert(more[0]==4 && owner.printer.size()==8);
 owner.detach();
 auto* old=new Isp;owner.attach(*old);begin();EmBankRegs::onBeforeDispose.Dispatch();delete old;
 auto* replacement=new Isp;owner.attach(*replacement);begin();EmBankRegs::onBeforeDispose.Dispatch();delete replacement;
 assert(owner.printer.state()==RawPrintJob::State::aborted);
 { UsbPrinterRuntime shutdown;shutdown.attach(i);std::string result;assert(shutdown.command("begin","",result)); }
 assert(!i.HasHostTarget(0));
 std::cout<<"PASS production owner, HC packets, cancellation, atomic invalid load and file failures\n";
}
