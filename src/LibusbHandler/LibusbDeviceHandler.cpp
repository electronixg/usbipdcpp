#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"

#include <chrono>
#include <spdlog/spdlog.h>

#include "usbipdcpp/Endpoint.h"
#include "usbipdcpp/LibusbHandler/LibusbTransferOperator.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"

using namespace usbipdcpp;

namespace {
// Get the device busid in the callback for logging: the libusb contract guarantees trx->dev_handle is
// valid throughout callback execution (the handle holds a reference count on submission, released only by libusb_close, while close
// occurs in on_disconnection after pending_count_ reaches zero and all callbacks finish),
// and libusb_get_device always returns a non-null handle->dev. The null check only guards against differences in libusb version
// behavior (e.g. some backends cleaning up internal handle state early on NO_DEVICE)
std::string get_trx_device_busid(libusb_transfer *trx) {
    if (auto *dev = libusb_get_device(trx->dev_handle)) {
        return get_device_busid(dev);
    }
    return "unknown";
}
} // namespace

// Normal-mode constructor
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, libusb_device *native_device) :
    AbstDeviceHandler(handle_device, std::make_unique<LibusbTransferOperator>()), native_device_(native_device) {
    // The device is not open yet; it will be opened in on_new_connection
}

// Android-mode constructor
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, intptr_t fd) :
    AbstDeviceHandler(handle_device, std::make_unique<LibusbTransferOperator>()), wrapped_fd_(fd) {
    // fd will be wrapped through libusb_wrap_sys_device in on_new_connection
}

usbipdcpp::LibusbDeviceHandler::~LibusbDeviceHandler() {
    if (native_device_) {
        libusb_unref_device(native_device_);
        native_device_ = nullptr;
    }
}

void usbipdcpp::LibusbDeviceHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    AbstDeviceHandler::on_new_connection(current_session, ec);

    if (native_device_) {
        // Normal mode: open the device
        if (!open_and_claim_device()) {
            SPDLOG_ERROR("Failed to open device");
            // The base class on_new_connection already registered this Session; unregister it after failure:
            // a remaining session pointer points to a Session about to be destroyed. The device then returns to available;
            // on the next import, try_moving_device_to_using first moves it to using. If the device is removed in the window
            // before on_new_connection registers it again, handle_device_left takes
            // the using branch and calls trigger_session_stop, accessing the destroyed
            // Session through the remaining pointer (use-after-free). remove_responder makes the pointer null, safely skipping it
            remove_responder();
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
    }
    else if (wrapped_fd_ >= 0) {
        // Android mode: wrap fd and claim interfaces
        if (!wrap_fd_and_claim_interfaces()) {
            SPDLOG_ERROR("Failed to wrap fd");
            remove_responder();
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
    }

    // Mark the client as connected
    client_disconnection = false;
}

void usbipdcpp::LibusbDeviceHandler::on_disconnection(error_code &ec) {
    client_disconnection = true;
    // Do not check device_removed because libusb correctly triggers the callback on device removal (LIBUSB_TRANSFER_NO_DEVICE)

    // Cancel all transfers
    {
        // Calling libusb_cancel_transfer while holding a shared lock cannot deadlock: libusb transfer callbacks
        // execute only on the handle_events thread; cancel merely submits a cancellation request asynchronously without invoking
        // any callback (URB_CANCEL ioctl on the usbfs backend). The callback thread may briefly block
        // waiting for unique_lock, then continue after cancel returns and releases the shared lock
        std::shared_lock lock(transfers_mutex_);
        for (auto &[seqnum, cb]: transfers_) {
            auto err = libusb_cancel_transfer(static_cast<libusb_transfer *>(cb->transfer.get()));
            if (err) {
                SPDLOG_ERROR("libusb_cancel_transfer failed on seqnum {}: {}", cb->seqnum, libusb_strerror(err));
            }
        }
    }

    // Wait for all transfers to complete
    {
        // The libusb contract guarantees submitted transfers eventually trigger callbacks (cancelled transfers also complete with
        // CANCELLED; see the libusb_cancel_transfer documentation), so the wait must end.
        // Timeouts only diagnose internal libusb errors (matching Server::stop's
        // reap_cv timeout pattern); do not abandon the wait and proceed after a timeout, since callbacks may still
        // execute on the event thread and access this handler, making early handler destruction a use-after-free
        std::unique_lock lock(transfer_complete_mutex_);
        if (!transfer_complete_cv_.wait_for(lock, std::chrono::seconds(10),
                                            [this]() { return pending_count_.load(std::memory_order_acquire) == 0; })) {
            SPDLOG_ERROR("on_disconnection timed out waiting for transfers ({} remaining); continuing to wait", pending_count_.load());
            transfer_complete_cv_.wait(lock, [this]() { return pending_count_.load(std::memory_order_acquire) == 0; });
        }
    }

    // Reset object pool state in preparation for the next connection
    callback_args_pool_.reset();

    // Release interfaces and close the device on disconnection
    if (interfaces_claimed_) {
        release_and_close_device();
    }

    AbstDeviceHandler::on_disconnection(ec);
}

void usbipdcpp::LibusbDeviceHandler::receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep,
                                                 std::optional<UsbInterface> interface, usbipdcpp::error_code &ec) {

    if (device_removed) [[unlikely]] {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    auto seqnum = cmd.header.seqnum;
    auto transfer_flags = cmd.transfer_flags;
    auto transfer_buffer_length = cmd.transfer_buffer_length;
    const auto &setup_packet = cmd.setup;

    // Dispatch by endpoint type
    if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Control)) [[unlikely]] {
        // Control transfer
        auto tweak_ret = tweak_special_requests(setup_packet);
        if (tweak_ret < 0) [[likely]] {
            // No tweak needed; submit transfer
            SPDLOG_DEBUG("Control transfer {}, ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            // Validate that wLength does not exceed transfer_buffer_length: libusb_fill_control_transfer
            // sets the transfer length from wLength (8 + wLength). If wLength exceeds the buffer allocated according to
            // transfer_buffer_length, reads/writes after libusb submission go out of bounds
            // (heap overflow). A malicious client can trigger this with wLength=0xFFFF; reject it.
            // Strict equality is not required: although kernel usbcore (usb_submit_urb) returns
            // -EBADR for wLength != transfer_buffer_length, that is a client
            // host-side constraint (Linux/Android vhci URBs necessarily have equal values). Windows
            // vhci (usbip-win2) has no such constraint and may send packets with wLength <
            // transfer_buffer_length. In that case, the data stage is read according to
            // transfer_buffer_length (the protocol's fixed field); excess bytes are
            // read and discarded, leaving the subsequent protocol stream aligned with no memory risk
            if (setup_packet.length > transfer_buffer_length) [[unlikely]] {
                SPDLOG_ERROR("Control transfer wLength({}) exceeds transfer_buffer_length({}); rejecting transfer",
                             setup_packet.length, transfer_buffer_length);
                // transfer is automatically released when cmd is destroyed (TransferHandle RAII)
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
            }

            auto *trx = static_cast<libusb_transfer *>(cmd.transfer.get());

            // Fill the setup packet at the start of buffer
            libusb_fill_control_setup(trx->buffer, setup_packet.request_type, setup_packet.request, setup_packet.value,
                                      setup_packet.index, setup_packet.length);

            auto *callback_args = callback_args_pool_.alloc();
            if (!callback_args) [[unlikely]] {
                callback_args = new libusb_callback_args{};
            }
            callback_args->handler = this;
            callback_args->seqnum = seqnum;
            callback_args->is_out = setup_packet.is_out();
            callback_args->transfer = std::move(cmd.transfer); // Transfer ownership

            libusb_fill_control_transfer(trx, native_handle, trx->buffer, LibusbDeviceHandler::transfer_callback,
                                         callback_args, timeout_milliseconds);
            trx->flags = get_libusb_transfer_flags(transfer_flags);
            masking_bogus_flags(setup_packet.is_out(), trx);

            bool duplicate_seqnum = false;
            {
                std::lock_guard lock(transfers_mutex_);
                // Reject duplicate seqnum: transfers_ uses seqnum as its key. When two in-flight
                // transfers share the same key, erase in the callback/unlink/submit failure paths
                // incorrectly deletes the earlier transfer's record, disrupting response and cancellation semantics
                // (RET_SUBMIT and RET_UNLINK may be reordered). The protocol requires seqnum
                // to increase monotonically (the kernel vhci increments atomically); duplicates violate the client protocol.
                // Do not submit this transfer to the device; reply with EPIPE
                if (transfers_.contains(seqnum)) [[unlikely]] {
                    duplicate_seqnum = true;
                }
                else {
                    transfers_.emplace(seqnum, callback_args);
                    pending_count_.fetch_add(1, std::memory_order_release);
                }
            }
            if (duplicate_seqnum) [[unlikely]] {
                callback_args->transfer.reset();
                if (!callback_args_pool_.free(callback_args)) {
                    delete callback_args;
                }
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
            }

            LATENCY_TRACK(*responder->latency_tracker(), seqnum, "LibusbDeviceHandler::receive_urb libusb_submit_transfer");
            auto err = libusb_submit_transfer(trx);

            if (err < 0) [[unlikely]] {
                SPDLOG_ERROR("Control transfer to device failed: {}", libusb_strerror(err));
                {
                    std::lock_guard lock(transfers_mutex_);
                    if (transfers_.erase(seqnum)) {
                        // Decrement without notifying: receive_urb and on_disconnection both run on the
                        // receiver thread, so this decrement necessarily executes before the wait predicate is checked
                        // (the predicate is immediately satisfied and the waiter does not sleep), requiring no notify
                        // and losing no wakeup. Also, do not wake here: after notify, a waiter
                        // (if on_disconnection is called across threads in the future) could clean up the object pool and
                        // possibly destroy handler, which this function still accesses afterward
                        // callback_args/session
                        pending_count_.fetch_sub(1, std::memory_order_release);
                    }
                    // No need to check unlinking: receive_urb and handle_unlink_seqnum
                    // execute serially on the receiver thread, so handle_unlink cannot already have set the flag
                    // when submit fails (see the handle_unlink_seqnum comment for the thread model).
                    // Match the kernel stub_recv_cmd_submit: on submission failure, return
                    // RET_SUBMIT(EPIPE) directly; if the host still sends CMD_UNLINK, the entry has already been
                    // erased, so handle_unlink_seqnum takes its else branch and returns
                    // RET_UNLINK(0), which vhci discards for an already completed URB
                }
                callback_args->transfer.reset();
                if (!callback_args_pool_.free(callback_args)) {
                    delete callback_args;
                }
                responder->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO) [[unlikely]] {
                    device_removed = true;
                    ec = make_error_code(ErrorType::NO_DEVICE);
                }
            }
        }
        else {
            // tweak_ret == 0: tweak already sent the request to the device through synchronous APIs; this is its completed
            // result, so do not submit it again (the kernel also skips usb_submit_urb when tweak succeeds; its comment
            // says Skip submitting this URB to not duplicate the request).
            // libusb failures return negative values, and no tweak needed returns -1; both take the submission branch above
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, transfer_buffer_length));
        }
    }
    else if (interface.has_value()) [[likely]] {
        bool is_out = !ep.is_in();

        auto *trx = static_cast<libusb_transfer *>(cmd.transfer.get());

        auto *callback_args = callback_args_pool_.alloc();
        if (!callback_args) [[unlikely]] {
            callback_args = new libusb_callback_args{};
        }
        callback_args->handler = this;
        callback_args->seqnum = seqnum;
        callback_args->is_out = is_out;
        callback_args->transfer = std::move(cmd.transfer); // Transfer ownership

        if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Bulk)) [[likely]] {
            LATENCY_TRACK(*responder->latency_tracker(), seqnum, "LibusbDeviceHandler::receive_urb bulk");

            libusb_fill_bulk_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                      LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
            SPDLOG_DEBUG("Interrupt transfer {}, ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            libusb_fill_interrupt_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                           LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Isochronous)) {
            int num_iso_packets = (cmd.number_of_packets != 0 && cmd.number_of_packets != 0xFFFFFFFF)
                                          ? static_cast<int>(cmd.number_of_packets)
                                          : 0;
            SPDLOG_DEBUG("Isochronous transfer {}, ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            libusb_fill_iso_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                     num_iso_packets, LibusbDeviceHandler::transfer_callback, callback_args,
                                     timeout_milliseconds);
        }
        else [[unlikely]] {
            SPDLOG_DEBUG("Unknown transfer type for endpoint {:02x}: {}", ep.address, ep.attributes);
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }

        trx->flags = get_libusb_transfer_flags(transfer_flags);
        masking_bogus_flags(is_out, trx);

        bool duplicate_seqnum = false;
        {
            std::lock_guard lock(transfers_mutex_);
            // Reject duplicate seqnum (as noted in the control-transfer branch: transfers_ uses
            // seqnum as its key; duplicates make two in-flight transfers share one entry,
            // disrupting erase/cancellation semantics). Reply with EPIPE without submitting to the device
            if (transfers_.contains(seqnum)) [[unlikely]] {
                duplicate_seqnum = true;
            }
            else {
                transfers_.emplace(seqnum, callback_args);
                pending_count_.fetch_add(1, std::memory_order_release);
            }
        }
        if (duplicate_seqnum) [[unlikely]] {
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }

        auto err = libusb_submit_transfer(trx);
        if (err < 0) [[unlikely]] {
            SPDLOG_ERROR("Transfer failed, {}", libusb_strerror(err));
            {
                std::lock_guard lock(transfers_mutex_);
                if (transfers_.erase(seqnum)) {
                    // Decrement without notifying (as in the control path: same thread as on_disconnection,
                    // so decrement precedes checking the wait predicate, which is immediately satisfied; no notify needed)
                    pending_count_.fetch_sub(1, std::memory_order_release);
                }
                // No need to check unlinking: serial execution on the same thread as handle_unlink_seqnum;
                // see the control-transfer branch comment for the reason; matches kernel stub_recv_cmd_submit
            }
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO) [[unlikely]] {
                device_removed = true;
                ec = make_error_code(ErrorType::NO_DEVICE);
            }
        }
    }
    else [[unlikely]] {
        SPDLOG_ERROR("Target interface does not exist for a non-control transfer");
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    if (device_removed) [[unlikely]]
        // The device was physically removed; the connection will disconnect shortly, and vhci will forcibly complete all URBs.
        // Intentionally do not reply with RET_UNLINK: its absence has no practical impact on a connection about to close
        return;

    {
        // Writing cb->unlinking / cb->unlink_cmd_seqnum under a shared lock is safe (not a data race):
        // 1) This function is called only by the single Session::receiver thread (each Session has one packet-receive
        //    loop; on_disconnection after receiver exits also runs on that thread, without concurrency)
        // 2) The only readers of these two fields, transfer_callback (the libusb event thread) and
        //    receive_urb's submit failure path, both hold exclusive locks and are mutually exclusive with this writer;
        //    there is no path that reads unlinking under a shared lock
        // 3) on_disconnection, which can hold a shared lock concurrently with this function, only reads cb->transfer (get()
        //    is read-only) and calls libusb_cancel_transfer (guaranteed thread-safe by libusb), without touching
        //    unlinking. The C++ memory model defines data races only for conflicting accesses to the same memory
        //    location; concurrent access to different members of the same object is not a race
        std::shared_lock lock(transfers_mutex_);
        auto it = transfers_.find(unlink_seqnum);
        if (it != transfers_.end()) {
            auto *cb = it->second;
            // transfer remains in the tracker; mark unlinking and cancel under the lock.
            // The callback sees unlinking==true, takes the RET_UNLINK branch, and is responsible for waking sender.
            cb->unlinking = true;
            cb->unlink_cmd_seqnum = cmd_seqnum;

            // Do not release the shared lock here: libusb_cancel_transfer only sets flags without invoking callbacks;
            // calling under the lock prevents transfer_callback from acquiring the exclusive lock and releasing cb, leaving cb below dangling.
            int err = libusb_cancel_transfer(static_cast<libusb_transfer *>(cb->transfer.get()));
            if (err == LIBUSB_ERROR_NOT_FOUND) [[unlikely]] {
                // transfer has completed at the libusb level but its callback has not executed yet. unlinking is set;
                // the callback takes the unlink branch, queues RET_UNLINK (with the actual status code), and wakes sender.
            }
            else if (err) [[unlikely]] {
                // libusb contract: submitted transfers eventually trigger callbacks (whether cancel succeeds or
                // NOT_FOUND means already completed awaiting dispatch, callbacks run with cancelled/completed status).
                // Since unlinking is set, the callback queues RET_UNLINK; no immediate
                // reply is needed here. This matches the reference project usbipd-libusb's stub_recv_cmd_unlink
                // (which only logs). Queuing RET_UNLINK immediately in the error branch followed by
                // a later callback would send two RET_UNLINK responses (a protocol violation)
                SPDLOG_ERROR("libusb_cancel_transfer failed: {}", libusb_strerror(err));
            }
        }
        else {
            SPDLOG_DEBUG("handle_unlink: transfer NOT found, enqueue RET_UNLINK({})", cmd_seqnum);
            // transfer was removed from the map by transfer_callback, meaning the callback has already decided between RET_SUBMIT
            // and RET_UNLINK and queued it. The host is about to receive (or has already received) this transfer's completion notification;
            // this CMD_UNLINK no longer matters: the transfer has completed, so unlink is inherently too late.
            // Send immediately with submit_ret_unlink (enqueue+wakeup): the callback's wakeup may already have occurred
            // (sender consumed the queue and slept again before this response was enqueued). Relying on the next wakeup to
            // send it would leave RET_UNLINK on an idle connection until closure, potentially making the client time out waiting
            responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(cmd_seqnum, 0));
            lock.unlock();
        }
    }
}

int usbipdcpp::LibusbDeviceHandler::tweak_clear_halt_cmd(const SetupPacket &setup_packet) {
    auto target_endp = setup_packet.index;
    SPDLOG_DEBUG("tweak_clear_halt_cmd");

    auto err = libusb_clear_halt(native_handle, target_endp);
    if (err) [[unlikely]] {
        SPDLOG_ERROR("libusb_clear_halt() error: endp {} returned {}", target_endp, libusb_strerror(err));
    }
    else {
        SPDLOG_DEBUG("libusb_clear_halt() done: endp {}", target_endp);
    }
    // Return a libusb error code (0=success, negative=error).
    // The caller treats tweak_ret < 0 as not handled: submit transfer for the device to handle itself,
    // and tweak_ret == 0 as handled: no transfer submission needed.
    // Thus tweak failure falls through to normal transfer submission as a fallback strategy.
    //
    // On failure, return the actual error code; never downgrade any error code to 0:
    // kernel usb_clear_halt (core/message.c) returns directly on any error; its comment states
    // "don't un-halt or force to DATA0 except on success": reset
    // the data toggle only on success. On failure the device-side toggle was not reset; reporting success makes the client think the endpoint
    // was reset and continue sending/receiving with DATA0, while the two toggles actually differ, causing unexplained later transfer failures.
    return err;
}


int usbipdcpp::LibusbDeviceHandler::tweak_set_interface_cmd(const SetupPacket &setup_packet) {
    uint16_t alternate = setup_packet.value;
    uint16_t interface = setup_packet.index;

    SPDLOG_DEBUG("set_interface: inf {} alt {}", interface, alternate);
    int err = libusb_set_interface_alt_setting(native_handle, interface, alternate);
    if (err) [[unlikely]] {
        SPDLOG_ERROR("{}: usb_set_interface error: inf {} alt {} err {}",
                     get_device_busid(libusb_get_device(native_handle)), interface, alternate, libusb_strerror(err));
    }
    else {
        SPDLOG_DEBUG("{}: usb_set_interface done: inf {} alt {}", get_device_busid(libusb_get_device(native_handle)),
                     interface, alternate);

        // Switch current_altsetting (endpoint data was prefilled during bind).
        // SET_INTERFACE's wIndex is bInterfaceNumber (USB specification), not an array
        // index: on devices with nonconsecutive interface numbers (e.g. 0 and 2), indices differ from interface numbers,
        // so using the index switches the wrong interface. Match interface_number (filled from the configuration descriptor's
        // bInterfaceNumber during bind; see LibusbServer.cpp)
        for (auto &dev_intf: handle_device.interfaces) {
            if (dev_intf.interface_number != interface) {
                continue;
            }
            if (alternate < dev_intf.endpoints.size()) {
                dev_intf.current_altsetting = static_cast<std::uint8_t>(alternate);
                SPDLOG_DEBUG("Switched interface {} to alt {}, endpoint count: {}", interface, alternate,
                             dev_intf.current_endpoints().size());
            }
            break;
        }
    }
    // Return a libusb error code (0=success, negative=error).
    // The caller treats tweak_ret < 0 as not handled: submit transfer for the device to handle itself,
    // and tweak_ret == 0 as handled: no transfer submission needed.
    // Thus tweak failure falls through to normal transfer submission as a fallback strategy.
    return err;
}


// setup_packet is not processed: SET_CONFIGURATION is intentionally skipped here (see the function's comments);
// the parameter is retained for consistency with other tweak function signatures
int usbipdcpp::LibusbDeviceHandler::tweak_set_configuration_cmd([[maybe_unused]] const SetupPacket &setup_packet) {
    SPDLOG_DEBUG("tweak_set_configuration_cmd");

    // uint16_t config = libusb_le16_to_cpu(setup_packet.value);

    // auto err = libusb_set_configuration(native_handle, config);
    // if (err) {
    //     SPDLOG_ERROR(
    //             "{}: libusb_set_configuration error: config {} ret {}",
    //             get_device_busid(libusb_get_device(native_handle)), config, libusb_strerror(err));
    // }
    // else {
    //     SPDLOG_DEBUG(
    //             "{}: libusb_set_configuration done: config {}",
    //             get_device_busid(libusb_get_device(native_handle)), config);
    // }
    // return err;

    // Do not call set_configuration; it would cause device_busy
    //  usbipd-libusb returns -1, meaning this command is not handled; continue normal transfer submission
    //  The device receives the set_configuration command
    return -1;
}

// setup_packet is not processed: reset only logs without actually executing (see the function's comments);
// the parameter is retained for consistency with other tweak function signatures
int usbipdcpp::LibusbDeviceHandler::tweak_reset_device_cmd([[maybe_unused]] const SetupPacket &setup_packet) {
    SPDLOG_INFO("{}: usb_queue_reset_device", get_device_busid(libusb_get_device(native_handle)));

    // Following usbipd-libusb: do not call libusb_reset_device
    // reset may cause device reenumeration, disconnecting the connection
    // libusb_reset_device(native_handle);
    return 0;
}

int usbipdcpp::LibusbDeviceHandler::tweak_special_requests(const SetupPacket &setup_packet) {
    // Return value:
    // Negative: not handled (no tweak needed or tweak failed); caller submits transfer normally
    //  0: tweak handled successfully; no transfer submission needed
    // Special requests are uncommon; most cases return -1 (no tweak needed)
    if (setup_packet.is_clear_halt_cmd()) [[unlikely]] {
        return tweak_clear_halt_cmd(setup_packet);
    }
    else if (setup_packet.is_set_interface_cmd()) [[unlikely]] {
        return tweak_set_interface_cmd(setup_packet);
    }
    else if (setup_packet.is_set_configuration_cmd()) [[unlikely]] {
        return tweak_set_configuration_cmd(setup_packet);
    }
    else if (setup_packet.is_reset_device_cmd()) [[unlikely]] {
        return tweak_reset_device_cmd(setup_packet);
    }
    SPDLOG_DEBUG("No packet adjustment needed");
    return -1; // No tweak needed
}

uint8_t usbipdcpp::LibusbDeviceHandler::get_libusb_transfer_flags(uint32_t in) {
    // Convert only flags with libusb counterparts: URB_ISO_ASAP is inherent in libusb ISO transfer
    // semantics (Linux usbfs forces ASAP; libusb has no corresponding flag to set). Other URB flags
    // (NO_TRANSFER_DMA_MAP, etc.) are unrelated to libusb transfers and need no conversion
    uint8_t flags = 0;

    if (in & static_cast<std::uint32_t>(TransferFlag::URB_SHORT_NOT_OK))
        flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
    if (in & static_cast<std::uint32_t>(TransferFlag::URB_ZERO_PACKET))
        flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;

    return flags;
}

void usbipdcpp::LibusbDeviceHandler::masking_bogus_flags(bool is_out, struct libusb_transfer *trx) {
    // Both the structure and is_out determination (control transfers count as OUT according to the direction bit or wLength==0, meaning no data
    // stage) match the reference project usbipd-libusb's masking_bogus_flags
    // (driver-libusb/stub_rx.c; both are usbip server implementations)
    std::uint32_t allowed = 0;
    /* enforce simple/standard policy */
    switch (trx->type) {
        case LIBUSB_TRANSFER_TYPE_BULK:
            if (is_out)
                allowed |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
        /* FALLTHROUGH */
        case LIBUSB_TRANSFER_TYPE_CONTROL:
            /*allowed |= URB_NO_FSBR; */ /* only affects UHCI */
            /* FALLTHROUGH */
        default: /* all non-iso endpoints */
            if (!is_out)
                allowed |= LIBUSB_TRANSFER_SHORT_NOT_OK;
            break;
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
            // Short ISO packets (actual packet length less than the allocated slot) are normal; do not set
            // SHORT_NOT_OK (otherwise ISO devices relying on short-packet status may incorrectly report
            // ERROR). This case must follow default: placed after CONTROL,
            // BULK/CONTROL fallthrough would break here, never reaching
            // default, and clearing SHORT_NOT_OK for Bulk IN / Control IN
            // (this regression occurred previously). Note: usbipd-libusb's masking_bogus_flags
            // has no such case, so ISO falls into default and gets SHORT_NOT_OK, but
            // Linux usbfs's URB_SHORT_NOT_OK has no practical effect on ISO transfers
            // (ISO completion status is expressed by iso_frame_desc and does not use the short check);
            // explicitly exclude it here to follow the intention of the comment "all non-iso endpoints"
            break;
    }
    trx->flags &= allowed;
}

int usbipdcpp::LibusbDeviceHandler::trxstat2error(enum libusb_transfer_status trxstat) {
    // The specific values are copied from Linux
    // TIMED_OUT also maps to EPIPE rather than ETIMEDOUT, matching the reference project usbipd-libusb's
    // trxstat2error (ERROR/STALL/TIMED_OUT/OVERFLOW in stub_common.c
    // all return -EPIPE). This backend's timeout_milliseconds is 0 (wait indefinitely),
    // so libusb does not actually return TIMED_OUT; this branch is unreachable
    switch (trxstat) {
        case LIBUSB_TRANSFER_COMPLETED:
            return static_cast<int>(UrbStatusType::StatusOK);
        case LIBUSB_TRANSFER_CANCELLED:
            return static_cast<int>(UrbStatusType::StatusECONNRESET);
        case LIBUSB_TRANSFER_ERROR:
        case LIBUSB_TRANSFER_STALL:
        case LIBUSB_TRANSFER_TIMED_OUT:
        case LIBUSB_TRANSFER_OVERFLOW:
            return static_cast<int>(UrbStatusType::StatusEPIPE);
        case LIBUSB_TRANSFER_NO_DEVICE:
            return static_cast<int>(UrbStatusType::StatusESHUTDOWN);
        default:
            return static_cast<int>(UrbStatusType::StatusENOENT);
    }
}


enum libusb_transfer_status usbipdcpp::LibusbDeviceHandler::error2trxstat(int e) {
    switch (e) {
        case static_cast<int>(UrbStatusType::StatusOK):
            return LIBUSB_TRANSFER_COMPLETED;
        case static_cast<int>(UrbStatusType::StatusENOENT):
            return LIBUSB_TRANSFER_ERROR;
        case static_cast<int>(UrbStatusType::StatusECONNRESET):
            return LIBUSB_TRANSFER_CANCELLED;
        case static_cast<int>(UrbStatusType::StatusETIMEDOUT):
            return LIBUSB_TRANSFER_TIMED_OUT;
        case static_cast<int>(UrbStatusType::StatusEPIPE):
            return LIBUSB_TRANSFER_STALL;
        case static_cast<int>(UrbStatusType::StatusESHUTDOWN):
            return LIBUSB_TRANSFER_NO_DEVICE;
        case static_cast<int>(UrbStatusType::StatusEEOVERFLOW): // EOVERFLOW
            return LIBUSB_TRANSFER_OVERFLOW;
        default:
            return LIBUSB_TRANSFER_ERROR;
    }
}

void LIBUSB_CALL usbipdcpp::LibusbDeviceHandler::transfer_callback(libusb_transfer *trx) {
    auto &callback_arg = *static_cast<libusb_callback_args *>(trx->user_data);

    // SPDLOG_WARN("callback: seqnum={} type={} num_iso={} actual_length={} is_out={}", callback_arg.seqnum,
    //             static_cast<int>(trx->type), trx->num_iso_packets, trx->actual_length, callback_arg.is_out);

    LATENCY_TRACK(*callback_arg.handler->responder->latency_tracker(), callback_arg.seqnum,
                  "LibusbDeviceHandler::transfer_callback called");

    // If disconnected, clean up directly and return (without sending a response)
    // This check happens after transfer completion; disconnection is a special case
    if (callback_arg.handler->client_disconnection) [[unlikely]] {
        auto *handler = callback_arg.handler;
        handler->transfers_mutex_.lock();
        handler->transfers_.erase(callback_arg.seqnum);
        handler->transfers_mutex_.unlock();
        callback_arg.transfer.reset(); // Release libusb_transfer rather than delaying until the next alloc
        if (!handler->callback_args_pool_.free(&callback_arg)) {
            delete &callback_arg;
        }
        // Decrement and notify under the lock (see the decrement_pending_and_notify comment for the basis)
        handler->decrement_pending_and_notify();
        return;
    }

    // Check status
    switch (trx->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            /* OK */
            break;
        case LIBUSB_TRANSFER_ERROR:
            // SHORT_NOT_OK not set: the caller accepts short packets, so SHORT_NOT_OK reporting ERROR is normal;
            //   actual data has been received; treat as completed to forward actual_length correctly.
            // SHORT_NOT_OK set: the caller explicitly requires the full length; this is a real transfer error.
            if (!(trx->flags & LIBUSB_TRANSFER_SHORT_NOT_OK)) {
                trx->status = LIBUSB_TRANSFER_COMPLETED;
            }
            else {
                SPDLOG_ERROR("dev {}: error on endpoint {}", get_trx_device_busid(trx),
                             trx->endpoint);
            }
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            SPDLOG_INFO("dev {}: unlinked by a call to usb_unlink_urb()",
                        get_trx_device_busid(trx));
            break;
        case LIBUSB_TRANSFER_STALL:
            SPDLOG_ERROR("dev {}: endpoint {} is stalled",
                         get_trx_device_busid(trx), trx->endpoint);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            SPDLOG_INFO("dev {}: device removed?", get_trx_device_busid(trx));
            callback_arg.handler->device_removed = true;
            break;
        default:
            SPDLOG_WARN("dev {}: urb completion with unknown status {}",
                        get_trx_device_busid(trx),
                        static_cast<int>(trx->status));
            break;
    }
    SPDLOG_DEBUG("libusb transferred {} bytes", trx->actual_length);

    // Calculate the actual length of the ISO transfer
    std::uint32_t actual_length = trx->actual_length;
    if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS && !callback_arg.is_out) {
        // ISO IN transfer: calculate the sum of all iso packets' actual lengths
        size_t iso_actual_length = 0;
        for (int i = 0; i < trx->num_iso_packets; i++) {
            iso_actual_length += trx->iso_packet_desc[i].actual_length;
        }
        actual_length = static_cast<std::uint32_t>(iso_actual_length);
    }

    // Count failed packets in ISO transfers (the protocol RET_SUBMIT error_count field;
    // the kernel stub fills the count computed by USB core, equivalent to counting non-COMPLETED packets here)
    std::uint32_t error_count = 0;
    if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) [[unlikely]] {
        for (int i = 0; i < trx->num_iso_packets; i++) {
            if (trx->iso_packet_desc[i].status != LIBUSB_TRANSFER_COMPLETED) {
                error_count++;
            }
        }
    }

    // Check unlinking, enqueue the response, and remove tracking under the lock: all three operations occur atomically.
    // If handle_unlink_seqnum runs before this, it sees this entry in the map and sets unlinking;
    // if afterward, it does not see it and queues RET_UNLINK(0) itself.
    bool unlinking = false;
    std::uint32_t unlink_cmd_seqnum = 0;

    {
        auto *handler = callback_arg.handler;
        std::unique_lock lock(handler->transfers_mutex_);
        // Contract: erase, checking unlinking, and queuing the response must happen under the same unique_lock.
        // handle_unlink_seqnum's else branch relies on not found => RET_SUBMIT already queued;
        // moving enqueue outside the lock makes RET_SUBMIT/RET_UNLINK send order impossible to guarantee
        unlinking = callback_arg.unlinking;
        unlink_cmd_seqnum = callback_arg.unlink_cmd_seqnum;
        handler->transfers_.erase(callback_arg.seqnum);

        if (unlinking) [[unlikely]] {
            // URB cancelled by unlink; queue RET_UNLINK (with the actual transfer status code)
            callback_arg.handler->responder->enqueue_ret_unlink(
                    UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(unlink_cmd_seqnum, trxstat2error(trx->status)));
            // unlink case: release transfer
            callback_arg.transfer.reset();
        }
        else {
            // Send ret_submit
            // OUT transfers need not return data to the client; send only the header (no data stage)
            // IN transfers must send header + data
            // For ISO transfers (both directions), the protocol requires returning the iso descriptor array: the kernel stub always
            // sends number_of_packets descriptors for ISO transfers (stub_tx.c's
            // iso_packet_descriptor branch does not check direction), and vhci reads according to the header's
            // number_of_packets (usbip_recv_iso skips np==0 without error).
            // Without returned descriptors, the client URB's number_of_packets is overwritten with 0,
            // and per-packet status/actual lengths are missing, causing abnormal behavior in drivers (audio, etc.)
            // that rely on per-packet results. Note that the sum of descriptor actual_length values must equal the header's
            // actual_length, or vhci validation fails and disconnects: libusb's trx->actual_length for ISO
            // OUT is the sum of per-packet actual_length values, inherently satisfying this
            UsbIpResponse::UsbIpRetSubmit ret;
            if (callback_arg.is_out) {
                if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) [[unlikely]] {
                    // ISO OUT: transfer ownership to the response; send_transfer_data sends
                    // the descriptors (OUT sends no data; see its comment).
                    // start_frame is always 0 (as noted in the IN branch: libusb has no frame-number concept,
                    // matching usbipd-libusb)
                    ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                            callback_arg.seqnum, trxstat2error(trx->status), actual_length,
                            0, trx->num_iso_packets, std::move(callback_arg.transfer));
                }
                else {
                    // Non-ISO OUT transfer: no data stage
                    ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                            callback_arg.seqnum, trxstat2error(trx->status), actual_length);
                    // OUT transfer: release transfer
                    callback_arg.transfer.reset();
                }
            }
            else {
                // IN transfer: data present; transfer ownership.
                // start_frame is always 0: libusb transfers have no frame-number concept (usbfs
                // ISO uses ASAP scheduling), matching the reference project usbipd-libusb
                // (stub_common.c's ret_submit packing also fills 0)
                ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(callback_arg.seqnum, trxstat2error(trx->status),
                                                                       actual_length,
                                                                       0, // start_frame
                                                                       trx->num_iso_packets,
                                                                       std::move(callback_arg.transfer) // Transfer ownership
                );
            }
            ret.error_count = error_count;
            callback_arg.handler->responder->enqueue_ret_submit(std::move(ret));
        }
    }

    SPDLOG_DEBUG("libusb transfer actual_length is {} bytes", actual_length);

    LATENCY_TRACK(*callback_arg.handler->responder->latency_tracker(), callback_arg.seqnum,
                  "LibusbDeviceHandler::transfer_callback submit_ret_submit");

    // Enqueue complete; wake the sender thread for unified sending
    callback_arg.handler->responder->wakeup_sender();

    // Return callback_arg after releasing libusb_transfer (value fields are cleared by Reset during alloc)
    auto *handler = callback_arg.handler;
    callback_arg.transfer.reset();
    if (!handler->callback_args_pool_.free(&callback_arg)) {
        delete &callback_arg;
    }
    // Decrement and notify uniformly through decrement_pending_and_notify (decrement+notify under the lock, solving
    // both lost wakeup and use-after-free concurrency problems; see its comment for the basis):
    // callbacks run on the libusb event thread, and on_disconnection on receiver; they run concurrently
    handler->decrement_pending_and_notify();
}

bool usbipdcpp::LibusbDeviceHandler::open_and_claim_device() {
    // This function is only for normal mode
    if (!native_device_) {
        SPDLOG_ERROR("native_device_ is null; cannot open device");
        return false;
    }

    int err = libusb_open(native_device_, &native_handle);
    if (err) {
        SPDLOG_ERROR("Cannot open device: {}", libusb_strerror(err));
        return false;
    }

    // Get the configuration descriptor
    struct libusb_config_descriptor *active_config_desc = nullptr;
    err = libusb_get_active_config_descriptor(native_device_, &active_config_desc);
    if (err) {
        SPDLOG_ERROR("Cannot get configuration descriptor: {}", libusb_strerror(err));
        libusb_close(native_handle);
        native_handle = nullptr;
        return false;
    }

    int num_interfaces = active_config_desc->bNumInterfaces;
    SPDLOG_DEBUG("Device has {} interfaces", num_interfaces);

    // Detach kernel drivers and claim all interfaces
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        // libusb interface-operation parameters are bInterfaceNumber, not array indices: devices with nonconsecutive numbers
        // (nonconsecutive interface numbers, e.g. 0 and 2) have indices different from interface numbers. Use the
        // interface number recorded in the bound interface (filled from the configuration descriptor's bInterfaceNumber during bind); defensively fall back
        // to the index (normally they correspond one-to-one)
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        err = libusb_detach_kernel_driver(native_handle, interface_number);
        if (err && err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("Cannot detach kernel driver for interface {}: {}", interface_number, libusb_strerror(err));
        }

        err = libusb_claim_interface(native_handle, interface_number);
        if (err) {
            SPDLOG_ERROR("Cannot claim interface {}: {}", interface_number, libusb_strerror(err));
            // Interface i was detached earlier; reattach its kernel driver first,
            // otherwise that interface's kernel driver remains disconnected permanently (attach failure is harmless; ignore the return value)
            libusb_attach_kernel_driver(native_handle, interface_number);
            // Roll back already claimed interfaces
            for (int j = 0; j < intf_i; j++) {
                const int rollback_number = j < static_cast<int>(handle_device.interfaces.size())
                                                   ? handle_device.interfaces[j].interface_number
                                                   : j;
                libusb_release_interface(native_handle, rollback_number);
                libusb_attach_kernel_driver(native_handle, rollback_number);
            }
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(native_handle);
            native_handle = nullptr;
            return false;
        }
    }

    // Ensure all interfaces use alt 0, consistent with current_altsetting
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        libusb_set_interface_alt_setting(native_handle, interface_number, 0);
        if (intf_i < static_cast<int>(handle_device.interfaces.size()))
            handle_device.interfaces[intf_i].current_altsetting = 0;
    }

    libusb_free_config_descriptor(active_config_desc);
    interfaces_claimed_ = true;
    SPDLOG_INFO("Successfully opened device and claimed {} interfaces", num_interfaces);
    return true;
}

bool usbipdcpp::LibusbDeviceHandler::wrap_fd_and_claim_interfaces() {
    // This function is only for Android mode
    if (wrapped_fd_ < 0) {
        SPDLOG_ERROR("wrapped_fd_ is invalid");
        return false;
    }

    int err = libusb_wrap_sys_device(nullptr, wrapped_fd_, &native_handle);
    if (err) {
        SPDLOG_ERROR("libusb_wrap_sys_device failed: {}", libusb_strerror(err));
        return false;
    }
    // The wrapped fd is not closed by libusb_close (official documentation: The system device handle
    // will not be closed by libusb_close()). The external caller manages fd, so after
    // closing native_handle here, the same fd can be wrapped repeatedly; no dup is needed

    // Note: the device obtained by wrap is destroyed after libusb_close
    // Therefore each connection requires wrapping again
    libusb_device *wrapped_device = libusb_get_device(native_handle);

    // Get the configuration descriptor
    struct libusb_config_descriptor *active_config_desc = nullptr;
    err = libusb_get_active_config_descriptor(wrapped_device, &active_config_desc);
    if (err) {
        SPDLOG_ERROR("Cannot get configuration descriptor: {}", libusb_strerror(err));
        libusb_close(native_handle);
        native_handle = nullptr;
        return false;
    }

    int num_interfaces = active_config_desc->bNumInterfaces;
    SPDLOG_DEBUG("Device has {} interfaces", num_interfaces);

    // Try detaching the kernel driver before claiming interfaces
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        // As in open_and_claim_device: libusb interface-operation parameters are bInterfaceNumber,
        // not array indices (interface numbers differ from indices on devices with nonconsecutive numbers)
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        int detach_err = libusb_detach_kernel_driver(native_handle, interface_number);
        if (detach_err && detach_err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("Failed to detach kernel driver for interface {} in Android mode: {}", interface_number,
                        libusb_strerror(detach_err));
        }
        err = libusb_claim_interface(native_handle, interface_number);
        if (err) {
            SPDLOG_ERROR("Cannot claim interface {}: {}", interface_number, libusb_strerror(err));
            // Interface i already had a detach attempt; perform an attach to restore its kernel driver.
            // On Android both usually return NOT_SUPPORTED; failure is harmless, so ignore the return value
            libusb_attach_kernel_driver(native_handle, interface_number);
            // Roll back already claimed interfaces
            for (int j = 0; j < intf_i; j++) {
                const int rollback_number = j < static_cast<int>(handle_device.interfaces.size())
                                                   ? handle_device.interfaces[j].interface_number
                                                   : j;
                libusb_release_interface(native_handle, rollback_number);
            }
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(native_handle);
            native_handle = nullptr;
            return false;
        }
    }

    // Ensure all interfaces use alt 0, consistent with current_altsetting
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        libusb_set_interface_alt_setting(native_handle, interface_number, 0);
        if (intf_i < static_cast<int>(handle_device.interfaces.size()))
            handle_device.interfaces[intf_i].current_altsetting = 0;
    }

    libusb_free_config_descriptor(active_config_desc);
    interfaces_claimed_ = true;
    SPDLOG_INFO("Successfully wrapped fd and claimed {} interfaces", num_interfaces);
    return true;
}

void usbipdcpp::LibusbDeviceHandler::release_and_close_device() {
    if (!native_handle) {
        return;
    }

    // Use the interface count cached during binding; do not retrieve the libusb configuration descriptor again.
    // After physical device removal, libusb_get_active_config_descriptor fails and returns 0,
    // causing the release loop to be skipped and preventing kernel drivers from ever being reattached.
    const int num_interfaces = static_cast<int>(handle_device.interfaces.size());

    // Release all interfaces
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        // As in open_and_claim_device: libusb interface-operation parameters are bInterfaceNumber,
        // not array indices (interface numbers differ from indices on devices with nonconsecutive numbers)
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        int err = libusb_release_interface(native_handle, interface_number);
        // LIBUSB_ERROR_NO_DEVICE: the device was physically removed; the kernel automatically reattaches drivers when handle closes; silently ignore.
        if (err && err != LIBUSB_ERROR_NO_DEVICE) {
            SPDLOG_ERROR("Error releasing interface {}: {}", interface_number, libusb_strerror(err));
        }

        // Reattach the kernel driver
        err = libusb_attach_kernel_driver(native_handle, interface_number);
        if (err && err != LIBUSB_ERROR_NOT_FOUND && err != LIBUSB_ERROR_NOT_SUPPORTED
                && err != LIBUSB_ERROR_NO_DEVICE) {
            SPDLOG_WARN("Failed to reattach kernel driver (interface {}): {}", interface_number, libusb_strerror(err));
        }
    }

    interfaces_claimed_ = false;

    // Close handle
    // Both normal and Android modes need libusb_close
    libusb_close(native_handle);
    native_handle = nullptr;

    SPDLOG_INFO("Device interfaces released");
}
