/* This file is part of the dynarmic project.
 * Copyright (c) 2019 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/exception_handler.h"

#ifdef __APPLE__
#    include <signal.h>
#    include <sys/ucontext.h>
#else
#    include <signal.h>
#    ifndef __OpenBSD__
#        include <ucontext.h>
#    endif
#endif

#if defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64)
#    include <dlfcn.h>
#    include <pthread.h>
#endif

#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <vector>

#include <mcl/assert.hpp>
#include <mcl/bit_cast.hpp>
#include <mcl/stdint.hpp>

#if defined(MCL_ARCHITECTURE_X86_64)
#    include "dynarmic/backend/x64/block_of_code.h"
#elif defined(MCL_ARCHITECTURE_ARM64)
#    include <oaknut/code_block.hpp>

#    include "dynarmic/backend/arm64/abi.h"
#elif defined(MCL_ARCHITECTURE_RISCV)
#    include "dynarmic/backend/riscv64/code_block.h"
#else
#    error "Invalid architecture"
#endif

namespace Dynarmic::Backend {

namespace {

struct CodeBlockInfo {
    u64 code_begin, code_end;
    std::function<FakeCall(u64)> cb;
};

#if defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64)
pthread_key_t active_code_key;
struct sigaction android_previous_segv;
decltype(&sigaction) android_sigaction = &sigaction;

class ThreadSignalStack {
public:
    ThreadSignalStack() {
        if (sigaltstack(nullptr, &previous) != 0) {
            fmt::print(stderr, "dynarmic: cannot query the thread signal stack\n");
            return;
        }
        if (!(previous.ss_flags & SS_DISABLE)) return;

        const size_t size = std::max<size_t>(SIGSTKSZ, 64 * 1024);
        auto allocation = std::unique_ptr<char[]>(new (std::nothrow) char[size]);
        stack_t stack{};
        stack.ss_sp = allocation.get();
        stack.ss_size = size;
        if (!allocation || sigaltstack(&stack, nullptr) != 0) {
            fmt::print(stderr, "dynarmic: cannot install the thread signal stack\n");
            return;
        }
        memory = std::move(allocation);
    }

    ~ThreadSignalStack() {
        if (memory && sigaltstack(&previous, nullptr) != 0)
            memory.release();  // The kernel may still use it until the thread exits.
    }

private:
    stack_t previous{};
    std::unique_ptr<char[]> memory;
};
#endif

class SigHandler {
public:
    SigHandler();
    ~SigHandler();

#if !(defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64))
    void AddCodeBlock(CodeBlockInfo info);
    void RemoveCodeBlock(u64 host_pc);
#endif

    bool SupportsFastmem() const { return supports_fast_mem; }

private:
    bool supports_fast_mem = true;

#if !(defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64))
    auto FindCodeBlockInfo(u64 host_pc) {
        return std::find_if(code_block_infos.begin(), code_block_infos.end(), [&](const auto& x) { return x.code_begin <= host_pc && x.code_end > host_pc; });
    }

    void* signal_stack_memory = nullptr;

    std::vector<CodeBlockInfo> code_block_infos;
    std::mutex code_block_infos_mutex;

    struct sigaction old_sa_segv;
    struct sigaction old_sa_bus;
#endif

    static void SigAction(int sig, siginfo_t* info, void* raw_context);
};

std::mutex handler_lock;
std::optional<SigHandler> sig_handler;

void RegisterHandler() {
    std::lock_guard<std::mutex> guard(handler_lock);
    if (!sig_handler) {
        sig_handler.emplace();
    }
}

SigHandler::SigHandler() {
#if defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64)
    if (pthread_key_create(&active_code_key, nullptr) != 0) {
        supports_fast_mem = false;
        return;
    }

    // app_process exports ART's sigaction wrapper. Use its user slot so ART's
    // special handlers retain priority and debuggerd still sees unhandled faults.
    if (void* executable = dlopen(nullptr, RTLD_NOW)) {
        if (const auto wrapper = dlsym(executable, "sigaction"))
            android_sigaction = reinterpret_cast<decltype(android_sigaction)>(wrapper);
        dlclose(executable);
    }

    struct sigaction sa{};
    sa.sa_sigaction = &SigHandler::SigAction;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    if (android_sigaction(SIGSEGV, nullptr, &android_previous_segv) != 0) {
        fmt::print(stderr, "dynarmic: cannot read the Android SIGSEGV handler\n");
        supports_fast_mem = false;
        return;
    }
    sa.sa_mask = android_previous_segv.sa_mask;
    sa.sa_flags |= android_previous_segv.sa_flags & SA_NODEFER;
    if (android_sigaction(SIGSEGV, &sa, nullptr) != 0) {
        fmt::print(stderr, "dynarmic: cannot install the Android fastmem signal handler\n");
        supports_fast_mem = false;
    } else if (std::getenv("FROND_JIT_STATS")) {
        Dl_info where{};
        if (dladdr(reinterpret_cast<void*>(android_sigaction), &where) && where.dli_fname)
            fmt::print(stderr, "dynarmic: Android fastmem sigaction from {}\n", where.dli_fname);
    }
#else
    const size_t signal_stack_size = std::max<size_t>(SIGSTKSZ, 2 * 1024 * 1024);

    signal_stack_memory = std::malloc(signal_stack_size);

    stack_t signal_stack;
    signal_stack.ss_sp = signal_stack_memory;
    signal_stack.ss_size = signal_stack_size;
    signal_stack.ss_flags = 0;
    if (sigaltstack(&signal_stack, nullptr) != 0) {
        fmt::print(stderr, "dynarmic: POSIX SigHandler: init failure at sigaltstack\n");
        supports_fast_mem = false;
        return;
    }

    struct sigaction sa;
    sa.sa_handler = nullptr;
    sa.sa_sigaction = &SigHandler::SigAction;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &old_sa_segv) != 0) {
        fmt::print(stderr, "dynarmic: POSIX SigHandler: could not set SIGSEGV handler\n");
        supports_fast_mem = false;
        return;
    }
#ifdef __APPLE__
    if (sigaction(SIGBUS, &sa, &old_sa_bus) != 0) {
        fmt::print(stderr, "dynarmic: POSIX SigHandler: could not set SIGBUS handler\n");
        supports_fast_mem = false;
        return;
    }
#endif
#endif
}

SigHandler::~SigHandler() {
#if !(defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64))
    std::free(signal_stack_memory);
#endif
}

#if !(defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64))
void SigHandler::AddCodeBlock(CodeBlockInfo cbi) {
    std::lock_guard<std::mutex> guard(code_block_infos_mutex);
    if (auto iter = FindCodeBlockInfo(cbi.code_begin); iter != code_block_infos.end()) {
        code_block_infos.erase(iter);
    }
    code_block_infos.push_back(cbi);
}

void SigHandler::RemoveCodeBlock(u64 host_pc) {
    std::lock_guard<std::mutex> guard(code_block_infos_mutex);
    const auto iter = FindCodeBlockInfo(host_pc);
    if (iter == code_block_infos.end()) {
        return;
    }
    code_block_infos.erase(iter);
}
#endif

void SigHandler::SigAction(int sig, siginfo_t* info, void* raw_context) {
#if defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64)
    const auto* code = static_cast<const CodeBlockInfo*>(pthread_getspecific(active_code_key));
    auto& pc = static_cast<ucontext_t*>(raw_context)->uc_mcontext.pc;
    if (info->si_code > 0 && code && code->code_begin <= pc && pc < code->code_end) {
        if (const auto call = code->cb(pc); call.call_pc) {
            pc = call.call_pc;
            return;
        }
    }
    const auto& old = android_previous_segv;
    if (old.sa_handler == SIG_DFL) {
        android_sigaction(sig, &old, nullptr);
        if (info->si_code <= 0) raise(sig);
    } else if (old.sa_handler != SIG_IGN) {
        if (old.sa_flags & SA_SIGINFO)
            old.sa_sigaction(sig, info, raw_context);
        else
            old.sa_handler(sig);
    }
#else
    ASSERT(sig == SIGSEGV || sig == SIGBUS);

#ifndef MCL_ARCHITECTURE_RISCV
    ucontext_t* ucontext = reinterpret_cast<ucontext_t*>(raw_context);
#    ifndef __OpenBSD__
    auto& mctx = ucontext->uc_mcontext;
#    endif
#endif

#if defined(MCL_ARCHITECTURE_X86_64)

#    if defined(__APPLE__)
#        define CTX_RIP (mctx->__ss.__rip)
#        define CTX_RSP (mctx->__ss.__rsp)
#    elif defined(__linux__)
#        define CTX_RIP (mctx.gregs[REG_RIP])
#        define CTX_RSP (mctx.gregs[REG_RSP])
#    elif defined(__FreeBSD__)
#        define CTX_RIP (mctx.mc_rip)
#        define CTX_RSP (mctx.mc_rsp)
#    elif defined(__NetBSD__)
#        define CTX_RIP (mctx.__gregs[_REG_RIP])
#        define CTX_RSP (mctx.__gregs[_REG_RSP])
#    elif defined(__OpenBSD__)
#        define CTX_RIP (ucontext->sc_rip)
#        define CTX_RSP (ucontext->sc_rsp)
#    else
#        error "Unknown platform"
#    endif

    {
        std::lock_guard<std::mutex> guard(sig_handler->code_block_infos_mutex);

        const auto iter = sig_handler->FindCodeBlockInfo(CTX_RIP);
        if (iter != sig_handler->code_block_infos.end()) {
            FakeCall fc = iter->cb(CTX_RIP);

            CTX_RSP -= sizeof(u64);
            *mcl::bit_cast<u64*>(CTX_RSP) = fc.ret_rip;
            CTX_RIP = fc.call_rip;

            return;
        }
    }

    fmt::print(stderr, "Unhandled {} at rip {:#018x}\n", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", CTX_RIP);

#elif defined(MCL_ARCHITECTURE_ARM64)

#    if defined(__APPLE__)
#        define CTX_PC (mctx->__ss.__pc)
#        define CTX_SP (mctx->__ss.__sp)
#        define CTX_LR (mctx->__ss.__lr)
#        define CTX_X(i) (mctx->__ss.__x[i])
#        define CTX_Q(i) (mctx->__ns.__v[i])
#    elif defined(__linux__)
#        define CTX_PC (mctx.pc)
#        define CTX_SP (mctx.sp)
#        define CTX_LR (mctx.regs[30])
#        define CTX_X(i) (mctx.regs[i])
#        define CTX_Q(i) (fpctx->vregs[i])
    [[maybe_unused]] const auto fpctx = [&mctx] {
        _aarch64_ctx* header = (_aarch64_ctx*)&mctx.__reserved;
        while (header->magic != FPSIMD_MAGIC) {
            ASSERT(header->magic && header->size);
            header = (_aarch64_ctx*)((char*)header + header->size);
        }
        return (fpsimd_context*)header;
    }();
#    elif defined(__FreeBSD__)
#        define CTX_PC (mctx.mc_gpregs.gp_elr)
#        define CTX_SP (mctx.mc_gpregs.gp_sp)
#        define CTX_LR (mctx.mc_gpregs.gp_lr)
#        define CTX_X(i) (mctx.mc_gpregs.gp_x[i])
#        define CTX_Q(i) (mctx.mc_fpregs.fp_q[i])
#    elif defined(__NetBSD__)
#        define CTX_PC (mctx.mc_gpregs.gp_elr)
#        define CTX_SP (mctx.mc_gpregs.gp_sp)
#        define CTX_LR (mctx.mc_gpregs.gp_lr)
#        define CTX_X(i) (mctx.mc_gpregs.gp_x[i])
#        define CTX_Q(i) (mctx.mc_fpregs.fp_q[i])
#    elif defined(__OpenBSD__)
#        define CTX_PC (ucontext->sc_elr)
#        define CTX_SP (ucontext->sc_sp)
#        define CTX_LR (ucontext->sc_lr)
#        define CTX_X(i) (ucontext->sc_x[i])
#        define CTX_Q(i) (ucontext->sc_q[i])
#    else
#        error "Unknown platform"
#    endif

    {
        std::lock_guard<std::mutex> guard(sig_handler->code_block_infos_mutex);

        const auto iter = sig_handler->FindCodeBlockInfo(CTX_PC);
        if (iter != sig_handler->code_block_infos.end()) {
            FakeCall fc = iter->cb(CTX_PC);

            CTX_PC = fc.call_pc;

            return;
        }
    }

    fmt::print(stderr, "Unhandled {} at pc {:#018x}\n", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", CTX_PC);

#elif defined(MCL_ARCHITECTURE_RISCV)

    ASSERT_FALSE("Unimplemented");

#else

#    error "Invalid architecture"

#endif

    struct sigaction* retry_sa = sig == SIGSEGV ? &sig_handler->old_sa_segv : &sig_handler->old_sa_bus;
    if (retry_sa->sa_flags & SA_SIGINFO) {
        retry_sa->sa_sigaction(sig, info, raw_context);
        return;
    }
    if (retry_sa->sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL);
        return;
    }
    if (retry_sa->sa_handler == SIG_IGN) {
        return;
    }
    retry_sa->sa_handler(sig);
#endif
}

}  // anonymous namespace

struct ExceptionHandler::Impl final {
    Impl(u64 code_begin_, u64 code_end_)
            : code_begin(code_begin_)
            , code_end(code_end_) {
        RegisterHandler();
    }

    void SetCallback(std::function<FakeCall(u64)> cb) {
        CodeBlockInfo cbi;
        cbi.code_begin = code_begin;
        cbi.code_end = code_end;
        cbi.cb = cb;
#if defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64)
        code = std::move(cbi);
#else
        sig_handler->AddCodeBlock(cbi);
#endif
    }

    ~Impl() {
#if !(defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64))
        sig_handler->RemoveCodeBlock(code_begin);
#endif
    }

#if defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64)
    CodeBlockInfo code{};
#endif

private:
    u64 code_begin, code_end;
};

ExceptionHandler::ExceptionHandler() = default;
ExceptionHandler::~ExceptionHandler() = default;

#if defined(MCL_ARCHITECTURE_X86_64)
void ExceptionHandler::Register(X64::BlockOfCode& code) {
    const u64 code_begin = mcl::bit_cast<u64>(code.getCode());
    const u64 code_end = code_begin + code.GetTotalCodeSize();
    impl = std::make_unique<Impl>(code_begin, code_end);
}
#elif defined(MCL_ARCHITECTURE_ARM64)
void ExceptionHandler::Register(oaknut::CodeBlock& mem, std::size_t size) {
    const u64 code_begin = mcl::bit_cast<u64>(mem.ptr());
    const u64 code_end = code_begin + size;
    impl = std::make_unique<Impl>(code_begin, code_end);
}
#elif defined(MCL_ARCHITECTURE_RISCV)
void ExceptionHandler::Register(RV64::CodeBlock& mem, std::size_t size) {
    const u64 code_begin = mcl::bit_cast<u64>(mem.ptr<u64>());
    const u64 code_end = code_begin + size;
    impl = std::make_unique<Impl>(code_begin, code_end);
}
#else
#    error "Invalid architecture"
#endif

bool ExceptionHandler::SupportsFastmem() const noexcept {
    return static_cast<bool>(impl) && sig_handler->SupportsFastmem();
}

#if defined(__ANDROID__) && defined(MCL_ARCHITECTURE_ARM64)
void ExceptionHandler::PrepareThreadSignalStack() {
    thread_local ThreadSignalStack stack;
}

ExceptionHandler::ScopedExecution::ScopedExecution(const ExceptionHandler& handler, bool enabled) {
    if (!enabled || !handler.SupportsFastmem()) return;
    PrepareThreadSignalStack();
    previous = pthread_getspecific(active_code_key);
    active = pthread_setspecific(active_code_key, &handler.impl->code) == 0;
}

ExceptionHandler::ScopedExecution::~ScopedExecution() {
    if (active) pthread_setspecific(active_code_key, previous);
}
#endif

void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {
    impl->SetCallback(cb);
}

}  // namespace Dynarmic::Backend
