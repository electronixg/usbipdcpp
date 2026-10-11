#include "usbipdcpp/Server.h"

#include <optional>
#include <thread>
#include <chrono>
#include <iostream>
#include <csignal>

#include <asio/ip/tcp.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/Session.h"

usbipdcpp::Server::Server(const ServerNetworkConfig &network_config) :
    network_config(std::move(network_config)) {
}

usbipdcpp::Server::Server(std::vector<UsbDevice> &&devices, ServerNetworkConfig network_config) :
    network_config(std::move(network_config)) {
    available_devices.reserve(devices.size());
    for (auto &device: devices) {
        available_devices.emplace_back(std::make_shared<UsbDevice>(std::move(device)));
    }
}

usbipdcpp::error_code usbipdcpp::Server::start(const asio::ip::tcp::endpoint &ep) {
    // Idempotent safeguard: return success immediately if already running (normal usage should avoid start after start)
    if (running) {
        return {};
    }

#if defined(SIGPIPE) && !defined(USBIPDCPP_DISABLE_SIGNAL)
    // Sending after the peer closes the connection triggers SIGPIPE (which terminates the process by default); the server should ignore it:
    // write failures are handled uniformly through the error_code path. asio socket sends already use MSG_NOSIGNAL;
    // explicitly ignoring the signal here provides protection across versions (repeated calls are harmless).
    // On platforms such as ESP-IDF, libc declares <csignal> but provides no implementation; define
    // USBIPDCPP_DISABLE_SIGNAL (or enable the USBIPDCPP_DISABLE_SIGNAL option) to skip it
    std::signal(SIGPIPE, SIG_IGN);
#endif

    running = true;

    // acceptor is a long-lived member: start() reinitializes it with open/bind/listen. This runs synchronously on the thread
    // calling start(): stop() has joined the network thread and closed it, so there is no concurrency here. Failures (e.g. the port
    // being in use) return an error for the caller to handle and clean up partially initialized state: if bind/listen fails after open,
    // acceptor remains open; failing to close it makes open fail on the next start()
    // (it cannot be opened twice), preventing service recovery
    asio::error_code ec;
    acceptor.open(ep.protocol(), ec);
    if (!ec) {
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    }
    if (!ec) {
        acceptor.bind(ep, ec);
    }
    if (!ec) {
        acceptor.listen(asio::socket_base::max_listen_connections, ec);
    }
    if (ec) {
        running = false;
        asio::error_code ignored;
        acceptor.close(ignored);
        return ec;
    }
    // Record the actual listening endpoint (assigned by the system for port 0), used by endpoint() and tests.
    // The network thread has not started yet, so local_endpoint queries have no concurrency here; afterward endpoint() only reads
    // actual_endpoint and no longer touches acceptor (concurrent local_endpoint and the network thread's
    // async_accept on a shared object are not covered by asio's thread-safety guarantee)
    {
        asio::error_code ep_ec;
        actual_endpoint = acceptor.local_endpoint(ep_ec);
        if (ep_ec) {
            running = false;
            asio::error_code ignored;
            acceptor.close(ignored);
            return ep_ec;
        }
    }
    spdlog::info("Listening on {}:{}", actual_endpoint.address().to_string(), actual_endpoint.port());

    // Reuse io_context: after run() returns in the previous stop(), it is stopped and must be
    // restarted to run again (otherwise this run() returns immediately and the coroutine never executes)
    asio_io_context.restart();

    if (before_thread_create_callback) {
        before_thread_create_callback(ThreadPurpose::NetworkIO);
    }
    // Network thread: schedule the accept coroutine and run io_context (connection event completion handlers and coroutine
    // resumption all execute on this thread). After stop() cancels the coroutine, there is no pending work,
    // run() returns, and the thread exits
    try {
        network_io_thread = std::thread([this] {
            asio::co_spawn(asio_io_context, accept_loop(), asio::detached);
            // run() propagates scheduler-level exceptions (resource errors such as failure to recreate the interrupter pipe) to
            // the calling thread. The catch inside the coroutine does not cover this layer; running without a guard lets exceptions escape the thread
            // function and directly cause std::terminate. asio guarantees that io_context can be
            // run() again after run() throws: catch, log, briefly back off, and retry; the service recovers when resources recover.
            // During stop(), run() returns normally and break exits the loop
            while (true) {
                try {
                    asio_io_context.run();
                    break;
                } catch (const std::exception &e) {
                    SPDLOG_ERROR("Network thread io_context exception; reentering the run loop after 1s: {}", e.what());
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                } catch (...) {
                    SPDLOG_ERROR("Unknown network thread io_context exception; reentering the run loop after 1s");
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        });
    } catch (...) {
        // If thread creation fails (e.g. insufficient system resources), restore the stopped state and close acceptor,
        // avoiding an intermediate state marked as running without a network thread. The after callback must still be
        // called (nullptr indicates creation failure), ensuring before/after are paired
        running = false;
        asio::error_code ignored;
        acceptor.close(ignored);
        if (after_thread_create_callback) {
            after_thread_create_callback(ThreadPurpose::NetworkIO, nullptr);
        }
        return std::make_error_code(std::errc::resource_unavailable_try_again);
    }
    if (after_thread_create_callback) {
        // Exceptions from user callbacks are not caught here: if the user's own code throws, what the user intended to do
        // may already have failed. Exceptions propagate along the existing path (start throws to the caller); the library does not
        // clean up after user callbacks
        after_thread_create_callback(ThreadPurpose::NetworkIO, &network_io_thread);
    }
    return {};
}

asio::ip::tcp::endpoint usbipdcpp::Server::endpoint() const {
    // Return the endpoint recorded by start() directly: do not touch acceptor while running, avoiding concurrency with the network
    // thread's async_accept (concurrency on shared objects is not guaranteed by asio). After stop(),
    // it is cleared (meaning not listening)
    return actual_endpoint;
}

void usbipdcpp::Server::stop() {
    // Atomically set the stop flag: this does not block atomic reads/writes in start()/accept_loop.
    if (!running.exchange(false)) {
        // Safeguard: even if never run, ensure no joinable thread remains.
        if (network_io_thread.joinable()) {
            network_io_thread.join();
        }
        return;
    }

    // Cancel the pending asynchronous accept under the lock (async_accept is an asynchronous operation for which asio supports cross-thread
    // cancellation, and cancel() is its official cancellation interface). After the completion handler receives operation_aborted,
    // the coroutine checks running and exits. The coroutine may miss this cancellation in the window
    // between accept completion and reregistration; io_context_.stop() below is the fallback.
    {
        std::lock_guard lock(acceptor_mutex);
        asio::error_code ignored;
        acceptor.cancel(ignored);
    }

    // Fallback: even if cancel occurs during the coroutine's registration window (with no pending accept to cancel),
    // make the network thread's run() return immediately so join does not wait forever.
    // Remaining coroutines are cleaned up on the next start() (run after restart) or when io_context is destroyed.
    asio_io_context.stop();

    if (network_io_thread.joinable()) {
        network_io_thread.join();
    }

    // The network thread has exited, so closing acceptor here has no concurrency and is safe.
    asio::error_code ignore_ec;
    acceptor.close(ignore_ec);
    actual_endpoint = {};  // The port is no longer valid; endpoint() returns empty (meaning not listening)

    // Finish remaining accept coroutines inside stop(): close cancels pending asynchronous accepts and posts
    // completion events, but io_context_.stop() above has already made run() return, leaving events queued.
    // Left unattended, the coroutine frame would keep holding the Session (shared_ptr) it created until the next
    // start() or until io_context is destroyed during Server destruction; by then the Server accessed by the session destruction callback
    // may already be destroyed (use-after-free). restart followed by poll processes these completion events;
    // the resumed coroutine checks that running is false and co_returns, destroying its frame and releasing the session reference.
    // One poll() is sufficient (based on the source): all asio backends synchronously post completion events when cancelling/closing
    // pending operations; win_iocp cancel/close posts completion packets to the IOCP queue
    // (ready as soon as posted); Linux epoll cancel_ops / deregister_descriptor
    // (epoll_reactor.ipp) use scheduler_.post_deferred_completions to synchronously put
    // operation_aborted completion events in the scheduler queue, without depending on an epoll_wait wakeup.
    // Thus poll() after close necessarily processes all cancellation completion events. A resumed coroutine receiving
    // operation_aborted necessarily co_returns and cannot coexist with a new coroutine from the next start().
    asio_io_context.restart();
    asio_io_context.poll();

    // Snapshot currently live sessions: successfully locking a weak_ptr means the session is still alive and must be stopped together with the others
    std::vector<std::shared_ptr<Session>> conns;
    {
        std::lock_guard lock(session_list_mutex);
        conns.reserve(sessions.size());
        for (auto &[session_id, weak_session]: sessions) {
            if (auto session = weak_session.lock()) {
                conns.push_back(std::move(session));
            }
        }
    }

    // Request graceful stop for all live sessions (shutdown+cancel interrupt blocking reads)
    for (auto &session: conns) {
        session->immediately_stop();
    }
    spdlog::info("Wait for all sessions to close");
    // Release references: session threads exit after interruption, and releasing the last reference causes self-destruction
    conns.clear();

    // Wait for the live session count to reach zero: accept has stopped and no new sessions can be created, so the count can only decrease
    // monotonically to 0. This means all created sessions' destructor bodies have finished executing (the callback at the end of the destructor
    // decrements it; see the active_sessions comment in Server.h). Waiting is independent of the sessions snapshot:
    // sessions in the window after remove_session but before destruction completes are not missed.
    {
        std::unique_lock lock(session_list_mutex);
        if (!reap_cv.wait_for(lock, std::chrono::seconds(10), [&] {
                return active_sessions.load() == 0;
            })) {
            // Diagnostic: failure to reach zero within 10 seconds indicates a leaked session reference or a blocked thread that has not exited. Print
            // the current state and continue waiting; under heavy load, session reclamation may take over 10 seconds, and forced termination would kill
            // a normal slow path. Only leave a diagnostic record here without interrupting (use with the crash handler and logs for investigation)
            SPDLOG_ERROR("stop() timed out waiting for session destruction ({} active sessions remaining); continuing to wait",
                         active_sessions.load());
            reap_cv.wait(lock, [&] { return active_sessions.load() == 0; });
        }
    }

    // Clear session records (self-destroyed sessions' records were already removed by remove_session before destruction)
    {
        std::lock_guard lock(session_list_mutex);
        sessions.clear();
    }
    spdlog::info("All sessions were successfully closed");
}

usbipdcpp::AddDeviceStatus usbipdcpp::Server::add_device(std::shared_ptr<UsbDevice> device) {
    const std::string busid = device->busid;
    {
        // Duplicate checking and insertion must occur under the same lock: two concurrent threads could both observe an available busid and then
        // both add it, bypassing the check and creating duplicate entries with the same busid (import matches by busid,
        // and removal removes by busid, so duplicates make behavior indeterminate)
        std::lock_guard lock(devices_mutex);
        for (const auto &available: available_devices) {
            if (available->busid == busid) {
                return AddDeviceStatus::AlreadyBound;
            }
        }
        if (using_devices.contains(busid)) {
            return AddDeviceStatus::InUse;
        }
        available_devices.emplace_back(std::move(device));
    }
    // Notify outside the lock: callbacks can safely call Server query interfaces (see the ServerObserver comment)
    notify_device_added(busid);
    return AddDeviceStatus::Added;
}


bool usbipdcpp::Server::has_bound_device_locked(const std::string &busid) const {
    // As long as this device is stored, a device exists, whether it is in available devices or devices in use
    for (auto &device: available_devices) {
        if (device->busid == busid) {
            return true;
        }
    }
    return using_devices.contains(busid);
}

bool usbipdcpp::Server::has_bound_device(const std::string &busid) {
    std::shared_lock lock(devices_mutex);
    return has_bound_device_locked(busid);
}

size_t usbipdcpp::Server::get_session_count() {
    // Count only sessions still alive: disconnected sessions were already erased from the table by remove_session during
    // cleanup (expired weak_ptrs do not remain), meaning the current number of available connections
    std::lock_guard lock(session_list_mutex);
    return sessions.size();
}

void usbipdcpp::Server::print_bound_devices() {
    std::shared_lock lock(devices_mutex);

    std::size_t device_index = 1;
    std::cout << "available devices:" << std::endl;
    for (auto &device: available_devices) {
        std::cout << std::format("\tNo.{} device {}\n", device_index, device->busid);
        ++device_index;
    }
    std::cout << '\n';
    device_index = 1;
    std::cout << "using devices:" << std::endl;
    for (auto &device: using_devices) {
        std::cout << std::format("\tNo.{} device {}\n", device_index, device.first);
        ++device_index;
    }
    std::cout << std::endl;
}

void usbipdcpp::Server::add_observer(ServerObserver *observer) {
    if (!observer) {
        return;
    }
    std::lock_guard lock(observers_mutex);
    for (std::size_t i = 0; i < observer_count; ++i) {
        if (observers[i] == observer) {
            return; // Idempotent: keep only one copy of the same pointer
        }
    }
    if (observer_count >= MAX_OBSERVERS) {
        SPDLOG_ERROR("Observer count has reached the limit {}; ignoring this registration", MAX_OBSERVERS);
        return;
    }
    observers[observer_count++] = observer;
}

void usbipdcpp::Server::remove_observer(ServerObserver *observer) {
    std::lock_guard lock(observers_mutex);
    for (std::size_t i = 0; i < observer_count; ++i) {
        if (observers[i] == observer) {
            // Replace with the last element: observer order has no meaning
            observers[i] = observers[--observer_count];
            observers[observer_count] = nullptr;
            return;
        }
    }
}

std::size_t usbipdcpp::Server::observer_snapshot(std::array<ServerObserver *, MAX_OBSERVERS> &out) {
    std::lock_guard lock(observers_mutex);
    out = observers;
    return observer_count;
}

// bool usbipdcpp::Server::remove_device(const std::string &busid) {
//     std::lock_guard lock(devices_mutex);
//     for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
//         if ((*it)->busid == busid) {
//             available_devices.erase(it);
//             return true;
//         }
//     }
//     for (auto it = using_devices.begin(); it != using_devices.end(); ++it) {
//         if (it->first == busid) {
//             SPDLOG_ERROR("{} is being used and can't be removed");
//             return false;
//         }
//     }
//     SPDLOG_ERROR("Can't find device {}");
//     return false;
// }

usbipdcpp::Server::~Server() {
    // The destructor does not automatically stop: the caller manages the Server lifetime (see the Server.h class comment for the contract).
    // stop() must have been called explicitly before destruction; otherwise a running network thread remains joinable and causes
    // std::terminate, and live sessions access the already destroyed Server through callbacks during their destruction
    // (undefined behavior)
}


namespace {
// Notify observers in the snapshot in turn: an exception from one callback must not affect other observers or server threads
// (callbacks run on session/backend threads; an escaping exception causes std::terminate)
template <typename Snapshot, typename Fn>
void notify_each(const Snapshot &snapshot, std::size_t count, Fn &&fn) {
    for (std::size_t i = 0; i < count; ++i) {
        try {
            fn(snapshot[i]);
        } catch (const std::exception &e) {
            SPDLOG_ERROR("Observer callback exception: {}", e.what());
        } catch (...) {
            SPDLOG_ERROR("Unknown observer callback exception");
        }
    }
}
} // namespace

void usbipdcpp::Server::notify_session_started(std::uint64_t session_id, const std::string &peer) {
    std::array<ServerObserver *, MAX_OBSERVERS> snapshot{};
    const auto count = observer_snapshot(snapshot);
    notify_each(snapshot, count, [&](ServerObserver *observer) { observer->on_session_started(session_id, peer); });
}

void usbipdcpp::Server::notify_session_ended(std::uint64_t session_id) {
    std::array<ServerObserver *, MAX_OBSERVERS> snapshot{};
    const auto count = observer_snapshot(snapshot);
    notify_each(snapshot, count, [&](ServerObserver *observer) { observer->on_session_ended(session_id); });
}

void usbipdcpp::Server::notify_device_attached(const std::string &busid) {
    std::array<ServerObserver *, MAX_OBSERVERS> snapshot{};
    const auto count = observer_snapshot(snapshot);
    notify_each(snapshot, count, [&](ServerObserver *observer) { observer->on_device_attached(busid); });
}

void usbipdcpp::Server::notify_device_released(const std::string &busid, DeviceReleaseReason reason) {
    std::array<ServerObserver *, MAX_OBSERVERS> snapshot{};
    const auto count = observer_snapshot(snapshot);
    notify_each(snapshot, count, [&](ServerObserver *observer) { observer->on_device_released(busid, reason); });
}

void usbipdcpp::Server::notify_device_added(const std::string &busid) {
    std::array<ServerObserver *, MAX_OBSERVERS> snapshot{};
    const auto count = observer_snapshot(snapshot);
    notify_each(snapshot, count, [&](ServerObserver *observer) { observer->on_device_added(busid); });
}

void usbipdcpp::Server::notify_device_gone(const std::string &busid) {
    // Disappearing from the available list means the device was physically removed, with the same meaning as removal of a device in use
    notify_device_released(busid, DeviceReleaseReason::DeviceRemoved);
}

std::shared_ptr<usbipdcpp::UsbDevice> usbipdcpp::Server::detach_available_device(const std::string &busid) {
    std::shared_ptr<UsbDevice> detached;
    {
        std::lock_guard lock(devices_mutex);
        for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
            if ((*it)->busid == busid) {
                detached = std::move(*it);
                available_devices.erase(it);
                break;
            }
        }
    }
    if (detached) {
        // Notify outside the lock (callbacks can safely query Server); the device is already absent from the available list
        notify_device_gone(busid);
    }
    return detached;
}

void usbipdcpp::Server::remove_session(std::uint64_t id) {
    {
        // Called before session destruction (during session thread cleanup), removing its own weak_ptr record
        std::lock_guard lock(session_list_mutex);
        // Idempotent: process each id only once. The accept_loop fallback cleanup when session->run() throws and
        // the session thread's normal cleanup may both call this function (if after_thread_create_callback throws after
        // detach inside run, both paths reach
        // here). On the second call, erase returns 0, so skip notification to avoid
        // issuing on_session_ended twice
        if (sessions.erase(id) == 0) {
            return;
        }
    }
    // Notify outside the lock: callbacks can safely call Server query interfaces (see the ServerObserver comment)
    notify_session_ended(id);
}


void usbipdcpp::Server::set_connection_filter(std::function<bool(const asio::ip::tcp::endpoint &)> filter) {
    connection_filter = std::move(filter);
}

asio::awaitable<void> usbipdcpp::Server::accept_loop() {
    try {
        while (true) {
            if (!running) {
                co_return;
            }

            // Create the session first (it internally owns io_context and socket; see Session.h), then asynchronously accept
            // the connection into the session's socket. IDs are allocated atomically and increase monotonically (never repeated); during session cleanup/destruction,
            // call the server's methods directly to remove itself and notify completion of reclamation
            uint64_t id = next_session_id.fetch_add(1);
            auto session = std::make_shared<Session>(*this, id);
            // Count it as live immediately on creation: the session may never accept a connection (the coroutine is cancelled), but its destruction callback
            // still decrements the count, so stop() waiting for zero covers every created session
            active_sessions.fetch_add(1);

            // Check before registration to narrow the window where stop() cancellation is missed (ultimately covered by io_context_.stop()).
            // Registration occurs when co_await is evaluated (await_suspend), so checking and registration cannot be performed under a lock.
            if (!running) {
                co_return;
            }

            // Asynchronous accept: stop() cancels the pending accept across threads through acceptor_.cancel()
            // (asio's official asynchronous cancellation interface). While the coroutine is suspended, its frame holds session
            asio::error_code ec;
            co_await acceptor.async_accept(session->socket, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                // Transient errors (the peer resetting/aborting the connection before accept) are common with frequent connection/disconnection
                // at high concurrency; exiting immediately would silently stop the server accepting, so continue the loop.
                // For fatal errors (e.g. EMFILE resource exhaustion, or acceptor cancellation/closure), close on the same thread
                // to release the listening port immediately, then exit: without closing, the port remains occupied until stop(),
                // preventing another start() (regression tests verify this behavior). The close in stop() is an idempotent
                // fallback. This coroutine (the network thread) is acceptor's sole operator, so closing on the same thread is safe
                if (ec == asio::error::connection_reset ||
                    ec == asio::error::connection_aborted ||
                    ec == asio::error::interrupted ||
                    // macOS-specific: for connections the client closes after establishment but before accept executes
                    // (FIN/RST), XNU accept() returns EINVAL; Linux instead
                    // returns an already dead fd, and the subsequent read reports ECONNRESET. This is a frequent transient error
                    // when connecting and disconnecting rapidly; continue the loop, or accept_loop
                    // exits and the server silently stops accepting connections (macOS CI actually observed
                    // DisconnectRightAfterDevlistRequest and other tests failing here)
                    ec == asio::error::invalid_argument) {
                    continue;
                }
                // Use the same lock as stop() cancellation: while this thread closes, stop()
                // may be cancelling on the main thread (asio does not guarantee concurrent cross-thread close/cancel
                // safety; clang TSan actually reports a data race)
                std::error_code close_ec;
                {
                    std::lock_guard lock(acceptor_mutex);
                    acceptor.close(close_ec);
                }
                // Do not set running=false here: stop()'s full cleanup path (cancel/join/wait for
                // session destruction to reach zero/clear the session table) requires running to be true to take the main path. Setting
                // it false early here makes stop() return early and skip all cleanup, leaking live
                // sessions and leaving acceptor/port state behind, preventing a clean subsequent start()
                co_return;
            }

            // This may be a connection completed after stop() cancellation (a race); discard it and exit
            if (!running) {
                co_return;
            }

            // Set socket options
            std::error_code socket_opt_ec;
            if (network_config.socket_recv_buffer_size > 0) {
                session->socket.set_option(
                        asio::socket_base::receive_buffer_size(network_config.socket_recv_buffer_size),
                        socket_opt_ec);
                if (socket_opt_ec) {
                    SPDLOG_WARN("Failed to set receive buffer size: {}", socket_opt_ec.message());
                }
            }
            if (network_config.socket_send_buffer_size > 0) {
                session->socket.set_option(
                        asio::socket_base::send_buffer_size(network_config.socket_send_buffer_size),
                        socket_opt_ec);
                if (socket_opt_ec) {
                    SPDLOG_WARN("Failed to set send buffer size: {}", socket_opt_ec.message());
                }
            }
            if (network_config.tcp_no_delay) {
                session->socket.set_option(asio::ip::tcp::no_delay(true), socket_opt_ec);
                if (socket_opt_ec) {
                    SPDLOG_WARN("Failed to set TCP no_delay: {}", socket_opt_ec.message());
                }
            }

            // Get the peer address using the ec overload: the client may disconnect after accept completes but before the coroutine continues
            // (especially with RST), making remote_endpoint() throw. Uncaught exceptions in detached
            // coroutines cause std::terminate, so this must not throw (use the ec
            // overload); if the connection is dead, discard session and continue the loop
            asio::error_code remote_ec;
            auto remote_endpoint = session->socket.remote_endpoint(remote_ec);
            if (remote_ec) {
                SPDLOG_DEBUG("Connection closed immediately after accept: {}", remote_ec.message());
                continue;
            }
            auto remote_endpoint_name = std::format("{}:{}", remote_endpoint.address().to_string(),
                                                    remote_endpoint.port());

            // Connection filter (source admission control): the last check before session registration. Rejected connections
            // continue directly; session is destroyed when leaving scope (closing socket, active_sessions
            // increments/decrements remain balanced), without entering the connection table or issuing on_session_started.
            // Treat filter exceptions as rejection (fail-closed): installing a filter means admission control is
            // required, so allowing a connection on exception would silently bypass the check.
            // The filter is called serially on the network thread: accept_loop is the only call site, at most once per iteration
            if (connection_filter) {
                bool allowed = false;
                try {
                    allowed = connection_filter(remote_endpoint);
                } catch (const std::exception &e) {
                    SPDLOG_ERROR("Connection filter threw an exception; treating as rejection: {}", e.what());
                } catch (...) {
                    SPDLOG_ERROR("Connection filter threw an unknown exception; treating as rejection");
                }
                if (!allowed) {
                    SPDLOG_WARN("Connection rejected: {}", remote_endpoint_name);
                    continue;
                }
            }

            spdlog::info("A new connection from {}", remote_endpoint_name);

            try {
                std::lock_guard lock(session_list_mutex);
                // Store only weak_ptr: Session manages its own lifetime; Server holds no reference
                sessions.emplace(id, session);
            } catch (const std::exception &e) {
                // If insertion into the connection table fails (e.g. out of memory), abandon the session: active_sessions was already
                // incremented on creation, and session is destroyed when leaving scope (the destruction callback decrements it), keeping the count
                // balanced. Do not throw into the detached coroutine (which would terminate the accept loop); continue accepting
                SPDLOG_ERROR("Failed to insert into the session table: {}", e.what());
                continue;
            } catch (...) {
                SPDLOG_ERROR("Unknown exception inserting into the session table");
                continue;
            }

            // Notify outside the lock (the lock_guard scope above has ended): callbacks can safely
            // call Server query interfaces. The session is now registered in the connection table, strictly paired with
            // remove_session's on_session_ended; the run() startup failure path also removes the session,
            // so started must be issued before run() (not after run() succeeds)
            notify_session_started(id, remote_endpoint_name);

            //The function returns directly, but obtains its own shared_ptr internally, so it will not be destroyed
            //Start one thread per session to prevent operations that must block from affecting other devices.
            try {
                session->run();
            } catch (const std::exception &e) {
                // If main thread creation fails (e.g. insufficient system resources) or the user's after callback throws:
                // the session has no thread and cannot clean itself up; remove its connection record and abandon it
                // (session is destroyed when leaving scope, and its destruction callback decrements active_sessions,
                // keeping the count balanced). Do not rethrow: an uncaught exception in a detached coroutine would
                // terminate the accept coroutine and silently stop the server accepting connections;
                // continue the accept loop here so service resumes when resources recover
                SPDLOG_ERROR("Exception starting session: {}", e.what());
                remove_session(id);
                continue;
            } catch (...) {
                SPDLOG_ERROR("Unknown exception starting session");
                remove_session(id);
                continue;
            }
        }
    } catch (const std::exception &e) {
        // Uncaught exceptions in detached coroutines already cause std::terminate; crash when a crash is required
        // (only unrecoverable exceptions such as bad_alloc are possible here, so continuing is meaningless).
        // Catch only to log the current state, then rethrow to preserve termination behavior
        SPDLOG_ERROR("accept_loop exception: {}", e.what());
        throw;
    } catch (...) {
        SPDLOG_ERROR("Unknown accept_loop exception");
        throw;
    }
}

bool usbipdcpp::Server::is_device_using(const std::string &busid) {
    std::shared_lock lock(devices_mutex);
    return using_devices.contains(busid);
}

void usbipdcpp::Server::release_device(const std::string &busid) {
    print_devices();
    SPDLOG_DEBUG("Releasing device {}", busid);
    // Notify outside the lock: callbacks can safely call Server query interfaces (see the ServerObserver comment);
    // if the device is not in the in-use list, no release occurred and no notification is issued (reason remains empty)
    std::optional<DeviceReleaseReason> reason;
    {
        std::lock_guard lock(devices_mutex);
        // SPDLOG_TRACE("Successfully acquired both locks");

        auto ret = using_devices.find(busid);
        if (ret != using_devices.end()) {
            // Discard physically removed devices directly instead of returning them to the available list: the backend cleans up the
            // list state observed at the moment of removal, and no subsequent event will find this device. Returning it would create a zombie
            // that can never be removed (the old implementation relied on the session exit callback for fallback cleanup)
            if (ret->second->handler && ret->second->handler->is_device_removed()) {
                SPDLOG_INFO("Device {} was physically removed; not returning it to available devices", busid);
                using_devices.erase(ret);
                reason = DeviceReleaseReason::DeviceRemoved;
            }
            else {
                SPDLOG_INFO("Successfully moved {} to available devices", busid);
                auto &dev = ret->second;
                available_devices.emplace_back(std::move(dev));
                using_devices.erase(busid);
                // If running is already false, stop() is cleaning up, so this release is attributed to
                // server shutdown; otherwise it is attributed to client disconnection
                reason = running ? DeviceReleaseReason::ClientDisconnected : DeviceReleaseReason::ServerStopped;
            }
        }
        else {
            SPDLOG_WARN("Cannot find device with busid {}", busid);
        }
    }
    if (reason) {
        notify_device_released(busid, *reason);
    }
}

std::shared_ptr<usbipdcpp::UsbDevice> usbipdcpp::Server::try_moving_device_to_using(const std::string &wanted_busid) {
    std::lock_guard lock(devices_mutex);
    //Find an available device
    for (auto i = available_devices.begin(); i != available_devices.end(); ++i) {
        //Found device
        if (wanted_busid == (*i)->busid) {
            SPDLOG_INFO("Moving {} to devices in use", wanted_busid);
            //Move the wanted device to devices in use
            auto ret = (using_devices[wanted_busid] = std::move(*i));
            //Remove this device from available devices
            available_devices.erase(i);
            return ret;
        }
    }
    SPDLOG_WARN("Cannot find device with busid {}", wanted_busid);
    return nullptr;
}

void usbipdcpp::Server::print_devices() {
    std::shared_lock guard(devices_mutex);
    spdlog::info("There are {} available devices", available_devices.size());
    spdlog::info("There are {} devices in use, listed below", using_devices.size());
    for (auto &dev: using_devices) {
        spdlog::info("{}", dev.first);
    }
}
