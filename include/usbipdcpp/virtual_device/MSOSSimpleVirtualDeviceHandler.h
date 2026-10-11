#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "usbipdcpp/virtual_device/MsOsConstants.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

namespace usbipdcpp {
/**
 * @brief Simple device handler supporting Microsoft OS 1.0 descriptor
 *
 * Windows relies on MS OS descriptor to load built-in drivers for some device classes (such as RNDIS network cards):
 * After  detects the string index 0xEE ("MSFT100" signature string) successfully, then use bMS_VendorCode to request
 * Compatible ID feature descriptor, bind the corresponding driver according to the Compatible ID - without it, RNDIS
 * device will be mistakenly bound to the serial port by usbser (actually the COM port appears instead of the network card). Align kernel gadget
 * configfs os_desc support for  (usb_os_string/fill_ext_compat for composite.c).
 *
 * is not called by default set_ms_os_compatible_id = does not provide MS OS descriptor (same behavior
 * SimpleVirtualDeviceHandler).
 */
class USBIPDCPP_API MSOSSimpleVirtualDeviceHandler : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;

    /**
     * @brief Set MS OS 1.0 Compatible ID (such as "RNDIS"), enable MS OS descriptor response
     * @param compatible_id ASCII compatible ID of at most 8 bytes (zero-pad if shorter)
     */
    void set_ms_os_compatible_id(std::string compatible_id) {
        ms_os_compatible_id_ = std::move(compatible_id);
    }

    /**
     * @brief Set MS OS 1.0 SubCompatibleID (such as "5162001" for RNDIS)
     *
     * Windows rndismp driver matches `USB\MS_COMP_RNDIS&MS_SUBCOMP_5162001`
     * (rndiscmp.inf) - If the SubCompatibleID is empty, the match fails and the device returns to usbser.
     * Real RNDIS devices (Android phones, etc.) have "5162001" in their responses and must be aligned
     * @param sub_compatible_id ASCII sub-compatible ID of at most 8 bytes (zero-pad if shorter)
     */
    void set_ms_os_sub_compatible_id(std::string sub_compatible_id) {
        ms_os_sub_compatible_id_ = std::move(sub_compatible_id);
    }

    /// Currently configured Compatible ID (empty = MS OS descriptor not enabled)
    [[nodiscard]] const std::string &get_ms_os_compatible_id() const {
        return ms_os_compatible_id_;
    }

    /// Currently configured SubCompatibleID
    [[nodiscard]] const std::string &get_ms_os_sub_compatible_id() const {
        return ms_os_sub_compatible_id_;
    }

    /**
     * @brief Set the MS OS descriptor request number (bMS_VendorCode in the 0xEE signature string,
     * defaults to 0x01, aligned with kernel configfs convention). Must be set before connecting
     */
    void set_ms_os_vendor_code(std::uint8_t vendor_code) {
        ms_os_vendor_code_ = vendor_code;
    }

    /// Current MS OS descriptor request number (bMS_VendorCode)
    [[nodiscard]] std::uint8_t get_ms_os_vendor_code() const {
        return ms_os_vendor_code_;
    }

    /// MS OS 1.0 signature string (string index 0xEE: "MSFT100" UTF-16LE + bMS_VendorCode)
    std::optional<data_type> get_special_string_descriptor(std::uint8_t string_index) override;

protected:
    /// MS OS 1.0 Compatible ID feature descriptor (Vendor|Device|IN + bMS_VendorCode/
    /// wIndex=4); when there is no match, go to EPIPE of SimpleVirtualDeviceHandler
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

private:
    /// MS OS 1.0 Compatible ID (empty = no MS OS descriptor provided)
    std::string ms_os_compatible_id_;
    /// MS OS 1.0 SubCompatibleID (default is empty; RNDIS requires "5162001" to match rndismp)
    std::string ms_os_sub_compatible_id_;
    /// MS OS descriptor request number (bMS_VendorCode in the 0xEE signature string)
    std::uint8_t ms_os_vendor_code_ = MS_OS_DEFAULT_VENDOR_CODE;
};
} // namespace usbipdcpp
