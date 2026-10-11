#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>

#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

namespace usbipdcpp {

class RndisDataInterfaceHandler;

// RNDIS device state machine (aligned kernel rndis.c: UNINITIALIZED → INITIALIZED →
// DATA_INITIALIZED (SET filter≠0 then carrier on), HALT/disconnected back to UNINITIALIZED)
enum class RndisState : std::uint8_t {
    Uninitialized,
    Initialized,
    DataInitialized,
};

/**
 * @brief RNDIS communication interface handler: responds to CDC encapsulation commands, maintains RNDIS state machine and response queue
 *
 * corresponds to the control interface of RNDIS (02/02/0xFF: COMM class + ACM subclass + Vendor protocol,
 * aligned kernel f_rndis.c - Windows and Linux rndis_host are matched according to this, no need to use Microsoft
 * proprietary RNDIS descriptor). RNDIS control messages (INIT/QUERY/SET/KEEPALIVE/RESET/HALT)
 * is all sent through SEND_ENCAPSULATED_COMMAND of ep0, and the response queue is
 * GET_ENCAPSULATED_RESPONSE Take away; each response is queued and sent through the interrupt IN endpoint
 * RESPONSE_AVAILABLE notification (resp_avail callback aligned to rndis.c).
 */
class USBIPDCPP_API RndisCommunicationInterfaceHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface This interface
     * @param string_pool string pool (needs to live longer than handler)
     * @param mac_address device MAC address (returned by OID_802_3_*_ADDRESS query)
     * @param speed Device speed (reported through OID_GEN_LINK_SPEED in 100bps units, consistent with
     *        kernel f_rndis.c's bitrate())
     */
    RndisCommunicationInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                       std::array<std::uint8_t, 6> mac_address, UsbSpeed speed);

    /**
     * @brief Create RNDIS communication interface (descriptor template, unbound handler)
     *
     * interface definition: COMM class + ACM subclass + Vendor protocol (02/02/0xFF), an interrupt IN
     * endpoint (mps=8, interval=32ms, RESPONSE_AVAILABLE notification; align kernel
     * fs_control_intf_ep of f_rndis.c: status endpoint mps≥8 and interval≠0 is
     * a mandatory requirement for the cdc_ether host driver to bind RNDIS).
     * @param interrupt_in_ep Interrupt IN endpoint address (device → host, status notification)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t interrupt_in_ep);

    /**
     * @brief associated data interface handler (called during assembly)
     *
     * bDataInterface (CallManagement/Union) reference of specific descriptor of communication interface class
     * data interface number, taken from get_interface() of the data handler during assembly - in the composite device
     * the communication interface does not start at interface 0; hardcoding 1 prevents host drivers (rndis_host/Windows
     * rndismp) from finding the data interface
     * @param handler data interface handler
     */
    void set_data_handler(RndisDataInterfaceHandler *handler) {
        data_handler_ = handler;
    }

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

    // ========== Status Query API ==========

    /// Is the data channel enabled: INIT and the host has a non-zero packet filter set (aligned to rndis.c
    /// RNDIS_DATA_INITIALIZED——filter ≠ 0 (carrier on starts sending and receiving)
    [[nodiscard]] bool is_data_enabled() const {
        return state_ == RndisState::DataInitialized;
    }

    /// Current RNDIS state machine status
    [[nodiscard]] RndisState get_state() const {
        return state_;
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    // response queue: GET_ENCAPSULATED_RESPONSE is taken in sequence (aligned to the response queue of rndis.c).
    // state machine and queue are the core of RNDIS protocol, and the data interface is queried through is_data_enabled()
    std::deque<data_type> response_queue_;
    RndisState state_ = RndisState::Uninitialized;
    std::uint16_t packet_filter_ = 0;
    std::array<std::uint8_t, 6> mac_address_{};
    std::uint32_t link_speed_100bps_ = 0; // OID_GEN_LINK_SPEED return value (100bps unit)
    std::uint8_t interface_number_; // This interface number (notified to wIndex, taken from the interface object during construction)
    RndisDataInterfaceHandler *data_handler_ = nullptr; // set_data_handler Write during assembly

    /// Status notification channel (interrupt IN endpoint): one notification = one complete message (8 bytes
    /// RESPONSE_AVAILABLE). If there is a pending request, respond directly, otherwise it will be buffered (upper limit 30)
    MessageInChannel notification_channel;

    // message processing (rndis_msg_parser aligned to rndis.c)
    void process_rndis_message(const data_type &msg);
    /// Enqueue a response and send RESPONSE_AVAILABLE notification (the oldest is discarded if the upper limit is exceeded)
    void enqueue_response(data_type &&response);
    /// Query OID returns data area bytes; unknown OID returns empty (assemble QUERY_C of NOT_SUPPORTED)
    data_type query_oid(std::uint32_t oid);
    /// Reset device status (HALT/disconnect): Return to UNINITIALIZED and clear the response queue
    void reset_device_state();
};

/**
 * @brief RNDIS data interface handler: bulk sends and receives RNDIS_MSG_PACKET encapsulated Ethernet frames
 *
 * corresponds to the RNDIS data interface (0A/00/00, no altsetting, aligned to the kernel f_rndis.c
 * rndis_data_intf - RNDIS does not require NOP altsetting for ECM). Sent by the host
 * bulk OUT is RNDIS_MSG_PACKET message (header 24 bytes + Ethernet frame, one transfer can
 * aggregates multi-packets/tail padding), strips headers and hands them to NetworkBackend frame by frame; on frame packets sent to the host
 * 44 byte header (DataOffset=36, data starts from byte 44). Package format aligned core rndis.c
 * of rndis_add_hdr / rndis_rm_hdr
 */
class USBIPDCPP_API RndisDataInterfaceHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface This interface
     * @param string_pool string pool (needs to live longer than handler)
     * @param backend Network backend (nullable). When nonnull, host frames are parsed, their headers stripped, and passed directly to
     *        backend for consumption and immediate response; when null, use suspend mode (on_frame_received /
     *        take_frame, the original RNDIS message is taken)
     * @param comm Communication interface handler (RNDIS state machine owner), needs to live longer than this handler
     */
    RndisDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool, NetworkBackend *backend,
                              RndisCommunicationInterfaceHandler *comm);

    /**
     * @brief Create RNDIS data interface (descriptor template, unbound handler)
     *
     * interface definition: CDC Data class (0A/00/00), two Bulk endpoints (Full speed, mps=64).
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
     * @param frame Ethernet frame (rvalue reference, RNDIS header stripped). Contract: Return false to indicate
     *        is not processed, and the request is suspended as a whole into the channel (the frame can be moved, and the original data remains in the suspended channel)
     *        request, wait for take_frame() to take it away)
     * @return true=This frame has been consumed; false=Unprocessed, pending
     */
    virtual bool on_frame_received(data_type &&frame);

    // ========== Frame sending API (device → host, message mode, non-blocking) ==========

    /**
     * @brief Send a frame to the host (automatically prepend RNDIS_MSG_PACKET header; nonblocking, discard the oldest when the buffer exceeds its limit:
     * network data may be dropped, with TCP retransmission as a fallback)
     * @return The actual number of buffered bytes (including RNDIS header); disconnect and return 0
     */
    std::size_t send_frame(const std::uint8_t *data, std::size_t size);

    /// Send a frame to the host (non-blocking)
    std::size_t send_frame(const data_type &frame);

    // ========== Frame fetching API (host → device, suspend mode) ==========

    /**
     * @brief Blocks the acquisition of a frame of raw data sent from the host. timeout_ms=0 Wait infinitely; disconnect and return
     * nullopt. The OUT request is suspended first and does not respond immediately (host NAK back pressure). It will only respond when it is taken away.
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
    /// Frame sending channel (message mode): one URB exactly one frame (one RNDIS_MSG_PACKET message),
    /// End with a short packet if the frame is shorter than the request: host usbnet splits by message length; byte-stream fragmentation corrupts frames
    MessageInChannel in_channel;

    /// Frame receiving channel: The host bulk OUT request is suspended first and does not respond (NAK back pressure), take_frame()
    /// Read the data and respond when taking it away
    OutEndpointChannel out_channel;

    NetworkBackend *backend_ = nullptr;
    RndisCommunicationInterfaceHandler *comm_ = nullptr; // RNDIS state machine ownership party
};

} // namespace usbipdcpp
