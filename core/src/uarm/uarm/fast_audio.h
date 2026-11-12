#ifndef FROND_FAST_AUDIO_H
#define FROND_FAST_AUDIO_H

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <utility>
#include "audio_queue.h"
#include "early_audio_ledger.h"
#include <cstring>
#include <stdexcept>

// Optional preview of one qualified PCM stream before its firmware queue.
class FastAudio {
public:
    using Reader = std::function<bool(uint32_t, void*, unsigned)>;
    enum class Driver { tx, e2, zire72 };
    struct ArmStartSignature {uint32_t entry,call,branch;};
    inline static constexpr ArmStartSignature Zire72Start{0x20049d5c,0x20049ddc,0xeb00543b};
    inline static constexpr ArmStartSignature E3Start{0x207a2e60,0x207a2ee0,0xeb00542b};
    explicit FastAudio(Reader read, Driver driver = Driver::tx)
        : FastAudio(std::move(read),driver,driver==Driver::zire72?Zire72Start:ArmStartSignature{}) {}
    FastAudio(Reader read, Driver driver, ArmStartSignature start)
        : read(std::move(read)),driver(driver),armStart(start) {}
    void SetQueue(AudioQueue* value) { queue = value; }
    // Host audio period in frames: the stream starts after one period of silence and falls
    // back past twelve queued.
    void SetPeriod(unsigned frames) { period = frames; }
    void RestartOutput() { ledger.CancelAll(); ledger.DeliveryDiscontinuity(); started=false; if (queue) audioQueueClear(queue); }
    void ValidateEarly(uint64_t now) {if(earlyPlayback && ledger.Failed() && !failed)FallBack(now,"submission contradiction",true);}
    void SetEarlyPlayback(bool enabled) {earlyPlayback=enabled;}
    bool Bypass() const { return active || quietUntil || ledger.HasPreviewed() || (sleeping && wasPreviewing); }
    bool ControlSuspended() const {return stopped || !controls.empty() || sleeping;}
    void Sleep() {
        if(sleeping)return;
        sleeping=true;wasPreviewing=active || quietUntil;ledger.CancelAll();
        active=false;started=false;quietUntil=0;if(queue)audioQueueClear(queue);
    }
    void Wake(uint64_t now) {
        if(!sleeping)return;
        sleeping=false;if(wasPreviewing)quietUntil=now+250000000;
        wasPreviewing=false;started=false;if(queue)audioQueueClear(queue);
    }
    bool Active() const { return active; }
    bool Failed() const { return failed; }
    void NotifyUnderrun(uint64_t now) {
        if (active) FallBack(now, "preview underrun", true);
    }
    void Reset() {
        ledger.NewTimeline();
        if(OutputFenced())ledger.Fence();
        active = failed = started = false;
        controls.clear();knownChannel=UINT32_MAX;stopped=sleeping=wasPreviewing=false;
        quietUntil = 0;
        if (queue) audioQueueClear(queue);
    }
    void Tick(uint64_t now) { if (quietUntil && now >= quietUntil && !ledger.HasPreviewed()) quietUntil = 0; }
    void Handle(uint32_t trap, const uint32_t* r, uint64_t now, bool outputEnabled, bool suppress=false) {
        if(trap==0x4314) {
            if(active || ledger.HasPreviewed())FallBack(now,"another stream created",true);
            return;
        }
        if (trap == 0x4318) {
            if(knownChannel!=UINT32_MAX && knownChannel!=r[0])return;
            knownChannel=UINT32_MAX;stopped=false;
            if (active) FallBack(now, "stream closed", false);
            return;
        }
        if (failed) return;
        if (trap == 0x4324) {
            if (r[0] >= 16) FallBack(now, "device-level controls", true);
            return;
        }
        if (trap != 0x4320 || !outputEnabled || !queue || OutputFenced() || ControlSuspended()) return;
        uint32_t object;
        uint16_t left, right;
        if (!Profile(r, object, left, right) || !r[2] || r[2] > 4096 || (r[2]&3) || (r[1]&3) || r[1] > UINT32_MAX-r[2]) {
            FallBack(now, "unsupported driver, format or multiple streams", true);
            return;
        }
        if(suppress)return;
        std::vector<uint8_t> pcm(r[2]);
        if (!read(r[1],pcm.data(),r[2])) {FallBack(now,"unreadable PCM",true);return;}
        Enqueue(pcm,left,right,now);
    }
    bool Enqueue(const std::vector<uint8_t>& pcm,uint16_t left,uint16_t right,uint64_t now) {
        if(failed || OutputFenced() || ControlSuspended() || !queue || pcm.empty() || pcm.size()>4096 || (pcm.size()&3))return false;
        const unsigned frames=pcm.size()/4;
        std::array<uint32_t,1024> block{};
        std::memcpy(block.data(),pcm.data(),pcm.size());
        bool audible=false;
        for(unsigned i=0;i<frames;++i) {
            const auto v=block[i];
            block[i]=uint16_t(Scale(int16_t(v),left)) | uint32_t(uint16_t(Scale(int16_t(v>>16),right)))<<16;
            audible=audible || block[i];
        }
        if(!active) {
            active=true;quietUntil=0;audioQueueClear(queue);
            std::fputs("low-latency audio: single stereo 44100 Hz stream active\n",stderr);
        }
        if(!started && !audible)return true;
        if(!started)for(unsigned i=0;i<period;++i)audioQueuePush(queue,0);
        started=true;
        if(audioQueuePendingSamples(queue)+frames>12*size_t(period)) {FallBack(now,"preview queue exceeded latency limit",true);return false;}
        for(unsigned i=0;i<frames;++i)audioQueuePush(queue,block[i]);
        return true;
    }
    void TryEarly(uint64_t now,bool outputEnabled=true) {
        if(!earlyPlayback || !outputEnabled || failed || ledger.Failed() || !ledger.Bound() ||
           OutputFenced() || ControlSuspended() || ledger.HasCanceled())return;
        ledger.Resume();
        // Batch in original submission order after an exact HAL ownership witness.
        const auto records=ledger.Records();
        for(const auto& r:records)if(!r.data.eligible || r.canceled || r.data.key.channel!=knownChannel)return;
        for(const auto& r:records) {
            if(r.previewed)continue;
            if(!Enqueue(r.data.pcm,r.data.left,r.data.right,now))return;
            if(!ledger.MarkPreviewed(r.sequence)) {FallBack(now,"lost submission ownership",true);return;}
            ++previews;
        }
    }
    struct Control {uint32_t channel,command,sp,lr;bool qualifiedStart;};
    bool QualifiedStart(const uint32_t* r) {
        if(armStart.entry) {
            // ARM SndStreamStart: ldr r0,[r4,#40]; add r2,sp,#12; mov r1,#5; bl HAL; cmp r0,#0.
            const uint32_t entry=armStart.entry,call=armStart.call;
            return r[14]==call+4 && Is(entry,0xe92d43f0) && Is(entry+4,0xe5999000) &&
                Is(call-12,0xe5940028) && Is(call-8,0xe28d200c) && Is(call-4,0xe3a01005) &&
                Is(call,armStart.branch) && Is(call+4,0xe3500000);
        }
        const bool e2=driver==Driver::e2;
        const uint32_t entry=e2?0x20065de4:0x2004ae3c,call=e2?0x20065e30:0x2004ae88;
        uint16_t next;
        return r[14]==call+5 && Is(entry,0xb089b5f0) && Is(entry+4,0x1c04464d) &&
            Is(call-4,0x21056aa0) && Is(call,e2?0xfa0ef020:0xfda0f021) && Half(call+4,next) && next==0x2800;
    }
    void ControlHead(const uint32_t* r,uint64_t now,bool canTrack) {
        if(failed)return;
        if(r[1]==2 || r[1]==4)return;
        if((knownChannel!=UINT32_MAX && knownChannel!=r[0]) ||
           r[0]>=16 || r[1]>6 || !canTrack || controls.size()==4) {
            FallBack(now,"untracked audio control",true);return;
        }
        if(r[1]==0)knownChannel=UINT32_MAX;
        ledger.Cancel(r[0]);started=false;if(queue)audioQueueClear(queue);
        controls.push_back({r[0],r[1],r[13],r[14],r[1]==5 && QualifiedStart(r)});
        if(r[1]==6)stopped=true;
    }
    void ControlTail(const uint32_t* before,const uint32_t* result,uint64_t now) {
        for(auto it=controls.begin();it!=controls.end();++it) {
            const auto c=*it;
            if(c.channel!=before[0] || c.command!=before[1] || c.sp!=before[13] || c.lr!=before[14])continue;
            controls.erase(it);
            if(c.command==5 && result[0]!=0) {FallBack(now,"failed stream start",true);return;}
            if(c.command==5 && result[0]==0) {
                if(ledger.Pending()) {FallBack(now,"start before previous writer retired",true);return;}
                ledger.NewTimeline();if(OutputFenced())ledger.Fence();
                knownChannel=c.qualifiedStart?c.channel:UINT32_MAX;stopped=false;
                ++starts;

            }
            // Stop remains suspended: the caller marks canceled messages after HAL returns.
            return;
        }
        // Fresh portable restores have no matching host invocation; never qualify them.
    }
    static int16_t Scale(int16_t sample, uint16_t gain) {
        const int32_t v = int32_t(sample)*gain;
        // ARM ASR rounds negative values down; the verified mixer uses Q11.
        return int16_t(v >= 0 ? v/2048 : -((-v+2047)/2048));
    }
    struct HostState {
        EarlyAudioLedger ledger;
        bool active,failed;
        uint64_t quietUntil;
        std::vector<Control> controls;
        uint32_t knownChannel;
        bool stopped,sleeping,wasPreviewing;
    };
    HostState CaptureHostState() const {return {ledger,active,failed,quietUntil,controls,knownChannel,stopped,sleeping,wasPreviewing};}
    bool OutputFenced() const {return !audioCheckpoints.empty() || replacementDepth;}
    uint64_t BeginAudioCheckpoint() {
        if(audioCheckpoints.size()>=8)throw std::runtime_error("audio checkpoint nesting exceeded");
        const uint64_t token=++checkpointSequence;
        audioCheckpoints.push_back({token,CaptureHostState()});
        ledger.Fence();started=false;if(queue)audioQueueClear(queue);
        return token;
    }
    void RestoreAudioCheckpoint(uint64_t token) {
        for(const auto& frame:audioCheckpoints)if(frame.token==token) {
            ledger=frame.state.ledger;active=frame.state.active;failed=frame.state.failed;
            quietUntil=frame.state.quietUntil;controls=frame.state.controls;knownChannel=frame.state.knownChannel;
            stopped=frame.state.stopped;sleeping=frame.state.sleeping;wasPreviewing=frame.state.wasPreviewing;
            started=false;ledger.Fence();
            if(queue)audioQueueClear(queue);
            return;
        }
        throw std::runtime_error("unknown audio checkpoint");
    }
    void EndAudioCheckpoint(uint64_t token) {
        for(auto it=audioCheckpoints.begin();it!=audioCheckpoints.end();++it)if(it->token==token) {
            audioCheckpoints.erase(it);ReprimeAfterFence();return;
        }
        throw std::runtime_error("unbalanced audio checkpoint");
    }
    void BeforeGuestReplacement() {
        if(replacementDepth>=8)throw std::runtime_error("audio replacement nesting exceeded");
        ++replacementDepth;ledger.Fence();started=false;if(queue)audioQueueClear(queue);
    }
    void GuestReplacementFinished() {
        if(!replacementDepth)throw std::runtime_error("unbalanced audio replacement");
        --replacementDepth;ReprimeAfterFence();
    }
    void GuestLoaded(bool checkpoint) {
        if(!checkpoint)Reset();
        else {started=false;if(queue)audioQueueClear(queue);if(OutputFenced())ledger.Fence();}
    }
    void ReprimeAfterFence() {
        started=false;if(queue)audioQueueClear(queue);
        if(OutputFenced())ledger.Fence();else ledger.Unfence();
    }
    struct EarlyDiagnostics { uint64_t ready,consumed,disposed,rejected,contradictions;size_t pending;uint64_t previewed,skipped; };
    EarlyDiagnostics EarlyStatus() const {return {earlyReady,earlyConsumed,earlyDisposed,earlyRejected,earlyContradictions,ledger.Pending(),previews,skips};}
    ~FastAudio() = default;
    void EarlyTransition(const uint32_t* regs,uint32_t pc,uint32_t from,uint64_t now=0,bool outputEnabled=true) {
        // ARM Sound Managers preview at HAL only; their writers have no Thumb early hook.
        if(armStart.entry)return;
        const bool e2=driver==Driver::e2;
        const uint32_t wrapper=e2?0x20072c6c:0x20057cc4;
        const uint32_t writer=e2?0x2006659c:0x2004b5f4;
        if(!Is(wrapper,0x1c04b510) || !Is(wrapper+4,0x43d22200) ||
           !Is(wrapper+8,0xff48f7ff) || !Is(wrapper+12,e2?0xf01368e0:0xf01468e0) ||
           !Is((e2?0x200665ec:0x2004b644)+0x34,0x2e00692e) ||
           !Is((e2?0x200665ec:0x2004b644)+0x58,e2?0xfd38f019:0xf8d2f01b) ||
           !Is(e2?0x200800b8:0x20066844,0x47384730) ||
           !Is(writer,0x1c05b5f8) || !Is(writer+0x18,0x6920d007) ||
           !Is(writer+0x1c,0xd00a2800) || !Is(writer+0x20,e2?0xf01e1c20:0xf0201c20)) {
            ++earlyRejected;ledger.Fail();return;
        }
        if(from==(e2?0x200665c0u:0x2004b618u)) {
            if((pc&~1u)!=(e2?0x20085410u:0x2006bb94u) || regs[0]!=regs[4])return;
            uint32_t msg[5];
            if(!read(regs[4],msg,sizeof(msg)) || msg[0]!=regs[5] || !msg[4])return;
            const bool hadPreview=ledger.HasPreviewed();
            auto result=ledger.Dispose(regs[5],regs[4]);
            if(hadPreview && !active && result!=EarlyAudioLedger::Result::untracked)quietUntil=now+250000000;
            if(result==EarlyAudioLedger::Result::contradiction)++earlyContradictions;
            else if(result!=EarlyAudioLedger::Result::untracked)++earlyDisposed;
            return;
        }
        if(from!=(e2?0x20072c7cu:0x20057cd4u) || (pc&~1u)!=(e2?0x200861f8u:0x2006c974u))return;
        if(failed)return;
        uint32_t stack[2],header[4],msg[5],stream[15];
        if(!read(regs[13],stack,sizeof(stack)) || stack[1]!=(e2?0x20066669u:0x2004b6c1u))return;
        const uint32_t queue=regs[4],message=stack[0],owner=regs[5];
        if(!read(queue,header,sizeof(header)) || header[3]!=regs[0] ||
           !read(message,msg,sizeof(msg)) || !read(owner,stream,sizeof(stream)) ||
           msg[0]!=owner || stream[9]!=queue || msg[4])return;
        const uint32_t count=header[1]>>16;
        uint32_t last=0;
        if(!count || count>64 || uint64_t(header[0])+4*(count-1)>UINT32_MAX ||
           !Word(header[0]+4*(count-1),last) || last!=message) {++earlyRejected;return;}
        uint32_t hal[16];std::memcpy(hal,regs,sizeof(hal));
        hal[0]=stream[10];hal[1]=msg[1];hal[2]=msg[3];
        if(regs[9]<4 || !Word(regs[9]-4,hal[12])) {++earlyRejected;return;}
        uint32_t object;uint16_t left,right;
        if(!msg[3] || msg[3]>4096 || msg[3]>msg[2] || msg[2]>4096 || (msg[3]&3) || (msg[1]&3) ||
           !Profile(hal,object,left,right)) {++earlyRejected;return;}
        uint32_t rate;
        if(!Word(object+44,rate) || rate!=44100) {++earlyRejected;return;}
        EarlyAudioLedger::Submission submission{{owner,message,msg[1],stream[10],object},left,right,std::vector<uint8_t>(msg[3])};
        if(!read(msg[1],submission.pcm.data(),msg[3])) {++earlyRejected;return;}
        const bool raw=(stream[0]&0xffff)==0x0101 && stream[1]==0 && (stream[2]&0xffff)==0x12 && stream[3]==44100 &&
            stream[4]>=0x10000 && stream[4]<0x02000000 && !(stream[4]&3) && stream[5]==0;
        bool ownedMembers=count<=2;
        for(unsigned i=0;i<count && ownedMembers;++i) {
            uint32_t member=0;
            ownedMembers=Word(header[0]+4*i,member) && (member==stream[13] || member==stream[14]);
        }
        submission.eligible=raw && ownedMembers && stream[13] && stream[14] && stream[13]!=stream[14] &&
            (message==stream[13] || message==stream[14]) && stream[10]==knownChannel;
        if(ledger.Observe(std::move(submission),false)) {
            ++earlyReady;
            if(ControlSuspended() || OutputFenced())ledger.Cancel(stream[10]);
            TryEarly(now,outputEnabled);
        }
        else ++earlyContradictions;
    }
    bool ObserveHal(uint32_t trap,const uint32_t* r,uint64_t now=0) {
        bool suppress=false;
        if(trap==0x4324)return false;
        if(trap==0x4318) {ledger.JoinedClose(r[0]);return false;}
        if(trap!=0x4320 || !ledger.Pending())return false;
        const auto& record=ledger.Records().front();
        const bool hadPreview=record.previewed;
        // Different channels have independent writers. They are unsupported for
        // early playback, but cannot retire or reorder this channel's ledger.
        if(record.data.key.channel!=r[0]) {++earlyRejected;return false;}
        if(record.data.key.message!=r[4] || record.data.key.stream!=r[5]) {
            // A restored guest may already have a buffer in flight before the
            // first observed handoff. Unknown startup HAL writes use HAL normally.
            if(!ledger.Bound() && !ledger.TrackedMessage(r[5],r[4]))return false;
            ledger.Fail();++earlyContradictions;return false;
        }
        uint32_t actualObject=0;uint16_t left=0,right=0;
        const bool supported=Profile(r,actualObject,left,right);
        if(!supported)++earlyRejected;
        if(r[2]>4096 || !actualObject || !Half(actualObject+40,left) || !Half(actualObject+42,right)) {
            ledger.Fail();++earlyContradictions;return false;
        }
        EarlyAudioLedger::Submission data{{r[5],r[4],r[1],r[0],actualObject},left,right,std::vector<uint8_t>(r[2])};
        if(!read(r[1],data.pcm.data(),r[2])) {ledger.Fail();++earlyContradictions;return false;}
        // Canceled submissions may legally reach HAL after a gain change.
        if(record.canceled) {data.left=record.data.left;data.right=record.data.right;}
        auto result=ledger.Hal(data,&suppress);
        if(hadPreview && !active)quietUntil=now+250000000;
        if(result==EarlyAudioLedger::Result::contradiction) {
            ++earlyContradictions;
        }
        else if(result!=EarlyAudioLedger::Result::untracked)++earlyConsumed;
        if(ledger.Failed() && earlyPlayback && !failed)FallBack(now,"submission mismatch",true);
        if(suppress)++skips;
        return suppress;
    }
private:
    bool Word(uint32_t addr, uint32_t& out) { return !(addr&3) && read(addr,&out,4); }
    bool Half(uint32_t addr, uint16_t& out) { return !(addr&1) && read(addr,&out,2); }
    bool Is(uint32_t addr, uint32_t value) { uint32_t got; return Word(addr,got) && got==value; }
    bool Profile(const uint32_t* r, uint32_t& object, uint16_t& left, uint16_t& right) {
        if (r[0]>=16 || r[12]>UINT32_MAX-0x320) return false;
        uint32_t entry, ldr, literal, globals, tableOffset;
        // Channel table: TX and E2 have 20-byte entries with the object at +4, the Zire 72 16-byte ones at +0.
        unsigned stride=20,slot=4;
        if (!Word(r[12]+0x320,entry) || entry<0x600 || entry>UINT32_MAX-8192) return false;
        if (driver == Driver::tx) {
            if (!Is(entry,0xe92d4200) || !Is(entry+4,0xe5999000) || !Is(entry+8,0xe5999000) ||
                !Is(entry+36,0xe0800100) || !Is(entry+40,0xe08ee009) ||
                !Is(entry+44,0xe08e0100) || !Is(entry+48,0xe5900004) ||
                !Word(entry+32,ldr) || (ldr&0xfffff000)!=0xe59fe000) return false;
            // Final mixed-output signed Q11 conversion.
            if (!Is(entry-0x5f8,0xe4920004) || !Is(entry-0x5f4,0xe1a005c0) ||
                !Is(entry-0x5f0,0xe250cc7f)) return false;
            literal=entry+40+(ldr&0xfff);
            if (!Word(literal,tableOffset)) return false;
        } else if (driver == Driver::e2) {
            // E2 HALSoundWrite uses an ARM interworking veneer into its Thumb driver.
            uint32_t delta;
            if (!Is(entry,0xe59fc004) || !Is(entry+4,0xe08cc00f) || !Is(entry+8,0xe12fff1c) ||
                !Word(entry+12,delta)) return false;
            const int64_t target=int64_t(entry)+12+int32_t(delta);
            if (target<0x421 || target>UINT32_MAX-8192 || !(target&1)) return false;
            const uint32_t code=uint32_t(target)&~1u;
            const std::pair<unsigned,uint16_t> lookup[] = {
                {0,0xb570},{2,0x1c1c},{4,0x2800},{10,0x2810},{14,0x2314},
                {16,0x4358},{18,0x464e},{20,0x6836},{22,0x6836},
                {26,0x199b},{28,0x331c},{30,0x18c0},{32,0x6840}};
            for (auto [offset,expected]:lookup) {
                uint16_t got;
                if (!Half(code+offset,got) || got!=expected) return false;
            }
            // Check the gain fields and final stereo mixer conversion as well.
            const std::pair<int,uint16_t> mixer[] = {
                {-0x420,0x6810},{-0x41e,0x6851},{-0x418,0x12c0},{-0x416,0x12c9},
                {0x1d7c,0x8981},{0x1d82,0x8941},{0x1d84,0x8541},{0x1d86,0x8501}};
            for (auto [offset,expected]:mixer) {
                uint16_t got;
                if (!Half(uint32_t(int64_t(code)+offset),got) || got!=expected) return false;
            }
            uint16_t tableLoad;
            if (!Half(code+24,tableLoad) || (tableLoad&0xff00)!=0x4b00) return false;
            literal=((code+28)&~3u)+(tableLoad&255)*4;
            if (!Word(literal,tableOffset) || tableOffset>UINT32_MAX-28) return false;
            tableOffset+=28;
        } else if (driver == Driver::zire72) {
            // ARM HALSoundWrite: checks the channel, loads the object from the 16-byte table and
            // branches to the queue writer.
            uint32_t branch;
            if (!Is(entry,0xe92d4200) || !Is(entry+4,0xe5999000) || !Is(entry+8,0xe5999000) ||
                !Is(entry+12,0xe3500000) || !Is(entry+20,0xe3500010) || !Word(entry+28,ldr) ||
                (ldr&0xfffff000)!=0xe59fc000 || !Is(entry+32,0xe089c00c) || !Is(entry+36,0xe79c0200) ||
                !Is(entry+40,0xe3500000) || !Is(entry+68,0xe8bd4200) || !Word(entry+72,branch) ||
                (branch&0xff000000)!=0xea000000) return false;
            const int64_t writer=int64_t(entry)+80+int64_t(int32_t(branch<<8)>>6);
            if (writer<0 || writer>UINT32_MAX-0x200) return false;
            // Queue writer: format and channel checks, 8-bit and byte-swap branches for formats 0x11 and 2.
            const std::pair<unsigned,uint32_t> queue[] = {
                {0x14,0xe5983000},{0x20,0xe1d830f8},{0x2c,0xe5983004},{0x118,0xe3530011},{0x1dc,0xe3530002}};
            for (auto [offset,expected]:queue) if (!Is(uint32_t(writer)+offset,expected)) return false;
            // Mixer thread: stereo 16-bit mixer for formats 0x12 and 2, then signed Q11 to 16 bits;
            // the stereo mixer reads the signed gains at +40 and +42.
            const std::pair<int,uint32_t> mixer[] = {
                {-0x5e0,0xe35c0012},{-0x5d0,0xe1d730f8},{-0x5cc,0xe3530002},{-0x5bc,0xebffc645},
                {-0x4ec,0xe493b004},{-0x4e4,0xe1a025cb},{-0xec08,0xe1db22f8},{-0xebf4,0xe1db22fa}};
            for (auto [offset,expected]:mixer) if (!Is(uint32_t(int64_t(entry)+offset),expected)) return false;
            literal=entry+36+(ldr&0xfff);
            if (!Word(literal,tableOffset)) return false;
            stride=16;slot=0;
        } else return false;
        if (!Word(r[9],globals) || !Word(globals,globals) ||
            uint64_t(globals)+tableOffset+16*stride>UINT32_MAX) return false;
        const uint32_t table=globals+tableOffset;
        unsigned count=0;
        object=0;
        for (unsigned i=0; i<16; ++i) {
            uint32_t p;
            if (!Word(table+stride*i+slot,p)) return false;
            if (p) ++count;
            if (i==r[0]) object=p;
        }
        if (count!=1 || !object || object>UINT32_MAX-44) return false;
        uint32_t format,rate;
        uint16_t channels;
        return Word(object,format) && format==0x12 && Word(object+4,rate) && rate==44100 &&
            Half(object+8,channels) && channels==2 && Half(object+40,left) && left<=1024 &&
            Half(object+42,right) && right<=1024;
    }
    void FallBack(uint64_t now, const char* reason, bool latch) {
        if(latch) {ledger.CancelAll();knownChannel=UINT32_MAX;controls.clear();stopped=false;}
        if(ledger.HasPreviewed() && !quietUntil)quietUntil=now+250000000;
        if (active) {
            active=false;
            started=false;
            quietUntil=now+250000000; // Let delayed hardware audio drain before reconnecting it.
            if (queue) audioQueueClear(queue);
        }
        failed=latch;
        std::fprintf(stderr,"low-latency audio: normal path (%s)\n",reason);
    }
    bool earlyPlayback=false,stopped=false,sleeping=false,wasPreviewing=false;
    uint32_t knownChannel=UINT32_MAX;
    std::vector<Control> controls;
    uint64_t previews=0,skips=0,starts=0,abandoned=0;
    Reader read;
    Driver driver;
    ArmStartSignature armStart;
    AudioQueue* queue{nullptr};
    unsigned period{512};
    bool active{false}, failed{false}, started{false};
    uint64_t quietUntil{0};
    EarlyAudioLedger ledger;
    struct AudioCheckpoint {uint64_t token;HostState state;};
    std::vector<AudioCheckpoint> audioCheckpoints;
    uint64_t checkpointSequence=0;
    unsigned replacementDepth=0;
    uint64_t earlyReady=0,earlyConsumed=0,earlyDisposed=0,earlyRejected=0,earlyContradictions=0;
};
#endif
