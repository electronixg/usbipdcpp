#pragma once

#include <atomic>
#include <unordered_map>
#include <shared_mutex>

#include <chrono>
#include <thread>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <asio/ip/tcp.hpp>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/utils/LatencyTracker.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/virtual_device/TransferResponder.h"

namespace usbipdcpp {
class Server;
class AbstDeviceHandler;

/**
 * @brief A connection creates a Session, and the life cycle is self-managed: the session thread holds shared_ptr
 * snapshot (run() obtains self), when thread returns, the last reference is released and is self-destructed (before destruction
 * call server.remove_session(id) before destruction to remove its weak_ptr record from Server; at the end of the destructor,
 * calls server.notify_session_destroyed() to notify the Server that recycling is completed). Server session
 * list consists of nonowning weak_ptr entries; after stop() releases snapshot references, it waits for all sessions to finish self-destruction,
 * ensures that no Session thread remains during the survival of the Server.
 * socket holds the io_context of this session (see member declaration sequence). The life cycle of the connection is completely determined by
 * This session is managed by itself and does not depend on the Server's io_context.
 * Please ensure that the Server is not destroyed while the Session is alive, otherwise it will be undefined behavior.
 */
class USBIPDCPP_API Session final : public std::enable_shared_from_this<Session> {
    friend class Server;

public:
    /**
     * @brief Created by Server. id is assigned by Server (atomic increment, never repeated), session
     *        Use it to remove itself from the Server's session table at the end
     */
    Session(Server &server, std::uint64_t id);
    Session(const Session &) = delete;
    Session(Session &&) = delete;

    /**
     * @brief This function is asynchronous and does not block. Enqueue the response packet write_buffer and wake up the sender thread,
     *        The actual network writing is completed by the sender thread. Internally locked, safe from any thread.
     * Ensure that a response packet is submitted for every URB
     * @param unlink
     */
    void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);

    /**
     * @brief transfer response interface (for virtual device side Handler/channel/scheduler): internal implementation
     * SessionResponder forwards to submit_ret_submit / submit_ret_unlink of this class.
     * The virtual device side does not directly rely on Session and submits responses through this interface (replaceable for testing)
     */
    TransferResponder *responder() { return &responder_; }

    /**
     * @brief This function is asynchronous and does not block. Enqueue the response packet write_buffer and wake up the sender thread,
     *        The actual network writing is completed by the sender thread. Internally locked, safe from any thread.
     * Ensure that a response packet is submitted for every URB
     * @param submit
     */
    void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);

    /**
     * @brief Only enqueue write_buffer without waking up the sender.
     * is used in scenarios where multiple responses need to be queued continuously and then woken up in a unified manner.
     */
    void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);
    void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);

    /**
     * @brief wakes up the sender thread without stuffing any data.
     * Use with enqueue_ret_*: enqueue multiple items first, then call wakeup_sender once at the end.
     */
    void wakeup_sender();

    /**
     * @brief sets the stop flag, shutdown + cancel interrupts the pending blocking read, and wakes up the sender thread.
     * is only available for Server stop process, AbstDeviceHandler::trigger_session_stop and language binding calls.
     * will not close the thread internally, but will only notify the thread to close
     */
    void immediately_stop();

    ~Session();

    LATENCY_TRACKER_MEMBER(latency_tracker);

private:
    /**
     * @brief transfer response adapter: forward all TransferResponder interfaces to Session
     * corresponding implementation (Session itself does not inherit the interface, the device side only obtains this object through responder()
     * submission, does not directly rely on Session)
     */
    class SessionResponder : public TransferResponder {
    public:
        explicit SessionResponder(Session &session) : session_(session) {}
        void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) override {
            session_.submit_ret_submit(std::move(submit));
        }
        void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) override {
            session_.submit_ret_unlink(std::move(unlink));
        }
        void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) override {
            session_.enqueue_ret_submit(std::move(submit));
        }
        void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) override {
            session_.enqueue_ret_unlink(std::move(unlink));
        }
        void wakeup_sender() override {
            session_.wakeup_sender();
        }
        void stop_transfer() override {
            session_.immediately_stop();
        }
        LatencyTracker *latency_tracker() override {
#if USBIPDCPP_TRACK_ENABLED
            return &session_.latency_tracker;
#else
            return nullptr;
#endif
        }
    private:
        Session &session_;
    };

    // transfer response adapter instance (this Session is bound in the member initializer)
    SessionResponder responder_{*this};

    /**
     * @brief Called by Server when creating a new Session
     */
    void run();

    // double buffer queue: producer writes write_buffer, consumer reads read_buffer
    // locks briefly during exchange, greatly reducing lock competition.
    std::deque<UsbIpResponse::RetVariant> write_buffer;
    std::deque<UsbIpResponse::RetVariant> read_buffer;
    mutable std::mutex swap_mutex;
    std::condition_variable data_available_cv;
    std::atomic_bool has_data{false};

    void parse_op();

    /**
     * @brief Continuously transfer URBs
     * @param transferring_ec Error code encountered while transferring URBs
     */
    void transfer_loop(usbipdcpp::error_code &transferring_ec);
    void receiver(usbipdcpp::error_code &receiver_ec);
    void sender(usbipdcpp::error_code &ec);
    std::optional<UsbIpResponse::RetVariant> sender_get_data(usbipdcpp::error_code &ec);

    std::atomic_bool should_immediately_stop = false;

    //Is  in the stage of transferring ret_submit?
    std::atomic_bool cmd_transferring = false;

    //is not allowed to be empty during the transfer process, and any writing is prohibited during the transfer process. Reading from non-network threads is not allowed unless locked
    std::optional<std::string> current_import_device_id = std::nullopt;
    //is not allowed to be empty during the transfer process, and any writing is prohibited during the transfer process. Reading from non-network threads is not allowed unless locked
    std::shared_ptr<UsbDevice> current_import_device = nullptr;
    //holds the handler directly to avoid transfer through device
    std::shared_ptr<AbstDeviceHandler> current_handler = nullptr;
    //Lock on the value of the above variable
    std::shared_mutex current_import_device_data_mutex;

    Server &server;
    // Session id: assigned by Server (atomic increment, never repeated), used during teardown to remove itself from Server's
    // removes itself from the session table
    std::uint64_t id;
    // The self-sustained processing context of this session is declared before the socket: when destructed, the socket is destructed first.
    // (the context is still alive), and then io_context is destroyed, the order is safe. socket self-sustaining context
    // The life cycle of the connection after  is completely managed by this session itself and does not depend on the Server's io_context——
    // Server's coroutine async_accept accepts the connection directly into this socket; there is no
    // transfer across io_context
    asio::io_context io_context;
    // associated self-sustaining context, accepted by accept_loop
    asio::ip::tcp::socket socket{io_context};
    // protects the socket close operation: close at the end of the session and immediately_stop
    // cancel/shutdown belong to different threads, and the concurrency of close and cancel will destroy the internals of asio
    // state (undefined behavior), must be mutually exclusive. Read and write send/receive do not pass this lock (asio
    // document allows concurrency with shutdown)
    std::mutex socket_mutex;


    // The main thread handle is not a member: run() detaches a local handle immediately; thread teardown no longer
    // touches its own handle, avoiding concurrent access to std::thread during run()'s assignment. The child thread
    // (sender) is a local variable of transfer_loop. The creation/join/destruction are all within this thread.
    // No member handle required
    // Flag indicating the sender thread has exited: transfer_loop uses it for a timed teardown wait (sender may
    // is stuck in pending writing, and close is forced to interrupt after timeout, see transfer_loop)
    std::atomic_bool sender_done = false;
};
}
