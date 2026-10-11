#pragma once

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"

namespace usbipdcpp {

/**
 * @brief OUT data channel base class (CRTP static distribution): Host OUT request is pending, and the business side responds when it is taken away (NAK back pressure)
 *
 * is symmetrical with InEndpointChannelBase: the request for In is suspended because "the device has no data to send", and the request for Out is suspended
 * because "the device has no space to receive" (the application has not consumed data). After a host OUT request arrives, data stays pending in
 * transfer without returning RET_SUBMIT: the host URB remains pending and naturally stops sending (consistent with kernel
 * vudc: OUT endpoint NAK when gadget request is exhausted, see u_serial.c gs_rx_push).
 * When the application retrieves it through take(), read the data and return RET_SUBMIT; after space is freed, the host resumes sending.
 *
 * data is read from TransferOperator::get_transfer_data held by TransferHandle.
 * component does not assume transfer internal structure.
 *
 * A single lock is sufficient: the lock is released when blocking and waiting (take), and the requesting side (on_out_request) can join the queue at any time.
 * does not require double locking of the In channel (the double locking of In is because the request queue lock cannot be occupied when writing to the waiting space).
 *
 * Pure notification request (transfer is empty, such as the setup of Pipe control IN is exposed): take only returns
 * setup, no response, used to notify the business side that "the host has a control request to be responded to"
 *
 * Typical access (within handler):
 *  - Called in the host OUT callback on_out_request(...): The request is suspended and the data remains in the transfer
 *  - Business thread takes data: take() (blocking, reads the data and responds when taking it, the vacancy is released and the host resumes sending)
 *    or try_take() (non-blocking, returns nullopt immediately if there is no data)
 *  - Lifecycle: during on_new_connection, pass the session to on_new_connection(session),
 *    and during on_disconnection call on_disconnection (clear pending requests, wake blocked take)
 */
template <typename Derived>
class OutEndpointChannelBase {
public:
    /// one consumption returned by take() (the business side will process it after getting the data)
    struct Pending {
        std::uint8_t ep;                       // endpoint address (including direction bit); when controlling the request, it is 0
        std::optional<SetupPacket> setup_req;  // is only valid when controlling the request (ep==0)
        data_type data;                        // OUT data (pure notification request is empty)
    };

    /**
     * @brief Set the pending-request limit (0 = unlimited, default). Once the limit is reached, new host
     * OUT requests immediately return EPIPE (device busy), without occupying the queue; allow them only after application consumption frees space.
     * Same direction as InEndpointChannelBase::set_max_pending_requests: both limit accumulation of "requests
     * from the host" (when the device has no space to receive), rather than data buffering
     */
    void set_max_pending_requests(std::size_t max_pending) {
        std::lock_guard lock(mutex_);
        max_pending_ = max_pending;
    }

    /** @brief Number of requests currently pending (received but not consumed) */
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return pending.size();
    }

    /**
     * @brief The seqnum of the oldest request in the pending queue (empty queue returns nullopt)
     * @note For cross-channel/cross-endpoint data acquisition according to USB global arrival order: seqnum monotonically increasing
     */
    std::optional<std::uint32_t> front_seqnum() const {
        std::lock_guard lock(mutex_);
        if (pending.empty()) {
            return std::nullopt;
        }
        return pending.front().seqnum;
    }

    /**
     * @brief Processes a host OUT request (forwarded in the interface OUT transfer callback, session receiver thread)
     *
     * Suspend the request without immediately returning RET_SUBMIT: a pending host URB applies backpressure and stops sending; respond only when the application
     * take()/try_take() responds after taking the data (NAK back pressure, aligning the OUT processing of core u_serial).
     * @param ep endpoint address (including direction bit, such as 0x02)
     * @param seqnum command sequence number
     * @param transfer This time the OUT handle, the data is in the handle, read out by op when take()
     * @param setup_req Only non-standard control requests (ep==0) are transparently transmitted to the business side; ordinary endpoint OUT does not need to be transmitted
     */
    void on_out_request(std::uint8_t ep, std::uint32_t seqnum, TransferHandle transfer,
                        std::optional<SetupPacket> setup_req = std::nullopt) {
        std::lock_guard lock(mutex_);
        if (disconnected) {
            return;  // No more reception after disconnection: Request direct release (the session has been closed and there is no way to respond)
        }
        // reaches the upper limit: the device is busy, and EPIPE rejects the request (the request is released directly and does not occupy the pending queue)
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            self().reply(seqnum, 0, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
            return;
        }
        pending.emplace_back(PendingRequest{ep, seqnum, setup_req, std::move(transfer)});
        data_cv.notify_one();
    }

    /**
     * @brief Retrieve a pending OUT request without blocking (called by the application thread)
     * @return If there is a request, it will return Pending (including endpoint and read data), and automatically respond (the host resumes sending);
     *         Queue empty returns nullopt
     */
    std::optional<Pending> try_take() {
        std::lock_guard lock(mutex_);
        if (pending.empty()) {
            return std::nullopt;
        }
        return take_locked();
    }

    /**
     * @brief Retrieve a pending OUT request with blocking (called by the application thread)
     * @param timeout_ms Wait timeout (milliseconds); 0 = infinite wait
     * @return Returns Pending if there is a request (including endpoint and read data, automatic response); returns nullopt when timeout or disconnection
     */
    std::optional<Pending> take(std::uint32_t timeout_ms = 0) {
        std::unique_lock lock(mutex_);
        while (pending.empty() && !disconnected) {
            if (timeout_ms == 0) {
                data_cv.wait(lock);
            }
            else {
                if (data_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
                    // The data may just enter the queue at the time of timeout: jump out of the loop and make a unified judgment to avoid losing data.
                    break;
                }
            }
        }
        if (pending.empty()) {
            return std::nullopt;
        }
        return take_locked();
    }

    /**
     * @brief Cancel a pending OUT request (host's UNLINK command processing)
     * @return true = The request is indeed still in the pending queue and has been canceled (response -ECONNRESET);
     *         false = The request does not exist or has been answered (response 0)
     */
    bool cancel_pending(std::uint32_t unlink_seqnum) {
        std::lock_guard lock(mutex_);
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->seqnum == unlink_seqnum) {
                pending.erase(it);  // transfer destruction and automatic release
                return true;
            }
        }
        return false;
    }

    /**
     * @brief New connection activation channel: Set the session pointer and reset the disconnection mark (the queue is cleared from the last disconnection).
     * Call from the handler's on_new_connection with the current session; when called without arguments (test stub reset), leave
     * The bound session remains unchanged
     */
    void on_new_connection(TransferResponder *current_session = nullptr) {
        std::lock_guard lock(mutex_);
        if (current_session) {
            responder = current_session;
        }
        disconnected = false;
    }

    /**
     * @brief Disconnection: clear pending requests (automatically released by transfer destructor), wake up blocked take and return by disconnection.
     * Take immediately returns nullopt; the connection status is managed by the handler
     */
    void on_disconnection() {
        {
            std::lock_guard lock(mutex_);
            disconnected = true;
            pending.clear();  // TransferHandle is automatically released after destruction
        }
        data_cv.notify_all();
    }

protected:
    Derived &self() {
        return static_cast<Derived &>(*this);
    }

    TransferResponder *responder = nullptr;

private:
    struct PendingRequest {
        std::uint8_t ep;
        std::uint32_t seqnum;
        std::optional<SetupPacket> setup_req;
        TransferHandle transfer;  // data carrier (OUT data has been read in); pure notification request is empty
    };

    // The caller must already hold mutex_
    std::optional<Pending> take_locked() {
        auto req = std::move(pending.front());
        pending.pop_front();
        if (req.transfer) {
            // has data: read and respond (data consumption completed = vacancy released, host resumes sending)
            assert(req.transfer.get_operator() != nullptr);
            bool supported = false;
            data_type data;
            auto len = req.transfer.get_operator()->get_transfer_data(req.transfer.get(), data, supported);
            assert(supported);  // Operators using suspend-consume must support reading data
            self().reply(req.seqnum, len);
            return Pending{req.ep, req.setup_req, std::move(data)};
        }
        // Pure notification (controlling the setup of IN): No response, only the setup is handed over to the business side
        return Pending{req.ep, req.setup_req, {}};
    }

    mutable std::mutex mutex_;
    std::condition_variable data_cv;  // The pending queue is not empty or disconnected
    std::deque<PendingRequest> pending;
    std::size_t max_pending_ = 0;  // 0 = Unlimited
    bool disconnected = true;      // is initially disconnected and available after on_new_connection
};

/**
 * @brief OUT data channel default implementation
 *
 * derived classes need to implement reply() (CRTP interface, respond to a request): usually
 * responder->submit_ret_submit returns RET_SUBMIT (status=0: OUT data consumption completed;
 * not 0: EPIPE as upper limit rejected). Tests can inherit from OutEndpointChannelBase to provide their own
 * reply() records the response and does not generate virtual function overhead
 */
class OutEndpointChannel : public OutEndpointChannelBase<OutEndpointChannel> {
public:
    /**
     * @brief responds to a pending request (CRTP implementation of this channel, internal call, no need to use it directly)
     * @param seqnum The sequence number corresponding to the request
     * @param length The length of the consumed data (when status==0, the host thinks this is the size)
     * @param status 0 = Normal completion; non-0 = Error status (such as EPIPE rejected by the upper limit, fill in the length 0)
     */
    void reply(std::uint32_t seqnum, std::uint32_t length, std::uint32_t status = 0) {
        if (status == 0) {
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, length));
        }
        else {
            // Error status response: actual_length Fill in 0 (the protocol error transfer does not contain valid data)
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
        }
    }
};

} // namespace usbipdcpp
