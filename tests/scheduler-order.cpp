// Keep timer queue shortcuts equivalent at ties, insertion, removal and callbacks.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "scheduler.h"
extern "C" void uarmAbort() { std::abort(); }
struct Delegate {
    Scheduler<Delegate>* scheduler = nullptr;
    std::vector<unsigned> calls;
    bool removeTimer = false, insertEarlier = false, rescheduleSelf = false;
    uint32_t DispatchTicks(uint32_t type, uint32_t ticks) {
        calls.push_back(type);
        if (type == SCHEDULER_TASK_TIMER && rescheduleSelf)
            scheduler->RescheduleTaskAtLeast(SCHEDULER_TASK_TIMER, 1);
        if (type == SCHEDULER_TASK_TIMER && insertEarlier) {
            insertEarlier = false;
            scheduler->ScheduleTask(SCHEDULER_TASK_LCD, 1, 1);
        }
        return type == SCHEDULER_TASK_TIMER && removeTimer ? 0 : ticks;
    }
};
static void cyclePhase() {
    Delegate d, split;
    Scheduler<Delegate> whole(d), parts(split);
    d.scheduler = &whole;
    split.scheduler = &parts;
    whole.ScheduleTask(SCHEDULER_TASK_TIMER, 25, 1);
    parts.ScheduleTask(SCHEDULER_TASK_TIMER, 25, 1);
    whole.Advance(39, 312000000);
    for (int i = 0; i < 39; ++i) parts.Advance(1, 312000000);
    assert(whole.GetTime() == 125 && parts.GetTime() == 125);
    assert(d.calls.size() == 5 && split.calls == d.calls);

    Delegate clock;
    Scheduler<Delegate> s(clock);
    clock.scheduler = &s;
    s.ScheduleTask(SCHEDULER_TASK_TIMER, 125, 1);
    s.Advance(1, 156000000);
    assert(s.GetTime() == 6);
    assert(s.CyclesToNextUpdate(312000000) == 38);
    // OSCR reads and peeks include the fraction from the earlier clock.
    assert(s.ElapsedTicks(SCHEDULER_TASK_TIMER, s.ElapsedNsec(36, 312000000)) == 0);
    assert(s.ElapsedTicks(SCHEDULER_TASK_TIMER, s.ElapsedNsec(37, 312000000)) == 1);
    assert(s.GetTime() == 6);
    s.Advance(37, 312000000);
    assert(s.GetTime() == 125 && clock.calls.size() == 1);

    // A half nanosecond of carried time can reduce the next request even at a fixed rate.
    s.ScheduleTask(SCHEDULER_TASK_TIMER, 1, 1);
    s.Advance(1, 2000000000);
    assert(s.CyclesToNextUpdate(2000000000) == 2);
    s.Advance(1, 2000000000);
    assert(s.GetTime() == 126 && clock.calls.size() == 2);
}

static void wideCycleConversion() {
    Delegate d;
    Scheduler<Delegate> s(d);
    assert(s.CyclesToNextUpdate(20000000000ull) == 20000000001ull);
    s.Advance(1, 2000000000);
    s.Advance(18446744073ull, 2000000000);
    assert(s.GetTime() == 9223372037ull);
    s.Advance(39000000000ull, 312000000);
    assert(s.GetTime() == 9223372037ull + 125_sec);
}

static void savePhase() {
    Delegate d;
    Scheduler<Delegate> s(d);
    s.Advance(1, 3000000000ull);
    Savestate<ChunkType> save;
    assert(save.Save(s));
    s.Advance(2, 3000000000ull);
    assert(s.GetTime() == 1);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(save.GetBuffer(), save.GetSize(), s));
    s.Advance(2, 3000000000ull);
    assert(s.GetTime() == 1);

    // Version zero ends after accTime. Load it over a scheduler with a fraction.
    auto* words = static_cast<uint32_t*>(save.GetBuffer());
    std::vector<uint32_t> old(words, words + save.GetSize() / 4 - 4);
    old[2] = htole32(le32toh(old[2]) - 4);
    old[3] = 0;
    s.Advance(1, 3000000000ull);
    assert(loader.Load(old.data(), old.size() * 4, s));
    s.Advance(2, 3000000000ull);
    assert(s.GetTime() == 0);
    s.Advance(1, 3000000000ull);
    assert(s.GetTime() == 1);
}
int main() {
    cyclePhase();
    wideCycleConversion();
    savePhase();
    {
        Delegate d;
        Scheduler<Delegate> s(d); d.scheduler = &s;
        s.ScheduleTask(SCHEDULER_TASK_TIMER, 10, 1);
        s.ScheduleTask(SCHEDULER_TASK_RTC, 30, 1);
        s.Advance(60, 1000000000);
        // The previously scheduled RTC precedes the timer when their deadlines tie.
        assert((d.calls == std::vector<unsigned>{0,0,1,0,0,0,1,0}));
        assert(s.GetTime() == 60);
        assert(s.CyclesToNextUpdate(1000000000) == 11);
        d.calls.clear();
        s.RescheduleTask(SCHEDULER_TASK_TIMER, 5);  // Move head behind RTC.
        s.Advance(30, 1000000000);
        assert((d.calls == std::vector<unsigned>{1}));
        d.calls.clear();
        s.RescheduleTaskAtLeast(SCHEDULER_TASK_TIMER, 1); // Overdue ticks, same phase.
        s.Advance(1, 1000000000);
        assert((d.calls == std::vector<unsigned>{0}));
        d.removeTimer = true;
        s.Advance(10, 1000000000);
        assert(d.calls.back() == 0);
        d.calls.clear();
        s.Advance(9, 1000000000);
        assert(d.calls.empty());
    }
    {
        Delegate d;
        Scheduler<Delegate> s(d); d.scheduler = &s;
        s.ScheduleTask(SCHEDULER_TASK_TIMER, 10, 1);
        d.insertEarlier = true;
        s.Advance(10, 1000000000);
        d.calls.clear();
        s.Advance(1, 1000000000);
        assert((d.calls == std::vector<unsigned>{2}));
        s.UnscheduleTask(SCHEDULER_TASK_LCD);
        d.calls.clear();
        s.Advance(9, 1000000000);
        assert((d.calls == std::vector<unsigned>{0}));
    }
    {
        Delegate d;
        Scheduler<Delegate> s(d); d.scheduler = &s;
        s.ScheduleTask(SCHEDULER_TASK_TIMER, 10, 1);
        d.rescheduleSelf = true;
        s.Advance(35, 1000000000);
        assert((d.calls == std::vector<unsigned>{0,0,0}));
        assert(s.ElapsedTicks(SCHEDULER_TASK_TIMER) == 0);
        assert(s.ElapsedTicks(SCHEDULER_TASK_TIMER, 10) == 1);
        assert(s.CyclesToNextUpdate(1000000000) == 6);
    }
    puts("PASS: scheduler preserves tied deadlines, phase, removal and callback insertion");
}
