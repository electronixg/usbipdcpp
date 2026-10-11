#pragma once

#include "usbipdcpp/virtual_device/MSOSSimpleVirtualDeviceHandler.h"

namespace usbipdcpp {
/**
 * @brief RNDIS dedicated MS OS device handler (final)
 *
 * Default MS OS 1.0 Compatible ID "RNDIS" + SubCompatibleID "5162001": Windows
 * The rndismp network card driver of  matches `USB\MS_COMP_RNDIS&MS_SUBCOMP_5162001`
 * (rndiscmp.inf), SubCompatibleID is empty, matching fails, and the device returns usbser and is bound to the serial port.
 * aligns the kernel f_rndis and binds ext_compat_id of os_desc into the function definition - RNDIS device
 * can be directly connected to with_handler <this category> during assembly, and the configuration is fixed and not exposed to the outside world.
 */
class USBIPDCPP_API RndisMsOsDeviceHandler final : public MSOSSimpleVirtualDeviceHandler {
public:
    RndisMsOsDeviceHandler(UsbDevice &handle_device, StringPool &string_pool) :
        MSOSSimpleVirtualDeviceHandler(handle_device, string_pool) {
        set_ms_os_compatible_id("RNDIS");
        set_ms_os_sub_compatible_id("5162001");
    }

private:
    // The MS OS configuration of RNDIS is part of the function definition (fixed by default): the configuration of the base class
    // setter is hidden as private to prevent external changes from destroying rndismp matching
    using MSOSSimpleVirtualDeviceHandler::set_ms_os_compatible_id;
    using MSOSSimpleVirtualDeviceHandler::set_ms_os_sub_compatible_id;
    using MSOSSimpleVirtualDeviceHandler::set_ms_os_vendor_code;
};
} // namespace usbipdcpp
