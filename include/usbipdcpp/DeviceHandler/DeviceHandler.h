#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/type.h"


namespace usbipdcpp {
struct UsbEndpoint;
struct UsbInterface;
struct SetupPacket;

class TransferResponder;

/**
 * @brief Abstract base class for USB device handlers.
 *
 * Each imported device corresponds to one DeviceHandler instance. Transfer data reads and writes are performed through
 * TransferOperator, obtained from get_transfer_operator().
 * uses GenericTransferOperator by default and can be replaced by subclasses.
 */
class USBIPDCPP_API AbstDeviceHandler {
public:
    explicit AbstDeviceHandler(UsbDevice &handle_device, std::unique_ptr<TransferOperator> op = nullptr) :
        handle_device(handle_device), transfer_op_(op ? std::move(op) : std::make_unique<GenericTransferOperator>()) {
    }

    // Moving is prohibited: handle_device is a reference member that cannot be rebound; if transfer_op_ and session
    // use default moving, the subclass's replacement operator and session pointer are lost, silently producing incorrect behavior.
    // currently does not have any caller, delete it directly to prevent future misuse.
    AbstDeviceHandler(AbstDeviceHandler &&other) = delete;

    /**
     * @brief Unified entrance for processing URB requests
     * @param cmd Complete CMD_SUBMIT command
     * @param ep endpoint information
     * @param interface optional interface information (required for non-control transfer)
     * @param ec error code
     */
    virtual void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep, std::optional<UsbInterface> interface,
                             usbipdcpp::error_code &ec) = 0;

    /**
     * @brief Called on a new client connection; may block. Subclass implementations should call this function at the beginning
     * @param responder Please save the response interface for communication
     * @param ec the ec that occurred
     *
     * @attention This function is responsible for failure rollback: before returning an error (setting ec), the implementation must fully undo
     * state established during this call: unregister the responder, stop started
     * threads/schedulers, and disconnect connected interfaces (also when a subclass fails after calling the base class).
     * The caller does not call on_disconnection as a fallback: a device left half-open (with running threads)
     * will terminate directly because of joinable threads when released and destroyed.
     */
    virtual void on_new_connection(TransferResponder &responder, error_code &ec) {
        register_responder(responder);
    }

    /**
     * @brief Register the response interface for the current communication (record the responder pointer).
     *
     * Internal infrastructure: called by base class on_new_connection; subclasses in on_new_connection
     * When  fails in the second half (such as failure to open the device), you must manually call remove_responder() to deregister.
     * A leftover pointer is not harmless: if the device returns to available and is imported again, try_moving_device_to_using
     * first moves it into using; if it is unplugged in the window before this on_new_connection registers it again,
     * trigger_session_stop accesses the destroyed Session through the dangling pointer left from the previous connection
     */
    void register_responder(TransferResponder &current_responder) {
        std::lock_guard lock(session_mutex_);
        responder = &current_responder;
    }

    /**
     * @brief Remove the registered Session (clear the session pointer).
     *
     * internal infrastructure: called by on_disconnection of the base class, and also used by subclasses during connection establishment
     * failure. Only clears the pointer, without resource teardown (each failure path handles its own resource rollback)
     */
    void remove_responder() {
        std::lock_guard lock(session_mutex_);
        responder = nullptr;
    }

    /**
     * @brief This function will be called when an error occurs and the transfer needs to be completely terminated. After being called, it is prohibited to submit messages and use the Session object \n
     * May block and handle all necessary work. Subclass implementations should call this function at the end
     */
    virtual void on_disconnection(error_code &ec) {
        remove_responder();
    }

    /**
     * @brief Check if the device has been removed
     * @return true indicates that the device has been physically unplugged
     *
     * @note After returning true, Server will treat the device as "disappeared": release (session disconnect/stop)
     * is directly discarded from the in-use list and will not be put back into the available list (the backend only scans the list at the moment of pulling out,
     * If  is put back, it will be a zombie that no one can clear out), and subsequent import attempts will inevitably fail. Default constant
     * false (virtual device does not involve physical unplugging); backends (such as libusb) override when unplugging is detected
     *
     * @attention It is forbidden to return true and then change it back to false: removed is a one-way final state - return true
     * After , the device may be discarded and destroyed at any time, and it is impossible to resurrect it.
     */
    virtual bool is_device_removed() const {
        return false; // default implementation
    }

    /**
     * @brief notifies the handler that "the device has been theoretically removed", please handle it accordingly (when to let
     *        is_device_removed() returns true at the discretion of the implementation, and the two are not forced to be bound)
     *
     * is not called by the core framework and is generally used for callbacks after other backends receive system unplug notifications (such as libusb
     * backend hotplug / transfer error paths)
     */
    virtual void on_device_removed() {
    }

    /**
     * @brief Stop Session in a thread-safe manner
     */
    void trigger_session_stop();

    /**
     * @brief handles USBIP_CMD_UNLINK (the client requested to cancel an unfinished transfer).
     *
     * Protocol contract:
     * - Each CMD_UNLINK must correspond to a RET_UNLINK, status is the USBIP error code.
     * - If the target transfer has completed normally, the status of RET_UNLINK must be 0 (URB completed).
     * - RET_SUBMIT must be sent before RET_UNLINK. The client first receives the actual result of the URB,
     *   then receives unlink confirmation; the order cannot be reversed. If an implementation can send both RET_SUBMIT
     *   and RET_UNLINK for the same transfer, RET_SUBMIT must reach the client before RET_UNLINK.
     *
     * @param unlink_seqnum seqnum of CMD_SUBMIT to be canceled
     * @param cmd_seqnum CMD_UNLINK own seqnum (returned unchanged when used in RET_UNLINK)
     *
     * The target transfer may be in the following states when unlink arrives, and the implementation needs to handle them respectively:
     *
     * status 1 - transfer has not started or is still in progress:
     *   Cancel the transfer. If the cancellation is successful, RET_UNLINK(cmd_seqnum, -ECONNRESET) will be sent when the transfer is completed.
     *   (indicating that the URB was canceled) and RET_SUBMIT is no longer sent.
     *
     * status 2 - transfer has been completed, but RET_SUBMIT has not been sent yet:
     *   sends RET_UNLINK (cmd_seqnum, actual completion status code), or 0 if there is no error.
     *   will no longer send RET_SUBMIT.
     *
     * status 3 —— RET_SUBMIT has been sent:
     *   directly sends RET_UNLINK(cmd_seqnum, 0). RET_SUBMIT has been sent first, in the correct order.
     *
     * status 4 - transfer has been completely processed (RET_SUBMIT and possible RET_UNLINK have been sent):
     *   directly sends RET_UNLINK(cmd_seqnum, 0).
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) = 0;

    // ========== TransferOperator interface ==========

    /**
     * @brief Get the transfer operator (subclasses can be overridden to replace the default implementation)
     */
    TransferOperator *get_transfer_operator() const {
        return transfer_op_.get();
    }

    /**
     * @brief replace transfer operator (ownership transfer)
     */
    void set_transfer_operator(std::unique_ptr<TransferOperator> op) {
        transfer_op_ = std::move(op);
    }

    virtual ~AbstDeviceHandler() = default;

protected:
    UsbDevice &handle_device;
    TransferResponder *responder = nullptr;
    mutable std::mutex session_mutex_;
    std::unique_ptr<TransferOperator> transfer_op_ = std::make_unique<GenericTransferOperator>();
};
} // namespace usbipdcpp
