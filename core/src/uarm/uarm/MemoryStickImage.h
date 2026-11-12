#ifndef FROND_MEMORY_STICK_IMAGE_H
#define FROND_MEMORY_STICK_IMAGE_H
#include "MemoryStickMedia.h"
#include "storage/Container.h"
#include "storage/MsLayout.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "zip/miniz.h"
namespace frondMs {
using Geometry=frondMsGeometry::Geometry;
constexpr size_t headerBytes=64, maxFileBytes=headerBytes+MemoryStickMedia::maxImageBytes;
inline size_t fileBytes(const Geometry& g){return headerBytes+size_t(g.PhysicalBytes());}
inline uint32_t get32(const uint8_t* p) {return p[0]|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline void put32(uint8_t* p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=n>>(8*i);}
inline void be16(uint8_t* p,uint16_t n){p[0]=n>>8;p[1]=n;}
inline uint16_t getBe16(const uint8_t* p){return uint16_t((p[0]<<8)|p[1]);}
inline uint32_t crc32(const uint8_t* bytes,size_t size) { return mz_crc32(0,bytes,size); }
// Header bytes 32 to 39 hold the stick's change counter (u64): 0 until a machine writes to the stick,
// then one more for each open or insert that writes to it. The rest of the header is zero.
constexpr size_t changesAt=32;
inline uint64_t headerChanges(const uint8_t* h){return get32(h+changesAt)|(uint64_t(get32(h+changesAt+4))<<32);}
inline void putChanges(uint8_t* h,uint64_t n){put32(h+changesAt,uint32_t(n));put32(h+changesAt+4,uint32_t(n>>32));}
// The geometry a v1 header names, or nullptr if it isn't one of the classic sizes.
inline const Geometry* headerGeometry(const uint8_t* h) {
    if(std::memcmp(h,"FRONDMS\0",8)||get32(h+8)!=1||get32(h+20)!=512||get32(h+24)!=9)return nullptr;
    for(unsigned i=changesAt+8;i<64;++i)if(h[i])return nullptr;
    return frondMsGeometry::byShape(get32(h+12),get32(h+16));
}
// Both boot blocks must carry the attributes the TG50 driver checks, for the stick's own size.
// `boot` is physical blocks 0 and 1, pages of 512+9 bytes.
inline bool validBootBlocks(const uint8_t* boot,const Geometry& g) {
    for(unsigned block=0;block<2;++block){
        const auto* b=boot+size_t(block)*g.pagesPerBlock*MemoryStickMedia::recordBytes;
        if(b[0]||b[1]!=1||b[2]!=1||b[3]!=0||b[0xbc]!=2||b[0x178]!=1||
           b[0x170]||b[0x171]||b[0x172]||b[0x173]||b[0x174]||b[0x175]||b[0x176]||!b[0x177]||
           b[0x1a0]!=1||b[0x1a1]!=2||getBe16(b+0x1a2)!=g.BlockKib()||getBe16(b+0x1a4)!=g.blocks||
           getBe16(b+0x1a6)!=g.Effective()||b[0x1a8]!=2||b[0x1a9]||b[0x1aa]!=16||b[0x1d6]!=1||
           !(b[512]&0x80)||(b[513]&4))return false;
    }
    return true;
}
inline bool validBoot(const std::vector<uint8_t>& image) {
    const auto* g=frondMsGeometry::byPhysicalBytes(image.size());
    return g&&validBootBlocks(image.data(),*g);
}
// A blank stick of one of the classic sizes, with boot blocks at physical blocks 0 and 1.
inline std::vector<uint8_t> create(const Geometry& g=frondMsGeometry::largest) {
    std::vector<uint8_t> image(size_t(g.PhysicalBytes()),0xff);
    for(unsigned block=0;block<2;++block)for(unsigned page=0;page<g.pagesPerBlock;++page){
        auto* b=image.data()+(size_t(block)*g.pagesPerBlock+page)*MemoryStickMedia::recordBytes;
        std::fill_n(b,512,0);b[512]=0xff;b[513]=0xfb;b[514]=b[515]=0;
        if(page==0){be16(b,1);be16(b+2,0x0100);b[0xbc]=2;b[0x177]=2;b[0x178]=1;
            b[0x1a0]=1;b[0x1a1]=2;be16(b+0x1a2,g.BlockKib());be16(b+0x1a4,g.blocks);be16(b+0x1a6,g.Effective());
            be16(b+0x1a8,512);b[0x1aa]=16;b[0x1ad]=20;b[0x1ae]=26;b[0x1af]=11;b[0x1b0]=1;
            b[0x1b8]=1;b[0x1d6]=1;
        }else if(page==1)b[0]=b[1]=0xff;
    }
    return image;
}
inline std::vector<uint8_t> encode(const std::vector<uint8_t>& physical) {
    if(!validBoot(physical))throw std::runtime_error("Invalid classic Memory Stick boot blocks.");
    const auto& g=*frondMsGeometry::byPhysicalBytes(physical.size());
    std::vector<uint8_t> file(fileBytes(g));
    std::memcpy(file.data(),"FRONDMS",7);put32(file.data()+8,1);put32(file.data()+12,g.blocks);
    put32(file.data()+16,g.pagesPerBlock);put32(file.data()+20,512);put32(file.data()+24,9);
    put32(file.data()+28,crc32(physical.data(),physical.size()));
    std::copy(physical.begin(),physical.end(),file.begin()+headerBytes);return file;
}
inline bool decode(const uint8_t* file,size_t size,std::vector<uint8_t>& physical) {
    if(size<headerBytes)return false;
    const auto* g=headerGeometry(file);
    if(!g||size!=fileBytes(*g))return false;
    if(get32(file+28)!=crc32(file+64,size-64))return false;
    std::vector<uint8_t> candidate(file+64,file+size);
    if(!validBoot(candidate))return false;
    physical.swap(candidate);return true;
}
// decode() of a .frondms on disk, reading the payload straight into one image-sized vector.
inline bool decodeFile(const std::filesystem::path& path,std::vector<uint8_t>& physical) {
    std::ifstream in(path,std::ios::binary);
    if(!in)return false;
    uint8_t h[headerBytes];
    if(!in.read(reinterpret_cast<char*>(h),headerBytes))return false;
    const auto* g=headerGeometry(h);
    if(!g)return false;
    std::vector<uint8_t> candidate(size_t(g->PhysicalBytes()));
    if(!in.read(reinterpret_cast<char*>(candidate.data()),std::streamsize(candidate.size())))return false;
    if(in.peek()!=std::char_traits<char>::eof())return false;
    if(get32(h+28)!=crc32(candidate.data(),candidate.size())||!validBoot(candidate))return false;
    physical.swap(candidate);return true;
}

// v2 .frondms: the storage container with magic "FRONDMS\0" over the LogicalMsMedia layout.
inline const frondMsLayout::Layout* layoutOf(const Geometry& g){return frondMsLayout::byShape(g.blocks,g.pagesPerBlock);}
inline const Geometry* geometryOf(const frondMsLayout::Layout& l){return frondMsGeometry::byShape(l.blocks,l.pagesPerBlock);}
// The geometry of a v2 header, or nullptr.
inline const Geometry* v2Geometry(const uint8_t* h,frondSdFile::Info* infoOut=nullptr) {
    frondSdFile::Info info;
    if(!frondSdFile::parseContainer(h,info)||!info.classic)return nullptr;
    const auto* l=frondMsLayout::of(info);
    if(!l)return nullptr;
    if(infoOut)*infoOut=info;
    return geometryOf(*l);
}
// The stored bytes (payload, padding, side region) of a flat physical image.
inline bool storedOf(const std::vector<uint8_t>& physical,std::vector<uint8_t>& stored) {
    const auto* g=frondMsGeometry::byPhysicalBytes(physical.size());
    const auto* l=g?layoutOf(*g):nullptr;
    return l&&LogicalMsMedia::Build(*l,physical.data(),stored);
}
// Reads a stick through LogicalMsMedia over `read` on its stored bytes.
inline bool openStored(LogicalMsMedia& media,const Geometry& g,LogicalMsMedia::ReadFn read) {
    const auto* l=layoutOf(g);
    return l&&media.Open(*l,std::move(read),[](uint64_t,uint64_t,const void*){return false;});
}
inline bool physicalOf(const LogicalMsMedia& media,const Geometry& g,std::vector<uint8_t>& physical) {
    std::vector<uint8_t> out(size_t(g.PhysicalBytes()));
    for(uint32_t b=0;b<g.blocks;++b)for(uint32_t p=0;p<g.pagesPerBlock;++p)
        if(!media.Read(b,p,out.data()+(size_t(b)*g.pagesPerBlock+p)*MemoryStickMedia::recordBytes))return false;
    physical.swap(out);return true;
}
// The logical image other tools take: user blocks x block bytes, L's data from its holder, zeros
// for an L nobody holds (as the Sony driver reads one).
inline bool logicalOf(const LogicalMsMedia& media,std::vector<uint8_t>& image) {
    const auto& l=media.Shape();
    std::vector<uint8_t> out(size_t(l.PayloadBytes()),0);
    uint8_t record[MemoryStickMedia::recordBytes];
    for(uint32_t L=0;L<l.UserBlocks();++L){
        const int32_t holder=media.Holder(L);
        if(holder<0)continue;
        for(uint32_t p=0;p<l.pagesPerBlock;++p){
            if(!media.Read(uint32_t(holder),p,record))return false;
            std::memcpy(out.data()+L*l.BlockBytes()+uint64_t(p)*512,record,512);
        }
    }
    image.swap(out);return true;
}
// The smallest classic size whose logical image holds `bytes`, a multiple of 512.
inline const Geometry* geometryForLogical(uint64_t bytes) {
    if(!bytes||bytes%512)return nullptr;
    for(const auto& l:frondMsLayout::layouts)if(bytes<=l.PayloadBytes())return geometryOf(l);
    return nullptr;
}
// A physical image of a logical one: fresh boot blocks, and L in physical block order (each L at
// its home block), every page's extra bytes f8 ff L ff ff ff ff ff as the Sony driver writes a
// new block. A short image is padded with zeros.
inline std::vector<uint8_t> fromLogical(const uint8_t* image,uint64_t bytes,const Geometry& g) {
    auto physical=create(g);
    const auto& l=*layoutOf(g);
    for(uint32_t L=0;L<l.UserBlocks();++L){
        const uint32_t block=l.Home(L);
        for(uint32_t p=0;p<g.pagesPerBlock;++p){
            auto* r=physical.data()+(size_t(block)*g.pagesPerBlock+p)*MemoryStickMedia::recordBytes;
            const uint64_t at=L*l.BlockBytes()+uint64_t(p)*512;
            std::memset(r,0,512);
            if(at<bytes)std::memcpy(r,image+at,size_t(std::min<uint64_t>(512,bytes-at)));
            r[512]=0xf8;r[513]=0xff;be16(r+514,uint16_t(L));std::fill_n(r+516,5,0xff);
        }
    }
    return physical;
}
// A new stick as Frond makes one: boot blocks, and every L at its home block with zero data, like a
// stick wiped on a PC. The TG50 driver then finds every L in its first mount scan; on an erased stick
// it reads every page of each free block before it claims one for an L, about 50 s at 128 MB.
inline std::vector<uint8_t> blank(const Geometry& g){return fromLogical(nullptr,0,g);}
// A v1 or v2 .frondms as a flat physical image. v2 is read whole; its CRCs are checked.
inline bool readPhysical(const std::filesystem::path& path,std::vector<uint8_t>& physical) {
    std::ifstream in(path,std::ios::binary);
    uint8_t h[headerBytes];
    if(!in||!in.read(reinterpret_cast<char*>(h),headerBytes))return false;
    if(get32(h+8)==1)return decodeFile(path,physical);
    frondSdFile::Info info;
    const auto* g=v2Geometry(h,&info);
    if(!g)return false;
    std::vector<uint8_t> file(size_t(info.fileBytes()));
    in.seekg(0);
    if(!in.read(reinterpret_cast<char*>(file.data()),std::streamsize(file.size()))||in.peek()!=std::char_traits<char>::eof())return false;
    std::vector<uint32_t> table;
    if(!frondSdFile::readTable(file.data(),file.data()+headerBytes+info.stored(),size_t(4*info.chunks()),info,table))return false;
    frondSdFile::ChunkCrcs crcs;crcs.add(file.data()+headerBytes,size_t(info.stored()));
    if(crcs.table()!=table)return false;
    LogicalMsMedia media;
    const auto* stored=file.data()+headerBytes;const auto size=info.stored();
    if(!openStored(media,*g,[stored,size](uint64_t off,uint64_t n,void* dest){
        if(off>size||n>size-off)return false;
        std::memcpy(dest,stored+off,size_t(n));return true;}))return false;
    std::vector<uint8_t> candidate;
    if(!physicalOf(media,*g,candidate)||!validBoot(candidate))return false;
    physical.swap(candidate);return true;
}
}
#endif
