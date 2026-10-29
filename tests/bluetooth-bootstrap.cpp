#include <cassert>
#include <cstdio>
#include <vector>
#include <string>
#include "BluetoothBootstrap.h"
#include "savestate/savestateAll.h"
struct Model {
    BluetoothBootstrap wire;
    explicit Model(bool e2):wire(e2){}
    template<class S>void Save(S& s){auto* c=s.GetChunk(ChunkType::bcm2035,1);SaveChunkHelper h(*c);wire.Transfer(h);}
    void Load(SavestateLoader<ChunkType>& l){auto*c=l.GetChunkOrFail(ChunkType::bcm2035,1,"bootstrap test");if(!c)return;LoadChunkHelper h(*c);wire.Transfer(h);if(!wire.Valid())l.NotifyError();}
};
static std::vector<uint8_t> drain(BluetoothBootstrap& b,uint64_t now){std::vector<uint8_t>v;for(unsigned i=0;i<256;++i){auto c=b.Read(now);if(c==0x100)return v;v.push_back(c);}assert(false);return v;}
int main(){
 for(bool e2:{false,true}){
    Model m(e2);auto& b=m.wire;std::vector<std::vector<uint8_t>> frames;
    auto sink=[&](const uint8_t*p,size_t n){frames.emplace_back(p,p+n);return true;};
    auto send=[&](const std::string&s,uint64_t now=0){for(uint8_t c:s)assert(b.Write(c,now,sink));};
    for(uint8_t c:{1,3,12,0})assert(b.Write(c,0,sink));
    assert(frames.size()==1 && (frames.back()==std::vector<uint8_t>{1,3,12,0}));
    for(uint8_t c:{1,0x2e,0xfc,0})assert(b.Write(c,0,sink));
    assert(drain(b,9999999).empty());
    assert((drain(b,10000000)==std::vector<uint8_t>{4,14,4,1,0x2e,0xfc,0}));
    assert(drain(b,159999999).empty());
    assert((drain(b,160000000)==std::vector<uint8_t>{'4','1'}));
    send(":0300000002");Savestate<ChunkType> s;assert(s.Save(m));
    std::vector<uint8_t> saved(static_cast<uint8_t*>(s.GetBuffer()),static_cast<uint8_t*>(s.GetBuffer())+s.GetSize());
    send("8EB0BD");assert((drain(b,160000000)==std::vector<uint8_t>{'1'}));
    SavestateLoader<ChunkType> l;assert(l.Load(saved.data(),saved.size(),m));
    send("8EB0BD");assert((drain(b,160000000)==std::vector<uint8_t>{'1'}));
    send(":03000000028EB0BE");assert((drain(b,160000000)==std::vector<uint8_t>{'0'})); // Bad checksum is NAK, never accepted.
    send(":00000001FF");assert((drain(b,160000000)==std::vector<uint8_t>{'1'}));
    send("@0010000034155500000D80003C0808236601B0350A");assert((drain(b,160000000)==std::vector<uint8_t>{'0','0','1'}));
    send("@0010000034155500000D80003C0808236601B0350B");assert((drain(b,160000000)==std::vector<uint8_t>{'0','0','0'}));
    send("Z",160000000);
    if(e2){assert(drain(b,259999999).empty());assert((drain(b,260000000)==std::vector<uint8_t>{4,0xff,1,0}));}
    else assert(drain(b,260000000).empty());
    assert(b.Ready());
    std::vector<uint8_t> acl(65540);acl[0]=2;acl[1]=0x20;acl[3]=acl[4]=0xff;
    for(size_t i=5;i<acl.size();++i)acl[i]=uint8_t(i*73+19);
    for(auto c:acl)assert(b.Write(c,300000000,sink));
    assert(frames.back()==acl);
    assert(!b.Write(5,300000000,sink) && b.Failed());
    b.Reset();assert(b.Write(1,0,{})); // Header alone cannot produce external I/O.
    assert(b.Write(3,0,{}));assert(b.Write(12,0,{}));assert(!b.Write(0,0,{}) && b.Failed());
    puts(e2?"PASS E2 upload/ACK/start notification, checksums, partial rollback, max ACL and fail-closed H4":"PASS TX upload/ACK, checksums, partial rollback, max ACL and fail-closed H4");
 }
 // Reject a forged frame count before a later write can index outside storage.
 Model bad(true);Savestate<ChunkType> s;assert(s.Save(bad));
 auto raw=std::vector<uint8_t>(static_cast<uint8_t*>(s.GetBuffer()),static_cast<uint8_t*>(s.GetBuffer())+s.GetSize());
 SavestateLoader<ChunkType> l;assert(!l.Load(raw.data(),raw.size()-4,bad));
 // One chunk: header count + TOC pair + version, then stage/rx/wanted/head/count.
 for(auto field:{4u,5u,6u,7u,8u}){
    auto forged=raw;auto* words=reinterpret_cast<uint32_t*>(forged.data());words[field]=0xffffffff;
    Model target(true);assert(!l.Load(forged.data(),forged.size(),target));
 }
 auto forged=raw;reinterpret_cast<uint32_t*>(forged.data())[6]=1;
 Model impossible(true);assert(!l.Load(forged.data(),forged.size(),impossible));
 puts("PASS truncated state, forged stage/frame/ring bounds and impossible partial frame refused");
}
