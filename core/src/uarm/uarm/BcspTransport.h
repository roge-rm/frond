// Bounded CSR BCSP framing and reliability, driven by emulator time.
#ifndef FROND_BCSP_TRANSPORT_H
#define FROND_BCSP_TRANSPORT_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace csr {
class BcspTransport {
 public:
    static constexpr size_t maxPayload=416, window=6, inboxLimit=8;
    static constexpr size_t rawLimit=maxPayload+6, wireLimit=2*rawLimit+2;
    using Bytes=std::vector<uint8_t>;
    struct Packet {uint8_t channel{}; Bytes payload;};
    enum class Link:uint8_t {off,sync,configure,active,failed};
    struct Timing {
        // Integration supplies emulator-time units, never wall clock time.
        uint64_t linkRetry, dataRetry;
        unsigned maxAttempts;
    };
    struct Counts {uint64_t malformed{},badAck{},duplicates{},backpressure{},unknown{};};
    explicit BcspTransport(Timing timing):timing(timing){}
    Link State() const {return link;}
    uint64_t Generation() const {return generation;}
    Bytes Save(uint64_t now) const;
    bool Load(const Bytes& bytes, uint64_t& clock);
    const Counts& Statistics() const {return counts;}
    size_t Outstanding() const {return flight.size();}
    size_t Received() const {return inbox.size();}
    bool ReliablePending(uint8_t sequence) const {
        for(const auto& packet:flight)if(packet.seq==sequence)return true;
        return false;
    }
    uint8_t Expected() const {return rxNext;}
    bool HasWire() const {return wireAt<wireSize;}
    bool Failed() const {return link==Link::failed;}
    void Stop() {Clear();link=Link::off;}
    void Start(uint64_t now) {
        ++generation;Clear();link=Link::sync;linkAt=now;control|=syncBit;
        if(!timing.linkRetry || !timing.dataRetry || !timing.maxAttempts)Fail();
    }
    // Whole payload ownership transfers only on true. A full window is retryable.
    bool Queue(uint8_t channel,const uint8_t* p,size_t n,bool crc=false,uint8_t* sequence=nullptr) {
        if(link!=Link::active || n>maxPayload || (!p && n) ||
           (channel!=2 && channel!=5 && channel!=6) || flight.size()==window)return false;
        Pending q;q.channel=channel;q.seq=txNext;q.crc=crc;
        if(n)q.payload.assign(p,p+n);
        flight.push_back(q);if(sequence)*sequence=q.seq;txNext=(txNext+1)&7;return true;
    }
    const Packet* Peek() const {return inbox.empty()?nullptr:&inbox.front();}
    void Pop() {if(!inbox.empty())inbox.pop_front();}
    // The UART clocks bytes and owns RTS/CTS. Call only for accepted guest bytes.
    void Write(uint8_t c,uint64_t now) {
        if(link==Link::off || link==Link::failed)return;
        if(c==0xc0) {
            if(inFrame && rawSize && !discard && !escape)Frame(now);
            else if(inFrame && (escape || discard))++counts.malformed;
            inFrame=true;rawSize=0;escape=discard=false;return;
        }
        if(!inFrame || discard)return;
        if(escape) {
            escape=false;
            if(c==0xdc)c=0xc0;
            else if(c==0xdd)c=0xdb;
            else {discard=true;return;}
        } else if(c==0xdb) {escape=true;return;}
        if(rawSize==raw.size()){discard=true;return;}
        raw[rawSize++]=c;
    }
    void Tick(uint64_t now) {
        if(link==Link::off || link==Link::failed)return;
        if((link==Link::sync || link==Link::configure) && now-linkAt>=timing.linkRetry) {
            if(++linkAttempts>=timing.maxAttempts){Fail();return;}
            control|=link==Link::sync?syncBit:confBit;linkAt=now;
        }
        if(HasWire())return;
        wireSize=wireAt=0;
        // Keep link replies and ACKs independent of the reliable window.
        for(unsigned bit:{syncRespBit,confRespBit,syncBit,confBit})if(control&bit) {
            control&=~bit;
            const auto& data=bit==syncRespBit?syncResponse:bit==confRespBit?confResponse:
                             bit==syncBit?sync:conf;
            Encode(1,data.data(),4,false,0,false);return;
        }
        if(link!=Link::active)return;
        if(ackPending){Encode(0,nullptr,0,false,0,false);ackPending=false;return;}
        // Go-back-N recovery is represented as a cursor across the outstanding
        // window. ACKs can cull its prefix while the frame on wire stays owned.
        if(!retrying && !flight.empty() && flight.front().sent &&
           now-flight.front().sentAt>=timing.dataRetry) {
            retrying=true;retrySeq=flight.front().seq;
        }
        if(retrying) {
            for(auto& q:flight)if(q.seq==retrySeq && q.sent) {
                if(q.attempts>=timing.maxAttempts){Fail();return;}
                EncodePending(q);retrySeq=(retrySeq+1)&7;
                if(retrySeq==txNext)retrying=false;
                return;
            }
            retrying=false;
        }
        for(auto& q:flight)if(!q.sent){EncodePending(q);return;}
    }
    uint16_t Read(uint64_t now,bool receiveReady=true) {
        Tick(now);
        if(!receiveReady || !HasWire())return 0x100;
        const auto c=wire[wireAt++];
        if(wireAt==wireSize && wireReliable) {
            for(auto& q:flight)if(q.seq==wireSeq) {
                q.sent=true;q.sentAt=now;++q.attempts;break;
            }
            wireReliable=false;
        }
        return c;
    }
    // Exposed for independent frame vectors and corruption tests.
    static Bytes FrameBytes(uint8_t channel,const Bytes& payload,bool reliable=false,
                            uint8_t seq=0,uint8_t ack=0,bool crc=false) {
        if(payload.size()>maxPayload || channel>15 || seq>7 || ack>7)return {};
        Bytes raw(4);raw[0]=(reliable?0x80:0)|(crc?0x40:0)|(ack<<3)|seq;
        raw[1]=uint8_t((payload.size()<<4)|channel);raw[2]=uint8_t(payload.size()>>4);
        raw[3]=uint8_t(~(raw[0]+raw[1]+raw[2]));raw.insert(raw.end(),payload.begin(),payload.end());
        if(crc){const auto c=Crc(raw.data(),raw.size());raw.push_back(c>>8);raw.push_back(c);}
        Bytes bytes{0xc0};
        for(auto c:raw) {
            if(c==0xc0){bytes.push_back(0xdb);bytes.push_back(0xdc);}
            else if(c==0xdb){bytes.push_back(0xdb);bytes.push_back(0xdd);}
            else bytes.push_back(c);
        }
        bytes.push_back(0xc0);return bytes;
    }
    static uint16_t Crc(const uint8_t* p,size_t n) {
        uint16_t crc=0xffff;
        for(size_t i=0;i<n;++i) {
            crc^=p[i];
            for(unsigned b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0x8408:0);
        }
        uint16_t reversed=0;
        for(unsigned b=0;b<16;++b){reversed=uint16_t((reversed<<1)|(crc&1));crc>>=1;}
        return reversed;
    }
    inline static constexpr std::array<uint8_t,4> sync{0xda,0xdc,0xed,0xed};
    inline static constexpr std::array<uint8_t,4> syncResponse{0xac,0xaf,0xef,0xee};
    inline static constexpr std::array<uint8_t,4> conf{0xad,0xef,0xac,0xed};
    inline static constexpr std::array<uint8_t,4> confResponse{0xde,0xad,0xd0,0xd0};
 private:
    enum:unsigned {syncBit=1,syncRespBit=2,confBit=4,confRespBit=8};
    struct Pending {
        uint8_t channel{},seq{};bool crc{},sent{};
        unsigned attempts{};uint64_t sentAt{};Bytes payload;
    };
    uint64_t generation{};
    Timing timing;Link link{Link::off};Counts counts{};
    std::array<uint8_t,rawLimit> raw{};
    std::array<uint8_t,wireLimit> wire{};
    size_t rawSize{},wireAt{},wireSize{};
    bool inFrame{},escape{},discard{},ackPending{},wireReliable{},retrying{};
    uint8_t rxNext{},txNext{},wireSeq{},retrySeq{};
    uint64_t linkAt{};unsigned linkAttempts{},control{};
    std::deque<Pending> flight;
    std::deque<Packet> inbox;
    void Clear() {
        rawSize=wireAt=wireSize=0;inFrame=escape=discard=ackPending=wireReliable=retrying=false;
        rxNext=txNext=wireSeq=retrySeq=0;linkAt=0;linkAttempts=control=0;
        flight.clear();inbox.clear();
    }
    void Fail(){Clear();link=Link::failed;}
    void Encode(uint8_t channel,const uint8_t* p,size_t n,bool rel,uint8_t seq,bool crc) {
        Bytes payload;if(n)payload.assign(p,p+n);
        const auto bytes=FrameBytes(channel,payload,rel,seq,rxNext,crc);
        wireSize=bytes.size();wireAt=0;
        for(size_t i=0;i<bytes.size();++i)wire[i]=bytes[i];
        wireReliable=rel;wireSeq=seq;
        // An unreliable link frame also carries the current cumulative ACK.
        ackPending=false;
    }
    void EncodePending(Pending& q){Encode(q.channel,q.payload.data(),q.payload.size(),true,q.seq,q.crc);}
    void Ack(uint8_t ack) {
        if(flight.empty()){if(ack!=txNext)++counts.badAck;return;}
        const unsigned count=(ack-flight.front().seq)&7;
        if(count>flight.size()){++counts.badAck;return;}
        for(unsigned i=0;i<count;++i)if(!flight[i].sent){++counts.badAck;return;}
        for(unsigned i=0;i<count;++i) {
            // An ACK can retire a retransmission while its UART suffix drains.
            if(wireReliable && flight.front().seq==wireSeq)wireReliable=false;
            flight.pop_front();
        }
    }
    void LinkPacket(const uint8_t* p,uint64_t now) {
        const auto same=[&](const auto& a){for(unsigned i=0;i<4;++i)if(a[i]!=p[i])return false;return true;};
        if(same(sync)) {
            if(link==Link::active){Start(now);}
            control|=syncRespBit;
            if(link==Link::sync)control|=syncBit;
        } else if(same(syncResponse)) {
            if(link==Link::sync){link=Link::configure;linkAttempts=0;linkAt=now;control&=~syncBit;control|=confBit;}
        } else if(same(conf)) {
            if(link==Link::configure || link==Link::active)control|=confRespBit;
            if(link==Link::configure)control|=confBit;
        } else if(same(confResponse)) {
            if(link==Link::configure){link=Link::active;linkAttempts=0;control&=~(syncBit|confBit);}
        } else ++counts.unknown;
    }
    void Frame(uint64_t now) {
        if(rawSize<4 || uint8_t(raw[0]+raw[1]+raw[2]+raw[3])!=0xff){++counts.malformed;return;}
        const size_t n=(raw[1]>>4)|(size_t(raw[2])<<4);
        const bool crc=raw[0]&0x40,rel=raw[0]&0x80;
        const uint8_t channel=raw[1]&15,seq=raw[0]&7,ack=(raw[0]>>3)&7;
        if(n>maxPayload || rawSize!=4+n+(crc?2:0) ||
           (crc && Crc(raw.data(),4+n)!=uint16_t((raw[4+n]<<8)|raw[5+n]))) {++counts.malformed;return;}
        if(channel==1 && !rel && n==4){LinkPacket(raw.data()+4,now);return;}
        if(link!=Link::active)return;
        // Invalid frame classes never acknowledge or mutate a live data window.
        if((channel==0 && (rel || n || seq)) ||
           (channel!=0 && (!rel || (channel!=2 && channel!=5 && channel!=6)))) {++counts.unknown;return;}
        Ack(ack);
        if(channel==0)return;
        if(seq!=rxNext){++counts.duplicates;ackPending=true;return;}
        // Leave expected sequence unchanged when the consumer has no room.
        if(inbox.size()==inboxLimit){++counts.backpressure;ackPending=true;return;}
        Packet p;p.channel=channel;p.payload.assign(raw.begin()+4,raw.begin()+4+n);
        inbox.push_back(p);rxNext=(rxNext+1)&7;ackPending=true;
    }
};
}
#endif
