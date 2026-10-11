#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

#include <algorithm>

#include "usbipdcpp/Session.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

using namespace usbipdcpp;

namespace usbipdcpp {

/**
 * @brief Pipe interface handler: all endpoints in the interface are forwarded uniformly to the device-level PipeDeviceHandler
 * (data-plane state is centralized at device level; read/write from any interface obtain the unified data stream)
 * Internal implementation class: declared in the source file, matching the header's friend declaration
 */
class PipeInterfaceHandler : public VirtualInterfaceHandler {
public:
    PipeInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool, PipeDeviceHandler *pipe) :
        VirtualInterfaceHandler(handle_interface, string_pool), pipe(pipe) {
    }

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override {
        forward_transfer(seqnum, ep, transfer_buffer_length, std::move(transfer));
    }

    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override {
        forward_transfer(seqnum, ep, transfer_buffer_length, std::move(transfer));
    }

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override {
        pipe->on_pipe_control_request(setup_packet, seqnum, transfer_buffer_length, std::move(transfer));
    }

    void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup_packet,
                                                                  TransferHandle transfer,
                                                                  std::error_code &ec) override {
        pipe->on_pipe_control_request(setup_packet, seqnum, transfer_buffer_length, std::move(transfer));
    }

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {
        pipe->on_pipe_unlink(unlink_seqnum, cmd_seqnum);
    }

    // ========== Standard request callbacks (unset callbacks use base defaults: feature/status succeed; unimplemented query requests return EPIPE) ==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.clear_feature) {
            pipe->standard_request_handler.clear_feature(*pipe, feature_selector, p_status);
        }
        else {
            VirtualInterfaceHandler::request_clear_feature(feature_selector, p_status);
        }
    }

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.endpoint_clear_feature) {
            pipe->standard_request_handler.endpoint_clear_feature(*pipe, feature_selector, ep_address, p_status);
        }
        else {
            VirtualInterfaceHandler::request_endpoint_clear_feature(feature_selector, ep_address, p_status);
        }
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.get_interface) {
            return pipe->standard_request_handler.get_interface(*pipe, p_status);
        }
        return VirtualInterfaceHandler::request_get_interface(p_status);
    }

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.set_interface) {
            pipe->standard_request_handler.set_interface(*pipe, alternate_setting, p_status);
        }
        else {
            VirtualInterfaceHandler::request_set_interface(alternate_setting, p_status);
        }
    }

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.get_status) {
            return pipe->standard_request_handler.get_status(*pipe, p_status);
        }
        return VirtualInterfaceHandler::request_get_status(p_status);
    }

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.endpoint_get_status) {
            return pipe->standard_request_handler.endpoint_get_status(*pipe, ep_address, p_status);
        }
        return VirtualInterfaceHandler::request_endpoint_get_status(ep_address, p_status);
    }

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.set_feature) {
            pipe->standard_request_handler.set_feature(*pipe, feature_selector, p_status);
        }
        else {
            VirtualInterfaceHandler::request_set_feature(feature_selector, p_status);
        }
    }

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override {
        if (pipe->standard_request_handler.endpoint_set_feature) {
            pipe->standard_request_handler.endpoint_set_feature(*pipe, feature_selector, ep_address, p_status);
        }
        else {
            VirtualInterfaceHandler::request_endpoint_set_feature(feature_selector, ep_address, p_status);
        }
    }

    [[nodiscard]] data_type get_class_specific_descriptor() override {
        // The generic pipe itself has no class descriptors; devices requiring specific USB classes such as HID provide them through
        // set_class_specific_descriptor (e.g. HID descriptor 0x21)
        return pipe->class_specific_descriptor;
    }

    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override {
        // GET_DESCRIPTOR (recipient=interface): return custom descriptors by type (e.g. HID
        // report descriptor type=0x22); unset types return EPIPE (base default behavior)
        auto it = pipe->custom_descriptors.find(type);
        if (it != pipe->custom_descriptors.end()) {
            return it->second;
        }
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {};
    }

private:
    void forward_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_buffer_length,
                          TransferHandle transfer) {
        if (ep.is_in()) {
            // IN: the host requests data; forward to device level (FIFO matching or suspension)
            pipe->on_pipe_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
        }
        else {
            // OUT: suspend the entire request (data remains in transfer); respond when device-level read() takes it
            // (NAK backpressure: the host URB remains pending if the application does not read, matching kernel vudc)
            pipe->on_pipe_out_transfer(ep.address, seqnum, std::move(transfer));
        }
    }

    PipeDeviceHandler *pipe;
};

} // namespace usbipdcpp

PipeDeviceHandler::PipeDeviceHandler(UsbDevice &handle_device, StringPool &string_pool) :
    SimpleVirtualDeviceHandler(handle_device, string_pool) {
}

void PipeDeviceHandler::setup_interface_handlers() {
    // Force all interfaces to pipe interface handlers: data plane/standard requests uniformly use pipe forwarding
    // (read/write obtain all endpoint data streams); mixing other interface handlers is forbidden,
    // otherwise read/write misses some endpoints. Create them at this stage rather than in the constructor,
    // because interface handlers hold a pointer to this object, which must be fully constructed first
    for (auto &intf: handle_device.interfaces) {
        intf.with_handler<PipeInterfaceHandler>(string_pool, this);
    }
    VirtualDeviceHandler::setup_interface_handlers();
}

bool PipeDeviceHandler::read(PipeXfer &xfer, std::uint32_t timeout_ms) {
    // Take from each endpoint's OUT channel (respond when taking a suspended request). Retrieved data includes:
    // bulk OUT payloads, control OUT data (with setup_req), and control IN notifications only
    // (with setup_req, no data). Retrieval order = global host-request arrival order: kernel vhci's
    // seqnum increases monotonically globally; take the oldest seqnum across endpoints (seqnum_newer
    // uses circular comparison to handle wraparound; strictly FIFO within each endpoint)
    std::unique_lock lock(pipe_mutex);
    auto take_one = [&]() -> bool {
        // Find the channel with the oldest seqnum (globally oldest request)
        OutEndpointChannel *oldest = nullptr;
        for (auto &[ep, ch]: out_channels) {
            auto sn = ch.front_seqnum();
            if (!sn) {
                continue;
            }
            if (!oldest || seqnum_newer(*oldest->front_seqnum(), *sn)) {
                oldest = &ch;
            }
        }
        if (!oldest) {
            return false;
        }
        auto p = oldest->try_take();
        if (p) {
            xfer.ep = p->ep;
            xfer.setup_req = p->setup_req;
            xfer.data = std::move(p->data);
            return true;
        }
        return false;
    };

    while (!disconnected) {
        if (take_one()) {
            return true;
        }
        // Wait for data or disconnection (on disconnection, consume already queued data before returning false)
        if (timeout_ms == 0) {
            out_cv.wait(lock);
        }
        else {
            if (out_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
                // Data may be queued exactly at timeout: break out for the common check to avoid losing data
                break;
            }
        }
    }
    // Disconnection or timeout: consume already queued data before returning false
    if (take_one()) {
        return true;
    }
    return false;
}

std::size_t PipeDeviceHandler::write(const PipeXfer &xfer, std::uint32_t timeout_ms) {
    if (xfer.ep != 0 && (xfer.ep & 0x80) == 0) {
        // The target is neither an IN endpoint (including direction bit) nor a control response: the host will not send it IN requests,
        // so data remains in FIFO; this is user misuse, so provide a defensive diagnostic
        SPDLOG_WARN("PipeDeviceHandler::write target endpoint {:02x} is not an IN endpoint", xfer.ep);
    }
    ByteStreamInChannel *channel;
    {
        // Hold pipe_mutex only when obtaining the channel reference: release it during blocking writes, otherwise the host IN
        // request path (on_pipe_in_request also needs pipe_mutex) cannot respond and free
        // space, so the writer never wakes (deadlock)
        std::lock_guard lock(pipe_mutex);
        if (disconnected) {
            return 0;
        }
        channel = &get_in_channel(xfer.ep);
    }
    // Blocking write: the channel waits internally for space (host takes data when full; timeout_ms=0 waits indefinitely); on disconnection,
    // return the amount written. Reference stability: on disconnection, on_disconnection first visits channels, sets disconnection
    // flags, and wakes waiters (writers wake, check disconnection, return, and stop accessing channels), then clears the map
    return channel->write(xfer.data.data(), xfer.data.size(), timeout_ms);
}

ByteStreamInChannel &PipeDeviceHandler::get_in_channel(std::uint8_t ep_addr) {
    // The caller must already hold pipe_mutex
    auto it = in_channels.find(ep_addr);
    if (it == in_channels.end()) {
        it = in_channels.try_emplace(ep_addr).first;
        auto &ch = it->second;
        // Lazy creation for a new session: use current configured capacity, bind session, and reset disconnection state
        ch.set_capacity(fifo_capacity);
        ch.on_new_connection(responder);
    }
    return it->second;
}

OutEndpointChannel &PipeDeviceHandler::get_out_channel(std::uint8_t ep_addr) {
    // The caller must already hold pipe_mutex
    auto it = out_channels.find(ep_addr);
    if (it == out_channels.end()) {
        it = out_channels.try_emplace(ep_addr).first;
        auto &ch = it->second;
        // Lazy creation for a new session: bind session and reset disconnection state
        ch.on_new_connection(responder);
    }
    return it->second;
}

void PipeDeviceHandler::set_in_fifo_capacity(std::size_t capacity) {
    std::lock_guard lock(pipe_mutex);
    fifo_capacity = capacity;
}

void PipeDeviceHandler::set_class_specific_descriptor(data_type descriptor) {
    class_specific_descriptor = std::move(descriptor);
}

void PipeDeviceHandler::set_custom_descriptor(std::uint8_t type, data_type descriptor) {
    custom_descriptors[type] = std::move(descriptor);
}

void PipeDeviceHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    // The parent registers session and traverses interfaces to connect them
    VirtualDeviceHandler::on_new_connection(current_session, ec);
    if (ec) {
        return;
    }
    // A new session starts clean: IN/OUT channels are lazily recreated on first endpoint access
    std::lock_guard lock(pipe_mutex);
    disconnected = false;
    in_channels.clear();
    out_channels.clear();
}

void PipeDeviceHandler::on_disconnection(error_code &ec) {
    {
        std::lock_guard lock(pipe_mutex);
        disconnected = true;
        // For each channel, clear the buffer + pending requests (TransferHandle destruction releases automatically) + wake
        // blocked writers so they return as disconnected
        for (auto &[ep, ch]: in_channels) {
            ch.on_disconnection();
        }
        for (auto &[ep, ch]: out_channels) {
            ch.on_disconnection();
        }
        in_channels.clear();
        out_channels.clear();
    }
    // Wake blocking read so it returns as disconnected
    out_cv.notify_all();
    // The parent traverses interfaces for on_disconnection and clears the session pointer
    VirtualDeviceHandler::on_disconnection(ec);
}

void PipeDeviceHandler::on_pipe_in_request(std::uint8_t ep_addr, std::uint32_t seqnum, std::uint32_t length,
                                           TransferHandle transfer) {
    std::lock_guard lock(pipe_mutex);
    // Handled within the channel: reply immediately if data is buffered; otherwise suspend waiting for write
    get_in_channel(ep_addr).on_in_request(ep_addr, seqnum, length, std::move(transfer));
}

void PipeDeviceHandler::on_pipe_out_transfer(std::uint8_t ep_addr, std::uint32_t seqnum,
                                             TransferHandle transfer) {
    // Suspend the request in this endpoint's channel (data remains in transfer); read and respond when read() takes it;
    // if the application does not read, the host URB for this endpoint remains pending (NAK backpressure, independent per endpoint), resuming after data is read
    std::lock_guard lock(pipe_mutex);
    get_out_channel(ep_addr).on_out_request(ep_addr, seqnum, std::move(transfer));
    out_cv.notify_all();
}

void PipeDeviceHandler::on_pipe_control_request(const SetupPacket &setup, std::uint32_t seqnum,
                                                std::uint32_t transfer_buffer_length, TransferHandle transfer) {
    std::lock_guard lock(pipe_mutex);
    if (setup.is_out()) {
        // OUT: data has arrived with the control transfer; suspend the entire request (read data and respond when read() takes it)
        get_out_channel(0).on_out_request(0, seqnum, std::move(transfer), setup);
    }
    else {
        // IN: suspend the request (waiting for write({ep=0, data}) to respond), and expose setup as a notification to the application:
        // after read() obtains the notification only (with setup_req, no data), it knows a control request awaits a response
        get_in_channel(0).on_in_request(0, seqnum, transfer_buffer_length, std::move(transfer));
        get_out_channel(0).on_out_request(0, seqnum, TransferHandle{}, setup);
    }
    out_cv.notify_all();
}

void PipeDeviceHandler::on_pipe_unlink(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = false;
    {
        // Pending requests are in two places: IN requests are distributed by endpoint across their channels, as are OUT requests;
        // try cancelling each in turn
        std::lock_guard lock(pipe_mutex);
        for (auto &[ep, ch]: in_channels) {
            if (ch.cancel_pending(unlink_seqnum)) {
                cancelled = true;
                break;
            }
        }
        if (!cancelled) {
            for (auto &[ep, ch]: out_channels) {
                if (ch.cancel_pending(unlink_seqnum)) {
                    cancelled = true;
                    break;
                }
            }
        }
    }
    // If a pending URB was actually cancelled from the queue, return -ECONNRESET (URB cancelled, with no further
    // RET_SUBMIT because the request was removed); if not found (URB completed/nonexistent), return 0.
    // Matches the unlink pattern in kernel stub_tx.c and this project's HID/CdcAcm
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}
