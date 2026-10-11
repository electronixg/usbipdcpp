#pragma once

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/RingBuffer.h"

namespace usbipdcpp {

/**
 * @brief endpoint request queue, manage transfer requests by endpoint address (pure data container, no locking)
 *
 * is used to manage pending IN transfer requests for each endpoint.
 * Note: All methods are not locked, and the caller needs to manage the mutex lock by himself.
 */
class EndpointRequestQueue {
public:
    struct Request {
        std::uint32_t seqnum;
        std::uint32_t length;
        TransferHandle transfer;
    };

    /**
     * @brief Request to join the queue to the specified endpoint
     * @note The caller must already hold the mutex lock
     */
    void enqueue(std::uint8_t ep_address, Request request) {
        queues_[ep_address].push_back(std::move(request));
    }

    /**
     * @brief Dequeue request from specified endpoint
     * @note The caller must already hold the mutex lock
     */
    std::optional<Request> dequeue(std::uint8_t ep_address) {
        auto it = queues_.find(ep_address);
        if (it == queues_.end() || it->second.empty()) {
            return std::nullopt;
        }
        auto req = std::move(it->second.front());
        it->second.pop_front();
        return req;
    }

    /**
     * @brief Dequeue the request from any requested endpoint (return the endpoint address and request)
     * @return pair<endpoint address, request>, if all queues are empty, return nullopt
     * @note The caller must already hold the mutex lock
     */
    std::optional<std::pair<std::uint8_t, Request>> dequeue_any() {
        for (auto &[ep, queue]: queues_) {
            if (!queue.empty()) {
                auto req = std::move(queue.front());
                queue.pop_front();
                return std::make_pair(ep, std::move(req));
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Get the first request of the specified endpoint queue (without dequeuing)
     * @note The caller must already hold the mutex lock
     */
    Request *peek(std::uint8_t ep_address) {
        auto it = queues_.find(ep_address);
        if (it == queues_.end() || it->second.empty()) {
            return nullptr;
        }
        return &it->second.front();
    }

    /**
     * @brief Check whether the specified endpoint queue is empty
     * @note The caller must already hold the mutex lock
     */
    bool empty(std::uint8_t ep_address) const {
        auto it = queues_.find(ep_address);
        return it == queues_.end() || it->second.empty();
    }

    /**
     * @brief Get the number of requests in the specified endpoint queue
     * @note The caller must already hold the mutex lock
     */
    std::size_t size(std::uint8_t ep_address) const {
        auto it = queues_.find(ep_address);
        return it == queues_.end() ? 0 : it->second.size();
    }

    /**
     * @brief Cancel request by seqnum (for UNLINK)
     * @return true if the request is found and removed
     * @note The caller must already hold the mutex lock
     */
    bool cancel_by_seqnum(std::uint32_t unlink_seqnum) {
        for (auto &[ep, queue]: queues_) {
            auto it = std::find_if(queue.begin(), queue.end(),
                                   [unlink_seqnum](const Request &r) { return r.seqnum == unlink_seqnum; });
            if (it != queue.end()) {
                queue.erase(it);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Clear all queues
     * @note The caller must already hold the mutex lock
     */
    void clear() {
        queues_.clear();
    }

private:
    std::unordered_map<std::uint8_t, std::deque<Request>> queues_;
};

/**
 * @brief IN data channel base class (CRTP static distribution): manages the pairing response of "IN request from the host" and "data to be sent by the device"
 *
 * Data direction: device → host (USB IN endpoint). The device may not have data when the host's IN request comes.
 * The request is suspended first; after the business side writes the data into the channel, it automatically matches the pending request response (read by the host).
 * The host IN callback and business data writing may be in different threads. The channel is internally locked and can be called directly.
 *
 * Select the derived class according to the data processing mode (determined at compile time, no virtual function):
 *  - MessageInChannel One piece of data = one complete message (HID input report, UVC status notification)
 *  - ByteStreamInChannel data is a continuous byte stream, fragmented according to the requested length (CDC data port, general pipe)
 *
 * accesses three things (in the handler, all are indispensable):
 *  1. Lifecycle: call on_new_connection(session) during on_new_connection,
 *     and on_disconnection() on disconnection (clear buffers and pending requests, wake blocked writers)
 *  2. Host IN request arrives: call on_in_request(ep, seqnum, length, transfer) in the IN transfer callback of the interface
 *  3. The business side generates data: call push() (message mode) or write()/write_nb() (byte stream mode)
 *
 * optional configuration (call on demand, don’t confuse the two "upper limits" - in opposite directions):
 *  - set_max_pending_requests: limits the pending queue of "IN requests from the host". Requests accumulate when the device has no data
 *    to respond with; exceeding the limit evicts the oldest and completes it with no data (the host resubmits an empty-completed URB,
 *    equivalent USB bus NAK back pressure). Default 0 = unlimited
 *  - set_max_pending_messages (message mode) / set_capacity (byte-stream mode): limits the buffer of "data the device is waiting to
 *    send". Data accumulates if the host does not read for a long time; upon exceeding the limit, push's drop_oldest determines whether to discard the oldest
 *    or returns false, push_blocking/write blocks and waits. Default 0 = unlimited
 *
 * derived classes need to implement (for the CRTP interface called within the lock, see the declaration of each derived class):
 *  buffer_empty/buffer_clear: Buffer is empty/cleared
 *  try_send_one: Get data from the buffer to respond to a request
 *  try_pull_data / send_pulled_locked: Optional pull model (data generated on site when request arrives)
 *  push_locked: data into buffer
 */
template <typename Derived>
class InEndpointChannelBase {
public:
    /**
     * @brief New connection activation channel: set the session pointer and reset the status (buffer clearing, disconnection mark clearing).
     * Call from the handler's on_new_connection with the current session; when called without arguments (test stub reset), leave
     * The bound session remains unchanged
     */
    void on_new_connection(TransferResponder *current_session = nullptr) {
        std::lock_guard lock(channel_mutex);
        if (current_session) {
            responder = current_session;
        }
        disconnected = false;
        self().buffer_clear();
    }

    /**
     * @brief Processes a host IN request (forwarded in the interface IN transfer callback, session receiver thread)
     *
     * channel automatic judgment: there is buffered data / can be pulled on site → return RET_SUBMIT immediately; otherwise the request is suspended.
     * and is automatically answered after application data is written.
     * @param ep endpoint address (including direction bit, such as 0x81)
     * @param seqnum The serial number of this command (in the CMD_SUBMIT header)
     * @param length Number of bytes requested by the host (transfer_buffer_length)
     * @param transfer This transfer handle is used to carry data and send it to the host when responding.
     */
    void on_in_request(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        std::lock(channel_mutex, requests_mutex);
        std::lock_guard lock1(channel_mutex, std::adopt_lock);
        std::lock_guard lock2(requests_mutex, std::adopt_lock);

        // No longer receives after disconnection: request direct release (transfer destructor automatically recycles), does not join the queue and does not respond
        //(Align the disconnection processing of OutEndpointChannel to prevent requests from remaining in the next connection under disconnection race conditions)
        if (disconnected) {
            return;
        }

        // When there are pending requests at the same endpoint, they must be queued at the back (same endpoint FIFO order), even if there is data in the buffer
        if (endpoint_requests.empty(ep) && !self().buffer_empty()) {
            self().try_send_one(ep, seqnum, length, std::move(transfer));
            return;
        }
        // pull model: Derived classes can generate data on the spot (such as on_data_requested for the CDC data interface).
        // is only called when the buffer is empty and there are no pending requests for the same endpoint (the buffer empty is guaranteed by the branch above).
        // The consumption method is determined by the derived class (byte stream: this request takes priority, and the remaining ones are buffered)
        if (endpoint_requests.empty(ep)) {
            auto pulled = self().try_pull_data(length);
            if (!pulled.empty()) {
                self().send_pulled_locked(ep, seqnum, length, std::move(transfer), std::move(pulled));
                try_send_pending_locked();
                return;
            }
        }
        // Suspend the request; if this endpoint already has pending requests and buffered data, also advance the queue (send earlier requests).
        // First squeeze out the oldest request according to the upper limit: when the endpoint suspension has reached the upper limit, the oldest request will be dequeued and the response will be empty.
        // completed (0 bytes) - the host URB is empty and resubmitted after completion, which is equivalent to the NAK back pressure of the USB bus.
        // The pending queue remains bounded (does not allow host requests to accumulate infinitely when the device has no time to reply)
        if (max_pending_requests_ != 0 && endpoint_requests.size(ep) >= max_pending_requests_) {
            auto evicted = endpoint_requests.dequeue(ep);
            if (evicted.has_value()) {
                self().reply_empty(evicted->seqnum, std::move(evicted->transfer));
            }
        }
        endpoint_requests.enqueue(ep, {seqnum, length, std::move(transfer)});
        try_send_pending_locked();
    }

    /**
     * @brief Set the pending-request limit (0 = unlimited, default). At the limit, a new request evicts this endpoint's
     * oldest request and completes it with no data (actual_length=0, normal status): the host URB
     * resubmits when empty, equivalent to the NAK back pressure of the USB bus, and the pending queue remains bounded
     * @note This limits "requests from the host", in the opposite
     * (MessageInChannel::set_max_pending / ByteStreamInChannel::set_capacity)
     * direction to the data buffer limit: the former limits request accumulation (device has no response data), the latter limits data accumulation (host does not read)
     */
    void set_max_pending_requests(std::size_t max_pending) {
        std::lock_guard lock(requests_mutex);
        max_pending_requests_ = max_pending;
    }

    /**
     * @brief Cancel a pending IN request (called when processing the host's UNLINK command)
     * @param unlink_seqnum Request to cancel seqnum
     * @return true = The request is indeed still in the pending queue and has been canceled (response -ECONNRESET);
     *         false = The request does not exist or has been answered (response 0)
     */
    bool cancel_pending(std::uint32_t unlink_seqnum) {
        std::lock_guard lock(requests_mutex);
        return endpoint_requests.cancel_by_seqnum(unlink_seqnum);
    }

    /**
     * @brief Disconnection: clear buffer and pending requests (transfer destructor automatically releases), wake-up blocking
     * writers. Subsequent write interfaces return 0/short writes; connection state is managed by the handler
     */
    void on_disconnection() {
        {
            std::lock_guard lock(channel_mutex);
            disconnected = true;
            self().buffer_clear();
        }
        {
            std::lock_guard lock(requests_mutex);
            // TransferHandle is automatically released when it is destroyed
            endpoint_requests.clear();
        }
        space_cv.notify_all();
    }

protected:
    /// Try to respond to a pending request: keep sending if the buffer is not empty (dequeue_any takes any endpoint, and stops if the buffer is empty).
    /// The caller must already hold channel_mutex and requests_mutex
    void try_send_pending_locked() {
        while (!self().buffer_empty()) {
            auto req_opt = endpoint_requests.dequeue_any();
            if (!req_opt.has_value()) {
                break;
            }
            auto &[ep_addr, req] = req_opt.value();
            self().try_send_one(ep_addr, req.seqnum, req.length, std::move(req.transfer));
        }
    }

    Derived &self() {
        return static_cast<Derived &>(*this);
    }

    // session pointer (handler binding, used for try_send_one submission response)
    TransferResponder *responder = nullptr;

    // Buffer lock and request queue lock: the data side and the request side may be in different threads (business thread vs session
    // receiver thread), the double lock allows the write side to wait for space without holding the queue lock.
    // mutable: const query method (size/available) also needs to be locked
    mutable std::mutex channel_mutex;
    mutable std::mutex requests_mutex;
    // Pending IN transfer request (queued by endpoint)
    EndpointRequestQueue endpoint_requests;
    // upper limit of pending requests (0 = unlimited); on_in_request checked by endpoint
    std::size_t max_pending_requests_ = 0;
    // Wakes up the blocked writer when the buffer frees up space (for byte stream mode)
    std::condition_variable space_cv;
    // disconnection mark (initially disconnected, available after on_new_connection)
    bool disconnected = true;
};

/**
 * @brief Message mode IN channel: The buffer is a message queue, and the entire message is consumed
 *
 * is suitable for scenarios where "one piece of data = one complete message" such as HID input report/UVC status notification.
 * push non-blocking: respond directly if there is a pending request, otherwise it will be buffered, and the oldest will be discarded if the upper limit is exceeded (the host has a long-term
 * prevents unlimited memory growth when not reading).
 *
 * data is filled with TransferOperator::set_transfer_data held by TransferHandle,
 * component does not assume the internal structure of transfer (supports any implementation of GenericTransfer, libusb, etc.)
 */
class MessageInChannel : public InEndpointChannelBase<MessageInChannel> {
public:
    /**
     * @brief Push a message non-blockingly (callable from any thread)
     *
     * Respond directly when there is a pending IN request; if not, it will be buffered and wait for the host to read.
     * @param data a complete message
     * @param drop_oldest true (default) = discard the oldest message when the buffer exceeds the upper limit, and keep the "latest message"
     *        "priority" semantics (historical calling behavior remains unchanged); false = do not join the queue when the buffer is full, return false
     *        tells the caller that it is full and the transmission was not successful this time. The caller decides to discard or retry.
     * @return Whether enqueueing succeeded (always true with drop_oldest=true)
     */
    bool push(data_type data, bool drop_oldest = true) {
        std::lock(this->channel_mutex, this->requests_mutex);
        std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
        std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
        // Stop accepting after disconnection: return false to indicate this send failed (data would be cleared on the next
        // connection, so accumulating it is pointless; consistent with write_nb returning 0 after disconnection)
        if (this->disconnected) {
            return false;
        }
        // is full and the caller does not discard the old one: does not join the queue and tells the caller that it is full.
        if (!drop_oldest && max_pending_ != 0 && pending.size() >= max_pending_) {
            return false;
        }
        push_locked(std::move(data));
        this->try_send_pending_locked();
        return true;
    }

    /**
     * @brief Blocking pushing a message: When the buffer is full, wait for the host to read it and free up space before entering the queue.
     *
     * is suitable for scenarios where "data must be delivered completely and can be waited for" (blocking write of symmetric ByteStream).
     * releases channel_mutex during the waiting period, and the host request can still join the queue and respond to take away the message (try_send_one
     * wakes up after vacating the space).
     * @param data a complete message
     * @param timeout_ms Timeout for waiting for vacancies in each stage (milliseconds); 0 = infinite wait
     * @return true = Successfully joined the queue; false = Disconnected or waiting timeout (still full)
     */
    bool push_blocking(data_type data, std::uint32_t timeout_ms = 0) {
        std::unique_lock lock(this->channel_mutex);
        while (max_pending_ != 0 && pending.size() >= max_pending_) {
            if (this->disconnected) {
                return false;
            }
            if (timeout_ms == 0) {
                this->space_cv.wait(lock);
            }
            else if (this->space_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
                break;
            }
        }
        if (this->disconnected) {
            return false;
        }
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            return false; // Waiting timeout is still full
        }
        // Space is available: additionally acquire the requests lock, enqueue, and advance pending requests (same lock order as push)
        std::lock_guard lock2(this->requests_mutex);
        push_locked(std::move(data));
        this->try_send_pending_locked();
        return true;
    }

    /**
     * @brief Set the upper limit of the message buffer to be sent (0 = unlimited, default)
     * @note If the host never reads, push discards the oldest at this limit to prevent unlimited memory growth.
     * This limits "data waiting to be sent by the device", in the opposite direction to the base class's set_max_pending_requests
     * (upper limit of "Host Request", NAK backpressure) in the opposite direction
     */
    void set_max_pending_messages(std::size_t max_pending) {
        std::lock_guard lock(this->channel_mutex);
        max_pending_ = max_pending;
    }

    /**
     * @brief Number of messages to be sent (unanswered) in the buffer
     * @note is used for queries such as has_pending_input_reports; callable from any thread
     */
    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return pending.size();
    }

    // ===== CRTP interface (call within lock) =====

    bool buffer_empty() const {
        return pending.empty();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // The caller has held a double lock and the buffer is not empty: take the oldest message and truncate the response according to the request length
        auto &front = pending.front();
        auto send_len = std::min(front.size(), static_cast<std::size_t>(length));
        data_type data(front.begin(), front.begin() + send_len);
        pending.pop_front();
        // data filling is done by op (component does not assume transfer internal structure).
        // Precondition: transfers reaching here necessarily hold an op (created by
        // alloc_transfer_handle during CMD_SUBMIT parsing and wrapped in TransferHandle; empty handles are never enqueued)
        assert(transfer.get_operator() != nullptr);
        auto written = transfer.get_operator()->set_transfer_data(transfer.get(), data, length);
        this->space_cv.notify_one(); // message is taken away to free up space and wake up the blocked push_blocking
        this->responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                seqnum, static_cast<std::uint32_t>(written), std::move(transfer)));
    }

    void reply_empty(std::uint32_t seqnum, TransferHandle /*transfer*/) {
        // Extruded pending request: response empty completed (0 bytes, status normal), transfer destructed automatically
        // released. Resubmit after the host URB is empty (equivalent to USB bus NAK back pressure)
        this->responder->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, 0));
    }

    data_type try_pull_data(std::uint32_t length) {
        // message mode generally does not require pull (data is actively pushed by the business thread), and is empty by default.
        return {};
    }

    void send_pulled_locked(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer,
                            data_type pulled) {
        // message mode is not commonly used in pull; if the derived class overrides try_pull_data and then comes here,
        // will be consumed by this request after the entire item is buffered (same semantics as push)
        push_locked(std::move(pulled));
        try_send_one(ep, seqnum, length, std::move(transfer));
    }

    void push_locked(data_type data) {
        // The buffer exceeds the limit and the oldest is discarded (keep the latest message semantics)
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            pending.pop_front();
        }
        pending.emplace_back(std::move(data));
    }

    void buffer_clear() {
        pending.clear();
    }

private:
    std::deque<data_type> pending;
    std::size_t max_pending_ = 0;
};

/**
 * @brief Byte stream mode IN channel: The buffer is a byte stream (RingBuffer), consumed in slices according to the requested length
 *
 * is suitable for CDC data interfaces, general pipelines and other scenarios where "data is a continuous byte stream".
 * write blocking: wait for the host to take away the buffer when it is full (timeout_ms=0 infinite), aligning the kernel FIFO semantics.
 *
 * data is filled with TransferOperator::set_transfer_data held by TransferHandle,
 * component does not assume the internal structure of transfer (supports any implementation of GenericTransfer, libusb, etc.)
 */
class ByteStreamInChannel : public InEndpointChannelBase<ByteStreamInChannel> {
public:
    /**
     * @brief Non-blocking write: write data to the buffer and try to respond to pending IN requests (callable from any thread)
     * @return The actual number of bytes written; when disconnected, 0 is returned; when the buffer is full, it may be less than size (the excess is discarded)
     */
    std::size_t write_nb(const std::uint8_t *data, std::size_t size) {
        std::lock(this->channel_mutex, this->requests_mutex);
        std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
        std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
        if (this->disconnected) {
            return 0;
        }
        std::size_t written = buffer.write(data, size);
        this->try_send_pending_locked();
        return written;
    }

    std::size_t write_nb(const data_type &data) {
        return write_nb(data.data(), data.size());
    }

    /**
     * @brief Blocking write: Write all the data into the buffer before returning. Wait for the host to read when the buffer is full
     * (timeout_ms=0 infinite wait, aligned kernel FIFO "full blocking" semantics).
     * @param data data starting pointer
     * @param size The number of bytes to write
     * @param timeout_ms Timeout (milliseconds) for waiting for buffer space in each stage; 0 = infinite wait
     * @return actual number of bytes written; disconnection returns the amount written; timeout returns partial write (< size)
     */
    std::size_t write(const std::uint8_t *data, std::size_t size, std::uint32_t timeout_ms = 0) {
        std::size_t total_written = 0;
        std::size_t offset = 0;

        while (offset < size) {
            // stage 1: Waiting for buffer space
            {
                std::unique_lock lock(this->channel_mutex);
                if (this->disconnected) {
                    return total_written;
                }
                if (buffer.available() == 0) {
                    // First try to answer pending requests to make space
                    {
                        std::lock_guard queue_lock(this->requests_mutex);
                        this->try_send_pending_locked();
                    }
                    if (this->disconnected) {
                        return total_written;
                    }
                    // If  is still full, wait: the host IN requests to take away the data and is awakened by try_send_one
                    while (buffer.available() == 0 && !this->disconnected) {
                        if (timeout_ms == 0) {
                            this->space_cv.wait(lock);
                        }
                        else {
                            if (this->space_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) ==
                                std::cv_status::timeout) {
                                // The space may just be freed up at the time of timeout: jump out of the loop and make a unified judgment, write if you can
                                break;
                            }
                        }
                    }
                    if (this->disconnected) {
                        return total_written;
                    }
                    if (buffer.available() == 0) {
                        return total_written; // Timed out and space has not been vacated yet
                    }
                }
                std::size_t written = buffer.write(data + offset, size - offset);
                total_written += written;
                offset += written;
            }
            // stage 2: There is data in the buffer, trying to respond to the pending request
            {
                std::lock(this->channel_mutex, this->requests_mutex);
                std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
                std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
                this->try_send_pending_locked();
            }
        }
        return total_written;
    }

    std::size_t write(const data_type &data, std::uint32_t timeout_ms = 0) {
        return write(data.data(), data.size(), timeout_ms);
    }

    /**
     * @brief Set buffer capacity (bytes, default 64KB)
     * @note must be called before connecting; changing it after connecting will have no effect.
     * This limits "data waiting to be sent by the device", in the opposite direction to the base class's set_max_pending_requests
     * (upper limit of "Host Request", NAK backpressure) in the opposite direction
     */
    void set_capacity(std::size_t capacity) {
        std::lock_guard lock(this->channel_mutex);
        buffer.resize(capacity);
    }

    /** @brief Total buffer capacity (bytes) */
    std::size_t capacity() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.capacity();
    }

    /** @brief Number of bytes to be sent (unread) in the buffer */
    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.size();
    }

    /** @brief Number of remaining writable bytes in the buffer */
    std::size_t available() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.available();
    }

    /**
     * @brief Set pull callback: called when the IN request arrives and the buffer/suspension queue is empty, can be generated on-site
     * data (such as the CDC data port produced by the derived data generator for continuous output).
     * @note The callback is called when the channel is locked. Do not call the write method of this channel in the callback (deadlock will occur)
     */
    void set_pull_callback(std::function<data_type(std::uint32_t length)> callback) {
        pull_callback = std::move(callback);
    }

    // ===== CRTP interface (call within lock) =====

    bool buffer_empty() const {
        return buffer.empty();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // The caller has held a double lock and the buffer is not empty: it is taken out in pieces according to the requested length.
        std::size_t send_len = std::min(buffer.size(), static_cast<std::size_t>(length));
        data_type data(send_len);
        buffer.read(data.data(), send_len);
        // data filling is done by op (component does not assume transfer internal structure).
        // Precondition: transfers reaching here necessarily hold an op (created by
        // alloc_transfer_handle during CMD_SUBMIT parsing and wrapped in TransferHandle; empty handles are never enqueued)
        assert(transfer.get_operator() != nullptr);
        auto written = transfer.get_operator()->set_transfer_data(transfer.get(), data, length);
        // The buffer makes room and wakes up the writer who is blocked and waiting.
        this->space_cv.notify_one();
        this->responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                seqnum, static_cast<std::uint32_t>(written), std::move(transfer)));
    }

    void reply_empty(std::uint32_t seqnum, TransferHandle /*transfer*/) {
        // Extruded pending request: response empty completed (0 bytes, status normal), transfer destructed automatically
        // released. Resubmit after the host URB is empty (equivalent to USB bus NAK back pressure)
        this->responder->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, 0));
    }

    data_type try_pull_data(std::uint32_t length) {
        if (pull_callback) {
            return pull_callback(length);
        }
        return {};
    }

    void send_pulled_locked(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer,
                            data_type pulled) {
        // pull data gives priority to satisfy this request (align CdcAcm original semantics: send min part),
        // and write excess data to the buffer (discard what does not fit, consistent with push_locked)
        auto send_len = std::min(pulled.size(), static_cast<std::size_t>(length));
        data_type data(pulled.begin(), pulled.begin() + send_len);
        // data filling is completed by op (see try_send_one for preconditions)
        assert(transfer.get_operator() != nullptr);
        auto written = transfer.get_operator()->set_transfer_data(transfer.get(), data, length);
        this->responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                seqnum, static_cast<std::uint32_t>(written), std::move(transfer)));
        if (pulled.size() > send_len) {
            buffer.write(pulled.data() + send_len, pulled.size() - send_len);
        }
    }

    void push_locked(data_type data) {
        // The entire packet is written, and the part that cannot be written is discarded (the caller usually guarantees that the capacity will not be exceeded)
        buffer.write(data.data(), data.size());
    }

    void buffer_clear() {
        buffer.clear();
    }

private:
    RingBuffer buffer;
    std::function<data_type(std::uint32_t)> pull_callback;
};

} // namespace usbipdcpp
