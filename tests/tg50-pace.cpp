#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>
#include "pace_patch.h"
extern "C" void uarmAbort() { std::abort(); }
static const uint8_t signature[] = {
        0xF1, 0x4F, 0x2D, 0xE9, 0x00, 0x00, 0x9D, 0xE5, 0x44, 0xA0, 0x90, 0xE5, 0x48, 0x40, 0x90,
        0xE5, 0x04, 0x50, 0x80, 0xE2, 0x24, 0x60, 0x80, 0xE2, 0xB2, 0x70, 0xDA, 0xE0, 0x44, 0xB0,
        0x8F, 0xE2, 0x01, 0x00, 0x00, 0xEA, 0xC0, 0x0C, 0x14, 0xE3, 0x47, 0x17, 0x00, 0x1A, 0x07,
        0x84, 0xA0, 0xE1, 0x27, 0x84, 0x88, 0xE1, 0xF0, 0x0C, 0x08, 0xE2, 0x20, 0x05, 0x9B, 0xE7,
        0xB2, 0x70, 0xDA, 0xE0, 0x00, 0xF0, 0x8B, 0xE0, 0x00, 0x00, 0x9D, 0xE5, 0x02, 0xA0, 0x4A,
        0xE2, 0x44, 0xA0, 0x80, 0xE5, 0x48, 0x40, 0x80, 0xE5, 0x00, 0x80, 0x80, 0xE5, 0x18, 0x90,
        0x9D, 0xE5, 0x00, 0x00, 0xA0, 0xE3, 0xF2, 0x4F, 0xBD, 0xE8, 0x1E, 0xFF, 0x2F, 0xE1};


using Image = std::vector<uint8_t>;
constexpr size_t entry = 0x100064, db = 0xd2f68, start = 0xd2fe8, end = 0x10b0d8;
static void word(Image& b, size_t off, uint32_t value) { memcpy(b.data()+off,&value,4); }
static Image image() {
    Image b(0xc70000);
    memcpy(b.data()+db,"PACE",4);
    word(b,db+0x20,0x10003); word(b,db+0x3c,0x6c696272); word(b,db+0x40,0x6136386b);
    word(b,db+0x4c,3);
    const uint32_t table[] = {0x616d6463,0,0x200d2fe8,0x616d6464,0,0x2010b0d8,0x616d6469,0,0x2010b12c};
    memcpy(b.data()+db+0x50,table,sizeof(table));
    memcpy(b.data()+entry,signature,sizeof(signature));
    word(b,entry+0x5b6c,0xeb0000a6);
    { const uint32_t prologue[] = {0xe92d4030,0xe1a04000,0xe3c10cf0,0xe3805ca0}; memcpy(b.data()+entry+0x5e0c,prologue,sizeof(prologue)); }
    word(b,entry+0x2d88,0xeb000cf2);
    { const uint32_t prologue[] = {0xe92d4070,0xe1b05002,0xe1a04000,0x01a00004}; memcpy(b.data()+entry+0x6158,prologue,sizeof(prologue)); }
    word(b,entry+0x2d34,0xeb000ce8);
    { const uint32_t prologue[] = {0xe92d4070,0xe1b05002,0xe1a04000,0x01a00004}; memcpy(b.data()+entry+0x60dc,prologue,sizeof(prologue)); }
    word(b,entry+0x5de8,0xea0000f8);
    { const uint32_t prologue[] = {0xe92d4010,0xe1a04000,0xe5900044,0xe24dd068}; memcpy(b.data()+entry+0x61d0,prologue,sizeof(prologue)); }
    word(b,entry+0x5cf0,0xeb000173);
    { const uint32_t prologue[] = {0xe92d4010,0xe1a04000,0xe5900044,0xe24dd068}; memcpy(b.data()+entry+0x62c4,prologue,sizeof(prologue)); }
    word(b,entry+0x5d2c,0xeb000182);
    { const uint32_t prologue[] = {0xe92d4010,0xe1a04000,0xe5900044,0xe24dd068}; memcpy(b.data()+entry+0x633c,prologue,sizeof(prologue)); }
    word(b,entry+0x5ae0,0xeb000234);
    { const uint32_t prologue[] = {0xe3a01080,0xeaff5a7f,0xe3a010bc,0xeaff5a7d}; memcpy(b.data()+entry+0x63b8,prologue,sizeof(prologue)); }
    word(b,entry+0x5b1c,0xeb000227);
    { const uint32_t prologue[] = {0xe3a010bc,0xeaff5a7d,0xe92d4070,0xe24dd080}; memcpy(b.data()+entry+0x63c0,prologue,sizeof(prologue)); }
    word(b,entry+0x5e08,0xea00010e);
    { const uint32_t prologue[] = {0xe92d4010,0xe1a04000,0xe5900044,0xe24dd068}; memcpy(b.data()+entry+0x6248,prologue,sizeof(prologue)); }
    const uint32_t frame[] = {
    0xe59d0000,0xe24aa002,0xe580a044,0xe5804048,0xe5808000,0xe59d9018,
    0xe59d0000,0xe1a0100c,0xeb0000a6,0xe59d0000,0xe590a044,0xe5904048,
    0xe2805004,0xe2806024,0xe0da70b2,0xeaffe925
};
    memcpy(b.data()+entry+0x5b4c,frame,sizeof(frame));
    return b;
}
static void reject(const Image& original, Image& shadow, size_t size = 0xc70000, uint32_t base = 0) {
    PacePatch p; memset(&p,0xff,sizeof(p));
    const auto before = shadow;
    assert(!pacePatchInitTg50(&p,base,original.data(),shadow.data(),size));
    PacePatch zero{};
    assert(!memcmp(&p,&zero,sizeof(p)) && shadow == before);
}
int main(int argc, char** argv) {
    Image good = image();
    if (argc == 2) { std::ifstream f(argv[1],std::ios::binary); assert(f); good.assign(std::istreambuf_iterator<char>(f),{}); }
    const auto original = good;
    auto shadow = good;
    PacePatch p{};
    assert(pacePatchDiscoverTg50(&p,0,good.data(),good.size()));
    assert(p.enterPace==entry && p.calloutSyscall==0x105e70 && p.calloutTrap8==0x106424);
    assert(good==original);
    assert(pacePatchInitTg50(&p,0,good.data(),shadow.data(),good.size()));
    const auto patched = shadow;
    assert(pacePatchInitTg50(&p,0,good.data(),shadow.data(),good.size()) && shadow==patched);
    assert(good==original);
    assert(pacePatchDiscoverTg50(&p,0x10000000,good.data(),good.size()) && p.enterPace==0x10100064);
    shadow=good;
    reject(good,shadow,0); reject(good,shadow,end-1); reject(good,shadow,good.size()-1);
    reject(good,shadow,good.size(),0xffff0000);
    std::vector<size_t> corrupt;
    for(size_t i=0;i<104;i++) corrupt.push_back(entry+i);
    for(size_t i=0;i<64;i++) corrupt.push_back(entry+0x5b4c+i);
    for(size_t i=0;i<36;i++) corrupt.push_back(db+0x50+i);
    const size_t sites[] = {0x5b6c,0x2d88,0x2d34,0x5de8,0x5cf0,0x5d2c,0x5ae0,0x5b1c,0x5e08};
    const size_t targets[] = {0x5e0c,0x6158,0x60dc,0x61d0,0x62c4,0x633c,0x63b8,0x63c0,0x6248};
    for(size_t i=0;i<9;i++) {
        for(size_t j=0;j<4;j++) corrupt.push_back(entry+sites[i]+j);
        for(size_t j=0;j<16;j++) corrupt.push_back(entry+targets[i]+j);
    }
    for(auto off:corrupt) {
        good[off]^=1; shadow=good; reject(good,shadow); good[off]^=1;
    }
    for(size_t off: {db,db+0x20,db+0x3c,db+0x40,db+0x48,db+0x4c}) {
        good[off]^=1; shadow=good; reject(good,shadow); good[off]^=1;
    }
    // An otherwise valid second entry inside the resource is ambiguous.
    memcpy(good.data()+start,signature,sizeof(signature)); shadow=good; reject(good,shadow); good=original;
    // A lookalike outside the resource never replaces the required original entry.
    memcpy(good.data()+0x100,signature,sizeof(signature)); memset(good.data()+entry,0,104);
    shadow=good; reject(good,shadow); good=original;
    // All shadow writes are deferred, including when a previously patched shadow is damaged.
    shadow=patched; shadow[entry+4]^=1; reject(good,shadow);
    shadow=patched; shadow[entry+0x5b4c]^=1; reject(good,shadow);
    shadow=good; shadow[start]^=1; reject(good,shadow);
    memset(&p,0xff,sizeof(p));
    assert(!pacePatchInitTg50(&p,0,good.data(),good.data(),good.size()) && !p.enterPace && good==original);
    memset(&p,0xff,sizeof(p));
    assert(!pacePatchDiscoverTg50(&p,0,nullptr,good.size()) && !p.enterPace);
    assert(!pacePatchInitTg50(&p,0,good.data(),nullptr,good.size()) && !p.enterPace);
    std::cout << "TG50 descriptor: " << corrupt.size()+20 << " rejection/geometry/shadow checks passed\n";
}
