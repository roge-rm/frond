#pragma once
#include "dana_keyboard5.h"
#include <array>
#include <optional>
#include <vector>
#include <string>
#include <string_view>

// A separate tag from KeyId and its 64-bit mask. Only inventory names create scans.
class DanaScan5 {
    uint8_t value;
    explicit DanaScan5(uint8_t value) : value(value) {}
  public:
    uint8_t Code() const { return value; }
    static std::optional<DanaScan5> Named(std::string_view name) {
        for(const auto& key:danaKeys5) if(name==key.name) return DanaScan5(key.scan);
        return std::nullopt;
    }
};
struct DanaTransition5 { DanaScan5 scan; bool down; };
enum class DanaSource5 { physical, physicalRight, physicalPage, physicalChord, console, replay, screen, count };

// Source ownership commits only after the complete ST batch is accepted.
class DanaInput5 {
    std::array<std::array<bool,128>,size_t(DanaSource5::count)> held{};
  public:
    void Clear() { held={}; }
    bool Held(DanaScan5 scan) const {
        for(const auto& row:held)if(row[scan.Code()])return true;
        return false;
    }
    template<class Send> bool Keys(DanaSource5 source,const std::vector<DanaTransition5>& changes,Send send) {
        if(size_t(source)>=held.size())return false;
        auto next=held;
        for(const auto& change:changes)next[size_t(source)][change.scan.Code()]=change.down;
        std::vector<DanaTransition5> batch;
        for(bool down:{false,true})for(const auto& key:danaKeys5) {
            bool before=false,after=false;
            for(const auto& row:held)before|=row[key.scan];
            for(const auto& row:next)after|=row[key.scan];
            if(before!=after && after==down)batch.push_back({*DanaScan5::Named(key.name),down});
        }
        if(!batch.empty()&&!send(batch))return false;
        held=next;return true;
    }
    template<class Send> bool Key(DanaSource5 source,DanaScan5 scan,bool down,Send send) {
        return Keys(source,{{scan,down}},send);
    }
    template<class Send> bool ReleaseSource(DanaSource5 source,Send send) {
        if(size_t(source)>=held.size()) return false;
        auto next=held;
        next[size_t(source)]={};
        std::vector<DanaTransition5> batch;
        for(const auto& key:danaKeys5) if(held[size_t(source)][key.scan]) {
            bool other=false;
            for(const auto& row:next) other|=row[key.scan];
            if(!other) batch.push_back({*DanaScan5::Named(key.name),false});
        }
        if(!send(batch)) return false;
        held=next; return true;
    }
    static std::optional<std::vector<DanaTransition5>> TextTransitions(const std::string& text) {
        std::vector<DanaTransition5> batch;
        const char* plain="`1234567890-=[]\\;',./";
        const char* upper="~!@#$%^&*()_+{}|:\"<>?";
        for(unsigned char ch:text) {
            bool shift=ch>='A' && ch<='Z';
            char c=shift?char(ch+32):char(ch);
            if(auto pos=std::strchr(upper,ch); pos && ch) { c=plain[pos-upper]; shift=true; }
            std::string name(1,c);
            if(ch==' ') name="space";
            if(ch=='\n' || ch=='\r') name="enter";
            if(ch=='\t') name="tab";
            auto scan=DanaScan5::Named(name);
            if(!scan || batch.size()+(shift?4:2)>24) return std::nullopt;
            auto modifier=*DanaScan5::Named("lshift");
            if(shift) batch.push_back({modifier,true});
            batch.push_back({*scan,true}); batch.push_back({*scan,false});
            if(shift) batch.push_back({modifier,false});
        }
        return batch;
    }
    template<class Send> bool Text(const std::string& text,Send send) {
        for(auto& row:held) for(bool down:row) if(down) return false;
        auto batch=TextTransitions(text);
        return batch && send(*batch);
    }
};
