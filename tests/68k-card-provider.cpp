#include "CardImage.h"
#include <cassert>
#include <cstring>
#include "FileCardBacking.h"
#include "card-history-limits.h"
#include <fstream>
#include <filesystem>
#include "../build/source/src/cloudpilot/frond/PortablePublication.h"
#include <iostream>
int main(int argc,char** argv) {
    assert(argc==2);
    const std::filesystem::path dir=std::string(argv[1])+"-"+std::to_string(getpid());
    assert(std::filesystem::create_directory(dir));
    HistoryLimits(dir/"history-limits", true);
    const auto path=dir/"card.frondsd";
    { std::ofstream f(path,std::ios::binary); std::string zeros(8192,0); f.write(zeros.data(),zeros.size()); }
    FileCardOptions options; options.identity="a";options.rejectInvalidRoot=true;
    FileCardIoHooks hooks;
    auto file=FileCardBacking::Open(path,8192,0,options,&hooks);assert(file);
    uint8_t sector[512];memset(sector,0x39,sizeof sector);
    assert(file->Write(0,512,sector) && file->Commit());
    file.reset();
    options.identity="b";
    assert(!FileCardBacking::Open(path,8192,0,options));
    assert(std::filesystem::exists(dir/".gen-card.frondsd-root"));
    options.identity="a";
    file=FileCardBacking::Open(path,8192,0,options,&hooks);assert(file);
    assert(file->Read(0,512,sector) && sector[0]==0x39);
    hooks.failReadAfter=hooks.reads;
    CardImage unreadable(std::shared_ptr<CardBacking>(std::move(file)),false,[]{return false;},1);
    assert(unreadable.Read(sector,15)==0);

    // Every malformed open must close its descriptor and leave the namespace unchanged.
    const auto hostile=dir/"hostile";
    { std::ofstream f(hostile); f << std::string(8192,0); }
    auto good=FileCardBacking::Open(hostile,8192,0,options); assert(good);
    memset(sector,0x71,sizeof sector); assert(good->Write(0,512,sector));
    assert(good->Write(4096,512,sector) && good->Commit());
    const auto gid=good->RootGenerationId(); good.reset();
    const auto rootPath=dir/".gen-hostile-root",genPath=dir/(".gen-hostile-"+std::to_string(gid));
    auto bytes=[](const auto& path) { std::ifstream f(path,std::ios::binary); return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),{}); };
    auto put=[](const auto& path,const auto& b) { std::ofstream f(path,std::ios::binary|std::ios::trunc); f.write(reinterpret_cast<const char*>(b.data()),b.size()); };
    const auto rootBytes=bytes(rootPath),genBytes=bytes(genPath);
    auto fds=[] { size_t n=0; for(const auto& e:std::filesystem::directory_iterator("/proc/self/fd")) { (void)e; ++n; } return n; };
    auto refuse=[&](const std::string& label) {
        const auto before=bytes(genPath),baseBefore=bytes(hostile),rootBefore=bytes(rootPath); const auto count=fds();
        size_t entries=0;for(const auto& e:std::filesystem::directory_iterator(dir)){(void)e;++entries;}
        for(int i=0;i<32;++i) assert(!FileCardBacking::Open(hostile,8192,0,options));
        size_t afterEntries=0;for(const auto& e:std::filesystem::directory_iterator(dir)){(void)e;++afterEntries;}
        assert(fds()==count && bytes(genPath)==before && bytes(hostile)==baseBefore && bytes(rootPath)==rootBefore && entries==afterEntries); std::cout << label << " PASS\n";
    };
    std::filesystem::remove(rootPath); refuse("missing root surviving generation"); put(rootPath,rootBytes);
    options.requireRoot=true; options.minimumRoot=gid;
    std::filesystem::remove(genPath); refuse("missing expected generation");
    std::filesystem::remove(rootPath); refuse("missing all expected history");
    put(rootPath,rootBytes);put(genPath,genBytes);
    auto mutate=[&](const char* label,size_t at,uint64_t value) { auto b=genBytes; frondSdFile::put64(b.data()+at,value);put(genPath,b);refuse(label); };
    mutate("unknown generation version",8,99);
    mutate("wrong generation identity",24,gid+1);
    mutate("payload geometry overflow",16,UINT64_MAX);
    mutate("reserved schema bytes",56,1);
    mutate("cyclic parent",32,gid);
    mutate("overflow index count",40,UINT64_MAX);
    mutate("overflow index offset",48,UINT64_MAX);
    const auto index=frondSdFile::get64(genBytes.data()+48);
    mutate("page outside geometry",index,2);
    mutate("duplicate unordered page",index+16,0);
    auto reversed=genBytes;frondSdFile::put64(reversed.data()+index,1);frondSdFile::put64(reversed.data()+index+16,0);
    put(genPath,reversed);refuse("descending page order");
    mutate("payload inside header",index+8,1);
    mutate("unaligned payload",index+8,65);
    mutate("payload past index",index+8,index);
    mutate("payload offset overflow",index+8,UINT64_MAX-1);
    mutate("page number overflow",index,UINT64_MAX);
    for(size_t length: {size_t(12),size_t(index-1),genBytes.size()-1}) {
        auto b=genBytes;b.resize(length);put(genPath,b);refuse("truncated generation");
    }
    // A long acyclic chain must stop before opening an unbounded number of files.
    auto deepRoot=rootBytes;frondSdFile::put64(deepRoot.data()+12,300);put(rootPath,deepRoot);
    for(uint64_t id=300;id>=44;--id) {
        std::vector<uint8_t> b(64,0);memcpy(b.data(),"FRONDGEN",8);
        frondSdFile::put32(b.data()+8,1);frondSdFile::put32(b.data()+12,4096);
        frondSdFile::put64(b.data()+16,8192);frondSdFile::put64(b.data()+24,id);
        frondSdFile::put64(b.data()+32,id==44?0:id-1);frondSdFile::put64(b.data()+48,64);
        put(dir/(".gen-hostile-"+std::to_string(id)),b);
    }
    refuse("bounded deep chain");
    for(uint64_t id=300;id>=44;--id) std::filesystem::remove(dir/(".gen-hostile-"+std::to_string(id)));
    put(rootPath,rootBytes);
    put(genPath,genBytes);
    assert(FileCardBacking::Open(hostile,8192,0,options));
    options.requireRoot=false; options.minimumRoot=0;
    const auto pristine=dir/"pristine"; { std::ofstream f(pristine); f << std::string(8192,0); }
    assert(FileCardBacking::Open(pristine,8192,0,options));
    // The production publication helper injects only filesystem operations.
    for(int failure=0;failure<3;++failure) {
        const auto stage=dir/("stage"+std::to_string(failure)),dest=dir/("dest"+std::to_string(failure));
        std::filesystem::create_directory(stage); { std::ofstream f(stage/"complete");f << "complete"; }
        int calls=0;
        auto result=PublishPortable(stage,dest,[&](const auto&) { return ++calls!=failure+1; });
        assert(result==(failure==0?PortablePublication::refused:failure==1?PortablePublication::publishedUncertain:PortablePublication::durable));
        assert(std::filesystem::exists((failure==0?stage:dest)/"complete"));
        if(failure==2) assert(PublishPortable(dir/"absent",dest,[](const auto&){return true;})==PortablePublication::refused);
    }
    std::cout << "portable pre/post publication sync outcomes PASS\n";

    auto backing=std::make_shared<RamCardBacking>(4096);
    bool fail=false;
    CardImage image(backing,false,[&] { return !fail; },123);
    uint8_t in[512],out[512]; memset(in,0x52,sizeof in);
    assert(image.RawData()==nullptr && image.Serial()==123);
    assert(image.Write(in,7)==1 && image.Read(out,7)==1 && !memcmp(in,out,512));
    fail=true; memset(in,0x81,sizeof in);
    assert(image.Write(in,7)==0 && image.Read(out,7)==1 && out[0]==0x52);
    CardImage ro(backing,true,[]{return true;},124);
    assert(ro.Write(in,0)==0 && !ro.WriteByteRange(in,0,512));
    assert(image.Read(out,8)==0 && !image.ReadByteRange(out,4095,2));
    assert(image.Write(in,8)==0);
    std::filesystem::remove_all(dir);
}
