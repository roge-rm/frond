#include <cassert>
#include <fstream>
#include <map>
#include <iterator>
#include "NativeDatabase.h"
struct ArmCpu { uint32_t regs[16]{}; };
uint32_t* cpuGetRegisters(ArmCpu* cpu) { return cpu->regs; }
struct Fake {
    ArmCpu cpu;
    std::vector<uint8_t> memory = std::vector<uint8_t>(131072);
    std::vector<uint8_t> pdb, installed;
    std::map<uint32_t,std::function<void(ArmCpu*)>> callbacks;
    bool writeFailure=false, readFailure=false, replacement=false;
    unsigned deletes=0,exports=0;
    uint32_t requestSize=128,exportSize=127;
    uint32_t DataAddress() const {return 1024;}
    uint32_t DataSize() const {return 256;}
    bool Read(uint32_t p,void* out,size_t n) {if(p>memory.size() || n>memory.size()-p)return false; memcpy(out,memory.data()+p,n);return true;}
    bool Write(uint32_t p,const void* in,size_t n) {if(p>memory.size() || n>memory.size()-p)return false;memcpy(memory.data()+p,in,n);return true;}
    void RegisterCallback(uint32_t p,std::function<void(ArmCpu*)> f){assert(!callbacks.count(p));callbacks[p]=std::move(f);}
    void UnregisterCallback(uint32_t p){assert(callbacks.erase(p)==1);}
    uint32_t Call(uint16_t trap,std::array<uint32_t,4> args,std::vector<uint32_t> stack) {
        auto& r=cpu.regs;
        switch(trap) {
        case 0x8584:return 1024;
        case 0x84b8:return 0;
        case 0x815c:return !strcmp(reinterpret_cast<char*>(memory.data()+args[0]),"missing") ? 0 : 0x80000010;
        case 0x88f8:return 0;
        case 0x812c: {
            auto be32=[&](size_t at){return (uint32_t(pdb[at])<<24)|(uint32_t(pdb[at+1])<<16)|(uint32_t(pdb[at+2])<<8)|pdb[at+3];};
            uint32_t creator=be32(64),type=be32(60);uint16_t attributes=(uint16_t(pdb[32])<<8)|pdb[33];
            Write(stack.at(7),&creator,4);
            if(args[1])Write(args[1],pdb.data(),32);
            if(args[2])Write(args[2],&attributes,2);
            if(stack.at(6))Write(stack.at(6),&type,4);
            return 0;
        }
        case 0x81d4:return 2;
        case 0x8188:return args[0] ? 0x80000010 : 0x20001000;
        case 0x8148:++deletes;return 0;
        case 0x8304: {
            ++exports;size_t pos=0;
            while(pos<pdb.size()) {
                uint32_t n=uint32_t(std::min<size_t>(exportSize,pdb.size()-pos));
                Write(8192,pdb.data()+pos,n); Write(4096,&n,4);
                r[0]=writeFailure ? uint32_t(memory.size()+1):8192;r[1]=4096;
                callbacks.at(args[0])(&cpu); if(r[0])return 1;pos+=n;
            }
            return 0;
        }
        case 0x82fc: {
            assert(stack.size()==2 && stack[1]==1);
            if(replacement) {r[2]=0x80000010;callbacks.at(args[1])(&cpu);if(!r[0])return 1;}
            installed.clear();
            for (;;) {
                uint32_t n=requestSize;Write(4096,&n,4);r[0]=8192;r[1]=4096;
                if(readFailure) r[1]=uint32_t(memory.size()+1);
                callbacks.at(args[0])(&cpu);if(r[0])return 1;
                Read(4096,&n,4);if(!n)break;
                installed.insert(installed.end(),memory.begin()+8192,memory.begin()+8192+n);
            }
            return 0;
        }
        }
        assert(false);return 1;
    }
};
void verifyZip(const std::vector<uint8_t>& bytes,const std::vector<uint8_t>& expected) {
    auto* zip=zip_stream_open(reinterpret_cast<const char*>(bytes.data()),bytes.size(),0,'r');
    assert(zip && zip_entries_total(zip)==1);
    std::string name(reinterpret_cast<const char*>(expected.data())); name += ".pdb";
    assert(zip_entry_open(zip,name.c_str())==0);
    void* raw=nullptr;size_t size=0;
    assert(zip_entry_read(zip,&raw,&size)>=0 && size==expected.size());
    assert(!memcmp(raw,expected.data(),size));free(raw);
    zip_entry_close(zip);zip_stream_close(zip);
}
int main(){
    assert(dbArchiveName("folder/name",0,0,false)=="folder%2Fname.pdb");
    assert(dbArchiveName("folder%2Fname",0,0,false)=="folder%252Fname.pdb");
    assert(dbArchiveName("../name\\part",1,0,false)=="..%2Fname%5Cpart.prc");
    assert(dbArchiveName("plain",0x200,0x636c7072,false)=="plain.pqa");
    Fake f;std::ifstream in("tests/fixtures/frond-pattern.pdb",std::ios::binary);f.pdb={std::istreambuf_iterator<char>(in),{}};assert(f.pdb.size()>78);
    NativeDatabase<Fake> db(f);
    assert(db.Find("missing")==0);assert(db.Launch("test"));
    assert(db.Install(f.pdb.data(),f.pdb.size())==0);assert(f.installed==f.pdb);assert(f.callbacks.empty());
    f.replacement=true;assert(db.Install(f.pdb.data(),f.pdb.size())==0);assert(f.deletes==1);assert(f.callbacks.empty());
    f.readFailure=true;assert(db.Install(f.pdb.data(),f.pdb.size())<0);assert(f.callbacks.empty());f.readFailure=false;
    auto zip=db.Export("test");assert(zip.size()>4 && zip[0]=='P' && zip[1]=='K');assert(f.callbacks.empty());verifyZip(zip,f.pdb);
    f.writeFailure=true;bool failed=false;try{db.Export("test");}catch(const std::runtime_error&){failed=true;}assert(failed && f.callbacks.empty());f.writeFailure=false;
    auto all=db.Export("");assert(all.size()>4);verifyZip(all,f.pdb);
    // List reads the directory entry of each RAM database only: no exports, no callbacks.
    {const auto exports=f.exports;auto list=db.List();assert(list.size()==1 && f.exports==exports && f.callbacks.empty());
     assert(!strncmp(list[0].name,reinterpret_cast<const char*>(f.pdb.data()),32) && list[0].localID==0x80000010);
     assert(list[0].type==((uint32_t(f.pdb[60])<<24)|(uint32_t(f.pdb[61])<<16)|(uint32_t(f.pdb[62])<<8)|f.pdb[63]));
     assert(list[0].attr==((uint16_t(f.pdb[32])<<8)|f.pdb[33]));}
    f.pdb.resize(200000,0x5a);f.requestSize=100000;f.exportSize=70000;
    assert(db.Install(f.pdb.data(),f.pdb.size())==0 && f.installed==f.pdb);
    verifyZip(db.Export("test"),f.pdb);
    memcpy(f.pdb.data()+64,"a68k",4);const auto exports=f.exports;
    auto empty=db.Export("");auto* archive=zip_stream_open(reinterpret_cast<const char*>(empty.data()),empty.size(),0,'r');
    assert(archive && zip_entries_total(archive)==0 && f.exports==exports);zip_stream_close(archive);
    assert(db.List().empty());
    db.Finish();
}
