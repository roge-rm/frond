#include "early_audio_ledger.h"
#include <cassert>
#include <cstdio>
using L=EarlyAudioLedger;
static L::Submission block(unsigned message=1,unsigned sample=42) {
    return {{100,message,200+message*4096,0,300},1024,1024,std::vector<uint8_t>(4096,sample)};
}
int main() {
    L l;
    assert(!l.Warm() && l.Hal(block())==L::Result::untracked);
    assert(l.Observe(block(),false));
    assert(l.Hal(block())==L::Result::observed && l.Warm());
    assert(l.Observe(block(),true));
    auto id=l.Records().front();
    assert(l.Hal(block())==L::Result::suppress);
    assert(l.Observe(block(),true)); // Address reuse after consumption is a new submission.
    assert(l.Records().front().sequence>id.sequence);
    l.Cancel(0);assert(l.Pending()==1 && !l.Warm() && l.HasPreviewed());
    assert(l.Hal(block())==L::Result::suppress && !l.HasPreviewed());
    assert(l.Observe(block(),true));l.Cancel(0);
    assert(l.Dispose(100,1)==L::Result::suppress && l.Pending()==0);
    assert(l.Observe(block(),true));l.JoinedClose(0);assert(!l.Pending());
    assert(l.Observe(block(),true));assert(l.Records().front().incarnation>id.incarnation);
    auto checkpoint=l;
    l.Fence();assert(!l.Warm());
    assert(l.Hal(block())==L::Result::suppress);assert(l.Observe(block(2),false));
    l=checkpoint;l.Unfence(); // Guest rollback restores suppression, never the output queue.
    assert(l.Hal(block())==L::Result::suppress && l.Pending()==0);
    assert(l.Observe(block(),true));l.NewTimeline();assert(!l.Pending() && !l.HasPreviewed());
    assert(l.Observe(block(),false));assert(l.Hal(block())==L::Result::observed);
    assert(l.Observe(block(),true));assert(!l.Observe(block(),true) && l.Failed());
    l.NewTimeline();
    assert(l.Observe(block(1),true));assert(l.Observe(block(2),true));
    assert(l.Hal(block(2))==L::Result::contradiction && l.Failed());
    assert(l.Pending()==2 && l.HasPreviewed()); // Failed matching never erases suppression history.
    l.NewTimeline();assert(l.Observe(block(),true));
    auto changed=block();changed.pcm[17]^=1;
    bool suppress=false;
    assert(l.Hal(changed,&suppress)==L::Result::contradiction && l.Failed() && suppress);
    l.NewTimeline();assert(l.Observe(block(),true));changed=block();changed.left=512;
    assert(l.Hal(changed)==L::Result::contradiction);
    l.NewTimeline();for(unsigned i=1;i<=8;++i)assert(l.Observe(block(i),false));
    assert(!l.Observe(block(9),false) && l.Pending()==8);
    l.NewTimeline();assert(l.Observe(block(),false));changed=block(2);changed.key.stream=101;
    assert(!l.Observe(changed,false));
    l.NewTimeline();changed=block();changed.pcm.resize(4100);assert(!l.Observe(changed,false));
    l.NewTimeline();changed=block();changed.pcm.resize(3);assert(!l.Observe(changed,false));
    l.NewTimeline();assert(l.Observe(block(),true));assert(l.Dispose(100,2)==L::Result::untracked);
    assert(l.Pending()==1 && l.HasPreviewed());
    std::puts("PASS owned ordered ledger: controls, disposal, generations, rollback, reuse, bounds, order, PCM and gain contradictions");
}
