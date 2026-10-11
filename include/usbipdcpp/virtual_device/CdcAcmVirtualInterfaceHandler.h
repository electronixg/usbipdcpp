#pragma once

#include <array>
#include <string_view>
#include <vector>
#include "usbipdcpp/virtual_device/CdcAcmConstants.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

namespace usbipdcpp {
/**
 * @brief CDC ACM line encoding structure
 */
struct LineCoding {
    std::uint32_t dwDTERate = 115200; // baud rate
    std::uint8_t bCharFormat = 0; // stop bits: 0=1 bit, 1=1.5 bit, 2=2 bit
    std::uint8_t bParityType = 0; // Parity: 0=None, 1=Odd, 2=Even, 3=Mark, 4=Space
    std::uint8_t bDataBits = 8; // Data bits: 5, 6, 7, 8, 16

    [[nodiscard]] std::array<std::uint8_t, 7> to_bytes() const {
        return {{static_cast<std::uint8_t>(dwDTERate & 0xFF), static_cast<std::uint8_t>((dwDTERate >> 8) & 0xFF),
                 static_cast<std::uint8_t>((dwDTERate >> 16) & 0xFF),
                 static_cast<std::uint8_t>((dwDTERate >> 24) & 0xFF), bCharFormat, bParityType, bDataBits}};
    }

    static LineCoding from_bytes(const std::vector<std::uint8_t> &data) {
        LineCoding coding{};
        if (data.size() >= 7) {
            coding.dwDTERate = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            coding.bCharFormat = data[4];
            coding.bParityType = data[5];
            coding.bDataBits = data[6];
        }
        return coding;
    }
};

/**
 * @brief CDC ACM control signal status
 */
struct ControlSignalState {
    bool dtr = false; // Data Terminal Ready
    bool rts = false; // Request To Send

    [[nodiscard]] std::uint16_t to_uint16() const {
        std::uint16_t value = 0;
        if (dtr)
            value |= static_cast<std::uint16_t>(CdcAcmControlSignal::DTR);
        if (rts)
            value |= static_cast<std::uint16_t>(CdcAcmControlSignal::RTS);
        return value;
    }

    static ControlSignalState from_uint16(std::uint16_t value) {
        ControlSignalState state;
        state.dtr = (value & static_cast<std::uint16_t>(CdcAcmControlSignal::DTR)) != 0;
        state.rts = (value & static_cast<std::uint16_t>(CdcAcmControlSignal::RTS)) != 0;
        return state;
    }
};

/**
 * @brief CDC ACM serial port status notification
 */
struct SerialStateNotification {
    std::uint8_t bmRequestType = 0xA1; // class specific, interface, IN
    std::uint8_t bNotification = 0x20; // SERIAL_STATE
    std::uint16_t wValue = 0;
    std::uint16_t wIndex = 0; // interface number
    std::uint16_t wLength = 2;
    std::uint16_t data = 0; // status bit

    [[nodiscard]] std::vector<std::uint8_t> to_bytes() const {
        std::vector<std::uint8_t> result;
        result.push_back(bmRequestType);
        result.push_back(bNotification);
        result.push_back(wValue & 0xFF);
        result.push_back((wValue >> 8) & 0xFF);
        result.push_back(wIndex & 0xFF);
        result.push_back((wIndex >> 8) & 0xFF);
        result.push_back(wLength & 0xFF);
        result.push_back((wLength >> 8) & 0xFF);
        result.push_back(data & 0xFF);
        result.push_back((data >> 8) & 0xFF);
        return result;
    }
};

// Forward declaration
class CdcAcmDataInterfaceHandler;

/**
 * @brief CDC ACM communication interface handler (processes control requests and status notifications)
 *
 * is used to handle the communication interface of the CDC ACM device, responding to control requests and sending status notifications.
 */
class USBIPDCPP_API CdcAcmCommunicationInterfaceHandler : public VirtualInterfaceHandler {
public:
    CdcAcmCommunicationInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    /**
     * @brief Create CDC ACM communication interface (descriptor template, unbound handler)
     *
     * interface definition: CDC class, ACM subclass, AT command protocol (02/02/01), an interrupt IN endpoint
     * (used for serial port status notification).
     * @param interrupt_in_ep Interrupt IN endpoint address (device → host, status notification)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t interrupt_in_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::CDC),
                .interface_subclass = 0x02, // ACM
                .interface_protocol = 0x01, // AT Commands (v25ter)
                .endpoints = {{UsbEndpoint{.address = interrupt_in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                           .max_packet_size = 64,
                                           .interval = 16}}},
        };
        // IAD: ACM is dual interface function (communication + data), aligned to core acm_iad_descriptor of f_acm.c
        // (bFunctionClass=COMM,bFunctionSubClass=ACM(0x02),bFunctionProtocol=
        // AT_V25TER(0x01), bInterfaceCount=2). Windows class driver when bDeviceClass=0x02
        // The whole machine binding does not look at the IAD, but the descriptor is consistent with the kernel, and it is also required for composite device identification in multi-function configuration.
        // bFirstInterface is backfilled according to the interface_number of the corresponding interface when generated by the configuration descriptor.
        i.interface_association_descriptor = IadDesc::make(2, static_cast<std::uint8_t>(ClassCode::CDC),
                                                           0x02 /* USB_CDC_SUBCLASS_ACM */,
                                                           0x01 /* USB_CDC_ACM_PROTO_AT_V25TER */);
        return i;
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

    // ========== Optional overridden callback for subclasses ==========

    /**
     * @brief Callback when the host sets the line encoding
     * @param coding new line coding parameter
     */
    virtual void on_set_line_coding(const LineCoding &coding);

    /**
     * @brief Callback when the host sets the control signal status
     * @param state control signal status (DTR, RTS)
     */
    virtual void on_set_control_line_state(const ControlSignalState &state);

    /**
     * @brief Callback when the host requests a break signal
     * @param duration Break duration
     */
    virtual void on_send_break(std::uint16_t duration);

    /**
     * @brief handles control transfer of non-CDC ACM class requests, subclasses can be rewritten to extend functionality
     */
    virtual void handle_non_cdc_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                         std::uint32_t transfer_flags,
                                                         std::uint32_t transfer_buffer_length,
                                                         const SetupPacket &setup_packet, TransferHandle transfer,
                                                         std::error_code &ec);

    // ========== Status Query API ==========

    /**
     * @brief Get the current line coding
     */
    [[nodiscard]] const LineCoding &get_line_coding() const {
        return line_coding_;
    }

    /**
     * @brief Get the current control signal status
     */
    [[nodiscard]] const ControlSignalState &get_control_signal_state() const {
        return control_signal_state_;
    }

    // ========== Send notification API ==========

    /**
     * @brief Send serial port status notification to the host
     * @param state_bits status bits (such as CTS, DSR, etc.)
     */
    void send_serial_state_notification(std::uint16_t state_bits);

    // ========== Interface association API ==========

    /**
     * @brief associated data interface handler
     */
    void set_data_handler(CdcAcmDataInterfaceHandler *handler) {
        data_handler_ = handler;
    }

    /**
     * @brief Get the associated data interface handler
     */
    CdcAcmDataInterfaceHandler *get_data_handler() const {
        return data_handler_;
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    LineCoding line_coding_;
    ControlSignalState control_signal_state_;

    /**
     * @brief associated data interface handler
     */
    CdcAcmDataInterfaceHandler *data_handler_ = nullptr;

    /// Serial port status notification channel (interrupt IN endpoint): one notification = one complete message. There are pending requests
    /// Direct response, otherwise enter the buffer (upper limit 1): only keep the latest status, align the original pending_notification_
    /// Semantics covered by new notifications; status changes are infrequent, just throw away the old and keep the new)
    MessageInChannel notification_channel;
};

/**
 * @brief CDC ACM data interface handler (processing data transfer)
 *
 * Handles the CDC ACM device's data interface and bulk data transfers.
 */
class USBIPDCPP_API CdcAcmDataInterfaceHandler : public VirtualInterfaceHandler {
public:
    CdcAcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    /**
     * @brief Create CDC ACM data interface (descriptor template, unbound handler)
     *
     * interface definition: CDC Data class (0A/00/00), one Bulk IN + one Bulk OUT endpoint
     * (Full speed,mps=64).
     * @param in_ep Bulk IN endpoint address (device → host, serial port data)
     * @param out_ep Bulk OUT endpoint address (host → device, serial port data)
     * @return UsbInterface with unbound handler
     */
    static UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::CDCData),
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 64,
                                           .interval = 0},
                               UsbEndpoint{.address = out_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 64,
                                           .interval = 0}}},
        };
        return i;
    }

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
     * @brief Callback when host OUT data arrives, allowing subclasses to generate data immediately from the input (usually sending generated
     * data to the IN direction, as for echo). Called before suspending the request (on_out_request)
     * @param data Received data (rvalue reference). Note the contract: return false to indicate not processed,
     * request will be suspended. At this time, it must be ensured that the data has not been moved (the data remains in the pending request, waiting for subclasses
     * to retrieve it later with take_out())
     * @return true=this OUT has been handled (handler responds immediately); false=unhandled, handler suspends
     * the request (host NAK backpressure), and the subclass later uses take_out() to retrieve the data and respond
     */
    virtual bool on_data_received(data_type &&data);

    /**
     * @brief Callback when the host requests data, used to generate data on demand in real time
     * @param length The data length requested by the host
     * @return returns the data to be sent, if it returns empty, wait for send_data call
     * @warning It is forbidden to call functions such as send_data inside, which will cause a deadlock. This function generates data on demand for you.
     * Generated data is sent immediately, so there is no need to call send_data or similar functions.
     */
    virtual data_type on_data_requested(std::uint16_t length);

    /**
     * @brief Callback when host RTS status changes
     * @param rts RTS status, true=the host is willing to receive data
     */
    virtual void on_rts_changed(bool rts);

    // ========== Send data API ==========

    /**
     * @brief Non-blocking sending data to the host
     * @param data data pointer
     * @param size data size
     * @return The number of bytes actually written to the buffer. When the buffer is full, it may be less than the requested value.
     */
    std::size_t send_data(const std::uint8_t *data, std::size_t size);
    std::size_t send_data(const data_type &data);
    std::size_t send_data(data_type &&data);
    std::size_t send_data(std::string_view data);

    /**
     * @brief Blocks sending data to the host, waiting for buffer space
     * @param data data pointer
     * @param size data size
     * @param timeout_ms timeout (milliseconds), 0 means infinite waiting
     * @return The number of bytes actually written to the buffer, which may be less than the requested value during timeout
     */
    std::size_t send_data_blocking(const std::uint8_t *data, std::size_t size, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(const data_type &data, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(data_type &&data, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(std::string_view data, std::uint32_t timeout_ms = 0);

    // ========== Receive data API (OUT direction, suspend mode) ==========

    /**
     * @brief Blocks the acquisition of a piece of host OUT data. timeout_ms=0 Wait indefinitely; disconnect and return nullopt.
     * OUT request is suspended first and does not respond immediately (host NAK back pressure), take_out() responds only after it is taken away
     */
    std::optional<OutEndpointChannel::Pending> take_out(std::uint32_t timeout_ms = 0);

    /// Non-blocking take a piece of host OUT data; no data returns nullopt
    std::optional<OutEndpointChannel::Pending> try_take_out();

    // ========== Buffer configuration API ==========

    /**
     * @brief Set TX buffer capacity
     * @param capacity buffer size (bytes)
     */
    void set_tx_buffer_capacity(std::size_t capacity);

    /**
     * @brief Set TX water level
     * @param high high water mark, it is recommended to trigger flow control when the buffer exceeds this value
     * @param low Low water mark. When the buffer is lower than this value, it is recommended to resume sending.
     */
    void set_tx_watermarks(std::size_t high, std::size_t low);

    /**
     * @brief Get the current data volume of the TX buffer
     * @return Number of bytes used in the buffer
     */
    [[nodiscard]] std::size_t get_tx_buffer_size() const;

    /**
     * @brief Get the remaining space of the TX buffer
     * @return Number of bytes available in the buffer
     */
    [[nodiscard]] std::size_t get_tx_buffer_available() const;

    // ========== Flow control status API ==========

    /**
     * @brief Set CTS status notification host
     * @param cts CTS status, true=the device can receive data
     */
    void set_cts(bool cts);

    /**
     * @brief Get the current RTS status (from the host)
     * @return RTS status, true=the host is willing to receive data
     */
    [[nodiscard]] bool get_rts() const;

    /**
     * @brief associated communication interface handler
     * @param handler communication interface handler pointer
     */
    void set_comm_handler(CdcAcmCommunicationInterfaceHandler *handler);

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;
    void on_disconnection(std::error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /**
     * @brief IN data channel (byte stream mode, sent back to the host)
     *
     * encapsulates "suspend-response" + blocking write: host bulk IN request suspends first, send_data /
     * send_data_blocking Matches the response when writing data; when the buffer is full, it blocks and waits for the host to take it away.
     * on_data_requested is accessed through set_pull_callback (called within the lock, with the original
     * handle_bulk_transfer The calling semantics within double locks are consistent)
     */
    ByteStreamInChannel in_channel;

    /// OUT data suspend channel: host bulk OUT request is suspended first and does not respond (NAK back pressure),
    /// take_out() reads the data and responds when taking it away
    OutEndpointChannel out_channel;

    std::size_t in_high_watermark_ = 48 * 1024;
    std::size_t in_low_watermark_ = 16 * 1024;

    /**
     * @brief associated communication interface handler
     */
    CdcAcmCommunicationInterfaceHandler *comm_handler_ = nullptr;
};
} // namespace usbipdcpp
