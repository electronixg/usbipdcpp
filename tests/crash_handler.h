#pragma once

// Register crash/exception handlers at process startup; print the current thread's call stack on a crash to help CI locate
// intermittent crash sites (on Windows, gtest's default crash stack is empty; in Linux Release builds without symbols,
// only addresses are available). Print stack addresses and symbolize them offline using addr2line (Linux) / pdb (Windows).
// Hangs (without a crash) are not covered here; ctest --timeout identifies the test name.
//
// Usage: test .cpp files include this file indirectly via test_utils.h; each test is a separate process
// (gtest_discover_tests), with registration once during static initialization (before main).

#if defined(_WIN32)
#include <windows.h>
#include <cstdio>

inline LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    std::fprintf(stderr, "\n=== CRASH: exception code 0x%08lx ===\n",
                 ep->ExceptionRecord->ExceptionCode);
    void *stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);
    for (USHORT i = 0; i < frames; ++i) {
        std::fprintf(stderr, "  [%u] %p\n", i, stack[i]);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER; // Let the system default handler handle it
}

inline int install_crash_handler() {
    SetUnhandledExceptionFilter(crash_filter);
    return 0;
}

#elif defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <cstdio>
#include <cstdlib>

// Collect return addresses using _Unwind_Backtrace (Itanium C++ ABI, supplied by the GCC/Clang compiler runtime,
// available in glibc/bionic/musl without API gating), then resolve them with dladdr into module + offset within the file
// (in libc on glibc 2.34+/bionic/macOS, without API gating), ready to feed directly to addr2line
#include <dlfcn.h>
#include <unwind.h>

namespace {

/// _Unwind_Backtrace callback: collect return addresses in buffer
struct unwind_state {
    void **buffer;
    int count;
    int max;
};

_Unwind_Reason_Code unwind_callback(struct _Unwind_Context *ctx, void *arg) {
    auto *state = static_cast<unwind_state *>(arg);
    if (state->count < state->max) {
        state->buffer[state->count++] = reinterpret_cast<void *>(_Unwind_GetIP(ctx));
    }
    return _URC_NO_REASON;
}

/// Print one frame: prefer dladdr resolution (module + symbol name + offset), falling back to the raw address on failure
void print_frame(int index, void *addr) {
    Dl_info info;
    if (dladdr(addr, &info) != 0 && info.dli_fname != nullptr) {
        if (info.dli_sname != nullptr) {
            std::fprintf(stderr, "  [%d] %s(%s+0x%tx) [%p]\n", index, info.dli_fname, info.dli_sname,
                         static_cast<char *>(addr) - static_cast<char *>(info.dli_saddr), addr);
        }
        else {
            std::fprintf(stderr, "  [%d] %s(+0x%tx) [%p]\n", index, info.dli_fname,
                         static_cast<char *>(addr) - static_cast<char *>(info.dli_fbase), addr);
        }
    }
    else {
        std::fprintf(stderr, "  [%d] %p\n", index, addr);
    }
}

} // namespace

inline void crash_signal_handler(int sig) {
    std::fprintf(stderr, "\n=== CRASH: signal %d ===\n", sig);
    void *stack[64];
    unwind_state state{stack, 0, 64};
    _Unwind_Backtrace(unwind_callback, &state);
    for (int i = 0; i < state.count; ++i) {
        print_frame(i, stack[i]);
    }
    std::fflush(stderr);
    _exit(128 + sig);
}

inline int install_crash_handler() {
    std::signal(SIGSEGV, crash_signal_handler);
    std::signal(SIGABRT, crash_signal_handler);
    std::signal(SIGBUS, crash_signal_handler);
    return 0;
}

#else
// Other platforms (without _WIN32 / Unix macros): fall back to no registration; the system handles crashes by default
inline int install_crash_handler() {
    return 0;
}
#endif

// Register during static initialization (C++17 inline variable, executed once per process)
inline int g_crash_handler_installed = install_crash_handler();
