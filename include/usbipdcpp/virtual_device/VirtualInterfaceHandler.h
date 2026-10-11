#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/InterfaceHandler/InterfaceHandler.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"

namespace usbipdcpp {

class VirtualDeviceHandler;

/**
 * @brief Virtual device interface handler base class: responds to interface-level standard/class requests and data-plane transfers
 *
 * @note When implementing static factory make_interface, do not call UsbInterface::with_handler inside it:
 *       the handler constructor retains an interface reference, while make_interface's interface object is local/temporary;
 *       after return (copy/move), the reference immediately dangles (ECM once crashed due to binding in make_interface).
 *       make_interface only returns a UsbInterface with correct descriptors; the caller binds its handler through
 *       device->interfaces[i].with_handler<T>(...) after completing device creation
 *       (interface object stored in device->interfaces, address stable).
 */
class USBIPDCPP_API VirtualInterfaceHandler : public AbstInterfaceHandler {
public:
    explicit VirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                     std::unique_ptr<TransferOperator> op = nullptr) :
        AbstInterfaceHandler(handle_interface), string_pool(string_pool),
        transfer_op_(op ? std::move(op) : std::make_unique<GenericTransferOperator>()) {

        string_interface = string_pool.new_string(L"Usbipdcpp Virtual Interface");
    }

    // ========== Connection lifecycle API ==========

    /**
     * @brief Set the owning DeviceHandler
     * @param handler DeviceHandler pointer
     */
    void set_device_handler(VirtualDeviceHandler *handler) {
        device_handler = handler;
    }

    /** Callback at the end of setup_interface_handlers; device_handler is set by then, so subclasses can initialize here */
    virtual void on_setup_interface_handlers() {
    }

    /**
     * @brief This function will be called when a new client connects
     * @param current_session
     * @param ec the ec that occurred
     * @note Overrides must call the base implementation at the beginning; the base sets the session pointer
     */
    void on_new_connection(TransferResponder &current_session, error_code &ec) override {
        responder = &current_session;
    }

    /**
     * @brief Called when errors, client detach, explicit server shutdown, etc. require transfers to stop completely. No further messages may be submitted after this call.
     * @note Overrides must call the base implementation at the end; the base clears the session pointer
     */
    void on_disconnection(error_code &ec) override {
        responder = nullptr;
    }

    // ========== Data-plane callbacks (return EPIPE by default; functional devices must override) ==========

    virtual void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                      std::uint32_t transfer_buffer_length, TransferHandle transfer, error_code &ec);
    virtual void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                           std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                           error_code &ec);
    virtual void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                             std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                             int num_iso_packets, error_code &ec);

    virtual void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                              std::uint32_t transfer_flags,
                                                              std::uint32_t transfer_buffer_length,
                                                              const SetupPacket &setup, TransferHandle transfer,
                                                              std::error_code &ec);
    virtual void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                          std::uint32_t transfer_flags,
                                                                          std::uint32_t transfer_buffer_length,
                                                                          const SetupPacket &setup,
                                                                          TransferHandle transfer, std::error_code &ec);

    // ========== Standard request callbacks (default implementation: success/EPIPE when unimplemented; override as needed) ==========
    // Default implementations are simple and inline in the header: the header serves as documentation, so subclasses need not consult the cpp for default behavior.
    
    // Feature requests return 0 (success): virtual devices have no UDC hardware endpoint-halt state machine;
    // on physical devices, UDC hardware responds successfully to ENDPOINT_HALT SET/CLEAR_FEATURE (software-layer
    // STALL occurs only for unknown selectors): no state to clear = success; returning EPIPE would unexpectedly give
    // host drivers a failure (when clearing stall during initialization/error recovery)

    virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
        *p_status = 0;
    }
    virtual void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                std::uint32_t *p_status) {
        *p_status = 0;
    }

    virtual std::uint8_t request_get_interface(std::uint32_t *p_status) {
        // Return the current alternate setting (already updated at device level after successful SET_INTERFACE).
        // Consistent with kernel f->get_alt: return 0 when an interface has no get_alt callback (only alt 0 supported)
        *p_status = 0;
        return handle_interface.current_altsetting;
    }
    virtual void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
        // Accept only alternate settings defined by the device (outer endpoints index is the alt number): return EPIPE for nonexistent
        // alternate settings, otherwise GET_INTERFACE's return value conflicts with the endpoint set (pretending success
        // makes the host driver read contradictory interface state). Consistent with kernel composite: interfaces without set_alt/
        // get_alt callbacks support only alt 0; other alt requests return STALL
        if (alternate_setting < handle_interface.endpoints.size()) {
            *p_status = 0;
        }
        else {
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        }
    }

    virtual std::uint16_t request_get_status(std::uint32_t *p_status) {
        // GET_STATUS returns 0: host stacks (such as Windows) fundamentally depend on status queries;
        // 0 is a permissive "no special status" response (consistent with the kernel returning 0 for GET_STATUS when no get_status
        // callback exists)
        *p_status = 0;
        return 0;
    }
    virtual std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
        *p_status = 0;
        return 0;
    }

    /**
     * @brief this function is not necessary for all device,
     * HID device is required to implement this function
     * @param type
     * @param language_id
     * @param descriptor_length
     * @param p_status
     * @return
     */
    virtual data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                             std::uint16_t descriptor_length, std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {};
    }

    virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
        *p_status = 0;
    }
    virtual void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                              std::uint32_t *p_status) {
        *p_status = 0;
    }

    /**
     * @brief Only use for isochronous transfer, so give a default empty implement.
     * @param ep_address
     * @param p_status
     * @return
     */
    virtual std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address, std::uint32_t *p_status) {
        // Unimplemented: consistent with composite's STALL behavior for SYNC_FRAME
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return 0;
    }


    /// Class descriptors (placed after interface descriptors in configuration descriptors); empty by default means no class descriptors
    [[nodiscard]] virtual data_type get_class_specific_descriptor() {
        return {};
    }

    /**
     * @brief Whether class-specific descriptors are placed after every alternate setting
     *
     * Default false: only place at alt 0 (classes with large descriptors, such as UVC, would exceed
     * the 255-byte configuration descriptor limit if placed at every alt, causing truncated parsing failures on Windows).
     * UAC AS interface descriptors are small, and alt 1 must contain format descriptors for driver parsing, so return true.
     */
    [[nodiscard]] virtual bool put_class_specific_descriptor_in_all_alts() const {
        return false;
    }

    // ========== TransferOperator ==========

    TransferOperator *get_transfer_operator() {
        return transfer_op_.get();
    }

    void set_transfer_operator(std::unique_ptr<TransferOperator> op) {
        // Note: classes based on GenericTransfer, such as HID (GenericTransfer::from_handle in handle_control_urb,
        // handle_interrupt_transfer, and send_input_report
        // ), strictly require handles to be GenericTransfer.
        // The default GenericTransferOperator satisfies this assumption; only storage classes (MSC's
        // StorageTransferOperator, using Bulk without from_handle) are exempt. Unless you
        // know what you are doing, do not substitute a non-Generic
        // op for interfaces relying on GenericTransfer; otherwise the from_handle cast has undefined behavior
        transfer_op_ = std::move(op);
    }

    // ========== Utility functions ==========

    [[nodiscard]] virtual std::uint8_t get_string_interface_value() const {
        return string_interface;
    }

    [[nodiscard]] virtual std::wstring get_string_interface() const {
        return string_pool.get_string(string_interface).value_or(L"");
    }

    void change_string_interface(const std::wstring &new_str) {
        string_pool.change_string(string_interface, new_str);
    }

    /// Match iInterface with another handler (after USBCCGP removes IAD, interfaces of the same function are grouped by iInterface)
    void sync_string_interface_from(const VirtualInterfaceHandler &other) {
        string_interface = other.string_interface;
    }

protected:
    TransferResponder *responder = nullptr;
    VirtualDeviceHandler *device_handler = nullptr;
    std::unique_ptr<TransferOperator> transfer_op_;

    std::uint8_t string_interface;

    StringPool &string_pool;
};

} // namespace usbipdcpp
