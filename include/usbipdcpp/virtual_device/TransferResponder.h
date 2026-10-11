#pragma once

#include "usbipdcpp/Export.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/LatencyTracker.h"

namespace usbipdcpp {

/**
 * @brief transfer response interface: device side (DeviceHandler/InterfaceHandler/Channel/TransferScheduler)
 * is the only exit to submit the transfer result (RET_SUBMIT/RET_UNLINK) and transfer stop control
 *
 * Background: This set of work of submitting responses was originally undertaken by Session, and the device side directly held the Session pointer to call.
 * However, Session is strongly bound to the usbip protocol, network thread, and Server life cycle, resulting in two problems:
 * ① Difficult to test: device behavior unit tests require a real Server + TCP connection + import handshake;
 * ② Difficult to expand - the virtual device side is actually a user-mode USB device stack, and will need to be connected to other backends in the future
 * (such as qemu's usbredir), the device-side code is entangled with the Session and cannot be reused.
 * Therefore, the responsibility of "submit response" is extracted to this interface: the device side only holds this interface pointer and does not rely on Session.
 *
 * Usage: Session owns an implementation of this interface (SessionResponder), exposed to the device side through
 * Session::responder(); lifecycle callbacks such as on_new_connection also consistently
 * pass this interface. Tests can substitute a stub implementation, without constructing a real Session/Server.
 * transfer stops stop_transfer (AbstDeviceHandler::trigger_session_stop is entrusted to this interface).
 */
class USBIPDCPP_API TransferResponder {
public:
    virtual ~TransferResponder() = default;

    /// Submit a RET_SUBMIT response (asynchronous, the implementation party enqueues/issues it)
    virtual void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) = 0;

    /// Submit a RET_UNLINK response (asynchronous, the implementation party enqueues/issues it)
    virtual void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) = 0;

    /// Only join the queue without waking up (scenario of joining multiple queues continuously and then unifying wakeup_sender)
    virtual void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) = 0;
    virtual void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) = 0;

    /// Wake up the sending thread (cooperated with enqueue_ret_*, called after the last time entering the queue)
    virtual void wakeup_sender() = 0;

    /// Interrupt the current transfer (called by trigger_session_stop when the device is unplugged/the server is stopped)
    virtual void stop_transfer() = 0;

    /**
     * @brief Latency statistics object. Instrumentation macros (LATENCY_TRACK, etc.) receive this pointer after dereferencing it.
     * returns nullptr by default (no override is required when statistics are not enabled during compilation or the implementation does not care);
     * When  statistics are not enabled during compilation (USBIPDCPP_TRACK_PACKAGE/FORCE_TRACK_PACKAGE are not defined)
     * instrumentation macros do not expand, so no null pointer is dereferenced
     */
    virtual LatencyTracker *latency_tracker() { return nullptr; }
};

} // namespace usbipdcpp
