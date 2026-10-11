#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

namespace usbipdcpp {

class PipeDeviceHandler;

/// A transfer returned by read() (aligned with VirtualUSBDevice::Xfer / FunctionFS SETUP event)
struct PipeXfer {
    std::uint8_t ep = 0;                  // endpoint address (including direction bit); when controlling the request, it is 0
    std::optional<SetupPacket> setup_req; // is only valid when controlling the request (ep==0)
    data_type data;                       // OUT data or control request data
};

/// Standard request behavior callback (the standard control request of the interface/endpoint recipient is parsed and forwarded here,
/// Reply with *p_status (0=success), accessing pipe's public API when needed.
/// Unset callbacks use default behavior: accept and return success (SET_INTERFACE accepts only alternate settings
/// defined by the interface; others return EPIPE). Called on the session receiver thread; set before
/// connecting and do not modify after connection
struct PipeStandardRequestHandler {
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint32_t *p_status)>
            clear_feature;
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint8_t ep_address,
                       std::uint32_t *p_status)>
            endpoint_clear_feature;
    std::function<std::uint8_t(PipeDeviceHandler &pipe, std::uint32_t *p_status)> get_interface;
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t alternate_setting, std::uint32_t *p_status)>
            set_interface;
    std::function<std::uint16_t(PipeDeviceHandler &pipe, std::uint32_t *p_status)> get_status;
    std::function<std::uint16_t(PipeDeviceHandler &pipe, std::uint8_t ep_address, std::uint32_t *p_status)>
            endpoint_get_status;
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint32_t *p_status)>
            set_feature;
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint8_t ep_address,
                       std::uint32_t *p_status)>
            endpoint_set_feature;
};

/**
 * @brief Universal pipeline device handler: all endpoints in the interface are automatically pipelined (standard requests are automatically processed),
 * data plane provides blocking read/write (aligned with FIFO semantics of kernel FunctionFS):
 * - read() blocks waiting for an OUT transfer or non-standard control request (class/vendor setup)
 * - write() writes data to the FIFO of the specified IN endpoint. When the FIFO is full, it blocks and waits for the host to take it away.
 *
 * This class is specifically designed to match kernel gadget mode (FunctionFS): the application operates devices as though reading/writing
 * files; the framework handles standard requests, with behavior adjustable via set_standard_request_handler
 * callbacks, while read() exposes nonstandard requests. However, generic pipe mode cannot cover every custom
 * implementation (independent per-endpoint semantics, complex class-protocol negotiation, etc.); devices requiring a specific USB class or
 * For devices with complex control protocols, it is recommended to implement them by inheriting VirtualInterfaceHandler instead.
 * (see docs/custom-device.md method two).
 *
 * Usage: assemble UsbDevice descriptors as in mock_keyboard, bind this class using with_handler,
 * and after setup_interface_handlers, the application thread directly calls read/write.
 */
class USBIPDCPP_API PipeDeviceHandler final : public SimpleVirtualDeviceHandler {
public:
    PipeDeviceHandler(UsbDevice &handle_device, StringPool &string_pool);
    // The binding of the internal pipeline interface handler is completed in setup_interface_handlers (see its declaration)

    // ===== Standard request behavior configuration =====

    /**
     * @brief Set the standard request behavior callback of the interface/endpoint recipient
     * @param handler callback structure (only set the members whose behavior needs to be changed, and keep the default if not set)
     * behavior). The callback is called in the session receiver thread and must be set before connection. After connection
     * Do not modify
     */
    void set_standard_request_handler(PipeStandardRequestHandler handler) {
        standard_request_handler = std::move(handler);
    }

    // ===== Data plane API (blocking semantics aligned kernel FIFO, read/write symmetrical PipeXfer) =====

    /**
     * @brief Blocking read: waiting for an OUT transfer or non-standard control request. timeout_ms=0 and so on indefinitely.
     * @param xfer Output parameter, write when there is data (control request with setup_req)
     * @param timeout_ms timeout in milliseconds, 0 means infinite waiting
     * @return true with data; false with timeout or disconnection
     */
    bool read(PipeXfer &xfer, std::uint32_t timeout_ms = 0);

    /**
     * @brief Blocked writing: Send xfer.data to xfer.ep (IN endpoint address contains direction bit; when ep==0
     * control request response). The data is put into the endpoint FIFO, and when the FIFO is full, it waits for the host to take it away (timeout_ms=0, infinite wait).
     * @param xfer transfer to be sent (ep specifies the target endpoint, data is the payload)
     * @param timeout_ms timeout in milliseconds, 0 means infinite waiting
     * @return The actual number of bytes written; the timeout may be less than the data size; disconnect and return 0
     */
    std::size_t write(const PipeXfer &xfer, std::uint32_t timeout_ms = 0);

    /**
     * @brief sets the FIFO capacity of each IN endpoint (default 64KB), must be called before connecting
     */
    void set_in_fifo_capacity(std::size_t capacity);

    /**
     * @brief Set class-specific descriptors shared by all interfaces (appended after the interface
     * descriptors in the configuration descriptor). A generic pipe has no class descriptors of its own; devices requiring a specific USB class
     * must be set (such as the HID descriptor 0x21 of the HID device, otherwise the driver cannot be loaded).
     * must be called before connecting
     */
    void set_class_specific_descriptor(data_type descriptor);

    /**
     * @brief Set custom descriptor: GET_DESCRIPTOR control request (recipient=interface,
     * standard request) returns corresponding data by type. HID devices must set the report
     * descriptor. Must be called before connecting
     */
    void set_custom_descriptor(std::uint8_t type, data_type descriptor);

    // ========== Connection life cycle ==========

    /// Interface handler needs to hold this object pointer and can only be created after the object is completely constructed, so it is added in the setup phase.
    void setup_interface_handlers() override;

    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

private:
    friend class PipeInterfaceHandler;

    // interface handler forwarding entry (session receiver thread call)
    void on_pipe_in_request(std::uint8_t ep_addr, std::uint32_t seqnum, std::uint32_t length,
                            TransferHandle transfer);
    void on_pipe_out_transfer(std::uint8_t ep_addr, std::uint32_t seqnum, TransferHandle transfer);
    void on_pipe_control_request(const SetupPacket &setup, std::uint32_t seqnum,
                                 std::uint32_t transfer_buffer_length, TransferHandle transfer);
    void on_pipe_unlink(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum);

    /// Get an endpoint IN channel (lazy creation: first access creates a channel with fifo_capacity and binds the session).
    /// The caller must already hold pipe_mutex
    ByteStreamInChannel &get_in_channel(std::uint8_t ep_addr);

    /// Get the endpoint OUT channel (lazy creation: bind the session and reset the disconnected state when accessing for the first time).
    /// The caller must already hold pipe_mutex
    OutEndpointChannel &get_out_channel(std::uint8_t ep_addr);

    // protection out_channels / in_channels / disconnected
    mutable std::mutex pipe_mutex;
    // Any OUT channel is not empty or disconnected (waiting condition for read); notify when joining/disconnecting
    std::condition_variable out_cv;
    // Each endpoint OUT channel (NAK back pressure), key = endpoint address (including direction bit); control request
    // (ep==0) also goes here. Pending requests are answered when picked up by read()
    std::map<std::uint8_t, OutEndpointChannel> out_channels;
    // Each endpoint IN channel (byte stream mode), key = endpoint address (including direction bit); control response
    // (ep==0) also uses this path. Suspend-response/blocking writes are handled inside the channel
    std::unordered_map<std::uint8_t, ByteStreamInChannel> in_channels;
    bool disconnected = true;

    std::size_t fifo_capacity = 64 * 1024;

    // Custom descriptor (set before connection, read-only at runtime, no lock required)
    data_type class_specific_descriptor;
    std::unordered_map<std::uint8_t, data_type> custom_descriptors;

    // standard request behavior callback (set before connection, read-only at runtime, no lock required)
    PipeStandardRequestHandler standard_request_handler;
};

} // namespace usbipdcpp
