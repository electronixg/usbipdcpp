#pragma once

#include "usbipdcpp/Export.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/StringPool.h"

namespace usbipdcpp {
struct UsbIpIsoPacketDescriptor;
struct UsbEndpoint;
struct SetupPacket;
struct UsbInterface;

class TransferResponder;
class TransferHandle;


/**
 * @brief Inherit the VirtualInterfaceHandler class, do not inherit this class
 */
class USBIPDCPP_API AbstInterfaceHandler {
public:
    explicit AbstInterfaceHandler(UsbInterface &handle_interface) :
        handle_interface(handle_interface) {
    }

    /**
     * @brief This function will be called when a new client connects
     * @param responder transfer response interface (used to submit response/stop transfer)
     * @param ec the ec that occurred
     */
    virtual void on_new_connection(TransferResponder &responder, error_code &ec) =0;

    /**
     * @brief returns a read-only reference to the interface to which it belongs.
     *
     * Access all interface description information (interface number/endpoints, etc.) through this; do not add field-level
     * getters to this class. When a descriptor spanning interfaces needs to refer to another interface (such as UAC AC Header's
     * baInterfaceNr), the assembler associates the other handler; obtain the information through
     * `handler->get_interface()`
     * @return const reference of the interface to which it belongs
     */
    [[nodiscard]] const UsbInterface &get_interface() const {
        return handle_interface;
    }

    /**
     * @brief This function is called when the transfer needs to be completely terminated due to errors, client detach, active server shutdown, etc. Messages cannot be submitted after being called.
     */
    virtual void on_disconnection(error_code &ec) =0;
    /**
     * @brief All seqnum will call this function, please make sure to only process your own seqnum
     * @param unlink_seqnum Sequence number of the packet to cancel
     * @param cmd_seqnum CMD_UNLINK The sequence number of the command (used to construct RET_UNLINK)
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum);

    virtual ~AbstInterfaceHandler() = default;

protected:
    UsbInterface &handle_interface;
};

}
