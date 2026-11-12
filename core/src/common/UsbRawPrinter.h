#pragma once
#include "RawPrintJob.h"
#include <algorithm>
#include <chrono>
#include <string>

// Synthetic full-speed protocol 1 leaf. Owner and packet callback share one thread.
// No job serialisation; the owner must call cancel at reset/detach/load/shutdown. bus() and setBus()
// carry the bus state across a session load.
class UsbRawPrinter {
 public:
    explicit UsbRawPrinter(size_t limit) : job(limit) {}
    bool begin() { return job.begin(); }
    // While on, a job is always collecting: it restarts after a cancel, clear or finish.
    void keepCollecting(bool on) { keep=on; if(on) restart(); }
    bool keeping() const { return keep; }
    // Drops the collected bytes and starts a new job. The leaf stays configured.
    void clear() { restart(); }
    // Milliseconds since the last byte of this job, or -1 when it has none.
    long long idleMs() const {
        if(!haveByte) return -1;
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-lastByte).count();
    }
    RawPrintJob::State state() const { return job.state(); }
    size_t size() const { return job.size(); }
    RawPrintJob::State finish(RawPrintJob::Output out, void* context) {
        auto result=job.finish(out, context);
        haveByte=false;
        if(keep) job.begin();
        return result;
    }
    void cancel() {
        restart(); address=configuration=0; halted=false; bulkToggle=false;
        phase=Phase::idle; pending=Action::none; data.clear(); cached.clear(); haveCache=false;
    }
    // The bus side a session keeps across a load: address, configuration, halt and bulk toggle.
    // Restoring it drops the job and any control transfer in flight.
    struct Bus { unsigned address=0, configuration=0; bool halted=false, bulkToggle=false; };
    Bus bus() const { return {address,configuration,halted,bulkToggle}; }
    bool setBus(const Bus& b) {
        if(b.address>127 || b.configuration>1 || (b.configuration && !b.address) || (!b.configuration && (b.halted || b.bulkToggle))) return false;
        cancel(); address=b.address; configuration=b.configuration; halted=b.halted; bulkToggle=b.bulkToggle;
        return true;
    }
    // Reply and Packet use the 0321 HostReply/HostPacket shape, without a core dependency.
    template<class Reply, class Packet> Reply packet(const Packet& p) noexcept {
        try { return packetImpl<Reply>(p); }
        catch(...) {
            restart(); phase=Phase::stall;
            return Reply{decltype(Reply{}.result)::stall,p.toggle,{}};
        }
    }
 private:
    template<class Reply, class Packet> Reply packetImpl(const Packet& p) {
        using Result=decltype(Reply{}.result);
        auto reply=[&](Result r) { return Reply{r,p.toggle,{}}; };
        if(p.lowSpeed || p.address!=address) return reply(Result::noAnswer);
        if(p.endpoint==1 && p.direction==1 && configuration) {
            if(halted) return reply(Result::stall);
            if(p.bytes.size()!=p.requested || p.requested>64) {
                restart(); halted=true; return reply(Result::stall);
            }
            if(p.toggle!=bulkToggle) return reply(Result::ack);
            if(job.state()!=RawPrintJob::State::collecting) return reply(Result::nak);
            if(!job.append(p.bytes.data(),p.bytes.size())) { halted=true; return reply(Result::stall); }
            if(!p.bytes.empty()) { lastByte=std::chrono::steady_clock::now(); haveByte=true; }
            bulkToggle=!bulkToggle; return reply(Result::ack);
        }
        if(p.endpoint) return reply(Result::stall);
        if(p.direction==0) {
            if(p.toggle || p.requested!=8 || p.bytes.size()!=8) return reply(Result::noAnswer);
            phase=Phase::stall; pending=Action::none; data.clear(); cached.clear(); haveCache=false;
            offset=0; controlToggle=true;
            try { setup(p.bytes); } catch(...) { phase=Phase::stall; }
            return reply(Result::ack);
        }
        if(phase==Phase::stall) return reply(Result::stall);
        if(p.direction==2 && haveCache && p.toggle==cacheToggle) {
            if(cached.size()>p.requested) return reply(Result::stall);
            return Reply{Result::ack,p.toggle,cached};
        }
        if(phase==Phase::read && p.direction==2 && p.toggle==controlToggle) {
            size_t n=std::min(size_t(8),data.size()-offset);
            if(p.requested<n) return reply(Result::stall);
            try { cached.assign(data.begin()+offset,data.begin()+offset+n); }
            catch(...) { phase=Phase::stall; return reply(Result::stall); }
            cacheToggle=p.toggle; haveCache=true; offset+=n; controlToggle=!controlToggle;
            if(n<8 || (offset==data.size() && !needZlp)) phase=Phase::statusOut;
            return Reply{Result::ack,p.toggle,cached};
        }
        if((phase==Phase::statusOut || phase==Phase::doneOut) && p.direction==1 &&
           p.toggle && !p.requested && p.bytes.empty()) {
            phase=Phase::doneOut; return reply(Result::ack);
        }
        if(phase==Phase::statusIn && p.direction==2 && p.toggle && p.requested==0) {
            apply(); phase=Phase::doneIn; cached.clear(); cacheToggle=true; haveCache=true;
            return reply(Result::ack);
        }
        phase=Phase::stall; return reply(Result::stall);
    }
 private:
    enum class Phase { idle, read, statusIn, statusOut, doneIn, doneOut, stall };
    enum class Action { none, address, configure, interface, halt, clear, reset };
    RawPrintJob job;
    bool keep=false, haveByte=false;
    std::chrono::steady_clock::time_point lastByte;
    void restart() { job.abort(); haveByte=false; if(keep) job.begin(); }
    unsigned address=0, configuration=0, value=0;
    bool halted=false, bulkToggle=false, controlToggle=true, needZlp=false;
    bool haveCache=false, cacheToggle=false;
    Phase phase=Phase::idle;
    Action pending=Action::none;
    size_t offset=0;
    std::vector<uint8_t> data,cached;
    void apply() {
        switch(pending) {
            case Action::address: address=value; break;
            case Action::configure: restart(); configuration=value; halted=false; bulkToggle=false; break;
            case Action::interface: restart(); halted=false; bulkToggle=false; break;
            case Action::halt: halted=true; break;
            case Action::clear: halted=false; bulkToggle=false; break;
            case Action::reset: restart(); halted=false; bulkToggle=false; break;
            default: break;
        }
        pending=Action::none;
    }
    void setup(const std::vector<uint8_t>& b) {
        unsigned type=b[0], r=b[1], v=b[2]|(b[3]<<8), i=b[4]|(b[5]<<8), len=b[6]|(b[7]<<8);
        value=v;
        bool ok=false;
        if(type==0x80 && r==6 && i==0) {
            if(v==0x100) { data={18,1,0,2,0,0,0,8,0,0,0,0,0,1,0,0,0,1}; ok=true; }
            if(v==0x200) { data={9,2,25,0,1,1,0,0x80,1,9,4,0,0,1,7,1,1,0,7,5,1,2,64,0,0}; ok=true; }
        } else if(type==0 && r==5 && v<128 && !i && !len && !configuration) {
            pending=Action::address; ok=true;
        } else if(type==0 && r==9 && v<=1 && !i && !len && address) {
            pending=Action::configure; ok=true;
        } else if(type==0x80 && r==8 && !v && !i && len==1 && address) {
            data={uint8_t(configuration)}; ok=true;
        } else if(type==0x81 && r==10 && !v && !i && len==1 && configuration) {
            data={0}; ok=true;
        } else if(type==1 && r==11 && !v && !i && !len && configuration) {
            pending=Action::interface; ok=true;
        } else if(r==0 && !v && len==2 &&
                  ((type==0x80 && !i) || (type==0x81 && !i && configuration) ||
                   (type==0x82 && (i==0 || i==0x80 || (i==1 && configuration))))) {
            data={uint8_t(type==0x82 && i==1 && halted),0}; ok=true;
        } else if(type==2 && (r==1 || r==3) && !v && i==1 && !len && configuration) {
            pending=r==1?Action::clear:Action::halt; ok=true;
        } else if(type==0xa1 && r==0 && !v && !i && configuration) {
            const std::string id="MFG:Frond;MDL:Synthetic raw byte sink;";
            unsigned n=id.size()+2; data={uint8_t(n>>8),uint8_t(n)};
            data.insert(data.end(),id.begin(),id.end()); ok=true;
        } else if(type==0xa1 && r==1 && !v && !i && len==1 && configuration) {
            data={uint8_t(job.state()==RawPrintJob::State::collecting?0x18:0x10)}; ok=true;
        } else if((type==0x21 || type==0x23) && r==2 && !v && !i && !len && configuration) {
            pending=Action::reset; ok=true;
        }
        if(!ok) return;
        if(type&0x80) {
            if(data.size()>len) data.resize(len);
            needZlp=data.size()<len && data.size()%8==0;
            phase=len?Phase::read:Phase::statusOut;
        } else phase=Phase::statusIn;
    }
};
