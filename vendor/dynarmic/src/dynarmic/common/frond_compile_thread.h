/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

// Frond: the compile thread of one Jit. The guest thread takes a snapshot of a block's code
// and queues it; the worker translates, optimises and emits it (the back end's compile
// function, which takes the Jit's code lock to emit); the guest thread installs finished
// blocks between runs. Blocks whose code was invalidated or cleared since the snapshot are
// dropped then. Everything here except the worker's loop runs on the guest thread.
//
// Compiling ahead: when the queue of requests is empty the worker compiles blocks the guest
// may ask for soon: each block the guest reaches that isn't hot yet, and the blocks a block
// can go to directly (its terminal's targets and the return site of a call), up to a set
// depth from one that was asked for. Their snapshots are taken on the guest thread, when the
// block is reached or when the block that leads to them is installed or comes back
// finished. An ahead block is kept ready, emitted but not installed, until the guest asks
// for it, so it changes nothing until then; it is installed at the request, after the same
// checks as any other.

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__linux__) || defined(__ANDROID__)
#    include <pthread.h>
#endif

#include <boost/icl/interval_set.hpp>

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Common {

// Workers still compiling when the process exits would use statics that exit is destroying,
// so an exit handler stops every live worker first. Exit runs handlers and static destructors
// newest first, and compiling builds statics lazily, so registering the handler once at the
// start isn't enough: it is registered again after compiles, counted over the whole process
// (the first 64, then each power of two), at most about 100 times in all.
class FrondCompileThreadBase {
public:
    virtual ~FrondCompileThreadBase() = default;
    virtual void Stop() = 0;

protected:
    struct Registry {
        std::mutex mutex;
        std::vector<FrondCompileThreadBase*> live;
        bool registered = false;
    };

    static Registry& GetRegistry() {
        static Registry* registry = new Registry;  // never freed, so it outlives exit
        return *registry;
    }

    static void Add(FrondCompileThreadBase* thread) {
        auto& registry = GetRegistry();
        std::lock_guard lock{registry.mutex};
        if (!registry.registered) {
            registry.registered = true;
            RegisterExitStop();
        }
        registry.live.push_back(thread);
    }

    static void Remove(FrondCompileThreadBase* thread) {
        auto& registry = GetRegistry();
        std::lock_guard lock{registry.mutex};
        std::erase(registry.live, thread);
    }

    // After each compile, on the worker.
    static void NoteCompiled() {
        static std::atomic<std::uint64_t> compiles{0};
        const std::uint64_t n = compiles.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 64 || std::has_single_bit(n)) {
            RegisterExitStop();
        }
    }

    static void RegisterExitStop() {
        std::atexit([] {
            auto& registry = GetRegistry();
            std::lock_guard lock{registry.mutex};
            for (auto* thread : registry.live) {
                thread->Stop();
            }
        });
    }
};

// The blocks a block can go to directly: its terminal's links and the return sites it pushes.
inline void FrondBlockTargets(const IR::Block& block, std::vector<std::uint64_t>& out) {
    constexpr std::size_t max_targets = 4;
    auto add = [&](IR::LocationDescriptor location) {
        if (out.size() < max_targets && std::find(out.begin(), out.end(), location.Value()) == out.end()) {
            out.push_back(location.Value());
        }
    };
    std::function<void(const IR::Terminal&)> walk = [&](const IR::Terminal& term) {
        if (const auto* t = boost::get<IR::Term::LinkBlock>(&term)) {
            add(t->next);
        } else if (const auto* t = boost::get<IR::Term::LinkBlockFast>(&term)) {
            add(t->next);
        } else if (const auto* t = boost::get<IR::Term::If>(&term)) {
            walk(t->then_);
            walk(t->else_);
        } else if (const auto* t = boost::get<IR::Term::CheckBit>(&term)) {
            walk(t->then_);
            walk(t->else_);
        } else if (const auto* t = boost::get<IR::Term::CheckHalt>(&term)) {
            walk(t->else_);
        }
    };
    walk(block.GetTerminal());
    for (const auto& inst : block) {
        if (inst.GetOpcode() == IR::Opcode::PushRSB && inst.GetArg(0).IsImmediate()) {
            add(IR::LocationDescriptor{inst.GetArg(0).GetU64()});
        }
    }
}

template<typename Result>
class FrondCompileThread final : public FrondCompileThreadBase {
public:
    // Skipped: an ahead job left undone, as the code cache is past its high-water mark.
    enum class Status { Queued, Done, Cancelled, Overflow, NoSpace, Skipped };

    // Most ahead jobs queued or in flight, and most kept ready.
    static constexpr std::size_t max_ahead_pending = 4096;
    static constexpr std::size_t max_ready = 8192;

    struct Job {
        IR::LocationDescriptor location{0};
        std::unique_ptr<A32::CodeSnapshot> snapshot;
        std::uint64_t epoch = 0;      // cache clears before the request
        std::uint64_t inval_seq = 0;  // range invalidations before the request
        Status status = Status::Queued;
        std::uint32_t first_pc = 0, last_pc = 0;  // guest code the block covers, set with Done
        Result result{};
        // A block that starts with an Interpret term ends where it starts (last_pc is
        // first_pc - 1) and covers no guest code, so invalidations leave it, as block_ranges does.
        bool HasCode() const { return first_pc <= last_pc; }
        bool ahead = false;              // compiled ahead, not asked for
        bool wanted = false;             // ahead, and asked for before it was done
        bool valid = true;               // none of its code invalidated since the snapshot
        std::uint64_t ready_seq = 0;     // order kept ready in
        unsigned depth = 0;              // ahead: steps from a block that was asked for
        std::vector<std::uint64_t> targets;  // set by the worker (FrondBlockTargets)
    };

    // Runs on the worker for each job not cancelled yet. Sets the status and, for Done,
    // the result and the range.
    using CompileFn = std::function<void(Job&)>;

    explicit FrondCompileThread(CompileFn fn)
            : compile(std::move(fn)), worker([this] { Loop(); }) {
        Add(this);
    }

    ~FrondCompileThread() override {
        Remove(this);
        Stop();
    }

    // Finishes the job in hand and drops the rest. Safe to call more than once.
    void Stop() override {
        std::lock_guard stop_lock{stop_mutex};
        {
            std::lock_guard lock{mutex};
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    std::uint64_t Epoch() const { return epoch.load(std::memory_order_acquire); }

    bool IsPending(IR::LocationDescriptor location) const { return pending.count(location.Value()) != 0; }
    bool Overflowed(IR::LocationDescriptor location) const { return overflowed.count(location.Value()) != 0; }

    void Push(IR::LocationDescriptor location, std::unique_ptr<A32::CodeSnapshot> snapshot) {
        stats.queued++;
        Enqueue(location, std::move(snapshot), false, 0);
    }

    // Sync mode with nothing compiling ahead: the request is compiled here, on the guest
    // thread, as the worker would, and left on the done list. The worker has nothing queued,
    // so this gives what Push and WaitIdle would, without waking it and waiting.
    void CompileHere(IR::LocationDescriptor location, std::unique_ptr<A32::CodeSnapshot> snapshot) {
        stats.queued++;
        auto job = std::make_unique<Job>();
        job->location = location;
        job->snapshot = std::move(snapshot);
        job->epoch = Epoch();
        job->inval_seq = inval_seq;
        pending.insert(location.Value());
        compile(*job);
        std::lock_guard lock{mutex};
        done.push_back(std::move(job));
        done_count.store(done.size(), std::memory_order_release);
    }

    // Compiling ahead, up to depth steps from a block asked for; 0 is off.
    void SetAhead(unsigned depth) { ahead_depth = depth; }
    bool AheadOn() const { return ahead_depth != 0; }

    // The guest asks for a block an ahead job is still compiling: the worker moves it before
    // the other ahead jobs, and it is installed as soon as it is done.
    void Want(IR::LocationDescriptor location) {
        if (!ahead_pending.count(location.Value()) || !wanted.insert(location.Value()).second) {
            return;
        }
        std::lock_guard lock{mutex};
        promote.push_back(location.Value());
    }

    // Queues the targets of a block depth steps from one asked for. A target ahead already
    // gets the smaller depth, and its own targets are looked at again. compiled(location)
    // says whether the block is installed; snapshot(location) takes its code, or null.
    template<typename Compiled, typename Snapshot>
    void QueueAhead(const std::vector<std::uint64_t>& targets, unsigned depth, const Compiled& compiled, const Snapshot& snapshot) {
        if (depth > ahead_depth) {
            return;
        }
        for (const std::uint64_t target : targets) {
            if (const auto r = ready.find(target); r != ready.end()) {
                if (r->second->depth > depth) {
                    r->second->depth = depth;
                    QueueAhead(r->second->targets, depth + 1, compiled, snapshot);
                }
                continue;
            }
            if (const auto p = ahead_pending.find(target); p != ahead_pending.end()) {
                p->second = std::min(p->second, depth);
                continue;
            }
            const IR::LocationDescriptor location{target};
            if (pending.count(target) || compiled(location)) {
                continue;
            }
            if (high_water.load(std::memory_order_acquire) || (ahead_pending.size() >= max_ahead_pending && !DropOldestAhead())) {
                return;
            }
            auto s = snapshot(location);
            if (!s || !s->runnable) {
                continue;
            }
            stats.ahead_queued++;
            Enqueue(location, std::move(s), true, depth);
        }
    }

    // A block the guest reached that isn't hot yet: compiled ahead, so it is ready when it
    // is. Its own targets wait until it is used.
    template<typename Compiled, typename Snapshot>
    void QueueCold(IR::LocationDescriptor location, const Compiled& compiled, const Snapshot& snapshot) {
        if (ahead_depth == 0) {
            return;
        }
        const std::uint64_t key = location.Value();
        if (ready.count(key) || pending.count(key)) {
            return;
        }
        const std::vector<std::uint64_t> one{key};
        QueueAhead(one, ahead_depth, compiled, snapshot);
    }

    // A finished ahead job nobody asked for yet: kept until asked for, or dropped if its
    // code is invalidated meanwhile. Past max_ready the oldest is dropped.
    void KeepReady(std::unique_ptr<Job> job) {
        if (!StillValid(*job)) {
            stats.stale++;
            return;
        }
        // One TakeDone batch can queue a target that is in the same batch, so the same block
        // can be kept twice. The first copy stays; a second index entry would outlive it.
        if (ready.count(job->location.Value())) {
            stats.stale++;
            return;
        }
        while (ready.size() >= max_ready && !ready_order.empty()) {
            const auto [key, seq] = ready_order.front();
            ready_order.pop_front();
            if (const auto it = ready.find(key); it != ready.end() && it->second->ready_seq == seq) {
                DropReadyIndex(*it->second);
                ready.erase(it);
            }
        }
        if (ready_order.size() >= 2 * max_ready) {
            std::erase_if(ready_order, [this](const auto& entry) {
                const auto it = ready.find(entry.first);
                return it == ready.end() || it->second->ready_seq != entry.second;
            });
        }
        job->ready_seq = ++ready_seq;
        ready_order.emplace_back(job->location.Value(), job->ready_seq);
        if (job->HasCode()) {
            ready_span = std::max(ready_span, job->last_pc - job->first_pc);
        }
        ready_by_pc.emplace(job->first_pc, job->location.Value());
        const std::uint64_t key = job->location.Value();
        ready.emplace(key, std::move(job));
    }

    // The ready block at location, taken off the ready list, or null.
    std::unique_ptr<Job> TakeReady(IR::LocationDescriptor location) {
        const auto it = ready.find(location.Value());
        if (it == ready.end()) {
            return nullptr;
        }
        std::unique_ptr<Job> job = std::move(it->second);
        ready.erase(it);
        DropReadyIndex(*job);
        return job;
    }

    void NoteAheadUsed() { stats.ahead_used++; }
    void NoteHighWater() { high_water.store(true, std::memory_order_release); }

    // Waits until every queued job is finished (sync mode).
    void WaitIdle() {
        std::unique_lock lock{mutex};
        idle.wait(lock, [this] { return busy == 0 || stopping; });
    }

    bool HasDone() const { return done_count.load(std::memory_order_acquire) != 0; }

    // Finished jobs, each taken off the pending list. Overflowed ones are noted so the next
    // request takes a whole snapshot.
    std::vector<std::unique_ptr<Job>> TakeDone() {
        std::vector<std::unique_ptr<Job>> jobs;
        {
            std::lock_guard lock{mutex};
            jobs.swap(done);
            done_count.store(0, std::memory_order_release);
        }
        for (auto& job : jobs) {
            pending.erase(job->location.Value());
            if (job->ahead) {
                const auto p = ahead_pending.find(job->location.Value());
                if (p != ahead_pending.end()) {
                    job->depth = p->second;
                    ahead_pending.erase(p);
                }
                job->wanted = wanted.erase(job->location.Value()) != 0;
                if (job->status == Status::Done) {
                    stats.ahead_compiled++;
                } else if (job->status != Status::Overflow) {
                    stats.ahead_skipped++;
                }
            }
            switch (job->status) {
            case Status::Cancelled:
                stats.cancelled++;
                break;
            case Status::Overflow:
                stats.overflow++;
                overflowed.insert(job->location.Value());
                break;
            case Status::NoSpace:
                stats.no_space++;
                break;
            case Status::Done:
                job->valid = Unaffected(*job);
                break;
            default:
                break;
            }
        }
        if (pending.empty()) {
            inval_log.clear();
        }
        return jobs;
    }

    // A Done job may be installed: same cache generation and none of its code invalidated
    // since the request (looked at when it was taken off the done list).
    bool StillValid(const Job& job) const {
        return job.valid && job.epoch == Epoch();
    }

    void NoteInstalled() { stats.installed++; }
    void NoteStale() { stats.stale++; }

private:
    bool Unaffected(const Job& job) const {
        if (!job.HasCode()) {
            return true;
        }
        const auto range = boost::icl::discrete_interval<std::uint32_t>::closed(job.first_pc, job.last_pc);
        for (const auto& [seq, ranges] : inval_log) {
            if (seq >= job.inval_seq && boost::icl::intersects(ranges, range)) {
                return false;
            }
        }
        return true;
    }

public:
    // The whole cache is being cleared, with the Jit's code lock held: nothing finished
    // before this is installed, and queued jobs are cancelled without compiling.
    void Cleared() {
        epoch.fetch_add(1, std::memory_order_acq_rel);
        overflowed.clear();
        inval_log.clear();
        ready.clear();
        ready_by_pc.clear();
        ready_order.clear();
        ready_span = 0;
        wanted.clear();
        high_water.store(false, std::memory_order_release);
    }

    // Ranges are being invalidated: jobs requested before this that cover them are dropped,
    // and so are ready blocks that cover them.
    void Invalidated(const boost::icl::interval_set<std::uint32_t>& ranges) {
        if (!pending.empty()) {
            inval_log.emplace_back(inval_seq, ranges);
        }
        inval_seq++;
        if (ready.empty()) {
            return;
        }
        for (const auto& interval : ranges) {
            const std::uint32_t first = boost::icl::first(interval), last = boost::icl::last(interval);
            auto it = ready_by_pc.lower_bound(first > ready_span ? first - ready_span : 0);
            while (it != ready_by_pc.end() && it->first <= last) {
                const auto r = ready.find(it->second);
                if (r == ready.end()) {
                    it = ready_by_pc.erase(it);
                    continue;
                }
                if (r->second->HasCode() && r->second->last_pc >= first) {
                    stats.stale++;
                    ready.erase(r);
                    it = ready_by_pc.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    A32::Jit::CompileStats GetStats() const { return stats; }

private:
    void Loop() {
#if defined(__linux__) || defined(__ANDROID__)
        pthread_setname_np(pthread_self(), "FrondJit");
#endif
        std::unique_lock lock{mutex};
        for (;;) {
            wake.wait(lock, [this] { return stopping || !queue.empty() || !ahead_queue.empty(); });
            if (stopping) {
                // Queued jobs are dropped; nobody waits for them.
                busy = 0;
                idle.notify_all();
                return;
            }
            // Requests first, wanted ahead jobs with them, then the newest ahead job: code the
            // guest has just reached is likelier to get hot soon.
            for (const std::uint64_t key : promote) {
                const auto it = std::find_if(ahead_queue.begin(), ahead_queue.end(), [&](const auto& job) { return job->location.Value() == key; });
                if (it != ahead_queue.end()) {
                    queue.push_back(std::move(*it));
                    ahead_queue.erase(it);
                }
            }
            promote.clear();
            std::unique_ptr<Job> job;
            if (!queue.empty()) {
                job = std::move(queue.front());
                queue.pop_front();
            } else {
                job = std::move(ahead_queue.back());
                ahead_queue.pop_back();
            }
            lock.unlock();
            if (job->epoch != Epoch()) {
                job->status = Status::Cancelled;
            } else {
                compile(*job);
                NoteCompiled();
            }
            lock.lock();
            done.push_back(std::move(job));
            done_count.store(done.size(), std::memory_order_release);
            if (--busy == 0) {
                idle.notify_all();
            }
        }
    }

    void Enqueue(IR::LocationDescriptor location, std::unique_ptr<A32::CodeSnapshot> snapshot, bool ahead, unsigned depth) {
        auto job = std::make_unique<Job>();
        job->location = location;
        job->snapshot = std::move(snapshot);
        job->epoch = Epoch();
        job->inval_seq = inval_seq;
        job->ahead = ahead;
        job->depth = depth;
        pending.insert(location.Value());
        if (ahead) {
            ahead_pending.emplace(location.Value(), depth);
        }
        {
            std::lock_guard lock{mutex};
            (ahead ? ahead_queue : queue).push_back(std::move(job));
            busy++;
        }
        wake.notify_one();
    }

    // Takes the oldest ahead job off the queue, to make room for a newer one. False if
    // there is none queued (all in flight or done).
    bool DropOldestAhead() {
        std::unique_ptr<Job> job;
        {
            std::lock_guard lock{mutex};
            if (ahead_queue.empty()) {
                return false;
            }
            job = std::move(ahead_queue.front());
            ahead_queue.pop_front();
            if (--busy == 0) {
                idle.notify_all();
            }
        }
        pending.erase(job->location.Value());
        ahead_pending.erase(job->location.Value());
        wanted.erase(job->location.Value());
        stats.ahead_skipped++;
        return true;
    }

    void DropReadyIndex(const Job& job) {
        auto [it, end] = ready_by_pc.equal_range(job.first_pc);
        for (; it != end; ++it) {
            if (it->second == job.location.Value()) {
                ready_by_pc.erase(it);
                break;
            }
        }
        if (ready.empty()) {
            ready_span = 0;
        }
    }

    CompileFn compile;

    // Shared with the worker, under mutex.
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<std::unique_ptr<Job>> queue, ahead_queue;
    std::vector<std::uint64_t> promote;  // wanted ahead jobs still queued, maybe

    std::vector<std::unique_ptr<Job>> done;
    std::size_t busy = 0;
    bool stopping = false;
    std::atomic<std::size_t> done_count{0};
    std::atomic<std::uint64_t> epoch{0};
    std::atomic<bool> high_water{false};  // set by the worker, cleared with the cache
    std::mutex stop_mutex;

    // Guest thread only.
    std::unordered_set<std::uint64_t> pending, overflowed;
    unsigned ahead_depth = 0;
    std::unordered_map<std::uint64_t, unsigned> ahead_pending;  // ahead jobs in pending, by depth
    std::unordered_set<std::uint64_t> wanted;                   // ahead jobs asked for meanwhile
    std::unordered_map<std::uint64_t, std::unique_ptr<Job>> ready;
    std::multimap<std::uint32_t, std::uint64_t> ready_by_pc;  // ready blocks by first_pc
    std::uint32_t ready_span = 0;                               // longest last_pc - first_pc of those
    std::deque<std::pair<std::uint64_t, std::uint64_t>> ready_order;  // key and ready_seq, oldest first
    std::uint64_t ready_seq = 0;
    std::uint64_t inval_seq = 0;
    std::vector<std::pair<std::uint64_t, boost::icl::interval_set<std::uint32_t>>> inval_log;
    A32::Jit::CompileStats stats;

    std::thread worker;  // last, so it starts after everything it uses
};

}  // namespace Dynarmic::Common
