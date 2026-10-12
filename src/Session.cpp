// This file has too much debug output; enabling TRACE would produce an overwhelming amount
// Force it to INFO here
#ifdef SPDLOG_ACTIVE_LEVEL
    #undef SPDLOG_ACTIVE_LEVEL
#endif
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include "usbipdcpp/Session.h"

#include <asio.hpp>
#include <optional>
#include <spdlog/spdlog.h>
#include <variant>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"

#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/protocol.h"

usbipdcpp::Session::Session(Server &server, std::uint64_t id) :
    server(server), id(id) {
}

void usbipdcpp::Session::enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    std::lock_guard lock(swap_mutex);
    write_buffer.emplace_back(std::move(submit));
}

void usbipdcpp::Session::enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    std::lock_guard lock(swap_mutex);
    write_buffer.emplace_back(std::move(unlink));
}

void usbipdcpp::Session::wakeup_sender() {
    has_data.store(true, std::memory_order_release);
    data_available_cv.notify_all();
}

void usbipdcpp::Session::submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    enqueue_ret_unlink(std::move(unlink));
    wakeup_sender();
}

void usbipdcpp::Session::submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    enqueue_ret_submit(std::move(submit));
    wakeup_sender();
}

usbipdcpp::Session::~Session() {
    // The main thread handle is detached in place in run() and is not stored as a member; the child thread (sender) has already been
    // joined inside transfer_loop, so no thread-handle cleanup is needed here
    // Note: do not log in the destructor. If the process is exiting (e.g. test main has returned), spdlog's
    // global registry may already have been destroyed during static destruction; logging then accesses a destroyed object and causes a segmentation fault
    // Notify Server that destruction has completed; this must be at the end of the destructor body:
    // if the callback runs at the start of the destructor, stop() may return early based on the count and destroy Server;
    // accessing Server again here (notify_session_destroyed touches reap_cv) would be use-after-free
    server.notify_session_destroyed();
}

void usbipdcpp::Session::run() {
    // Obtain a pointer to self first to prevent destruction by the smart pointer
    auto self = shared_from_this();
    if (server.before_thread_create_callback) {
        server.before_thread_create_callback(ThreadPurpose::SessionMain);
    }
    // Detach the main thread handle in place without storing it as a member: during
    // cleanup the thread no longer touches its own handle, avoiding concurrent access to the std::thread object during assignment in run()
    // (std::thread objects are not thread-safe). The thread body holds self; when it returns, releasing the last
    // reference causes self-destruction
    std::thread main_thread;
    try {
        main_thread = std::thread([self = std::move(self)]() {
            try {
                self->parse_op();
            } catch (const std::exception &e) {
                // Fallback: no exception may escape the thread function (otherwise std::terminate crashes the entire process)
                SPDLOG_ERROR("Uncaught session thread exception: {}", e.what());
            } catch (...) {
                SPDLOG_ERROR("Unknown uncaught session thread exception");
            }

            // Automatically remove itself from the server and trigger the exit callback after processing ends
            self->server.remove_session(self->id);
        });
    } catch (...) {
        // If thread creation fails (e.g. insufficient system resources), the after callback must still be called (nullptr
        // indicates creation failure), ensuring before/after are paired; the exception continues propagating to
        // accept_loop's fallback catch to remove the session
        if (server.after_thread_create_callback) {
            server.after_thread_create_callback(ThreadPurpose::SessionMain, nullptr);
        }
        throw;
    }
    main_thread.detach();
    if (server.after_thread_create_callback) {
        // Exceptions from user callbacks are not caught here: if the user's own code throws, what the user intended to do
        // may already have failed. Exceptions propagate along the existing path (accept_loop's fallback catch
        // removes the session record); the library does not clean up after user callbacks
        server.after_thread_create_callback(ThreadPurpose::SessionMain, &main_thread);
    }
}

void usbipdcpp::Session::parse_op() {
    usbipdcpp::error_code ec;
    SPDLOG_TRACE("Attempting to read OP");
    auto op = UsbIpCommand::get_op_from_socket(socket, ec);
    if (ec) {
        SPDLOG_DEBUG("Error getting op from socket: {}", ec.message());
        if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
            SPDLOG_DEBUG("Connection closed");
        }
        else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
            SPDLOG_DEBUG("Socket error occurred");
        }

        goto close_socket;
    }
    std::visit(
            [&, this](auto &&cmd) {
                using T = std::remove_cvref_t<decltype(cmd)>;
                if constexpr (std::is_same_v<UsbIpCommand::OpReqDevlist, T>) {
                    SPDLOG_TRACE("Received OpReqDevlist packet");
                    data_type to_be_sent;
                    {
                        std::shared_lock lock(server.devices_mutex);
                        to_be_sent =
                                UsbIpResponse::OpRepDevlist::create_from_devices(server.available_devices).to_bytes();
                    }
                    asio::write(socket, asio::buffer(to_be_sent), ec);
                    if (!ec) [[likely]]
                        SPDLOG_TRACE("Successfully sent OpRepDevlist packet");
                    else
                        SPDLOG_TRACE("Error sending OpRepDevlist packet {}", ec.message());
                }
                else if constexpr (std::is_same_v<UsbIpCommand::OpReqImport, T>) {
                    SPDLOG_TRACE("Received OpReqImport packet");
                    // busid is a client-controlled 32-byte field, not guaranteed to contain \0 (from_socket
                    // only reads all 32 bytes). Force termination at the last byte before constructing string,
                    // otherwise std::string(ptr) reads past the end into stack memory (a security vulnerability)
                    cmd.busid[31] = '\0';
                    auto wanted_busid = std::string(reinterpret_cast<char *>(cmd.busid.data()));
                    UsbIpResponse::OpRepImport op_rep_import{};
                    SPDLOG_TRACE("Client wants to connect to device with busid {}", wanted_busid);

                    bool target_device_is_using = false;
                    bool open_device_failed = false;
                    // Devices already in use cannot be exported
                    if (server.is_device_using(wanted_busid)) {
                        spdlog::warn("Devices in use cannot be exported");
                        // See recv_request_import in tools/usbip/src/usbipd.c in the kernel source;
                        // the source shows that NA should be returned rather than DevBusy
                        op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                                static_cast<std::uint32_t>(OperationStatuType::NA));
                        target_device_is_using = true;
                    }
                    else {
                        if (auto using_device = server.try_moving_device_to_using(wanted_busid)) {
                            std::lock_guard lock(current_import_device_data_mutex);
                            spdlog::info("Successfully moved device to devices in use");
                            current_import_device_id = wanted_busid;
                            // Point the currently used device to this device
                            current_import_device = using_device;
                            current_handler = using_device->handler;
                            spdlog::info("Successfully cached the device in use");

                            // Try opening the device before sending OpRepImport
                            usbipdcpp::error_code open_ec;
                            current_handler->on_new_connection(*this->responder(), open_ec);
                            if (open_ec) {
                                SPDLOG_ERROR("Failed to open device: {}", open_ec.message());
                                open_device_failed = true;
                                // Release the device (Server directly discards physically removed devices)
                                server.release_device(wanted_busid);
                                current_import_device.reset();
                                current_handler.reset();
                                current_import_device_id.reset();
                            }
                        }
                    }

                    if (!target_device_is_using && !open_device_failed) {
                        bool imported = false;
                        {
                            std::shared_lock lock(current_import_device_data_mutex);
                            if (current_import_device) {
                                spdlog::info("Target device found; import is possible");
                                op_rep_import = UsbIpResponse::OpRepImport::create_on_success(current_import_device);
                                cmd_transferring = true;
                                imported = true;
                            }
                            else {
                                spdlog::info("Target device does not exist; import is impossible");
                                op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                                        static_cast<std::uint32_t>(OperationStatuType::NoDev));
                            }
                        }
                        if (imported) {
                            // Notify outside the lock: callbacks can safely call Server query interfaces
                            // (see the ServerObserver comment); the currently imported busid must be
                            // wanted_busid (recorded when try_moving_device_to_using succeeded)
                            server.notify_device_attached(wanted_busid);
                        }
                    }
                    else if (open_device_failed) {
                        op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                                static_cast<std::uint32_t>(OperationStatuType::NA));
                    }

                    auto to_be_sent = op_rep_import.to_bytes();
                    SPDLOG_TRACE("About to send {} to the server, {} bytes total", get_every_byte(to_be_sent), to_be_sent.size());
                    [[maybe_unused]] auto size = asio::write(socket, asio::buffer(to_be_sent), ec);
                    if (!ec) [[likely]]
                        SPDLOG_TRACE("Successfully sent OpRepImport packet", size);
                    else
                        SPDLOG_TRACE("Error sending OpRepImport packet {}", ec.message());

                    if (!ec) [[likely]] {
                        if (cmd_transferring) {
                            usbipdcpp::error_code transferring_ec;
                            // Entering communication state
                            transfer_loop(transferring_ec);
                            if (transferring_ec) {
                                // Disconnection errors (see the check at 459) are a normal path; downgrade to debug
                                auto is_disconnect = transferring_ec == make_error_code(ErrorType::SOCKET_EOF) ||
                                                     transferring_ec == make_error_code(ErrorType::SOCKET_ERR);
                                if (is_disconnect) {
                                    SPDLOG_DEBUG("Transfer ended (client disconnected): {}", transferring_ec.message());
                                }
                                else {
                                    SPDLOG_ERROR("Error occurred during transferring : {}", transferring_ec.message());
                                }
                                ec = transferring_ec;
                            }

                            // on_disconnection and device cleanup have already been handled in receiver
                        }
                    }
                    else if (cmd_transferring) {
                        // OP_REP_IMPORT send failed: the connection is already unusable; clean up immediately,
                        // without waiting for receiver to read an error (the device would remain in the
                        // using list during this window, causing inconsistent state in races with removal/stop). Cleanup
                        // matches the sender thread creation failure path in transfer_loop:
                        // notify the handler of disconnection to release device interfaces, and let Server release the device
                        // (physically removed devices are discarded directly). Do not enter
                        // transfer_loop: receiver would clean up again (current_handler
                        // is already reset), and a second cleanup would access a null pointer
                        SPDLOG_ERROR("Failed to send OpRepImport; disconnecting this connection: {}", ec.message());
                        usbipdcpp::error_code disconnect_ec;
                        current_handler->on_disconnection(disconnect_ec);
                        // Release the device: Server directly discards physically removed devices under the lock (without returning them to the available list)
                        server.release_device(*current_import_device_id);
                        current_import_device_id.reset();
                        current_import_device.reset();
                    }
                }
                else {
                    // Ensure all possible types are handled
                    static_assert(!std::is_same_v<T, T>);
                }
            },
            op);

close_socket:
    std::error_code ignore_ec;
    SPDLOG_INFO("Attempting to close socket");
    // Cleanup uniformly closes socket (shutdown + close). This and immediately_stop's
    // shutdown/cancel run on different threads; concurrent close and cancel on the same socket are
    // undefined behavior in asio (on the reactive backend, close's cleanup_descriptor_data and
    // cancel's cancel_ops operate on the same op_queue; occasional data races cause hangs, reproduced locally
    // on iteration 1303), so serialize them with socket_mutex.
    // After socket is closed early, ~Session's socket destruction no longer accesses io_context
    // (destruction of an already closed socket returns quickly), so self-destruction on the Session thread does not access dangling state
    // even if it occurs after Server destruction
    {
        std::lock_guard lock(socket_mutex);
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
        socket.close(ignore_ec);
    }
}

void usbipdcpp::Session::immediately_stop() {
    {
        // Set the flag while holding swap_mutex (for the same reason as the receiver cleanup comment): sender's
        // wait predicate at 455 checks should_immediately_stop under the lock; setting it without the lock
        // causes a lost wakeup
        std::lock_guard lock(swap_mutex);
        if (should_immediately_stop.exchange(true)) {
            // Idempotent: handle only once. Setting the flag guarantees socket has been handled by this function (shutdown+cancel
            // interrupt blocking reads), without repeated operations. Note that this function is not the only flag-setting path: receiver /
            // sender also set it when exiting (only as an exit flag, without operating on socket). Only this function and cleanup's
            // close_socket actually operate on socket (serialized by socket_mutex),
            // so there is no path where the flag is set but nobody operates on socket
            return;
        }
    }

    std::error_code ignore_ec;
    // Both shutdown and cancel are required; the mechanisms for interrupting pending synchronous reads complement each other on the two platforms
    // (locally tested with plain asio):
    // - Linux/macOS: shutdown wakes a pending poll and the read immediately returns EOF; cancel has no effect
    // - Windows: shutdown does not wake a pending WSAPoll (observed still blocked after over 15 seconds; it actually
    //   waits for the TcpTimedWaitDelay 240-second connection timeout before returning); cancel internally calls
    //   WSACancelBlockingCall to interrupt the synchronous read immediately
    // Each has no effect on the other platform but is harmless. shutdown must precede cancel: if cancel comes first,
    // and the read thread is in the window between checking the buffer and registering WSARecv/WSASend, cancel
    // finds no pending operation to cancel and is lost. Subsequently registered I/O hangs, and shutdown does not interrupt already
    // pending operations, so the session blocks forever. With shutdown first, any subsequently registered I/O
    // fails immediately, while cancel cancels already pending operations.
    // Do not use close: asio explicitly documents close as not thread-safe (only
    // send/receive/connect/shutdown on a shared socket object are mutually thread-safe). Concurrent cross-thread close and read
    // previously caused Linux/macOS CI hangs. close is performed only by session cleanup (parse_op's
    // close_socket), serialized with shutdown/cancel here using socket_mutex (concurrent close
    // and cancel are undefined behavior)
    {
        std::lock_guard lock(socket_mutex);
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
        socket.cancel(ignore_ec);
    }
    // Wake the sender thread; otherwise it remains stuck in data_available_cv.wait() until receiver exits.
    data_available_cv.notify_all();
    SPDLOG_INFO("Successfully called shutdown");
}

void usbipdcpp::Session::transfer_loop(usbipdcpp::error_code &transferring_ec) {
    // on_new_connection was already called in parse_op; do not call it again here

    error_code receiver_ec;
    error_code sender_ec;
    if (server.before_thread_create_callback) {
        server.before_thread_create_callback(ThreadPurpose::SessionSender);
    }
    // Create the sender thread before executing the receive loop: sender_thread's handle necessarily exists before cleanup
    // (join sender), with no intermediate state where the handle is unassigned. If the two threads' handles were assigned
    // one after the other, the main thread might clean up before the child thread's handle was assigned, and the subsequently created child thread would
    // write remaining responses to an already closed socket.
    // If creation fails (e.g. insufficient system resources), the connection is established (on_new_connection has been called,
    // the device opened/interfaces claimed) but the transfer loop cannot start; take the disconnection cleanup path (see catch):
    // notify the handler of disconnection to release device interfaces and return the device to the available list, otherwise it remains
    // in the using list and the next import incorrectly reports it as in use. Then rethrow so run()'s
    // fallback catch removes session, preventing an escaping exception from causing std::terminate
    std::thread sender_thread;
    try {
        sender_thread = std::thread([&, this]() {
            try {
                sender(sender_ec);
            } catch (const std::exception &e) {
                // send_transfer_data in sender's to_socket may throw
                // (bad_alloc during SmallVector growth, etc.); exceptions must not escape the thread function
                // (otherwise std::terminate crashes the entire process). Set an error code for
                // transfer_loop to report
                SPDLOG_ERROR("sender thread exception: {}", e.what());
                sender_ec = make_error_code(ErrorType::INTERNAL_ERROR);
            } catch (...) {
                SPDLOG_ERROR("Unknown sender thread exception");
                sender_ec = make_error_code(ErrorType::INTERNAL_ERROR);
            }
            // Thread exit flag: wake transfer_loop's timed wait (the normal path at the end of sender
            // already sets it; this is a fallback for exceptions, and repeated setting is harmless)
            sender_done.store(true);
            data_available_cv.notify_all();
        });
    } catch (...) {
        SPDLOG_ERROR("sender thread creation failed; disconnecting this connection");
        usbipdcpp::error_code disconnect_ec;
        current_handler->on_disconnection(disconnect_ec);
        // Release the device: Server directly discards physically removed devices under the lock (without returning them to the available list)
        server.release_device(*current_import_device_id);
        current_import_device_id.reset();
        current_import_device.reset();
        // The after callback must still be called (nullptr indicates creation failure), ensuring before/after are paired
        if (server.after_thread_create_callback) {
            server.after_thread_create_callback(ThreadPurpose::SessionSender, nullptr);
        }
        throw;
    }
    if (server.after_thread_create_callback) {
        server.after_thread_create_callback(ThreadPurpose::SessionSender, &sender_thread);
    }

    bool receiver_ok = true;
    try {
        receiver(receiver_ec);
    } catch (const std::exception &e) {
        SPDLOG_ERROR("receiver exception: {}", e.what());
        receiver_ec = make_error_code(ErrorType::INTERNAL_ERROR);
        receiver_ok = false;
    } catch (...) {
        SPDLOG_ERROR("Unknown receiver exception");
        receiver_ec = make_error_code(ErrorType::INTERNAL_ERROR);
        receiver_ok = false;
    }
    if (!receiver_ok) {
        // The receiver exception path skipped its cleanup (on_disconnection + return device to pool);
        // complete it here, otherwise the device remains in the using list (cleanup matches the end of
        // receiver). Do not let it escape transfer_loop: sender_thread has not been joined,
        // and std::thread destruction would cause std::terminate, crashing the entire process
        usbipdcpp::error_code disconnect_ec;
        try {
            current_handler->on_disconnection(disconnect_ec);
        } catch (...) {
            SPDLOG_ERROR("on_disconnection exception");
        }
        // Complete flag setting and wakeup: on exceptional receiver exit, the normal cleanup's flag setting+notify (572-573)
        // did not execute, and sender is still stuck in data_available_cv.wait. Without setting the flag and notifying,
        // transfer_loop's timed wait expires and join then hangs forever
        {
            std::lock_guard lock(swap_mutex);
            should_immediately_stop = true;
        }
        data_available_cv.notify_all();
        // Release the device: Server directly discards physically removed devices under the lock (without returning them to the available list)
        server.release_device(*current_import_device_id);
        current_import_device_id.reset();
        current_import_device.reset();
    }
    SPDLOG_INFO("receiver exited");

    // After receiver ends, sender must be on its way out (should_immediately_stop is set);
    // only a pending write can remain stuck (the peer is not reading and the TCP window is full). Wait with a timeout for sender
    // to exit naturally; on timeout, close under the lock to interrupt forcibly:
    // Concurrent close and sender's pending write are an intended use rather than UB, based on the following:
    // 1) asio documentation (the cancel description): portable cancellation should "Use the close() function
    //    to simultaneously cancel the outstanding operations and close the
    //    socket"; close is specifically used to interrupt pending asynchronous operations (completing with operation_aborted);
    // 2) Windows: close is closesocket; after handle closure, the kernel guarantees pending WSASend
    //    IRPs complete and are posted to IOCP (win_iocp_socket_service_base.ipp), so blocking
    //    write_some returns immediately;
    // 3) Linux: close uses deregister_descriptor (epoll_reactor.ipp) under the
    //    descriptor mutex to mark pending operations operation_aborted and synchronously post them through
    //    post_deferred_completions, waking the blocking write to return.
    // The documentation's "Shared objects: Unsafe" is a conservative statement at the language level; close cancelling pending
    // operations is an explicitly recommended exception in the documentation.
    {
        std::unique_lock lock(swap_mutex);
        data_available_cv.wait_for(lock, std::chrono::milliseconds(100),
                                   [this] { return sender_done.load(); });
    }
    if (!sender_done.load()) {
        std::error_code ignore_ec;
        {
            std::lock_guard lock(socket_mutex);
            socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
            socket.close(ignore_ec);
        }
    }
    sender_thread.join();
    SPDLOG_INFO("sender thread exited");

    // Clear the queue while the handler is alive, ensuring handler is still valid when TransferHandle is destroyed
    write_buffer.clear();
    read_buffer.clear();

    if (sender_ec) {
        SPDLOG_ERROR("An error occur during sending: {}", sender_ec.message());
        transferring_ec = sender_ec;
    }
    // Generally receiver_ec's error code is more important, so it overwrites the other one
    else if (receiver_ec) {
        // Disconnection errors (client detach/closure, network interruption) are routine server paths,
        // not exceptional: disconnection while idle reads eof (SOCKET_EOF), and disconnection during a transfer reads
        // connection_reset etc. (classified as SOCKET_ERR); these are equivalent and both downgraded to debug
        auto is_disconnect = receiver_ec == make_error_code(ErrorType::SOCKET_EOF) ||
                             receiver_ec == make_error_code(ErrorType::SOCKET_ERR);
        if (is_disconnect) {
            SPDLOG_DEBUG("Connection disconnected: {}", receiver_ec.message());
        }
        else {
            SPDLOG_ERROR("An error occur during receiving: {}", receiver_ec.message());
        }
        transferring_ec = receiver_ec;
    }
    cmd_transferring = false;
}

std::optional<usbipdcpp::UsbIpResponse::RetVariant> usbipdcpp::Session::sender_get_data(usbipdcpp::error_code &ec) {
    // If read_buffer is empty, try swapping
    if (read_buffer.empty()) [[likely]] {
        std::unique_lock lock(swap_mutex);
        data_available_cv.wait(
                lock, [this]() { return has_data.load(std::memory_order_acquire) || should_immediately_stop; });
        if (!write_buffer.empty()) {
            read_buffer.swap(write_buffer);
            has_data.store(false, std::memory_order_release);
        }
    }
    if (!read_buffer.empty()) [[likely]] {
        usbipdcpp::UsbIpResponse::RetVariant ret_v = std::move(read_buffer.front());
        read_buffer.pop_front();
        return ret_v;
    }
    else {
        return std::nullopt;
    }
}

void usbipdcpp::Session::receiver(usbipdcpp::error_code &receiver_ec) {
    // spdlog::info("should_immediately_stop:{}", should_immediately_stop.load());
    while (!should_immediately_stop) {
        usbipdcpp::error_code ec;

        auto command = UsbIpCommand::get_cmd_from_socket(socket, current_handler.get(), ec);
        if (ec) [[unlikely]] {
            if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
                SPDLOG_DEBUG("Connection closed");
            }
            else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
                SPDLOG_DEBUG("Socket error occurred");
            }
            else {
                SPDLOG_ERROR("Error getting command from socket: {}", ec.message());
            }
            // Pass the error out to transfer_loop (otherwise the upper layer only sees default 0,
            // and cannot distinguish normal disconnection from disconnection due to a socket error)
            receiver_ec = ec;
            break;
        }
        if (should_immediately_stop) [[unlikely]]
            break;
        std::visit(
                [&, this](auto &&cmd) {
                    using T = std::remove_cvref_t<decltype(cmd)>;
                    if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdSubmit, T>) {
                        UsbIpCommand::UsbIpCmdSubmit &cmd2 = cmd;
                        LATENCY_TRACK_START(latency_tracker, cmd2.header.seqnum);
                        SPDLOG_TRACE("Received UsbIpCmdSubmit packet, sequence number: {}", cmd2.header.seqnum);
                        auto out = cmd2.header.direction == UsbIpDirection::Out;
                        SPDLOG_TRACE("Usbip transfer direction: {}", out ? "out" : "in");
                        std::uint8_t real_ep = out ? static_cast<std::uint8_t>(cmd2.header.ep)
                                                   : (static_cast<std::uint8_t>(cmd2.header.ep) | 0x80);
                        SPDLOG_TRACE("Actual transfer endpoint: {:02x}", real_ep);
                        [[maybe_unused]] auto current_seqnum = cmd2.header.seqnum;

                        auto ep_find_ret = current_import_device->find_ep(real_ep);
                        if (ep_find_ret.has_value()) [[likely]] {
                            auto &ep = ep_find_ret->first;
                            auto &intf = ep_find_ret->second;

                            SPDLOG_TRACE("->endpoint {0:02x}", ep.address);
                            SPDLOG_TRACE("->setup data {}", get_every_byte(cmd2.setup.to_bytes()));


                            usbipdcpp::error_code ec_during_handling_urb;
                            // start_processing_urb();
                            LATENCY_TRACK(latency_tracker, cmd2.header.seqnum, "Preparing to pass to device receive_urb");
                            current_handler->receive_urb(std::move(cmd2), ep, std::move(intf), ec_during_handling_urb);

                            if (ec_during_handling_urb) [[unlikely]] {
                                SPDLOG_ERROR("Error during handling urb : {}", ec_during_handling_urb.message());
                                // An error means communication can no longer continue
                                receiver_ec = ec_during_handling_urb;
                                should_immediately_stop = true;
                                return;
                            }
                        }
                        else {
                            // No endpoint corresponds to real_ep. Silently discard without returning EPIPE,
                            // matching the kernel usbip stub_rx.c get_pipe() (if (pipe==-1)
                            // return, without response) and usbipd-libusb's
                            // stub_get_transfer_type() (return when type>MASK):
                            // normal clients send only endpoints actually present in the device configuration descriptor;
                            // failure to find one is a fallback for abnormal/invalid input, so simply discard it
                            SPDLOG_WARN("Cannot find endpoint {}; silently discarding this CMD_SUBMIT", real_ep);
                        }
                    }
                    else if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdUnlink, T>) {
                        UsbIpCommand::UsbIpCmdUnlink &cmd2 = cmd;
                        SPDLOG_TRACE("Received UsbIpCmdUnlink packet, sequence number: {}", cmd2.header.seqnum);

                        current_handler->handle_unlink_seqnum(cmd2.unlink_seqnum, cmd2.header.seqnum);
                    }
                    else if constexpr (std::is_same_v<std::monostate, T>) {
                        SPDLOG_ERROR("Received unknown packet");
                        receiver_ec = make_error_code(ErrorType::UNKNOWN_CMD);
                    }
                    else {
                        // Ensure all possible types are handled
                        static_assert(!std::is_same_v<T, T>);
                    }
                    return;
                },
                command);
    }
    // Order matters: notify the device of disconnection before stopping sender; never reverse it.
    // Until on_disconnection is called, handler still believes the connection is communicating normally
    // (virtual devices keep writing data to the queue, such as bytes received by a virtual serial port).
    // on_disconnection notifies that the connection has disconnected: only after receiving it does handler stop writing,
    // complete its cleanup (possibly queuing the last batch of data), and then sender stops,
    // trying its best to send this final batch (even if the connection is invalid, send failure takes
    // sender's error exit path and is harmless). With the reverse order, sender stops while handler has not yet
    // received the disconnection notification, silently discarding data queued during cleanup and making the virtual device
    // appear to disconnect without warning. The libusb backend's on_disconnection blocks until all
    // transfer callbacks finish (callbacks necessarily occur after cancel); meanwhile sender may still consume the queue
    // and send to the invalid socket (exiting on failure), which is allowed by design
    current_handler->on_disconnection(receiver_ec);
    // Then close the sender thread, preventing errors if it closes first while the device has not yet been notified of closure.
    // The flag must be set under swap_mutex: sender's wait predicate at 455 checks
    // should_immediately_stop under the lock. Setting it without the lock allows notify to fall between sender checking the predicate
    // and entering the cond_wait queue, causing a lost wakeup, so sender
    // waits indefinitely and hangs forever
    {
        std::lock_guard lock(swap_mutex);
        should_immediately_stop = true;
    }
    data_available_cv.notify_all();

    /* Marking it available first here is valid
     * First, device on_disconnection must block and finish all work required for its own disconnection
     * Second, this session is about to be destroyed, so the two current_import_device variables will not be reused
     * Therefore mark it available before clearing the state of these two variables
     */
    // Release the device: Server directly discards physically removed devices under the lock (without returning them to the available list)
    server.release_device(*current_import_device_id);
    current_import_device_id.reset();
    current_import_device.reset();
    SPDLOG_TRACE("Clear the currently imported device's busid");
}

void usbipdcpp::Session::sender(usbipdcpp::error_code &ec) {
    // RET_SUBMIT and RET_UNLINK share one write_buffer queue; enqueue order is send order,
    // so FIFO sending suffices. Unlike the kernel/usbipd-libusb's separate priv_tx and unlink_tx
    // queues, whose relative order cannot be determined and which require manually sending SUBMIT before UNLINK.
    //
    // For example, in the libusb backend, transfer_callback performs both operations under transfers_mutex_:
    // remove from map -> enqueue RET_SUBMIT. handle_unlink_seqnum must wait for the lock to be released to intervene.
    // When it acquires the lock, the transfer is already absent from the map, so the RET_UNLINK enqueued then naturally follows RET_SUBMIT,
    // giving the correct order.
    //
    // Discarding remaining responses in the queue on exit is correct: the loop exits only after receiver exits (socket
    // error/EOF) or stop(), and the connection is unusable in both cases. TCP clients
    // (Linux vhci/usbip) have no protocol sequence that waits for responses after half-closing: they either communicate normally
    // or close the connection entirely, so lost responses do not hang the client; this matches the kernel stub stopping sends
    // and discarding unsent data on connection errors
    while (!should_immediately_stop) {
        auto send_data_opt = sender_get_data(ec);
        if (ec || should_immediately_stop) [[unlikely]] {
            break;
        }
        if (!send_data_opt.has_value()) [[unlikely]] {
            // Spurious wakeup (after separating enqueue and wakeup, has_data is true but the previous iteration already consumed the data).
            // Exit only when should_immediately_stop; otherwise return to waiting.
            if (should_immediately_stop)
                break;
            continue;
        }
        auto send_data = std::move(send_data_opt.value());

        SPDLOG_TRACE("channel received a message");
        error_code sending_ec;
        std::visit(
                [&](auto &&cmd) {
                    using T = std::remove_cvref_t<decltype(cmd)>;
                    if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetSubmit, T>) {
                        SPDLOG_INFO("USBIP DIAG RET_SUBMIT sending: seq={} status={} actual_length={} packets={}",
                                    cmd.header.seqnum, cmd.status, cmd.actual_length, cmd.number_of_packets);
                        cmd.to_socket(socket, sending_ec);
                        if (sending_ec) {
                            SPDLOG_ERROR("USBIP DIAG RET_SUBMIT send failed: seq={} error={}",
                                         cmd.header.seqnum, sending_ec.message());
                        }
                        else {
                            SPDLOG_INFO("USBIP DIAG RET_SUBMIT sent successfully: seq={} actual_length={}",
                                        cmd.header.seqnum, cmd.actual_length);
                        }
                        LATENCY_TRACK_END_MSG(latency_tracker, cmd.header.seqnum, "to_socket call completed");
                    }
                    else if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetUnlink, T>) {
                        cmd.to_socket(socket, sending_ec);
                        LATENCY_TRACK_END_MSG(latency_tracker, cmd.header.seqnum, "to_socket call completed");
                    }
                    else if constexpr (std::is_same_v<std::monostate, T>) {
                        SPDLOG_ERROR("Received unknown packet");
                        sending_ec = make_error_code(ErrorType::UNKNOWN_CMD);
                    }
                    else {
                        static_assert(!std::is_same_v<T, T>);
                    }
                },
                send_data);

        if (sending_ec) {
            // TCP write failed; immediately close bidirectional session communication.
            // Merely breaking out of sender does not make receiver exit: it is blocked on a synchronous read, and without
            // TCP keepalive the read does not return by itself unless the peer disconnects (see immediately_stop tests:
            // Linux remains blocked, and Windows waits for an approximately 240-second connection timeout).
            // shutdown + cancel interrupt receiver's pending synchronous read across platforms (see the
            // immediately_stop comment), serialized with cleanup's close (socket_mutex)
            should_immediately_stop = true;
            std::error_code ignore_ec;
            {
                std::lock_guard lock(socket_mutex);
                socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
                socket.cancel(ignore_ec);
            }
            // Do not pass sending_ec to transfer_loop: send failure has already used shutdown+cancel
            // to interrupt receiver's pending read, so receiver necessarily exits with a socket error and sets
            // receiver_ec. transfer_loop prefers receiver_ec (taking the else if branch when sender_ec is
            // default 0), so callers still distinguish the error source, and sending_ec provides no
            // additional information
            // ec = sending_ec;
            break;
        }
    }
    // Thread exit flag: wake transfer_loop's timed wait (see the transfer_loop comment)
    sender_done.store(true);
    data_available_cv.notify_all();
}
