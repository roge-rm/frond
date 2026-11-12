#ifndef FROND_EARLY_AUDIO_LEDGER_H
#define FROND_EARLY_AUDIO_LEDGER_H
#include <cstdint>
#include <deque>
#include <vector>
#include <utility>

// Host-only ownership. Guest adapter supplies already-qualified snapshots.
// No pointers into guest memory survive a call; payload storage is bounded.
class EarlyAudioLedger {
 public:
    struct Key {
        uint32_t stream, message, buffer, channel, object;
        bool operator==(const Key& b) const {
            return stream==b.stream && message==b.message && buffer==b.buffer &&
                   channel==b.channel && object==b.object;
        }
    };
    struct Submission {
        Key key;
        uint16_t left, right;
        std::vector<uint8_t> pcm;
        bool eligible=false;
    };
    struct Record {
        Submission data;
        uint64_t generation, incarnation, sequence;
        bool previewed, canceled;
    };
    enum class Result { untracked, observed, suppress, contradiction };
    static constexpr size_t capacity=8;
    bool Bound() const { return bound; }
    bool TrackedMessage(uint32_t stream,uint32_t message) const { for(const auto& r:records)if(r.data.key.stream==stream && r.data.key.message==message)return true;return false; }
    bool Failed() const { return failed; }
    bool Warm() const { return warm && !failed && !fenced && !blocked; }
    bool Fenced() const { return fenced; }
    size_t Pending() const { return records.size(); }
    bool HasPreviewed() const {
        for(const auto& r:records)if(r.previewed)return true;
        return false;
    }
    const std::deque<Record>& Records() const { return records; }
    bool HasCanceled() const {for(const auto& r:records)if(r.canceled)return true;return false;}
    void Resume() {if(!HasCanceled())blocked=false;}
    bool MarkPreviewed(uint64_t seq) {for(auto& r:records)if(r.sequence==seq){r.previewed=true;return true;}return false;}
    void Fail() { failed=true; warm=false; }
    void NewTimeline() {
        records.clear(); ++generation; ++incarnation;
        owner=ownerChannel=0; failed=false; warm=false; fenced=false; blocked=false; bound=false;
    }
    void DeliveryDiscontinuity() {warm=false;++generation;}
    void CancelAll() {warm=false;blocked=true;for(auto& r:records)r.canceled=true;}
    void Fence() { fenced=true; warm=false; }
    void Unfence() { fenced=false; warm=false; ++generation; }
    void Cancel(uint32_t channel) {
        warm=false; blocked=true;
        for(auto& r:records)if(r.data.key.channel==channel)r.canceled=true;
    }
    // Only call after the guest has joined its writer, before HAL destroys channel.
    void JoinedClose(uint32_t channel) {
        for(auto it=records.begin();it!=records.end();) {
            if(it->data.key.channel==channel)it=records.erase(it);else ++it;
        }
        if(owner && ownerChannel==channel) {owner=ownerChannel=0; ++incarnation; warm=false; blocked=false; bound=false;}
    }
    bool Observe(Submission data, bool previewed) {
        if(failed || data.pcm.empty() || data.pcm.size()>4096 || (data.pcm.size()&3) ||
           !data.key.stream || !data.key.message || !data.key.object) {
            Fail(); return false;
        }
        if(records.size()==capacity || (owner && owner!=data.key.stream)) {
            Fail(); return false;
        }
        for(const auto& r:records)if(r.data.key.message==data.key.message) {
            Fail(); return false; // A live message cannot be reused before retirement.
        }
        if(!owner) {owner=data.key.stream; ownerChannel=data.key.channel; ++incarnation;}
        records.push_back({std::move(data),generation,incarnation,++sequence,previewed,false});
        return true;
    }
    Result Hal(const Submission& data, bool* suppress=nullptr) {
        if(suppress)*suppress=false;
        if(records.empty())return Result::untracked;
        const auto& head=records.front();
        if(!(head.data.key==data.key)) {Fail();return Result::contradiction;}
        const bool previewed=head.previewed;
        if(suppress)*suppress=previewed || head.canceled;
        const bool exact=head.data.left==data.left && head.data.right==data.right && head.data.pcm==data.pcm;
        records.pop_front();
        if(!exact) {Fail();return Result::contradiction;}
        if(!failed)bound=true;
        if(records.empty() && !failed && !fenced) {warm=true;bound=true;}
        return previewed?Result::suppress:Result::observed;
    }
    Result Dispose(uint32_t stream,uint32_t message) {
        if(records.empty())return Result::untracked;
        bool known=false;
        for(const auto& r:records)known=known || (r.data.key.stream==stream && r.data.key.message==message);
        if(!known)return Result::untracked;
        const auto& head=records.front();
        if(head.data.key.stream!=stream || head.data.key.message!=message) {
            Fail();return Result::contradiction;
        }
        const bool previewed=head.previewed;
        records.pop_front();
        if(!failed)bound=true;
        if(records.empty() && !failed && !fenced) {warm=true;bound=true;}
        return previewed?Result::suppress:Result::observed;
    }
 private:
    std::deque<Record> records;
    uint64_t generation=1,incarnation=0,sequence=0;
    uint32_t owner=0,ownerChannel=0;
    bool failed=false,warm=false,fenced=false,blocked=false,bound=false;
};
#endif
