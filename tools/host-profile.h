#ifndef FROND_BENCHMARK_HOST_PROFILE_H
#define FROND_BENCHMARK_HOST_PROFILE_H
// Optional CPU-time sampling for the standalone benchmark, without perf privileges.
// Linux/x86-64 only. No allocation or stdio in the SIGPROF handler.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#if defined(__linux__) && defined(__x86_64__)
#include <csignal>
#include <dlfcn.h>
#include <sys/time.h>
#include <ucontext.h>
namespace hostProfile {
inline uintptr_t samples[200000];
inline volatile sig_atomic_t count=0;
inline volatile sig_atomic_t dropped=0;
inline void sample(int, siginfo_t*, void* context) {
    const auto pc=static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP];
    if (count<200000) samples[count++]=pc;
    else dropped=1;
}
}
#endif
class HostProfile {
    bool enabled=std::getenv("FROND_HOST_PROFILE")!=nullptr;
    std::filesystem::path root;
#if defined(__linux__) && defined(__x86_64__)
    struct sigaction previous{};
#endif
public:
    explicit HostProfile(const std::filesystem::path& output):root(output) {
        if (!enabled) return;
#if defined(__linux__) && defined(__x86_64__)
        itimerval old{};
        if (getitimer(ITIMER_PROF,&old) || old.it_value.tv_sec || old.it_value.tv_usec)
            throw std::runtime_error("CPU profiling timer already active");
        struct sigaction action{};
        action.sa_sigaction=hostProfile::sample;
        action.sa_flags=SA_SIGINFO|SA_RESTART;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGPROF,&action,&previous)) throw std::runtime_error("cannot install sampler");
        hostProfile::count=0;
        hostProfile::dropped=0;
        std::ifstream maps("/proc/self/maps");
        std::ofstream saved(root/"host-maps.txt"); saved << maps.rdbuf();
        itimerval timer{}; timer.it_interval.tv_usec=1000; timer.it_value.tv_usec=1000;
        if (setitimer(ITIMER_PROF,&timer,nullptr)) {
            sigaction(SIGPROF,&previous,nullptr);
            throw std::runtime_error("cannot start sampler");
        }
#else
        throw std::runtime_error("host profiler requires Linux/x86-64");
#endif
    }
    void Stop() {
        if (!enabled) return;
        enabled=false;
#if defined(__linux__) && defined(__x86_64__)
        // Drain a pending sample before restoring the previous signal action.
        sigset_t blocked, oldMask;
        sigemptyset(&blocked); sigaddset(&blocked,SIGPROF);
        sigprocmask(SIG_BLOCK,&blocked,&oldMask);
        itimerval timer{}; setitimer(ITIMER_PROF,&timer,nullptr);
        timespec noWait{};
        while (sigtimedwait(&blocked,nullptr,&noWait)>=0) {}
        sigaction(SIGPROF,&previous,nullptr);
        sigprocmask(SIG_SETMASK,&oldMask,nullptr);
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void*>(hostProfile::sample),&info))
            throw std::runtime_error("cannot locate benchmark executable base");
        std::map<uintptr_t,size_t> counts;
        for (sig_atomic_t i=0;i<hostProfile::count;++i) ++counts[hostProfile::samples[i]];
        std::ofstream out(root/"host-pc.csv");
        out << "# executable_base=" << std::hex << uintptr_t(info.dli_fbase) << std::dec
            << " samples=" << hostProfile::count << " dropped=" << hostProfile::dropped << "\n";
        out << "pc,executable_relative_pc,count\n";
        for (const auto& item:counts)
            out << std::hex << item.first << ',' << item.first-uintptr_t(info.dli_fbase) << std::dec << ',' << item.second << '\n';
        out.flush(); if (!out) throw std::runtime_error("cannot write host profile");
#endif
    }
    ~HostProfile() { if (enabled) { try { Stop(); } catch (...) {} } }
};
#endif
