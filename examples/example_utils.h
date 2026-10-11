#pragma once

#include <csignal>
#include <cstdint>
#include <cxxopts.hpp>
#include <iostream>
#include <semaphore>
#include <string>

#if defined(_WIN32)
// Console event handling requires windows.h; WIN32_LEAN_AND_MEAN excludes winsock.h (which otherwise conflicts with
// asio's winsock2.h); NOMINMAX prevents min/max macros from breaking std::min/std::max.
// Guard both macros: asio's config.hpp defines NOMINMAX first (as 1); redefining it produces a warning.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/// Create cxxopts::Options with common parameters (--port, --busid, --help).
inline cxxopts::Options make_example_options(const std::string &name, const std::string &desc) {
    cxxopts::Options opts(name, desc);
    opts.add_options()
        ("p,port", "TCP port", cxxopts::value<std::uint16_t>()->default_value("53240"))
        ("b,busid", "Bus ID", cxxopts::value<std::string>()->default_value("1-1"))
        ("h,help", "Print help");
    return opts;
}

/// Standard parsing + error / help printing, ParseResult is returned successfully.
inline cxxopts::ParseResult parse_example_args(cxxopts::Options &opts, int argc, char **argv) {
    try {
        auto result = opts.parse(argc, argv);
        if (result.count("help")) {
            std::cout << opts.help() << std::endl;
            std::exit(0);
        }
        return result;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        std::cout << opts.help() << std::endl;
        std::exit(1);
    }
}

namespace {

// Exit semaphore: the handler signals it, and wait_for_exit blocks waiting for it.
// header file tool function uses anonymous namespace: one copy for each TU (only used in this TU, no cross-TU sharing).
// POSIX signal handler calls release(): libstdc++/libc++ implement it with atomics + futex,
// with no locks or allocations; in practice it is async-signal-safe (sem_post of sem_t is guaranteed by the standard
// equivalent); Windows console handler executes in a normal thread context and does not have this constraint
std::binary_semaphore g_wait_exit_sem(0);

#if defined(_WIN32)
/// Windows console event handler (Ctrl+C / Ctrl+Break / close window / log off / shutdown).
/// Return TRUE to indicate that it has been processed: the process will not terminate according to the default action, and the main thread will clean up and exit after being awakened by the semaphore.
BOOL WINAPI console_ctrl_handler(DWORD) {
    g_wait_exit_sem.release();
    return TRUE;
}
#else
/// POSIX signal processor (SIGINT / SIGTERM)
void exit_signal_handler(int) {
    g_wait_exit_sem.release();
}
#endif

} // namespace

/// Blocks and waits for the exit signal. After returning, the caller cleans up and exits (server.stop, etc.).
/// POSIX: Ctrl+C (SIGINT)/kill -TERM (SIGTERM) - background deployment directly
/// `./mock_xxx &`, exit `kill -TERM <pid>`, the server takes the normal cleanup path;
/// Windows: Console events (Ctrl+C / Ctrl+Break / close window, etc.) are also cleaned up gracefully.
/// Use handler + semaphore instead of sigwait: sigwait requires calling thread to block the signal first, and the server
/// connection threads do not block signals, so process signals would be handled there by the default action (immediate process termination), preventing cleanup
/// code from running; a process-wide handler avoids this problem
inline void wait_for_exit() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    g_wait_exit_sem.acquire();
#else
    struct sigaction sa {};
    sa.sa_handler = exit_signal_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    g_wait_exit_sem.acquire();
#endif
}
