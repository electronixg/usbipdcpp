#pragma once

#include <cstddef>
#include <cstdint>
#include <system_error>

#include <asio.hpp>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"

namespace usbipdcpp {

/**
 * @brief Abstract base class for transfer operators
 *
 * Encapsulates transfer_handle creation, reading/writing, and release for delegation by AbstDeviceHandler.
 * Each use case has its own implementation: GenericTransferOperator (default), libusb, zero-copy storage, etc.
 */
class USBIPDCPP_API TransferOperator {
public:
    virtual ~TransferOperator() = default;

    /**
     * @brief Create a transfer handle
     *
     * The implementation must record the transfer direction in the returned transfer structure (using header.direction
     * to determine in/out): transfer_is_in depends on this information; RET_SUBMIT::to_socket uses
     * the direction to determine the length of returned data (always 0 for OUT, which returns no data). If the direction is not stored, an OUT
     * response may mistakenly send already received data back to the client as response data.
     */
    /**
     * @brief Create a transfer handle
     *
     * The implementation must record the transfer direction (in/out, from header.direction). The storage
     * method is unrestricted (a transfer structure field, map, etc.); transfer_is_in
     * depends on it: RET_SUBMIT::to_socket determines the returned data length from the direction, always 0 for OUT.
     */
    virtual void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                        const SetupPacket &setup_packet) = 0;
    virtual void free_transfer_handle(void *handle) = 0;

    virtual std::size_t get_actual_length(void *handle) = 0;

    virtual UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) = 0;
    virtual void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) = 0;

    /**
     * @brief Fill handle with data (prepare IN response data, e.g. a virtual device's pending-request/response mechanism)
     *
     * Called by the response paths of components such as InEndpointChannel: this method writes buffered data into
     * the private transfer (truncated to max_length; the actual amount written depends on the transfer capacity,
     * usually min(data.size(), max_length)), and updates actual_length.
     * The caller guarantees that handle was created by this operator's alloc_transfer_handle.
     *
     * Each implementation handles its own transfer internals (GenericTransfer fills the data vector;
     * libusb_transfer fills buffer); the component makes no assumptions about fields.
     *
     * The default no-op implementation returns 0: used only by operators such as libusb that do not participate in the pending-request/response mechanism (their data
     * paths do not pass through InEndpointChannel). Operators that actually populate data (such as
     * GenericTransferOperator) must override this method
     *
     * @return Actual number of bytes written (0 in the default implementation)
     */
    virtual std::size_t set_transfer_data(void *handle, const data_type &data, std::size_t max_length) {
        return 0;
    }

    /**
     * @brief Read received data from handle and append it to out (OUT data consumption)
     *
     * Called by the consumption paths of components such as OutEndpointChannel: when the application takes a pending OUT request,
     * this method reads its data (appending it, using the actual received length).
     * The caller guarantees that handle was created by this operator's alloc_transfer_handle.
     *
     * The default implementation sets supported=false (does not participate in the pending-request/consumption mechanism; used only by operators such as libusb);
     * operators that actually read data (such as GenericTransferOperator) set supported=true and append the data
     *
     * @param out Output buffer; data is appended to its end
     * @param supported Output parameter: whether this operator supports reading
     * @return Number of bytes appended
     */
    virtual std::size_t get_transfer_data(void *handle, data_type &out, bool &supported) {
        supported = false;
        return 0;
    }

    /**
     * @brief Check whether the transfer represented by handle is IN (the direction that returns data)
     *
     * Called by RET_SUBMIT::to_socket() when calculating length for send_transfer_data:
     * IN passes actual_length (data must be returned); OUT passes 0 (no data is returned).
     * The direction is determined from CMD_SUBMIT's direction during alloc_transfer_handle;
     * each implementation obtains it from its own transfer structure (Generic stores a field; libusb checks endpoint).
     */
    virtual bool transfer_is_in(void *handle) = 0;

    /**
     * @brief Send transfer data (IN direction: server → client)
     *
     * Called by RET_SUBMIT::to_socket() after writing the USBIP header.
     * The caller guarantees that handle was created by this operator's alloc_transfer_handle.
     *
     * The implementation must follow these steps exactly and perform no additional reads/writes on sock, to avoid misaligning the protocol stream:
     *
     * 1. If length > 0, send length bytes of data from the private transfer to sock.
     *    The caller (RET_SUBMIT::to_socket) calculates length using transfer_is_in:
     *    actual_length for IN; always 0 for OUT (OUT returns no data), so this step is skipped for OUT.
     *
     * 2. Send N UsbIpIsoPacketDescriptor entries, where N is num_iso_packets supplied to alloc_transfer_handle.
     *    For each UsbIpIsoPacketDescriptor in a for loop, call to_bytes()
     *    to obtain network byte order, then write to sock.
     *    For non-isochronous transfers, N is 0 or 0xFFFFFFFF; skip this step.
     *
     * All iso packets are packed contiguously on the wire; do not insert gaps between the byte ranges described by the descriptors.
     *
     * See GenericTransferOperator and LibusbTransferOperator for concrete implementations.
     */
    virtual void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                    std::error_code &ec) = 0;

    /**
     * @brief Receive transfer data (OUT direction: client → server, including IN isochronous descriptors)
     *
     * Called by CMD_SUBMIT::from_socket() after reading the USBIP header.
     * The caller guarantees that handle was created by this operator's alloc_transfer_handle.
     *
     * The implementation must follow these steps exactly and perform no additional reads/writes on sock, to avoid misaligning the protocol stream:
     *
     * 1. If length > 0, read length bytes from sock and write them into the private transfer.
     *    For OUT transfers, length is transfer_buffer_length; for IN transfers, it is always 0, so this step is skipped.
     *
     * 2. Continue by reading N descriptors from sock, where N is num_iso_packets supplied to alloc_transfer_handle:
     *    UsbIpIsoPacketDescriptor entries. In a for loop, use
     *    UsbIpIsoPacketDescriptor::from_socket(sock) to read them,
     *    and use set_iso_descriptor to write them into the corresponding iso packet descriptor section of the private transfer.
     *    For non-isochronous transfers, N is 0 or 0xFFFFFFFF; skip this step.
     *
     * All iso packets are packed contiguously on the wire; do not insert gaps between the byte ranges described by the descriptors.
     *
     * See GenericTransferOperator and LibusbTransferOperator for concrete implementations.
     */
    virtual void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                    std::error_code &ec) = 0;

    /**
     * @brief Return the leaf TransferOperator for the specified endpoint
     *
     * Used for endpoint routing in from_socket: a routing-layer operator (such as VirtualDeviceTransferOperator) uses this method
     * to return the final leaf operator for ep; the caller then performs alloc / I/O directly on that leaf operator without further map lookups.
     * Operators outside the routing layer simply return this.
     */
    virtual TransferOperator *get_operator_for_ep(std::uint8_t ep) {
        return this;
    }
};

/**
 * @brief Default transfer operator based on GenericTransfer
 *
 * Identical to AbstDeviceHandler's original default implementation: creates GenericTransfer,
 * stores data in a vector, and uses asio to read/write the full data in send/recv.
 */
class USBIPDCPP_API GenericTransferOperator : public TransferOperator {
public:
    void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                const SetupPacket &setup_packet) override;
    void free_transfer_handle(void *handle) override;

    std::size_t get_actual_length(void *handle) override;

    bool transfer_is_in(void *handle) override;

    UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) override;
    void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) override;

    std::size_t set_transfer_data(void *handle, const data_type &data, std::size_t max_length) override;
    std::size_t get_transfer_data(void *handle, data_type &out, bool &supported) override;

    void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
    void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
};

} // namespace usbipdcpp
