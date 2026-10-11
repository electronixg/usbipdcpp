// See https://www.kernel.org/doc/html/latest/usb/usbip_protocol.html for the detailed definitions

#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <system_error>
#include <variant>
#include <vector>

#include <asio.hpp>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/network.h"

// Maximum transfer buffer size (to prevent maliciously large memory allocations)
#ifndef USBIPDCPP_MAX_TRANSFER_BUFFER_SIZE
#define USBIPDCPP_MAX_TRANSFER_BUFFER_SIZE (16 * 1024 * 1024) // 16MB
#endif

// Maximum number of isochronous transfer packets (to prevent maliciously large memory allocations)
// Real devices use at most tens to hundreds of isochronous packets per URB; the 1024 limit leaves ample headroom
#ifndef USBIPDCPP_MAX_ISO_PACKETS
#define USBIPDCPP_MAX_ISO_PACKETS 1024
#endif

namespace usbipdcpp {
constexpr std::uint16_t USBIP_VERSION = 0x0111;


class AbstDeviceHandler; // Forward declaration
class TransferOperator; // Forward declaration


constexpr std::uint16_t OP_REQ_DEVLIST = 0x8005;
constexpr std::uint16_t OP_REP_DEVLIST = 0x0005;
constexpr std::uint16_t OP_REQ_IMPORT = 0x8003;
constexpr std::uint16_t OP_REP_IMPORT = 0x0003;


constexpr std::uint32_t USBIP_CMD_SUBMIT = 0x0001;
constexpr std::uint32_t USBIP_CMD_UNLINK = 0x0002;
constexpr std::uint32_t USBIP_RET_SUBMIT = 0x0003;
constexpr std::uint32_t USBIP_RET_UNLINK = 0x0004;

enum UsbIpDirection {
    Out = 0,
    In = 1,
};

enum class ErrorType {
    OK = 0,
    UNKNOWN_VERSION,
    UNKNOWN_CMD,
    PROTOCOL_ERROR,
    NO_DEVICE,
    SOCKET_EOF,
    SOCKET_ERR,
    INTERNAL_ERROR,
    INVALID_ARG,
    UNIMPLEMENTED,
    TRANSFER_ERROR,
};

// tools/usbip_common.h
enum class OperationStatuType {
    // Request Completed Successfully
    OK = 0,
    // Request Failed
    NA,
    // Device busy (exported)
    DevBusy,
    // Device in error state
    DevError,
    // Device not found
    NoDev,
    // Unexpected response
    Error
};

enum class UrbStatusType {
    StatusOK = -0,
    StatusECONNRESET = -104,
    StatusEPIPE = -32,
    StatusESHUTDOWN = -108,
    StatusENODEV = -19,
    StatusENOENT = -2,
    StatusETIMEDOUT = -110,
    StatusEEOVERFLOW = -75
};

class USBIPDCPP_API TransferErrorCategory : public std::error_category {
public:
    [[nodiscard]] const char *name() const noexcept override;
    [[nodiscard]] std::string message(int _Errval) const override;
};

const TransferErrorCategory g_error_category;
USBIPDCPP_API std::error_code make_error_code(ErrorType e);


struct USBIPDCPP_API UsbIpHeaderBasic {
    /**
     * This field does not need to be read from the socket; it is set by the subcommand.
     * Determine which packet to create from the fields read first
     */
    std::uint32_t command;
    std::uint32_t seqnum;
    std::uint32_t devid;
    std::uint32_t direction;
    /// USB/IP wire-format endpoint number, without the direction bit: direction is specified by the direction field;
    /// both senders (Linux vhci / usbip-win2) use usb_endpoint_num (the low 4 bits).
    /// The server reconstructs the actual endpoint address by adding the direction bit according to direction (|0x80 for In).
    /// Parsing checks ep <= 0x7F and rejects out-of-range values as protocol errors (see UsbIpCmdSubmit::from_socket)
    std::uint32_t ep;

    [[nodiscard]] array_data_type<calculate_total_size_with_array<decltype(command), decltype(seqnum), decltype(devid),
                                                                  decltype(direction), decltype(ep)>()>
    to_bytes() const;
    void from_socket(asio::ip::tcp::socket &sock);

    void set_as_server() {
        devid = 0;
        direction = 0;
        ep = 0;
    }

    static UsbIpHeaderBasic get_server_header(std::uint32_t command, std::uint32_t seqnum) {
        return UsbIpHeaderBasic{.command = command, .seqnum = seqnum, .devid = 0, .direction = 0, .ep = 0};
    }
};

static_assert(Serializable<UsbIpHeaderBasic>);


struct USBIPDCPP_API UsbIpIsoPacketDescriptor {
    std::uint32_t offset; ///< Starting position of this packet's data in the USB request buffer (advances by length; not the packed wire offset)
    std::uint32_t length; ///< Size of the slot allocated for this packet in buffer (determined by the client; may exceed actual_length, with gaps between packets)
    std::uint32_t actual_length; ///< Number of valid data bytes in the packet (≤ length); only this data is transmitted, packed contiguously, on the wire
    std::uint32_t status; ///< Transfer status of this packet (URB status code)

    [[nodiscard]] array_data_type<calculate_total_size_with_array<decltype(offset), decltype(length),
                                                                  decltype(actual_length), decltype(status)>()>
    to_bytes() const;
    void from_socket(asio::ip::tcp::socket &sock);
};

static_assert(Serializable<UsbIpIsoPacketDescriptor>);

/**
 * @brief Transfer object type constraint: the handle pointer can be retrieved as TransferType*
 *
 * @tparam TransferType Transfer object type (the concrete type held by TransferHandle,
 *          such as GenericTransfer or StorageIoTransfer)
 * @tparam FromHandleProvider Type providing a static from_handle(void *) that returns TransferType*
 *          Usually the same as TransferType (the type itself provides from_handle),
 *          but may be separate: for example, member functions cannot be added to a third-party C type (libusb_transfer),
 *          so from_handle is provided by a wrapper type. Project convention: after defining any custom transfer type,
 *          static_assert this constraint to prevent code that depends on the concrete handle type from
 *          invoking UB when retrieving it through a cast
 */
template <typename TransferType, typename FromHandleProvider>
concept FromHandleTransfer = requires(void *ptr) {
    { FromHandleProvider::from_handle(ptr) } -> std::same_as<TransferType *>;
};

// Generic transfer structure used by virtual devices
struct GenericTransfer {
    std::vector<std::uint8_t> data;
    std::vector<UsbIpIsoPacketDescriptor> iso_descriptors;
    std::size_t actual_length = 0;
    std::size_t data_offset = 0;
    // Transfer direction (CMD_SUBMIT's direction, determined during alloc): true=IN, data must be returned
    bool is_in = true;

    static GenericTransfer *from_handle(void *ptr) {
        return static_cast<GenericTransfer *>(ptr);
    }
};

// After defining a custom transfer type, static_assert it according to the project convention (see the FromHandleTransfer comment)
static_assert(FromHandleTransfer<GenericTransfer, GenericTransfer>);

/**
 * @brief RAII wrapper managing the lifetime of transfer_handle
 *
 * Holds the handle pointer and its TransferOperator; destruction automatically calls
 * op_->free_transfer_handle(handle_) to release resources. Movable, but not copyable.
 *
 * Usage rules:
 * - Takes ownership on construction: TransferHandle handle(ptr, op);
 * - Automatically releases on destruction; no manual management required
 * - Can transfer ownership to another TransferHandle using std::move()
 * - Calling release() relinquishes ownership; the caller must release the resource manually
 *
 * Typical usage:
 * @code
 *   void* ptr = op->alloc_transfer_handle(1024, 0, header, setup);
 *   TransferHandle handle(ptr, op);  // Take ownership
 *   / / Read/write data through op->send_transfer_data / op->recv_transfer_data
 *   / / At function exit, handle is destroyed, automatically calling op->free_transfer_handle(ptr)
 * @endcode
 */
class USBIPDCPP_API TransferHandle {
    void *handle_ = nullptr;
    TransferOperator *op_ = nullptr;

public:
    TransferHandle() = default;

    /**
     * @brief Construct and take ownership
     * @param handle Pointer returned by op->alloc_transfer_handle()
     * @param op     TransferOperator that created this handle, used for release and subsequent I/O
     */
    TransferHandle(void *handle, TransferOperator *op);

    // Copying is prohibited (exclusive ownership)
    TransferHandle(const TransferHandle &) = delete;
    TransferHandle &operator=(const TransferHandle &) = delete;

    /**
     * @brief Move construction, transferring ownership
     * @param other Source object, left empty after the move
     */
    TransferHandle(TransferHandle &&other) noexcept;
    TransferHandle &operator=(TransferHandle &&other) noexcept;

    /**
     * @brief Automatically release handle on destruction
     *
     * If handle_ and op_ are both non-null, call op_->free_transfer_handle(handle_)
     */
    ~TransferHandle();

    /**
     * @brief Release the currently owned handle and clear it
     *
     * Calls op_->free_transfer_handle(handle_), then sets handle_ and op_ to null.
     * Calling this function on an empty object is safe (a no-op).
     */
    void reset();

    /**
     * @brief Get the raw pointer (without transferring ownership)
     * @return Raw pointer, possibly nullptr
     *
     * Note: TransferHandle manages the returned pointer's lifetime; do not release it externally.
     */
    [[nodiscard]] void *get() const {
        return handle_;
    }

    /**
     * @brief Get the TransferOperator that created this handle
     * @return TransferOperator pointer for I/O operations such as send / recv
     *
     * After alloc completes in for_socket, op points to the final leaf operator (such as StorageTransferOperator);
     * subsequent I/O operations use this op directly, without VirtualDeviceTransferOperator's map lookups.
     */
    [[nodiscard]] TransferOperator *get_operator() const {
        return op_;
    }

    /**
     * @brief Check whether a valid handle is owned
     * @return true if a valid handle is owned
     */
    explicit operator bool() const {
        return handle_ != nullptr;
    }

    /**
     * @brief Set the routing TransferOperator (called before from_socket)
     *
     * Sets the routing-layer op (such as VirtualDeviceTransferOperator) before protocol deserialization;
     * from_socket obtains the leaf op via get_operator_for_ep, then replaces it using set_handle.
     */
    void set_operator(TransferOperator *op) {
        op_ = op;
    }

    /**
     * @brief Set both handle and its TransferOperator
     *
     * Called after alloc completes in from_socket, replacing the routing-layer op with the final leaf operator.
     */
    void set_handle(void *handle, TransferOperator *op) {
        handle_ = handle;
        op_ = op;
    }

    /**
     * @brief Relinquish ownership and return the raw pointer
     * @return Raw pointer; the caller must release it manually
     *
     * Warning: after this call, TransferHandle no longer manages the handle;
     * the caller must ensure that op->free_transfer_handle() is called to release resources,
     * otherwise memory will leak!
     *
     * @code
     *   void* ptr = handle.release();
     *   / / ... use ptr ...
     *   handle.get_operator()->free_transfer_handle(ptr);  // Must release manually!
     * @endcode
     */
    void *release();
};

namespace UsbIpCommand {
    struct USBIPDCPP_API OpReqDevlist {
        std::uint32_t status;

        [[nodiscard]] array_data_type<
                calculate_total_size_with_array<decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST), decltype(status)>()>
        to_bytes() const;
        void from_socket(asio::ip::tcp::socket &sock);
    };

    static_assert(Serializable<OpReqDevlist>);

    struct USBIPDCPP_API OpReqImport {
        std::uint32_t status;
        array_data_type<32> busid;

        [[nodiscard]] array_data_type<calculate_total_size_with_array<decltype(USBIP_VERSION), decltype(OP_REQ_IMPORT),
                                                                      decltype(status), decltype(busid)>()>
        to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);
    };

    static_assert(SerializableFromSocket<OpReqImport>);

    struct USBIPDCPP_API UsbIpCmdSubmit {
        UsbIpHeaderBasic header;
        std::uint32_t transfer_flags;
        // Specifies the maximum amount of transfer data
        std::uint32_t transfer_buffer_length;
        std::uint32_t start_frame;
        // Number of isochronous transfer packets
        std::uint32_t number_of_packets;
        std::uint32_t interval;
        SetupPacket setup;
        // For IN, transfer_buffer_length==data.size(); for OUT, IN-direction transfer_buffer_length=0

        // RAII-wrapped transfer_handle
        mutable TransferHandle transfer;

        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        // This function reads only some values, not the subsequent data section; it may be called only once per object.
        // Before calling this function, ensure that transfer has a TransferOperator set; otherwise there will be a null pointer
        void from_socket(asio::ip::tcp::socket &sock);
    };

    static_assert(SerializableFromSocket<UsbIpCmdSubmit>);

    struct USBIPDCPP_API UsbIpCmdUnlink {
        UsbIpHeaderBasic header;
        std::uint32_t unlink_seqnum;

        [[nodiscard]] array_data_type<
                calculate_total_size_with_array<decltype(UsbIpHeaderBasic{}.to_bytes()), decltype(unlink_seqnum)>() +
                24>
        to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        // May be called only once per object
        void from_socket(asio::ip::tcp::socket &sock);
    };

    static_assert(SerializableFromSocket<UsbIpCmdUnlink>);

    using OpCmdVariant = std::variant<OpReqDevlist, OpReqImport>;
    using CmdVariant = std::variant<UsbIpCmdSubmit, UsbIpCmdUnlink>;


    /**
     * @brief Returns an empty value only when ec is set; with no error in ec, a value is guaranteed and no second check is needed
     * @param sock
     * @param ec
     * @return The received command
     */
    USBIPDCPP_API usbipdcpp::UsbIpCommand::OpCmdVariant get_op_from_socket(asio::ip::tcp::socket &sock,
                                                                           usbipdcpp::error_code &ec);


    /**
     * @brief Returns an empty value only when ec is set; with no error in ec, a value is guaranteed and no second check is needed
     * @param sock
     * @param handler Used to create transfer_handle
     * @param ec
     * @return The received command
     */
    USBIPDCPP_API usbipdcpp::UsbIpCommand::CmdVariant
    get_cmd_from_socket(asio::ip::tcp::socket &sock, AbstDeviceHandler *handler, usbipdcpp::error_code &ec);
} // namespace UsbIpCommand

namespace UsbIpResponse {
    struct USBIPDCPP_API OpRepDevlist {
        std::uint32_t status;
        std::uint32_t device_count;
        std::vector<UsbDevice> devices;

        [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);

        static OpRepDevlist create_from_devices(const std::vector<std::shared_ptr<UsbDevice>> &devices);
    };

    static_assert(SerializableFromSocket<OpRepDevlist>);

    struct USBIPDCPP_API OpRepImport {
        std::uint32_t status;
        std::shared_ptr<UsbDevice> device;

        [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);

        static OpRepImport create_on_failure_with_status(std::uint32_t status);
        static OpRepImport create_on_failure();
        /**
         * @brief A shared pointer copy construct that shares the same pointed object
         * @param device
         * @return
         */
        static OpRepImport create_on_success(std::shared_ptr<UsbDevice> device);
    };

    static_assert(SerializableFromSocket<OpRepImport>);

    struct USBIPDCPP_API UsbIpRetSubmit {
        UsbIpHeaderBasic header;
        std::uint32_t status;
        std::uint32_t actual_length;
        std::uint32_t start_frame;
        std::uint32_t number_of_packets;
        std::uint32_t error_count;

        // RAII-wrapped transfer_handle
        // Do not assign if there is no data stage (actual_length == 0)
        mutable TransferHandle transfer;

        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);

        /**
         * @brief Create a RET_SUBMIT response (taking ownership of transfer)
         */
        static UsbIpRetSubmit create_ret_submit(std::uint32_t seqnum, std::uint32_t status, std::uint32_t actual_length,
                                                std::uint32_t start_frame, std::uint32_t number_of_packets,
                                                TransferHandle transfer);
        /**
         * @brief Create a successful RET_SUBMIT response (no data; does not take ownership of transfer)
         */
        static UsbIpRetSubmit create_ret_submit_ok_without_data(std::uint32_t seqnum, std::uint32_t actual_length);

        /**
         * @brief Create a RET_SUBMIT response with status (no data; does not take ownership of transfer)
         */
        static UsbIpRetSubmit create_ret_submit_with_status_and_no_data(std::uint32_t seqnum, std::uint32_t status,
                                                                        std::uint32_t actual_length);
        /**
         * @brief Create a RET_SUBMIT response with status (no isochronous packets; takes ownership of transfer)
         */
        static UsbIpRetSubmit create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum, std::uint32_t status,
                                                                       std::uint32_t actual_length,
                                                                       TransferHandle transfer);
        /**
         * @brief Create a RET_SUBMIT response with EPIPE status (taking ownership of transfer)
         */
        static UsbIpRetSubmit create_ret_submit_epipe_no_iso(std::uint32_t seqnum, std::uint32_t actual_length,
                                                             TransferHandle transfer);
        /**
         * @brief Create a RET_SUBMIT response with EPIPE status (no data; does not take ownership of transfer)
         */
        static UsbIpRetSubmit create_ret_submit_epipe_without_data(std::uint32_t seqnum, std::uint32_t actual_length);
        /**
         * @brief Create a successful RET_SUBMIT response (no isochronous packets; takes ownership of transfer)
         */
        static UsbIpRetSubmit create_ret_submit_ok_with_no_iso(std::uint32_t seqnum, std::uint32_t actual_length,
                                                               TransferHandle transfer);
    };

    static_assert(SerializableFromSocket<UsbIpRetSubmit>);

    struct USBIPDCPP_API UsbIpRetUnlink {
        UsbIpHeaderBasic header;
        std::uint32_t status;

        [[nodiscard]] array_data_type<
                calculate_total_size_with_array<decltype(UsbIpHeaderBasic{}.to_bytes()), decltype(status)>() + 24>
        to_bytes() const;
        void to_socket(asio::ip::tcp::socket &sock, error_code &ec) const;
        void from_socket(asio::ip::tcp::socket &sock);

        static UsbIpRetUnlink create_ret_unlink(std::uint32_t seqnum, std::uint32_t status);
        static UsbIpRetUnlink create_ret_unlink_success(std::uint32_t seqnum);
    };

    static_assert(SerializableFromSocket<UsbIpRetUnlink>);

    using OpRepVariant = std::variant<OpRepDevlist, OpRepImport>;
    using RetVariant = std::variant<UsbIpRetSubmit, UsbIpRetUnlink>;
    using AllRepVariant = std::variant<OpRepDevlist, OpRepImport, UsbIpRetSubmit, UsbIpRetUnlink>;
} // namespace UsbIpResponse


} // namespace usbipdcpp
