#include <cassert>
#include <cstring>
#include <vector>
#include "EmHAL.h"
#include "EmSPISlaveSD.h"
#include "ExternalStorage.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"

struct Storage : EmHALHandler {
    bool SupportsImageInSlot(EmHAL::Slot slot, uint32 blocks) override {
        return (slot == EmHAL::Slot::sdcard || slot == EmHAL::Slot::sdcard2) && EmSPISlaveSD::IsSizeRepresentable(blocks);
    }
    void Mount(EmHAL::Slot, CardImage&) override {}
    void Unmount(EmHAL::Slot) override {}
    void Remount(EmHAL::Slot, CardImage&) override {}
};
static uint8 Byte(EmSPISlaveSD& sd, uint8 value=255) { return sd.DoExchange(7,value); }
static void Command(EmSPISlaveSD& sd, unsigned command, uint32 argument=0) {
    Byte(sd,0x40|command);
    for(int shift: {24,16,8,0}) Byte(sd,argument>>shift);
    Byte(sd,0x95);
    assert(Byte(sd)==255);
}
static std::vector<uint8> Save(EmSPISlaveSD& sd) {
    Savestate<ChunkType> s; assert(s.Save(sd));
    auto p=static_cast<uint8*>(s.GetBuffer()); return {p,p+s.GetSize()};
}
static bool Load(EmSPISlaveSD& sd,std::vector<uint8> bytes) {
    SavestateLoader<ChunkType> s; return s.Load(bytes.data(),bytes.size(),sd);
}
struct Words {
    std::vector<uint32> words;
    template<class T> void Save(T& s) {
        auto c=s.GetChunk(ChunkType::spiSlaveSD); if(c) for(auto w:words)c->Put32(w);
    }
    void Load(SavestateLoader<ChunkType>& s) {
        auto c=s.GetChunk(ChunkType::spiSlaveSD); assert(c);
        while(!c->IsAtEnd())words.push_back(c->Get32());
    }
};
static std::vector<uint8> Encode(Words& w) {
    Savestate<ChunkType> s; assert(s.Save(w));
    auto p=static_cast<uint8*>(s.GetBuffer()); return {p,p+s.GetSize()};
}
struct Pair {
    EmSPISlaveSD first, second{EmHAL::Slot::sdcard2};
    template<class T> void Save(T& s) { first.Save(s); second.Save(s); gExternalStorage.Save(s); }
    void Load(SavestateLoader<ChunkType>& s) { first.Load(s); second.Load(s); gExternalStorage.Load(s); }
};
struct Metadata {
    unsigned version;
    std::vector<std::string> keys;
    bool terminated=true, trailing=false;
    template<class T> void Save(T& s) {
        auto c=s.GetChunk(ChunkType::externalStorage); if(!c)return;
        c->Put32(version);
        for(auto& key:keys) {
            if(terminated)c->PutString(key,32);
            else {char bytes[33];memset(bytes,'x',33);c->PutBuffer(bytes,33);}
        }
        if(trailing)c->Put32(0);
    }
};
static bool MetadataLoad(Metadata m) {
    Savestate<ChunkType> s;assert(s.Save(m));
    SavestateLoader<ChunkType> l;return l.Load(s.GetBuffer(),s.GetSize(),gExternalStorage);
}
static void Dual() {
    static_assert(static_cast<int>(EmHAL::Slot::sdcard)==0);
    static_assert(static_cast<int>(EmHAL::Slot::memorystick)==1);
    auto a=new uint8[4096](), b=new uint8[4096]();
    a[512]=0x37; b[512]=0xa9;
    assert(gExternalStorage.AddImage("a",a,4096));
    assert(gExternalStorage.AddImage("b",b,4096));
    for(auto slot: {EmHAL::Slot::none,static_cast<EmHAL::Slot>(3),static_cast<EmHAL::Slot>(255)}) {
        assert(!gExternalStorage.IsMounted(slot));
        assert(!gExternalStorage.Mount("a",slot));
        assert(!gExternalStorage.Unmount(slot));
    }
    assert(gExternalStorage.Mount("a",EmHAL::Slot::sdcard));
    assert(!gExternalStorage.Mount("a",EmHAL::Slot::sdcard2));
    assert(gExternalStorage.Mount("b",EmHAL::Slot::sdcard2));
    gExternalStorage.RekeyImage("a","b");
    gExternalStorage.RekeyImage("a","");
    assert(gExternalStorage.GetImage("a")->RawData()==a);
    assert(gExternalStorage.GetImage("b")->RawData()==b);
    assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard)=="a");
    assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard2)=="b");
    Pair pair;
    pair.first.Reset();pair.second.Reset();pair.first.Enable();pair.second.Enable();
    for(auto sd: {&pair.first,&pair.second}){Command(*sd,1);assert(Byte(*sd)==0);}
    Command(pair.first,17,512);assert(Byte(pair.first)==0);Byte(pair.first);Byte(pair.first);
    Command(pair.second,24,512);assert(Byte(pair.second)==0);Byte(pair.second,0xfe);
    for(int i=0;i<29;i++)Byte(pair.second,i);
    Savestate<ChunkType> snapshot;assert(snapshot.Save(pair));
    auto ptr=static_cast<uint8*>(snapshot.GetBuffer());
    std::vector<uint8> bytes(ptr,ptr+snapshot.GetSize());
    SavestateLoader<ChunkType> load;assert(load.Load(bytes.data(),bytes.size(),pair));
    gExternalStorage.Remount();assert(!gExternalStorage.RemountFailed());
    assert(Byte(pair.first)==0x37);
    for(int i=29;i<514;i++)Byte(pair.second,i);
    assert((Byte(pair.second)&31)==5);Byte(pair.second);
    for(int i=0;i<512;i++)assert(b[512+i]==uint8(i));
    assert(a[512]==0x37);
    assert(gExternalStorage.Unmount(EmHAL::Slot::sdcard));
    assert(Byte(pair.first)==255);
    assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard2)=="b");
    assert(gExternalStorage.Mount("a",EmHAL::Slot::sdcard));
    // Loading metadata restores both identities, but committed media bytes do not rewind.
    SavestateLoader<ChunkType> again;assert(again.Load(bytes.data(),bytes.size(),pair));
    gExternalStorage.Remount();
    assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard)=="a");
    assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard2)=="b");
    assert(b[512+29]==29);
    for(auto m: std::vector<Metadata>{
        {0,{"a",""}},{3,{"a","","b"}},{2,{"a","","a"}},
        {2,{"a",""}},{2,{"a","","b"},false},
        {2,{"a","","b"},true,true}}) {
        assert(!MetadataLoad(m));
        gExternalStorage.Remount();
        assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard)=="a");
        assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard2)=="b");
    }
    // A late missing image or unsupported slot must retain the complete live pair.
    // The empty first entry would otherwise eject the first card before the error.
    for (auto m: std::vector<Metadata>{
        {2,{"","","missing"}}, {2,{"a","b",""}},
        {2,{"missing","","b"}}}) {
        assert(MetadataLoad(m));
        gExternalStorage.Remount();
        assert(gExternalStorage.RemountFailed());
        assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard)=="a");
        assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard2)=="b");
        assert(gExternalStorage.GetImage("a")->RawData()==a && a[512]==0x37);
        assert(gExternalStorage.GetImage("b")->RawData()==b && b[512+29]==29);
    }
    assert(MetadataLoad({2,{"a","","b"}}));
    gExternalStorage.Remount();
    assert(!gExternalStorage.RemountFailed());
    // Old snapshots contain only the original SD and Memory Stick names.
    assert(MetadataLoad({1,{"a",""}}));
    gExternalStorage.Remount();
    assert(gExternalStorage.GetImageKeyInSlot(EmHAL::Slot::sdcard)=="a");
    assert(!gExternalStorage.IsMounted(EmHAL::Slot::sdcard2));
    gExternalStorage.Clear();
}
struct FailedReads : RamCardBacking {
    FailedReads() : RamCardBacking(4096) {}
    bool ReadBytes(size_t, size_t, void*) const override { return false; }
};
static void ProviderErrors() {
    auto backing=std::make_shared<FailedReads>();
    auto image=std::make_shared<CardImage>(backing,false,[]{return false;},321);
    assert(gExternalStorage.AddImage("provider",image));
    assert(gExternalStorage.Mount("provider",EmHAL::Slot::sdcard));
    EmSPISlaveSD sd; sd.Reset(); sd.Enable();
    Command(sd,1); assert(Byte(sd)==0);
    Command(sd,17); assert(Byte(sd)==0x40); // Failed provider never sends a data token.
    Command(sd,18); assert(Byte(sd)==0); assert(Byte(sd)==255); assert(Byte(sd)==8);
    Command(sd,24); assert(Byte(sd)==0); Byte(sd,0xfe);
    for(int i=0;i<514;++i) Byte(sd,0x73);
    assert((Byte(sd)&31)==13);
    for(auto b:backing->Bytes()) assert(b==0);
    gExternalStorage.Clear();
}
static void MMC() {
    auto data = new uint8[8*512]();
    auto image = std::make_shared<CardImage>(data, 8, false, CardImage::Protocol::mmc);
    assert(gExternalStorage.AddImage("mmc", image));
    assert(gExternalStorage.Mount("mmc", EmHAL::Slot::sdcard));
    EmSPISlaveSD sd; sd.Reset(); sd.Enable();
    Command(sd,0); assert(Byte(sd)==1);
    Command(sd,55); assert(Byte(sd)==5);
    Command(sd,41); assert(Byte(sd)==5);
    Command(sd,8,0x1aa); assert(Byte(sd)==5);
    Command(sd,58); assert(Byte(sd)==1); assert(Byte(sd)==0);
    assert(Byte(sd)==255); assert(Byte(sd)==128); assert(Byte(sd)==0);
    Command(sd,1); assert(Byte(sd)==0);
    Command(sd,55); assert(Byte(sd)==4);
    Command(sd,51); assert(Byte(sd)==4);
    Command(sd,9); assert(Byte(sd)==0); assert(Byte(sd)==255); assert(Byte(sd)==254);
    std::vector<uint8> csd; for(int i=0;i<16;++i) csd.push_back(Byte(sd));
    Byte(sd); Byte(sd);
    assert(csd[0]==0x8c && csd[3]==0x2a && csd[4]==1 && csd[5]==0x59 && (csd[15]&1));
    Command(sd,10); assert(Byte(sd)==0); Byte(sd); assert(Byte(sd)==254);
    std::vector<uint8> cid; for(int i=0;i<16;++i) cid.push_back(Byte(sd));
    Byte(sd); Byte(sd);
    assert(!memcmp(cid.data()+3,"FRNDMC",6) && cid[9]==0x10 && cid[14]==0x91 && (cid[15]&1));
    Command(sd,24,512); assert(Byte(sd)==0); Byte(sd,254);
    for(int i=0;i<66;++i) Byte(sd,0x41);
    auto pending=Save(sd); assert(Load(sd,pending)); assert(sd.IsProtocol(CardImage::Protocol::mmc));
    Words original; { SavestateLoader<ChunkType> l; assert(l.Load(pending.data(),pending.size(),original)); }
    for(auto item:std::vector<std::pair<unsigned,uint32>>{{5,1},{unsigned(original.words.size()-1),2},{0,1}}) {
        auto bad=original; bad.words[item.first]=item.second;
        assert(!Load(sd,Encode(bad))); assert(Save(sd)==pending);
    }
    for(int i=66;i<514;++i) Byte(sd,0x41);
    assert((Byte(sd)&31)==5); Byte(sd);
    assert(data[512]==0x41);
    sd.Disable(); assert(gExternalStorage.Unmount(EmHAL::Slot::sdcard));
    sd.Reset(); auto ejected=Save(sd); assert(Load(sd,ejected));
    assert(sd.IsProtocol(CardImage::Protocol::mmc));
    assert(gExternalStorage.Mount("mmc",EmHAL::Slot::sdcard)); sd.Reset(); sd.Enable();
    Command(sd,1); assert(Byte(sd)==0);
    Command(sd,17,513); assert(Byte(sd)==0x20);
    Command(sd,17,8*512); assert(Byte(sd)==0x40);
    Command(sd,0); assert(Byte(sd)==1); Command(sd,17); assert(Byte(sd)==1);
    gExternalStorage.Clear();
}
int main() {
    Storage storage;
    assert(!EmSPISlaveSD::IsSizeRepresentable(0));
    assert(!EmSPISlaveSD::IsSizeRepresentable(3));
    assert(!EmSPISlaveSD::IsSizeRepresentable(5));
    assert(EmSPISlaveSD::IsSizeRepresentable(8));
    auto data=new uint8[8*512](); data[0]=0x31; data[512]=0x72;
    assert(gExternalStorage.AddImage("test",data,8*512));
    assert(gExternalStorage.Mount("test",EmHAL::Slot::sdcard));
    EmSPISlaveSD sd; sd.Reset(); sd.Enable();
    Command(sd,1); assert(Byte(sd)==0);
    Command(sd,16,513); assert(Byte(sd)==0x40);
    Command(sd,16,512); assert(Byte(sd)==0);
    // An unaligned address still uses bits 31:9; the low byte must not become
    // the high byte. A nonzero low byte here must not change the block index.
    Command(sd,17,512+0xa5); assert(Byte(sd)==0);
    assert(Byte(sd)==255); assert(Byte(sd)==0xfe); assert(Byte(sd)==0x72);
    for(int i=0;i<513;i++)Byte(sd);
    // Partial read and write cursors continue at the same byte.
    Command(sd,24,512); assert(Byte(sd)==0); Byte(sd,0xfe);
    for(int i=0;i<137;i++)Byte(sd,i);
    assert(sd.IsBlockInRange(8));
    assert(!sd.IsBlockInRange(1));
    auto partial=Save(sd);
    assert(Load(sd,partial));
    for(int i=137;i<514;i++)Byte(sd,i);
    assert((Byte(sd)&31)==5); assert(Byte(sd)==255);
    for(int i=0;i<512;i++)assert(data[512+i]==uint8(i));
    Command(sd,17,512); assert(Byte(sd)==0); Byte(sd); Byte(sd);
    for(int i=0;i<137;i++)assert(Byte(sd)==uint8(i));
    partial=Save(sd); assert(Load(sd,partial));
    for(int i=137;i<512;i++)assert(Byte(sd)==uint8(i));
    Byte(sd); Byte(sd);
    auto stable=Save(sd);
    Words original; { SavestateLoader<ChunkType> l; assert(l.Load(stable.data(),stable.size(),original)); }
    // Production descriptor preflight also checks the staged media geometry.
    auto wrongBlock=original; wrongBlock.words[4]=9;
    assert(Load(sd,Encode(wrongBlock))); assert(!sd.IsBlockInRange(8));
    assert(Load(sd,stable));
    // Rejected state leaves the live protocol untouched.
    for(auto item: std::vector<std::pair<unsigned,uint32>>{
        {0,0},{0,2},{1,0xff000000},{1,0x00ff0000},{1,0x00004000},
        {1,7},{1,0x04010000},{1,0x06010000},{2,551},{3,0xffffffff},{5,2}}) {
        auto bad=original; bad.words[item.first]=item.second;
        assert(!Load(sd,Encode(bad))); assert(Save(sd)==stable);
    }
    auto shortState=original;shortState.words.pop_back();
    assert(!Load(sd,Encode(shortState)));assert(Save(sd)==stable);
    auto longState=original;longState.words.push_back(0);
    assert(!Load(sd,Encode(longState)));assert(Save(sd)==stable);
    // End-of-media multiblock reads and writes never wrap onto sector zero.
    Command(sd,18,7*512);assert(Byte(sd)==0);assert(Byte(sd)==255);
    assert(Byte(sd)==0xfe);for(int i=0;i<514;i++)Byte(sd);
    assert(Byte(sd)==255);assert(Byte(sd)==8);
    Command(sd,25,7*512);assert(Byte(sd)==0);
    for(int block=0;block<2;block++) {
        Byte(sd,0xfc);for(int i=0;i<514;i++)Byte(sd,0x91);
        if(block==0){assert((Byte(sd)&31)==5);assert(Byte(sd)==255);}
        else assert((Byte(sd)&31)==13);
    }
    assert(data[0]==0x31);
    Byte(sd,0xfd);Command(sd,0);assert(Byte(sd)==1);
    Command(sd,17);assert(Byte(sd)==1);
    gExternalStorage.Clear();assert(Byte(sd)==255);
    Dual();
    ProviderErrors();
    MMC();
}
