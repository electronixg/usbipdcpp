#pragma once

#include <vector>
#include <map>
#include <array>
#include <shared_mutex>
#include <memory>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

#include <asio/ip/tcp.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>

#include "usbipdcpp/Device.h"


namespace usbipdcpp {
class Session;

/**
 * @brief thread purpose identifier, used for callback before thread creation
 */
enum class ThreadPurpose {
    NetworkIO,      // Server network I/O thread
    SessionMain,    // Session main thread
    SessionSender   // Session sender thread
};

/**
 * @brief server network configuration
 */
struct ServerNetworkConfig {
    /// socket receive buffer size (bytes), 0 means using the system default value
    std::size_t socket_recv_buffer_size = 128 * 1024;
    /// socket send buffer size (bytes), 0 means using the system default value
    std::size_t socket_send_buffer_size = 128 * 1024;
    /// Whether to disable Nagle algorithm (reduce small packet delay)
    bool tcp_no_delay = true;
};

/**
 * @brief Server::add_device results
 */
enum class AddDeviceStatus {
    Added,        ///< added successfully
    AlreadyBound, ///< busid is already in the available list (bound, not occupied)
    InUse         ///< busid is occupied by a session
};

/**
 * @brief The reason why the device was released (input parameter of ServerObserver::on_device_released)
 */
enum class DeviceReleaseReason {
    ClientDisconnected, ///< Client disconnected
    DeviceRemoved,      ///< The device disappears from the server (the backend detects physical unplugging, or the backend actively unbinds it)
    ServerStopped       ///< The server is stopped
};

/**
 * @brief Server state observer: inherit and override events of interest as needed (default no-op implementations for other events)
 *
 * @attention The observer must outlive Server, or unregister earlier by calling remove_observer()
 *
 * notification semantics (same for all events):
 *  - Called synchronously on the thread triggering the event: session events on the session teardown thread; device events on the thread triggering
 *    the change (the session thread for client import/disconnection, the backend thread for backend binding/unplugging,
 *    and the caller's thread when directly calling add_device / detach_available_device)
 *  - Device event pairing: entering the available list emits on_device_added; client import emits
 *    on_device_attached; leaving always emits on_device_released once: client disconnection
 *    (ClientDisconnected), server stopped (ServerStopped), physical removal and backend
 *    unbinding (DeviceRemoved) all use on_device_released, distinguished by reason
 *  - The notification is issued outside the lock: the server's query interface can be safely called in the callback, or
 *    add_observer / remove_observer
 *  - A window exists between notification and state change: for example, on_session_ended is emitted only after the session has been removed from the connection table
 *    (get_session_count reaches zero). Determine whether an event has occurred from receipt of
 *    the notification, rather than inferring it from Server state queries (which can observe the intermediate state where "state has changed but notification has not arrived"
 *    ).
 *  - The exception thrown by the callback is captured and logged by the library, and will not affect the server thread and other observers
 *  - The callback is on the closing critical path and should be returned quickly (please transfer heavy work to your own thread)
 */
class USBIPDCPP_API ServerObserver {
public:
    virtual ~ServerObserver() = default;

    /**
     * @brief New client connection established (session registered)
     * @param session_id session id
     * @param peer peer address, in the form of "ip:port"
     */
    virtual void on_session_started(std::uint64_t session_id, const std::string &peer) {
    }

    /**
     * @brief The session ends (the client disconnects or the server stops)
     * @param session_id session id
     */
    virtual void on_session_ended(std::uint64_t session_id) {
    }

    /**
     * @brief The device enters the available list (backend binding successful: hotplug insertion, binding at startup, user addition)
     * @param busid device busid
     *
     * @note This event will not be triggered when the device returns from "occupied" to available (for reasons such as client disconnection)——
     * That change is represented by on_device_released
     */
    virtual void on_device_added(const std::string &busid) {
    }

    /**
     * @brief The device is imported and occupied (the client successfully obtains the device)
     * @param busid device busid
     */
    virtual void on_device_attached(const std::string &busid) {
    }

    /**
     * @brief Device released: back to available list (client disconnected/server stopped), or discarded
     * (physical unplug/backend unbinding)
     * @param busid device busid
     * @param reason release reason
     */
    virtual void on_device_released(const std::string &busid, DeviceReleaseReason reason) {
    }
};

/**
 * @brief USB/IP Server
 *
 * @attention Thread safety summary:
 *   - Construction / start / stop / ~Server: life cycle method, must be called serially in the same thread
 *   - add_device / detach_available_device / has_bound_device / get_session_count /
 *     print_bound_devices / add_observer / remove_observer: internally locked, safe from any thread
 *   - get_available_devices / get_using_devices: No lock, the caller must hold get_devices_mutex()
 *   - get_devices_mutex: always safe, only returns mutex references
 *   - set_before_thread_create_callback / set_after_thread_create_callback / set_connection_filter:
 *     must be called before start()
 */
class USBIPDCPP_API Server final {
public:
    friend class Session;

    Server() = default;
    explicit Server(const ServerNetworkConfig &network_config);
    explicit Server(std::vector<UsbDevice> &&devices, ServerNetworkConfig network_config = {});
    Server(const Server &) = delete;
    Server(Server &&) = delete;
    /**
     * @brief Start a server without blocking (idempotent: return success directly when running), internal
     * starts the network thread to run the coroutine accept loop (async_accept). Called before and after start
     * add_device can be used.
     * @param ep listening address. If the port is 0 (assigned by the system), use it after start returns
     * endpoint() queries the actual listening endpoint (including the port allocated by the system).
     * @return An error is returned when the startup fails (such as the port is occupied), and there is no error when the startup is successful. No exception is thrown,
     * making this usable on embedded platforms with exception-free environments
     *
     * @attention Idempotence is only for defense. Normal usage should avoid starting again without stopping after start.
     * (already running returns immediately without listening again).
     * @thread_safety cannot be called concurrently. stop() can then be called again to restart.
     */
    usbipdcpp::error_code start(const asio::ip::tcp::endpoint &ep);
    /**
     * @brief The current actual listening endpoint. After start() starts with port 0 (assigned by the system),
     *        Use this function to query the actual endpoint (including address and port); if it is not started, an empty endpoint is returned.
     *
     * @thread_safety has no internal lock and should only be read after start() returns and before stop().
     */
    asio::ip::tcp::endpoint endpoint() const;

    /**
     * @brief Close the listener and wait for all sessions to finish self-destruction; after the call all Sessions have stopped
     *        has been destructed and it is safe to start() again or destroy the Server.
     *        Order: set the stop flag → cancel pending asynchronous accept under the lock (io_context.stop()
     *        as a fallback) → join the network thread → close acceptor to release the port → restart+poll
     *        to clear remaining accept coroutines → request stop for every session (shutdown+cancel interrupt
     *        blocking read) → release the snapshot reference and wait for the active session count to return to zero (all session destructors
     *        execution completed) → Clear the session list. This function must be called before destruction (destruction does not stop automatically,
     *        see class annotation contract).
     *
     * @attention Idempotent is only for defense (only cleans the remaining threads and returns when not running), normal usage should
     * Avoid stopping again without starting after stopping.
     * @thread_safety cannot be called concurrently. Called once after each start(); supports start→stop→start loop.
     */
    void stop();

    /**
     * @brief Add a device (checking for duplicate busid), thread-safe. May be called whether or not server has started
     * @param device The device to be added (passed by value: internal move into the device list, the caller
     *               decides by itself whether to pass lvalue copy or move transfer)
     * @return Added Added successfully; AlreadyBound The busid is already in the available list;
     *         InUse The busid is being occupied by a session (neither conflict is added)
     *
     * Duplicate checking and insertion complete atomically under the same lock (preventing two concurrent threads from both finding "available"
     * and both inserting); after success, emit on_device_added outside the lock. busid uniqueness is an invariant relied upon
     * internally by Server (import matches by busid, unplugging removes by busid; duplicates make
     * behavior indeterminate), so no "unconditional insertion" entry point exists.
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    AddDeviceStatus add_device(std::shared_ptr<UsbDevice> device);

    /**
     * @brief Check whether busid is already occupied (present in either available or in-use). Does not hold a lock internally;
     *        is for the caller who already holds get_devices_mutex() to check for duplicates to avoid repeated entry and exit of the lock.
     *
     * New code adding devices should use add_device() directly: it checks for duplicates and inserts under the same lock,
     * and is responsible for sending notifications outside the lock. This function is only used by the caller who needs to hold the lock to traverse the device list.
     * and also checks occupancy (a read lock suffices).
     *
     * While holding the lock, do not call add_device() / detach_available_device() or other interfaces that
     * acquire their own locks (shared_mutex is nonrecursive and will deadlock).
     *
     * @param busid busid to be queried
     * @return true Already occupied
     *
     * @thread_safety The caller must hold the read lock or write lock of get_devices_mutex().
     */
    bool has_bound_device_locked(const std::string &busid) const;

    /**
     * @brief Check whether busid is already occupied (present in either available or in-use)
     * @param busid busid to be queried
     * @return true Already occupied
     *
     * @thread_safety Internally locked, safe from any thread. If the caller already holds
     *                 get_devices_mutex(), use has_bound_device_locked instead
     *                 (This function will lock again, and calling it while holding the lock will cause a deadlock).
     */
    bool has_bound_device(const std::string &busid);

    /**
     * @thread_safety Internally locked, safe from any thread.
     */
    size_t get_session_count();

    /**
     * @thread_safety Internally locked, safe from any thread.
     */
    void print_bound_devices();

    /**
     * @brief Provides no thread safety; call get_devices_mutex yourself to acquire the lock
     * @return
     *
     * @thread_safety The caller must hold the read lock or write lock of get_devices_mutex().
     */
    [[nodiscard]] std::vector<std::shared_ptr<UsbDevice>> &get_available_devices() {
        return available_devices;
    }

    /**
     * @brief Provides no thread safety; call get_devices_mutex yourself to acquire the lock
     * @return
     *
     * @thread_safety The caller must hold the read lock or write lock of get_devices_mutex().
     */
    [[nodiscard]] std::map<std::string, std::shared_ptr<UsbDevice>> &get_using_devices() {
        return using_devices;
    }

    /**
     * @brief To operate device data, please call this function to obtain the lock and use it.
     * @return
     *
     * @thread_safety is always safe (only a reference is returned).
     */
    [[nodiscard]] std::shared_mutex &get_devices_mutex() const {
        return devices_mutex;
    }

    /**
     * @brief Register status observer (idempotent: only one is retained for repeated registration of the same pointer)
     * @param observer observer pointer; the library does not take over ownership, and the life cycle must cover Server
     *                 (call remove_observer to unregister earlier)
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    void add_observer(ServerObserver *observer);

    /**
     * @brief Unregister status observer (idempotent: unregistered pointer calls are harmless)
     * @param observer observer pointer
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    void remove_observer(ServerObserver *observer);

    /**
     * @brief Sets the callback before thread creation, which is used for embedded platforms to set thread core affinity, etc.
     * @param callback callback function, receives thread purpose identification
     * (pass by value: internal move into member, caller can pass lvalue/rvalue)
     *
     * @thread_safety must be called before start().
     */
    void set_before_thread_create_callback(std::function<void(ThreadPurpose)> callback) {
        before_thread_create_callback = std::move(callback);
    }

    /**
     * @brief Set the callback after thread creation, used to set the thread name, etc.
     * @param callback callback function, receives thread purpose identifier and thread pointer. pointer is nullptr
     * indicates that the thread creation failed (the before callback has been executed but the thread was not created successfully). It can be used when it is not empty.
     * Set the thread name, etc. (the pointer is valid within the thread life cycle)
     * (pass by value: internal move into member, caller can pass lvalue/rvalue)
     *
     * @thread_safety must be called before start().
     */
    void set_after_thread_create_callback(std::function<void(ThreadPurpose, std::thread*)> callback) {
        after_thread_create_callback = std::move(callback);
    }

    /**
     * @brief Set a connection filter (source admission control): called after accepting the client connection and before registering
     * the session; returning false rejects the connection (close socket, send no data, create no session,
     * does not send on_session_started notification, and when rejected, a WARN log containing the peer address is recorded)
     * @param filter Filter receiving the peer address (IP and source port); return true to allow,
     * false to reject. Passing an empty std::function clears the filter (allow all connections again)
     * (pass by value: internal move into member, caller can pass lvalue/rvalue)
     *
     * Typical use: Only allow clients from specific sources to access (such as authorization by source port in local loopback scenarios,
     * Embedded device limits allowed client IPs). When no filter is set, all are allowed and the default behavior remains unchanged.
     *
     * @attention The callback is called **serially** on the network thread (accept_loop is the only call site,
     * At most one callback is executing at the same time, no need to lock it yourself), and it is on the accept path——
     * should return quickly, please transfer heavy work to your own thread. The rejected connection has been accepted before the callback returns.
     * closes if rejected.
     * @attention The exception thrown by the callback is handled as "reject" and recorded as ERROR: if the filter is set, it means
     * admission control is required; allowing connections on exceptions would silently disable the check (deliberately different from ServerObserver's
     * "only log exceptions without affecting the main flow" semantics)
     * @attention must be called before start() (same discipline as thread creation callback),
     * Runtime replacement is not supported
     */
    void set_connection_filter(std::function<bool(const asio::ip::tcp::endpoint &)> filter);

    /**
     * @brief removes the specified session and notifies on_session_ended
     * @param id The id of the session to be removed
     *
     * @thread_safety is internally locked, but should only be called in the Session exit path.
     */
    void remove_session(std::uint64_t id);

    /**
     * @brief Remove a device from the available list and notify its disappearance: remove under the lock, then outside the lock
     * issues on_device_released(busid, DeviceRemoved)
     * @param busid device busid
     * @return The removed device; when the busid is not in the available list, nullptr is returned
     *
     * is used by the backend to handle physical unplugging/unbinding: after getting the return value, do the backend cleanup by yourself (such as libusb's
     * close/unref), the device has been removed, and there is no need to hold the lock during cleaning. The device is still occupied at the moment
     * (in the using list), return nullptr without notification: that path emits a notification via
     * release_device during session teardown.
     */
    std::shared_ptr<UsbDevice> detach_available_device(const std::string &busid);

    ~Server();

protected:
    /**
     * @brief Coroutine accept loop: scheduled on the network thread's io_context (co_spawn),
     * creates a session in each round and accepts connections asynchronously. Use stop() to cancel it with acceptor_.cancel()
     * (cancel is asio's official asynchronous cancellation interface), with io_context_.stop() as a fallback.
     * Transient error (the peer resets/aborts the connection before accept) continues to loop, fatal error (acceptor
     * was canceled/closed) exit is taken over by the running flag of stop().
     */
    asio::awaitable<void> accept_loop();

    /**
     * @brief Determine whether the device is being used (in using_devices)
     * @param busid device busid
     * @return true is in use (has been imported by a session)
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    bool is_device_using(const std::string &busid);

    /**
     * @brief Release the device: Move the device from using_devices back to available_devices
     *
     * Devices that have been physically removed by the backend (is_device_removed()) are directly discarded and are not put back into the available list:
     * The backend cleans up only the list state it finds at the moment the device disappears; no later event will scan for
     * remnants, so returning it would leave an unremovable zombie.
     *
     * @param busid busid of the device to be released
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    void release_device(const std::string &busid);

    /**
     * @brief Occupying the device: Move the device from available_devices to using_devices and return
     * @param busid device busid
     * @return device; returns nullptr when the device is not in the available list
     *
     * @thread_safety Internally locked, safe from any thread.
     */
    std::shared_ptr<UsbDevice> try_moving_device_to_using(const std::string &busid);

    void print_devices();

    ServerNetworkConfig network_config;

    // callback before thread creation
    std::function<void(ThreadPurpose)> before_thread_create_callback;
    // Callback after thread creation; argument is a thread pointer, nullptr means thread creation failed
    std::function<void(ThreadPurpose, std::thread*)> after_thread_create_callback;

    // connection filter: called after accept and before session registration, returns false to reject the connection.
    // requires setting before start() (see set_connection_filter comment), accept_loop
    // no longer locks when reading
    std::function<bool(const asio::ip::tcp::endpoint &)> connection_filter;

    // Session connection table: Server observes through weak_ptr only; each Session manages its own lifetime
    // (the session thread serves as the main thread, and the last reference is released when returning and is self-destructed).
    // weak_ptr allows stop() to lock surviving sessions to stop them, and to query their active count.
    // id is assigned atomically by next_session_id: monotonically increasing, never reset across start/stop,
    // ensures that no two clients will get the same ID.
    std::unordered_map<std::uint64_t, std::weak_ptr<Session>> sessions;
    std::atomic<std::uint64_t> next_session_id{1};
    // Live session count (atomic, no lock required): accept_loop +1 immediately after session creation,
    // The destruction notification callback at the end of the session destructor decrements it by 1. stop() waits for zero, meaning
    // "All sessions that have been created have been destroyed" - Decoupled from sessions snapshot:
    // Session window between remove_session and destruction completion (removed from the table but destruction is not complete)
    // will not be missed by the snapshot, nor will the wait condition be satisfied in advance by the destruction of other sessions.
    // cannot be judged by weak_ptr::expired() - expired is true at the beginning of the destructor.
    // At this time, the destructor is still executing. If stop() returns early and destructs the server, it will hit the still executing
    // destruction callback (it accesses the Server member, use-after-free).
    std::condition_variable reap_cv;
    std::atomic<std::size_t> active_sessions{0};
    mutable std::mutex session_list_mutex;

    // network stack adopts long-lived io_context + acceptor:
    // start() initializes acceptor by reopening/binding/listening; stop(), after joining the network thread,
    // closes it to release the port. The network thread runs the coroutine accept_loop (co_spawn on this io_context
    // and run()), the Session's socket holds its own io_context (see Session.h),
    // There is no transfer between io_context instances.
    // (used to use "whole generation rebuild" - rebuild io_context + acceptor each time start() - to handle the stop
    // leftover-state issues, combined with each Session owning its own io_context; on macOS this triggered cross-
    // io_context accept failures with EINVAL, so it was replaced with a long-lived io_context + coroutine.)
    asio::io_context asio_io_context;
    //listening acceptor is saved as a member and closed when stop() is executed to release the port, ensuring that it can be started() again.
    //access discipline (acceptor is an asio shared object, all operations must be in a single thread):
    // - open/bind/listen: start() main thread (the network thread has not been started yet, no concurrency)
    // - async_accept: network thread coroutine, the only operator
    // - cancel(stop()): main thread, called under acceptor_mutex_ (async_accept is
    //   an asynchronous operation supporting cross-thread cancellation; cancel() is its official cancellation interface)
    // - close: called under acceptor_mutex_ both on the stop() main thread (after join) and in accept_loop error branches (EMFILE and other
    //   fatal errors): close in the error branch and cancel in stop()
    //   may run concurrently, so the same lock provides mutual exclusion
    asio::ip::tcp::acceptor acceptor{asio_io_context};
    // protects the cancellation operation of the acceptor (stop() cancels the pending asynchronous accept within the lock).
    // The registration of  coroutine occurs when co_await is evaluated (await_suspend), and the lock cannot be held.
    // The reliability of  cancellation is guaranteed by stop() of io_context_.stop() (run() returns immediately).
    // running is an atomic variable, start/stop/accept_loop can be read and written independently without locking.
    mutable std::mutex acceptor_mutex;
    // stop flag: After stop() is set to false, the canceled/awakened accept_loop will exit after detection.
    // life cycle method requires serial calls, atomic reading and writing are enough, no additional mutual exclusion is needed
    std::atomic_bool running = false;
    // start() records the actual listening endpoint (system-assigned for port 0); endpoint() reads it directly.
    // Cleared after stop() (meaning not listening)
    asio::ip::tcp::endpoint actual_endpoint;
    //Please run all network communications in the following thread. Network communications cannot be run in other threads.
    std::thread network_io_thread;

private:
    /// Maximum number of observers: fixed capacity array, notification path (ending critical path) zero heap allocation,
    /// Registrations exceeding the upper limit are ignored and logged
    static constexpr std::size_t MAX_OBSERVERS = 8;

    // Observer list: raw pointer (lifetime is guaranteed by the user, see ServerObserver annotation).
    // The notification flow is consistently "copy to a stack snapshot under the lock → call sequentially outside the lock": callbacks can safely call
    // Server query interfaces, and can also add/remove_observer themselves without deadlock
    std::array<ServerObserver *, MAX_OBSERVERS> observers{};
    std::size_t observer_count = 0;
    mutable std::mutex observers_mutex;

    /// Copy the observer into out (inside the lock) and return the number; notify the path of zero heap allocation
    std::size_t observer_snapshot(std::array<ServerObserver *, MAX_OBSERVERS> &out);

    void notify_session_started(std::uint64_t session_id, const std::string &peer);
    void notify_session_ended(std::uint64_t session_id);
    void notify_device_attached(const std::string &busid);
    void notify_device_released(const std::string &busid, DeviceReleaseReason reason);

    /// Device enters available list (issued by add_device outside the lock)
    void notify_device_added(const std::string &busid);
    /// Device leaves available list (issued outside lock by detach_available_device)
    void notify_device_gone(const std::string &busid);

    // Called at the end of the Session destructor (Session is a friend): decrement the live count and wake
    // stop()'s wait (see active_sessions comments for count semantics). The decrement must occur under
    // session_list_mutex: stop()'s predicate check and entering the wait are synchronized by the same lock;
    // If no lock is held here, the decrement + notification may fall in the window "between predicate check and wait" and be discarded
    // (lost wakeup), stop() incorrectly times out
    void notify_session_destroyed() {
        {
            std::lock_guard lock(session_list_mutex);
            active_sessions.fetch_sub(1);
            // notify must be completed within the lock: stop() may return and be destroyed immediately after the lock is released
            // Server (destroying reap_cv); notification outside the lock would run concurrently with condvar destruction
            // (Undefined behavior, TSan measured hit). Lock release/acquisition happens-before,
            // guarantees notification before any Server destruction after stop() wake-up
            reap_cv.notify_all();
        }
    }

    //Devices available for import
    std::vector<std::shared_ptr<UsbDevice>> available_devices;
    //The device in use, busid is used for indexing only and has nothing to do with the usbip protocol.
    std::map<std::string, std::shared_ptr<UsbDevice>> using_devices;
    //locks two variables available_devices and using_devices
    mutable std::shared_mutex devices_mutex;
};
}
