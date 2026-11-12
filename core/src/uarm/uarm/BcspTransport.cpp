#include "BcspTransport.h"
#include <algorithm>

namespace csr {
namespace {
struct Writer {
    BcspTransport::Bytes bytes;
    void Number(uint64_t n,unsigned count){for(unsigned i=0;i<count;++i){bytes.push_back(n);n>>=8;}}
    void Data(const uint8_t* p,size_t n){if(n)bytes.insert(bytes.end(),p,p+n);}
};
struct Reader {
    const BcspTransport::Bytes& bytes;size_t at{};bool good{true};
    uint64_t Number(unsigned count){
        if(count>bytes.size()-at){good=false;return 0;}
        uint64_t n=0;for(unsigned i=0;i<count;++i)n|=uint64_t(bytes[at++])<<(8*i);return n;
    }
    bool Flag(){auto n=Number(1);if(n>1)good=false;return n!=0;}
    void Data(uint8_t* p,size_t n){if(n>bytes.size()-at){good=false;return;}std::copy_n(bytes.data()+at,n,p);at+=n;}
};
}
BcspTransport::Bytes BcspTransport::Save(uint64_t now) const {
    Writer w;w.Number(1,4);w.Number(now,8);w.Number(unsigned(link),1);
    w.Number(inFrame,1);w.Number(escape,1);w.Number(discard,1);
    w.Number(ackPending,1);w.Number(wireReliable,1);w.Number(retrying,1);
    w.Number(rxNext,1);w.Number(txNext,1);w.Number(wireSeq,1);w.Number(retrySeq,1);
    w.Number(control,1);w.Number(linkAttempts,4);w.Number(now>=linkAt?now-linkAt:0,8);
    w.Number(rawSize,2);w.Data(raw.data(),rawSize);
    w.Number(wireSize,2);w.Number(wireAt,2);w.Data(wire.data(),wireSize);
    w.Number(flight.size(),1);
    for(const auto& q:flight){
        w.Number(q.channel,1);w.Number(q.seq,1);w.Number(q.crc,1);w.Number(q.sent,1);
        w.Number(q.attempts,4);w.Number(q.sent && now>=q.sentAt?now-q.sentAt:0,8);
        w.Number(q.payload.size(),2);w.Data(q.payload.data(),q.payload.size());
    }
    w.Number(inbox.size(),1);
    for(const auto& q:inbox){w.Number(q.channel,1);w.Number(q.payload.size(),2);w.Data(q.payload.data(),q.payload.size());}
    return w.bytes;
}
bool BcspTransport::Load(const Bytes& bytes,uint64_t& clock) {
    if(bytes.size()>8192)return false;
    Reader r{bytes};if(r.Number(4)!=1)return false;
    const auto epoch=r.Number(8);BcspTransport s(timing);
    const auto phase=r.Number(1);if(phase>unsigned(Link::failed))return false;s.link=Link(phase);
    s.inFrame=r.Flag();s.escape=r.Flag();s.discard=r.Flag();
    s.ackPending=r.Flag();s.wireReliable=r.Flag();s.retrying=r.Flag();
    s.rxNext=r.Number(1);s.txNext=r.Number(1);s.wireSeq=r.Number(1);s.retrySeq=r.Number(1);
    s.control=r.Number(1);s.linkAttempts=r.Number(4);const auto linkAge=r.Number(8);
    if(linkAge>epoch || s.rxNext>7 || s.txNext>7 || s.wireSeq>7 || s.retrySeq>7 ||
       s.control>15 || s.linkAttempts>=timing.maxAttempts)return false;
    s.linkAt=epoch-linkAge;s.rawSize=r.Number(2);if(s.rawSize>rawLimit)return false;
    r.Data(s.raw.data(),s.rawSize);s.wireSize=r.Number(2);s.wireAt=r.Number(2);
    if(s.wireSize>wireLimit || s.wireAt>s.wireSize)return false;
    r.Data(s.wire.data(),s.wireSize);
    const auto flights=r.Number(1);if(flights>window)return false;
    bool unsent=false;
    for(size_t i=0;i<flights;++i){
        Pending q;q.channel=r.Number(1);q.seq=r.Number(1);q.crc=r.Flag();q.sent=r.Flag();
        q.attempts=r.Number(4);const auto age=r.Number(8);const auto n=r.Number(2);
        if(n>maxPayload || age>epoch || q.seq>7 ||
           (q.channel!=2 && q.channel!=5 && q.channel!=6) ||
           (i && q.seq!=((s.flight.back().seq+1)&7)) ||
           q.attempts>timing.maxAttempts || (q.sent && (!q.attempts || unsent)) ||
           (!q.sent && (q.attempts || age)))return false;
        unsent|=!q.sent;q.sentAt=epoch-age;q.payload.resize(n);r.Data(q.payload.data(),n);s.flight.push_back(q);
    }
    if(!s.flight.empty() && s.txNext!=((s.flight.back().seq+1)&7))return false;
    const auto received=r.Number(1);if(received>inboxLimit)return false;
    for(size_t i=0;i<received;++i){
        Packet q;q.channel=r.Number(1);const auto n=r.Number(2);
        if(n>maxPayload || (q.channel!=2 && q.channel!=5 && q.channel!=6))return false;
        q.payload.resize(n);r.Data(q.payload.data(),n);s.inbox.push_back(q);
    }
    if(!r.good || r.at!=bytes.size() || (!s.inFrame && (s.escape || s.discard || s.rawSize)) ||
       (s.escape && s.discard) || (s.wireReliable && !s.HasWire()))return false;
    if(s.wireSize){
        if(s.wireSize<6 || s.wire[0]!=0xc0 || s.wire[s.wireSize-1]!=0xc0)return false;
        Bytes decoded;
        for(size_t i=1;i+1<s.wireSize;++i){
            auto c=s.wire[i];if(c==0xc0)return false;
            if(c==0xdb){if(++i+1>=s.wireSize)return false;c=s.wire[i];
                if(c==0xdc)c=0xc0;else if(c==0xdd)c=0xdb;else return false;}
            decoded.push_back(c);
        }
        if(decoded.size()<4)return false;
        const auto n=(decoded[1]>>4)|(size_t(decoded[2])<<4);
        const bool crc=decoded[0]&0x40,rel=decoded[0]&0x80;
        if(n>maxPayload || decoded.size()!=4+n+(crc?2:0) ||
           uint8_t(decoded[0]+decoded[1]+decoded[2]+decoded[3])!=0xff ||
           (crc && Crc(decoded.data(),4+n)!=uint16_t((decoded[4+n]<<8)|decoded[5+n])))return false;
        if(s.wireReliable){
            bool found=false;
            for(const auto& q:s.flight)if(q.seq==s.wireSeq && q.seq==(decoded[0]&7) &&
                q.channel==(decoded[1]&15) && q.crc==crc && rel && q.payload.size()==n &&
                std::equal(q.payload.begin(),q.payload.end(),decoded.begin()+4))found=true;
            if(!found)return false;
        }
    }
    if((s.link==Link::off || s.link==Link::failed) &&
       (s.rawSize || s.wireSize || !s.flight.empty() || !s.inbox.empty() || s.inFrame ||
        s.control || s.ackPending || s.retrying || s.rxNext || s.txNext))return false;
    if(s.link!=Link::active && (!s.flight.empty() || !s.inbox.empty() || s.retrying))return false;
    s.generation=generation;s.counts=counts;*this=std::move(s);clock=epoch;return true;
}
}
