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
int main() {
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
