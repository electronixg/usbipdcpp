#pragma once


#include <asio.hpp>

#include "usbipdcpp/virtual_device/HidConstants.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"


namespace usbipdcpp {
/**
 * @brief HID device interface handler base class
 *
 * provides the default implementation of interrupt transfer. Users only need to implement the report descriptor and control request processing.
 */
class USBIPDCPP_API HidVirtualInterfaceHandler : public VirtualInterfaceHandler {
public:
    HidVirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
        VirtualInterfaceHandler(handle_interface, string_pool) {
        // Report queue limit: prevents unlimited memory growth when the host does not read interrupt IN for a long time (discard oldest when exceeding the limit)
        input_channel.set_max_pending_messages(MAX_PENDING_INPUT_REPORTS);
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    /**
     * @brief handles interrupt transfer (default implementation)
     *
     * Interrupt IN: The host requests input report and calls on_input_report_requested() to obtain data
     * interrupt OUT: The host sends an output report and calls on_output_report_received()
     */
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;

    virtual void handle_non_hid_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                         std::uint32_t transfer_flags,
                                                         std::uint32_t transfer_buffer_length,
                                                         const SetupPacket &setup_packet, TransferHandle transfer,
                                                         std::error_code &ec);

    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== Virtual functions that subclasses must implement ==========

    /**
     * @brief Get HID report descriptor
     * @return report descriptor data
     */
    virtual data_type get_report_descriptor() = 0;

    /**
     * @brief Get HID report descriptor size
     * @return descriptor length (bytes)
     */
    virtual std::uint16_t get_report_descriptor_size() = 0;

    // ========== Send data API ==========

    /**
     * @brief Send input report (zero copy)
     *
     * If there are requests in the queue, respond to the first one immediately; otherwise, store the data and wait for the next request.
     *
     * @param data report data (you can use std::array + asio::buffer on the stack)
     */
    void send_input_report(asio::const_buffer data);

    // ========== Optional overridden callback for subclasses ==========

    /**
     * @brief Callback when the host requests input report
     *
     * @warning This function is called every time the host polls the interrupt endpoint. inside the function
     *          can directly call functions such as send_input_report because it does not acquire any lock.
     *          If send_input_report() is called every time, the host will immediately take the data and poll again,
     *          This causes very high CPU usage. Unless specifically required, do not
     *          call send_input_report() on every invocation of this function.
     *
     * @param length The data length requested by the host
     */
    virtual void on_input_report_requested(std::uint16_t length);

    /**
     * @brief Callback when receiving the output report
     *
     * @param data output report data
     */
    virtual void on_output_report_received(asio::const_buffer data);

    // ========== HID class specific request (optional override for subclasses) ==========

    /**
     * @brief Get the current protocol (Boot/Report); the nominal response follows kernel f_hid.c:
     *        kernel GET_PROTOCOL returns protocol value (configfs initial 0),
     *        Answers as usual even if the device does not support switching. Subclasses can override and return the actual value when implementing protocol switching.
     * @param p_status
     * @return protocol value (0=Boot, 1=Report)
     */
    virtual std::uint8_t request_get_protocol(std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        return 0;
    };

    /**
     * @brief Set the protocol and align the kernel f_hid.c: Only Boot subclass interface (bInterfaceSubClass=1)
     *        accepts SET_PROTOCOL (wValue ≤ 1); others stall. The protocol value is only recorded internally;
     *        This project does not perform Boot report switching by default, just accept it; subclasses can be rewritten
     * @param type wValue (0=Boot protocol, 1=Report protocol)
     * @param p_status
     */
    virtual void request_set_protocol(std::uint16_t type, std::uint32_t *p_status) {
        if (handle_interface.interface_subclass == 0x01 /* USB_INTERFACE_SUBCLASS_BOOT */
            && type <= static_cast<std::uint16_t>(HIDProtocolType::Report)) {
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        }
        else {
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        }
    };

    virtual data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                         std::uint32_t *p_status);
    virtual void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                    const data_type &data, std::uint32_t *p_status);

    /**
     * @brief Get the idle throttling value; the nominal response follows kernel f_hid.c: GET_IDLE returns
     *        idle value (initial 0), the device does not do idle throttling by default; subclasses can be rewritten
     */
    virtual data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                       std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        return {0x00};
    };

    /**
     * @brief Set the idle throttling value and align the core f_hid.c: Unconditionally accept SET_IDLE
     *        (idle is only the throttling parameter on the device side, this project does not throttle by default, just accept it)
     */
    virtual void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
    };

    // ========== Standard request default implementation ==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    // ========== Internal implementation (subclasses do not need to care) ==========

    void on_new_connection(TransferResponder &current_session, error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /**
     * @brief IN input report channel (message mode)
     *
     * encapsulates "suspend-response": the host IN request is suspended first, and send_input_report() pushes data
     * matches the response; the buffer exceeds the upper limit and discards the oldest one (see MAX_PENDING_INPUT_REPORTS)
     */
    MessageInChannel input_channel;

    /**
     * @brief Limit on pending input reports; discard the oldest when exceeded (reports accumulate when the host does not issue interrupt IN requests for a long time;
     * this prevents unlimited memory growth)
     */
    static constexpr std::size_t MAX_PENDING_INPUT_REPORTS = 1024;

    bool has_pending_input_reports() const {
        return input_channel.size() != 0;
    }
};
} // namespace usbipdcpp
