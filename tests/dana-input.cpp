#include "dana_input5.h"
#include <cassert>
#include <iostream>
int main() {
    DanaInput5 input;
    auto a=*DanaScan5::Named("a");
    unsigned calls=0;
    auto accept=[&](const auto& events) { calls+=events.size(); return true; };
    auto refuse=[](const auto&) { return false; };
    assert(!DanaScan5::Named("256"));
    assert(DanaScan5::Named("enter")->Code()==0x6e);
    assert(!DanaScan5::Named("iso"));
    assert(danaScanValid5(0x29));
    assert(!DanaScan5::Named(std::string("a\0x",3)));
    assert(!input.Key(DanaSource5(100),a,true,accept));
    assert(!input.Key(DanaSource5::physical,a,true,refuse));
    assert(input.Key(DanaSource5::physical,a,true,accept)); assert(calls==1);
    assert(input.Key(DanaSource5::console,a,true,accept)); assert(calls==1);
    assert(input.Key(DanaSource5::physical,a,false,accept)); assert(calls==1);
    assert(!input.Key(DanaSource5::console,a,false,refuse));
    assert(!input.Text("a",accept));
    assert(input.Key(DanaSource5::console,a,false,accept)); assert(calls==2);
    assert(!input.Text("a\xc3\xa9",accept)); assert(calls==2);
    assert(!input.Text(std::string(13,'a'),accept)); assert(calls==2);
    assert(input.Key(DanaSource5::physical,a,true,accept));
    assert(input.Key(DanaSource5::replay,a,true,accept));
    assert(!input.ReleaseSource(DanaSource5::replay,refuse));
    unsigned owned=calls;
    assert(input.ReleaseSource(DanaSource5::replay,accept));assert(calls==owned);
    assert(!input.Text("a",accept));
    assert(input.ReleaseSource(DanaSource5::physical,accept));assert(calls==owned+1);
    assert(input.Text("aA!?",[&](const auto& events) {
        assert(events.size()==14);
        assert(events[0].scan.Code()==a.Code() && events[0].down);
        assert(events[2].scan.Code()==danaScanNamed5("lshift"));
        return true;
    }));
    for(const auto& key:danaKeys5) {
        assert(DanaScan5::Named(key.name)->Code()==key.scan);
    }
    std::cout << "Dana typed ownership, atomic refusal and text synthesis passed\n";
}
