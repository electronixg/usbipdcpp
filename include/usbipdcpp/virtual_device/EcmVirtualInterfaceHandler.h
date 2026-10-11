#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

namespace usbipdcpp {

class EcmDataInterfaceHandler;

/**
 * @brief ECM communication interface handler: responds to CDC class control request, interrupts IN endpoint to send status notification
 *
 * Corresponds to the first of the ECM specification's two interfaces (02/06/00). The control plane has only one mandatory request, SET_ETHERNET_PACKET_FILTER
 * (Linux cdc_ether always sends it when bringing the interface up; no data, just return success); other
 * class requests align kernel f_ecm.c always STALL (we bmEthernetStatistics=0, bNumberPowerFilters=0
 * states that statistics and power filtering are not supported). NETWORK_CONNECTION / SPEED_CHANGE notification optional:
 * cdc_ether The host driver is initially linked up and can communicate without sending notifications (notifications are used to reflect status changes on the device side).
 */
class USBIPDCPP_API EcmCommunicationInterfaceHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface This interface (its interface number is used for notification wIndex)
     * @param string_pool string pool (needs to live longer than handler)
     * @param mac_address device MAC address (12 hex characters write iMACAddress string descriptor)
     */
    EcmCommunicationInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                     std::array<std::uint8_t, 6> mac_address);

    /**
     * @brief Create ECM communication interface (descriptor template, unbound handler)
     *
     * interface definition: CDC class, Ethernet Networking subclass (02/06/00), an interrupt IN
     * endpoint (mps=16, interval=32ms, used for NETWORK_CONNECTION / SPEED_CHANGE status notification).
     * @param interrupt_in_ep Interrupt IN endpoint address (device → host, status notification)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t interrupt_in_ep);

    // ========== Internal implementation (subclasses do not need to care) ==========

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== Optional overridden callback for subclasses ==========

    /**
     * @brief Called back when the host sets the Ethernet packet filter (must be sent when the Linux cdc_ether interface is up)
     * @param filter Filter bitmap (ECM120 §6.2.4 Table 8: D0 promiscuous / D1 all multicast / D2 directed /
     *        D3 broadcast / D4 multicast)
     */
    virtual void on_set_packet_filter(std::uint16_t filter);

    // ========== Notification API ==========

    /**
     * @brief Send NETWORK_CONNECTION notification (connection status, wValue=0/1, no data bytes)
     * @param up true=Link connection (device side can send and receive frames), false=Disconnected
     * @note Align kernel f_ecm.c to ecm_do_notify: state puts wValue, wLength=0
     */
    void send_network_connection(bool up);

    /**
     * @brief Send CONNECTION_SPEED_CHANGE notification (uplink and downlink rate)
     * @param up_speed Upstream rate (device → host, bits/sec)
     * @param down_speed Downlink rate (host → device, bits/sec)
     * @note aligned kernel: data 8 bytes = up/down each 4 bytes little-endian
     */
    void send_speed_change(std::uint32_t up_speed, std::uint32_t down_speed);

    // ========== Interface association API ==========

    /// Associated data interface handler (Union descriptor is arranged according to the fixed interface 0/1, the example structure is as follows)
    void set_data_handler(EcmDataInterfaceHandler *handler) {
        data_handler_ = handler;
    }

    /// Get the associated data interface handler
    EcmDataInterfaceHandler *get_data_handler() const {
        return data_handler_;
    }

    // ========== Status Query API ==========

    /// Get the current packet filter bitmap (host SET_ETHERNET_PACKET_FILTER settings)
    [[nodiscard]] std::uint16_t get_packet_filter() const {
        return packet_filter_;
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    std::uint16_t packet_filter_ = 0;
    std::uint8_t mac_string_index_;
    std::uint8_t interface_number_; // This interface number (notified to wIndex, taken from the interface object during construction)

    /// Status notification channel (interrupt IN endpoint): One notification = one complete message. There are pending requests directly
    /// Reply, otherwise enter the buffer (upper limit 1: only keep the latest status, just notify the low frequency to discard the old and keep the new)
    MessageInChannel notification_channel;

    EcmDataInterfaceHandler *data_handler_ = nullptr;
};

/**
 * @brief ECM data interface handler: bulk sends and receives Ethernet frames
 *
 * corresponds to the second data interface of the ECM specification (0A/00/00). alt0 has no endpoint, alt1 has two bulk——
 * The host starts to send and receive frames after alt1 is activated (set_alt of kernel f_ecm.c turns on the semantics of the data plane,
 * just checks the existence of alt by the base class request_set_interface, no additional status is required).
 *
 * The data plane uses one frame per message: bulk IN uses a message-mode channel (each URB response contains exactly one frame; with
 * byte-stream fragmentation, multiple frames would be packed into the host's rx_urb_size buffer, and usbnet treats the entire URB as one frame,
 * parsing results in bad frames). The OUT direction is suspended (NAK back pressure) or handed over to NetworkBackend for immediate consumption.
 */
class USBIPDCPP_API EcmDataInterfaceHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface This interface
     * @param string_pool string pool (needs to live longer than handler)
     * @param backend Network backend (nullable). When nonnull, frames from the host are passed directly to backend
     *        for consumption and responded to immediately; when null, use suspend mode (on_frame_received / take_frame)
     */
    EcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                            NetworkBackend *backend = nullptr);

    /**
     * @brief Create ECM data interface (descriptor template, unbound handler)
     *
     * interface definition: CDC Data class (0A/00/00), alt0 no endpoint / alt1 two Bulk endpoints
     * (Full speed,mps=64).
     * @param in_ep Bulk IN endpoint address (device → host, send frame)
     * @param out_ep Bulk OUT endpoint address (host → device, receive frame)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep);

    // ========== Internal implementation (subclasses do not need to care) ==========

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== Optional overridden callback for subclasses ==========

    /**
     * @brief Callback when the host sends a frame (consumption entry when NetworkBackend is not set)
     * @param frame Ethernet frame (rvalue reference). Contract: Return false to indicate that the request has not been processed
     *        is suspended in the channel; frame must not have been moved from (data remains in the pending request,
     *        waiting for take_frame() to retrieve it)
     * @return true=This frame has been consumed (handler responds immediately); false=Unprocessed, pending
     */
    virtual bool on_frame_received(data_type &&frame);

    // ========== Frame sending API (device → host, message mode, non-blocking) ==========

    /**
     * @brief Send a frame to the host (nonblocking; discard the oldest when the buffer exceeds its limit, preventing unlimited memory
     * growth when the host does not read for a long time: network data may be dropped, with TCP retransmission as a fallback)
     * @return The actual number of buffered bytes; return 0 when disconnected
     */
    std::size_t send_frame(const std::uint8_t *data, std::size_t size);

    /// Send a frame to the host (non-blocking)
    std::size_t send_frame(const data_type &frame);

    // ========== Frame fetching API (host → device, suspend mode) ==========

    /**
     * @brief Blocks fetching a frame of data sent from the host. timeout_ms=0 Wait indefinitely; disconnect and return nullopt.
     * OUT request is suspended first and does not respond immediately (host NAK back pressure), and take_frame() will only respond after taking it away.
     */
    std::optional<OutEndpointChannel::Pending> take_frame(std::uint32_t timeout_ms = 0);

    /// Non-blocking fetching a frame; no data returned nullopt
    std::optional<OutEndpointChannel::Pending> try_take_frame();

    // ========== Buffer configuration API ==========

    /// Set the upper limit of the frame buffer (number of frames, default 0 = unlimited; discard the oldest if it exceeds the limit)
    void set_tx_max_pending(std::size_t max_pending);

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /// Frame sending channel (message mode): One URB has exactly one frame. If the frame is shorter than the request, a short packet is sent and ends.
    /// (The usbnet host treats each URB as a frame; byte stream fragmentation will pack multiple frames into one URB)
    MessageInChannel in_channel;

    /// Frame receiving channel: The host bulk OUT request is suspended first and does not respond (NAK back pressure), take_frame()
    /// Read the data and respond when taking it away
    OutEndpointChannel out_channel;

    NetworkBackend *backend_ = nullptr;
};

} // namespace usbipdcpp
