#pragma once

#include <atomic>
#include <shared_mutex>
#include <unordered_map>

#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/LibusbHandler/tools.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/ObjectPool.h"

namespace usbipdcpp {
class USBIPDCPP_API LibusbDeviceHandler : public AbstDeviceHandler {
    friend class LibusbServer;

public:
    /**
     * @brief Normal mode constructor (delayed binding)
     *
     * The  device is only opened when the client connects (on_new_connection).
     *
     * @param handle_device The UsbDevice this handler is attached to.
     * @param native_device The libusb device (not yet opened). The handler takes ownership of this reference.
     */
    explicit LibusbDeviceHandler(UsbDevice &handle_device, libusb_device *native_device);

    /**
     * @brief Android mode constructor
     *
     * uses the system device file descriptor. The libusb_wrap_sys_device wrapper fd is called every time a client connects.
     * The handle will be closed when disconnected and re-wrapped the next time it is connected, and reconnection is supported.
     *
     * @param handle_device The UsbDevice this handler is attached to.
     * @param fd A valid file descriptor opened on the device node.
     *           The fd must remain valid until the handler is destroyed.
     */
    explicit LibusbDeviceHandler(UsbDevice &handle_device, intptr_t fd);

    ~LibusbDeviceHandler() override;
    void on_new_connection(TransferResponder &responder, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

    bool is_device_removed() const override {
        return device_removed;
    }

    void on_device_removed() override {
        device_removed = true;
    }

public:
    void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep, std::optional<UsbInterface> interface,
                     usbipdcpp::error_code &ec) override;

    int tweak_clear_halt_cmd(const SetupPacket &setup_packet);
    int tweak_set_interface_cmd(const SetupPacket &setup_packet);
    int tweak_set_configuration_cmd(const SetupPacket &setup_packet);
    int tweak_reset_device_cmd(const SetupPacket &setup_packet);

    /**
     * @brief handles special control requests
     * @param setup_packet Control transfer setup packet
     * @return 0: tweak has been successfully processed, no need to submit transfer
     *        <0: Not processed, you need to submit a transfer to let the device process it by itself, including two sources:
     *            -1, no tweaking is required (in most cases);
     *            libusb error code (all negative), tweak execution failed.
     *            Both share the same outlet, which is consistent with the !err normalization of kernel tweak_special_requests:
     *            The kernel counts "failure" and "no tweak required" as "not tweaked" and submits it to the device as usual
     */
    int tweak_special_requests(const SetupPacket &setup_packet);

    static uint8_t get_libusb_transfer_flags(uint32_t in);

    static void masking_bogus_flags(bool is_out, struct libusb_transfer *trx);

    static int trxstat2error(enum libusb_transfer_status trxstat);
    static enum libusb_transfer_status error2trxstat(int e);

    struct libusb_callback_args {
        LibusbDeviceHandler *handler = nullptr;
        std::uint32_t seqnum; // seqnum of  CMD_SUBMIT
        bool is_out;
        TransferHandle transfer; // owns ownership of libusb_transfer*
        bool unlinking = false; // unlink is being canceled
        std::uint32_t unlink_cmd_seqnum = 0; // corresponding CMD_UNLINK seqnum

        void reset() {
            handler = nullptr;
            seqnum = 0;
            is_out = false;
            transfer.reset();
            unlinking = false;
            unlink_cmd_seqnum = 0;
        }
    };

    struct CallbackArgsReset {
        static void reset(libusb_callback_args &args) {
            args.reset();
        }
    };

    static void LIBUSB_CALL transfer_callback(libusb_transfer *trx);

    // object pool: 256, reset() is automatically called during alloc to clean up dirty data
    using CallbackArgsPool =
            ObjectPool<libusb_callback_args, 256, true, detail::DefaultLM<libusb_callback_args>, CallbackArgsReset>;
    CallbackArgsPool callback_args_pool_;

    // is used to wait for all transfers to complete
    std::mutex transfer_complete_mutex_;
    std::condition_variable transfer_complete_cv_;

    /**
     * @brief Decrements pending_count_ and notifies the waiter.
     *
     * All decrement points use this function uniformly (transfer_callback ending, receive_urb submit
     * failed path). Decrement and notification must be placed in the transfer_complete_mutex_ lock (standard
     * CV mode), the waiter (on_disconnection) waits in the same lock, two concurrent
     * The correctness problem is solved by this lock:
     * 1) Lost wakeup: the waiter follows "check the predicate (pending_count_==0) while holding the lock → if not
     *    satisfied, release the lock and sleep". If this function decrements+notifies outside the lock, the decrement can occur between the waiter's
     *    "predicate check and releasing the lock to sleep": the notification occurs before sleep, and after sleeping the waiter
     *    never receives another notification, blocking forever. Decrementing under the lock forces fetch_sub to wait for on_disconnection
     *    to release the lock (i.e., enter sleep); the subsequent notification is necessarily received by the sleeping waiter
     * 2) use-after-free: Return immediately after the waiter's predicate is satisfied (the count returns to zero), receiver
     *    can then clean up the device immediately and destruct this handler (the device removal path has no other references).
     *    If members of this object (such as cv) are accessed between decrement and notification, it is a dangling access. inside the lock
     *    completed fetch_sub + notify ensures that "count returns to zero" and "handler is no longer touched" are synchronized
     *    occurs: When the waiter gets the lock and sees that the predicate is satisfied, this function must have been fully executed.
     */
    void decrement_pending_and_notify() {
        std::lock_guard lock(transfer_complete_mutex_);
        pending_count_.fetch_sub(1, std::memory_order_release);
        transfer_complete_cv_.notify_one();
    }

    // Once this mark is true, communication should be stopped immediately. All variables used to mark the communication status are invalid.
    std::atomic_bool client_disconnection = false;
    std::atomic_bool device_removed = false;

    // Transfer in progress: seqnum → callback_args*
    std::shared_mutex transfers_mutex_;
    std::unordered_map<std::uint32_t, libusb_callback_args *> transfers_;
    std::atomic<std::size_t> pending_count_{0};

    // device handle
    // - Normal mode: Assign value at on_new_connection
    // - Android mode: created by libusb_wrap_sys_device at on_new_connection
    libusb_device_handle *native_handle = nullptr;

    // device reference (only used in normal mode)
    // distinguishes between normal mode and Android mode by judging native_device_ != nullptr
    libusb_device *native_device_ = nullptr;

    // Android mode: system device file descriptor
    intptr_t wrapped_fd_ = -1;

    bool interfaces_claimed_ = false; // Whether the interface has been claimed

    /**
     * @brief Open device and claim interfaces (normal mode).
     * Called on client connection.
     * @return true on success, false on failure.
     */
    bool open_and_claim_device();

    /**
     * @brief Wrap fd and claim interfaces (Android mode).
     * Called on client connection.
     * @return true on success, false on failure.
     */
    bool wrap_fd_and_claim_interfaces();

    /**
     * @brief Release interfaces and close device.
     * Called on client disconnection.
     */
    void release_and_close_device();

    // cannot have timeout, because timeout means that the device data is not ready, not an error.
    // If a timeout occurs, a rep_submit will still be submitted, but the device does not respond at this time and therefore no submission can be made.
    static constexpr int timeout_milliseconds = 0;
};

} // namespace usbipdcpp
