#include "RawPrintJob.h"
#include <cassert>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>
using J = RawPrintJob;
struct Sink { J* job; unsigned calls=0; std::vector<uint8_t> bytes; J::Publication result=J::Publication::published; bool throws=false; };
static J::Publication output(void* ctx, const uint8_t* data, size_t size) {
    auto& s=*static_cast<Sink*>(ctx); ++s.calls;
    assert(s.job->state()==J::State::publishing);
    assert(!s.job->begin() && !s.job->abort() && !s.job->append(nullptr,0));
    assert(s.job->finish(output,ctx)==J::State::publishing);
    if(size) s.bytes.assign(data,data+size);
    if(s.throws) throw std::runtime_error("partial external output");
    return s.result;
}
int main() {
    static_assert(!std::is_copy_constructible<J>::value);
    J job(4); Sink s{&job,0,{}}; uint8_t bytes[]={0,255,10,13};
    assert(!job.append(bytes,1) && !job.abort());
    assert(job.begin() && !job.begin());
    assert(job.append(bytes,2) && job.append(bytes+2,2));
    assert(job.finish(output,&s)==J::State::published);
    assert(s.bytes==std::vector<uint8_t>(bytes,bytes+4) && job.size()==0);
    assert(job.finish(output,&s)==J::State::published && s.calls==1);
    assert(!job.append(bytes,1) && !job.abort());
    assert(job.begin() && job.append(bytes,3));
    assert(!job.append(bytes,2) && job.state()==J::State::overflow && !job.size());
    assert(job.finish(output,&s)==J::State::overflow && s.calls==1);
    assert(job.begin() && !job.append(nullptr,1) && job.state()==J::State::invalid);
    assert(job.begin() && !job.append(bytes,std::numeric_limits<size_t>::max()));
    assert(job.begin() && job.append(bytes,2) && job.abort());
    assert(job.finish(output,&s)==J::State::aborted && s.calls==1);
    assert(job.begin() && job.finish(nullptr,nullptr)==J::State::rejected);
    for(auto result : {J::Publication::rejected,J::Publication::uncertain}) {
        s.result=result; assert(job.begin() && job.append(bytes,2));
        auto state=job.finish(output,&s);
        assert(state==(result==J::Publication::rejected ? J::State::rejected : J::State::uncertain));
        auto calls=s.calls; assert(job.finish(output,&s)==state && s.calls==calls);
    }
    s.throws=true; assert(job.begin() && job.append(bytes,1));
    assert(job.finish(output,&s)==J::State::uncertain && !job.size());
    J empty(0); Sink e{&empty,0,{}};
    assert(empty.begin() && empty.append(nullptr,0));
    assert(empty.finish(output,&e)==J::State::published && e.calls==1 && e.bytes.empty());
    assert(empty.begin() && !empty.append(bytes,1));
}
