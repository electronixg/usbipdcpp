#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "usbipdcpp/Endpoint.h"
#include "usbipdcpp/Export.h"
#include "usbipdcpp/protocol.h"

namespace usbipdcpp {

class TransferResponder;

/**
 * @brief transfer frame scheduler (frame scheduling idea of aligning kernel usbip vudc_transfer.c)
 *
 * The virtual device does not have a real USB bus: if it responds immediately when the URB arrives, the host driver
 * The "Complete→Resubmit" loop will lose control if it loses bus throttling (actually measured UAC virtual device oversends 158 times).
 * This class uses event-driven timers to simulate frame rhythm and control URB service timing.
 *
 * Typical usage of  in custom VirtualInterfaceHandler:
 *   0. The instance is owned by VirtualDeviceHandler; the handler accesses it through
 *      device_handler->get_transfer_scheduler()
 *   1. The device bus speed is passed in during construction (taken from UsbDevice by VirtualDeviceHandler,
 *      endpoint transaction interval is derived based on it)
 *   2. start() in on_new_connection(), stop() in on_disconnection()——
 *      stop() discards the unfinished URB and stops the thread. After reconnection, the start() status is clean.
 *   3. After receiving the iso/bulk/interrupt URB, select the submission method according to whether the data is ready:
 *      - Data is ready (e.g. microphone IN has generated PCM, or pure delayed response):
 *        submit(session, ep, type, num_iso_packets, ret_submit, data_duration)
 *        ——The scheduler sends the response directly at the service time
 *      - requires handler processing (such as speaker OUT receiving and writing sink, bulk data processing):
 *        submit(session, ep, type, data_duration, seqnum, transfer, processor)
 *        ——The service time callback processor notifies the handler to start processing
 *   4. Processor version: Process data in processor callback (memory level operation, microsecond level), processing is completed
 *      session.submit_ret_submit(...) Send response + on_urb_done(ep_address, seqnum)
 *      reporting completed - the next URB of the same endpoint will be served after reporting
 *   5. cancel(seqnum) when UNLINK is received (response semantics are consistent with kernel vudc_rx.c)
 *   Control URB does not go through the scheduler (control requests must be fast, aligned with ep0 of vudc)
 *
 * scheduling semantics:
 *   - isochronous: Delay service by data duration - completion cadence = host data rate, equivalent core
 *     gadget u_audio distributes N packets of URB in N frames. data_duration by
 *     the caller from transfer_buffer_length / data rate (may be negative = early service,
 *     used by the receive-rate feedback loop to correct fixed host submission overhead)
 *   - Bulk/Interrupt: The service time is aligned to the frame boundary (waiting for 0-1ms), each frame and each endpoint are at most
 *     serves a URB (vudc frame driven semantics)
 *   - Independence between endpoints: suspension of one endpoint (processing/NAK) does not affect other endpoints
 *
 * thread model: self-sustaining io_context + a scheduling thread, start() when the connection is established, and when it is disconnected
 * stop(). The timer is not scheduled when the queue is empty (aligned with the IDLE state of vudc), and new URBs are enqueued.
 * rescheduled (aligned to v_kick_timer). The session is guaranteed to survive until stop() by the caller.
 */
class USBIPDCPP_API TransferScheduler {
public:
    /**
     * @brief URB processing callback (interrupt semantics, vudc frame service)
     *
     * Called by the scheduling thread at service time, passing data (TransferHandle rvalue reference) to the handler
     * to begin processing: write to sink / populate data / collect feedback statistics. After processing, the handler itself uses
     * session.submit_ret_submit to send the response (consistent with other handler response paths, safe from any
     * thread), and calls on_urb_done to report completion (same-endpoint serialization depends on this).
     *
     * @note The callback is executed in the scheduling thread and is equivalent to the interrupt handler: it must return quickly (memory level
     * operations, such as UAC write buffering/generating PCM, microsecond level), blocking and time-consuming tasks are prohibited——
     * Otherwise, the scheduler will be stuck and other endpoints will be dragged down (same as vudc's transfer synchronization memcpy semantics).
     * Time-consuming work (disk IO, etc.) is arranged by the handler itself and must not wait within the callback.
     * The  callback does not hold the scheduler lock and can safely re-enter submit()/on_urb_done().
     * closure does not capture TransferHandle (it is not copyable while std::function requires the callback to be copyable;
     * Android NDK's libc++ lacks move_only_function, so passing it as an argument is the only clean
     * cross-platform approach)
     */
    using UrbProcessCallback = std::function<void(TransferResponder &responder, const UsbEndpoint &ep,
                                                  std::uint32_t seqnum, TransferHandle &&transfer)>;

    /**
     * @brief constructs the scheduler
     * @param speed device bus speed (device-level attribute, when constructed by VirtualDeviceHandler
     * is passed in from UsbDevice), and the endpoint transaction interval is derived based on it.
     */
    explicit TransferScheduler(UsbSpeed speed) : speed(speed) {}

    /**
     * @brief Start the scheduling thread when the connection is established
     * @note Idempotent; you must stop before starting again after disconnection
     */
    void start();

    /**
     * @brief Stop when connection is disconnected
     *
     * Discard unfinished URBs (connection is gone, do not respond; consistent with vudc's stop_activity clearing
     * queue), cancel the timer, and stop scheduling threads.
     * @note Idempotent, automatically called during destruction
     */
    void stop();

    ~TransferScheduler();

    /**
     * @brief submits a pending URB (notification semantics)
     *
     * is not processed immediately. The scheduling thread calls the processor at the service time to notify the handler to start processing.
     * (transfer is handed over as an rvalue reference, seqnum is passed in by the scheduler), after the handler is processed
     * sends a response to submit_ret_submit by itself and calls on_urb_done to report completion.
     * Rate limiting controls "service timing": isochronous service is delayed by data_duration (completion cadence = host data
     * rate; data_duration is converted by the caller according to transfer_buffer_length/data rate,
     * includes flow rate closed-loop correction, negative = early service); bulk/interrupt aligned frame boundary service
     * (waiting for 0-1ms). The next URB with the same endpoint is served after the same delay after on_urb_done.
     * @param session Session, guaranteed to survive by the caller until stop() (stop first and then destroy the session when the connection is disconnected)
     * @param ep endpoint (passed to processor during service)
     * @param type transfer type (isochronous speed limit based on data duration, other frames aligned)
     * @param data_duration isochronous: audio duration corresponding to the data; bulk/interrupt ignored
     * @param seqnum URB sequence number (cancel matching + on_urb_done verification)
     * @param transfer data (handled to processor as rvalue reference during service)
     * @param processor processing callback (interruption semantics, see UrbProcessCallback annotation)
     * @note Discarded when the connection is disconnected (after stop) and will not be processed or responded.
     * Safe from any thread (safe to re-enter submit within the processor callback)
     */
    void submit(TransferResponder &responder, const UsbEndpoint &ep, EndpointAttributes type,
                std::chrono::microseconds data_duration, std::uint32_t seqnum, TransferHandle transfer,
                UrbProcessCallback processor);

    /**
     * @brief submits a transfer request with filled data, and the scheduler controls the response timing according to the transfer type.
     *
     * endpoint version: The transaction interval is derived by the scheduler based on endpoint bInterval and device speed.
     * Explicit interval version: The transaction interval is given by the caller (such as non-standard interval for testing).
     * isochronous: Delay response by data_duration (default = num_iso_packets × interval), the same
     * endpoint URB serial completion; when num_iso_packets <= 0, it does not occupy the scheduling window and responds immediately.
     * data_duration Substitute when specified Number of packets × interval - completion rate = host data rate, device
     * The frequency deviation between the local clock and the host clock does not accumulate (the adaptive OUT endpoint follows the correct amount of host data
     * behavior, see UacAudioStreamingSinkHandler annotation). Can be negative: negative = early response
     * (used to correct the fixed submission overhead of each URB of the host in a closed-loop water level, completed immediately when the delay ≤ 0).
     * @param session session, guaranteed to survive until stop() by the caller
     * @param ep endpoint (endpoint version)/@param ep_address endpoint address (explicit interval version)
     * @param type transfer type
     * @param num_iso_packets isochronous packet number - only used by isochronous transfer, other types are transferred 0
     * @param submit Filled response (sent directly when the service time arrives)
     * @param data_duration isochronous: audio duration corresponding to the data, default = number of packets × interval
     * @note Discarded when the connection is disconnected (after stop) and does not respond.
     */
    void submit(TransferResponder &responder, const UsbEndpoint &ep, EndpointAttributes type, int num_iso_packets,
                UsbIpResponse::UsbIpRetSubmit &&submit,
                std::chrono::microseconds data_duration = std::chrono::microseconds::zero());
    void submit(TransferResponder &responder, std::uint8_t ep_address, EndpointAttributes type,
                std::chrono::microseconds interval, int num_iso_packets, UsbIpResponse::UsbIpRetSubmit &&submit,
                std::chrono::microseconds data_duration = std::chrono::microseconds::zero());

    /**
     * @brief Called by the handler after processing the URB (the core of notification semantics)
     *
     * Clear the endpoint's processing state and allow the next queued URB (its service time was fixed at submission
     * and is not recalculated: processing time/trigger delay do not affect cadence). If processing is slow (past the chained service
     * time), the next URB is served immediately after release (serial protection, rhythm is restored immediately).
     * @param ep_address endpoint address
     * @param seqnum URB sequence number after processing - must match the URB currently being processed, if not matched
     * Ignore (scenario where the processor has finished finishing)
     * @note Safe from any thread (processing can be asynchronous)
     */
    void on_urb_done(std::uint8_t ep_address, std::uint32_t seqnum);

    /**
     * @brief Cancel the pending URB corresponding to seqnum (for UNLINK)
     * @param seqnum URB sequence number to be canceled
     * @return true = Cancellation is successful (the caller responds RET_UNLINK(-ECONNRESET) and no longer sends
     * RET_SUBMIT); false = URB is being processed/completed or does not exist (caller response
     * RET_UNLINK(0)). Consistent with CMD_UNLINK semantics of kernel vudc_rx.c
     */
    bool cancel(std::uint32_t seqnum);

    /**
     * @brief Calculate endpoint transaction interval according to USB specification bInterval semantics
     * @param ep endpoint (take bInterval)
     * @param speed device bus speed
     * @return high speed 2^(bInterval-1) × 125µs; full speed/low speed bInterval × 1ms;
     * Super bInterval × 125µs
     */
    static std::chrono::microseconds endpoint_interval(const UsbEndpoint &ep, UsbSpeed speed);

protected:
    /**
     * @brief URB response exit when completed
     *
     * is submitted to the network send queue of the current session by default (submit_ret_submit, safe from any thread,
     * For the implementation of , see TransferScheduler.cpp).
     * @note Subclasses can override intercept responses (such as test collection)
     */
    virtual void on_urb_completed(TransferResponder &current_session, UsbIpResponse::UsbIpRetSubmit &&submit);

private:
    struct PendingUrb {
        std::chrono::steady_clock::time_point service_time; // service time (for scheduling): calculated by arm_next when the endpoint is idle
        // non-null = pending: call handler during service (notification semantics, handler self-sends +
        // on_urb_done reported). Empty = data is ready: send submit directly during service
        UrbProcessCallback processor;
        UsbIpResponse::UsbIpRetSubmit submit; // data is ready for use
        UsbEndpoint ep;                       // processor parameters (for processor version)
        EndpointAttributes type;              // Service time calculation: isochronous uses delay, others align to frames
        std::uint32_t seqnum = 0;             // cancel matching + on_urb_done verification
        std::chrono::microseconds delay{};    // isochronous: service time = serial point + data duration (bulk/int ignored)
        TransferHandle transfer;              // processor version: move to the processor when serving
        TransferResponder *responder = nullptr;           // is passed to the processor/sent during service (the submitter is guaranteed to survive until stop)
    };
    struct EndpointState {
        std::deque<PendingUrb> queue;
        // Last planned service time (self-sustaining chain benchmark determined when submitted): The service interval is only determined by
        // data_duration (including receive-stream feedback correction): processing time and timer trigger delay
        // does not enter the rhythm (on_urb_done only passes without recalculation). If we proceed according to the actual completion time,
        // processing time + trigger delay will be added to the service interval (actual measurement ~1.15ms), closed-loop limiting
        // cannot compensate, making the receive rate lower than the consumption rate → underrun noise
        std::chrono::steady_clock::time_point last_scheduled{};
        bool processing = false;            // Processing: handler notified, awaiting on_urb_done (at most one per endpoint at a time)
        std::uint32_t processing_seqnum = 0; // seqnum in  processing (on_urb_done check)
    };

    void run();

    /**
     * @brief Call with lock: When the timer is not scheduled (IDLE), post to the scheduling thread for scheduling
     *
     * Do nothing when it is scheduled (RUNNING) - the new URB is only queued and will be scanned when it expires.
     * keeps IDLE if the queue is empty. Once the schedule takes effect, it will not be canceled (align with v_timer of vudc)
     */
    void kick();

    /**
     * @brief Scheduling thread: Process the kick post request and schedule the timer according to the earliest queue head service time
     */
    void schedule_on_thread();

    /**
     * @brief Lock-holding call: schedule the timer according to the earliest service time of the queue head; no scheduling if the queue is empty (IDLE)
     * @note can only be called in the scheduling thread: cross-thread registration timer async_wait on Windows
     * Occasionally does not wake up (registration race condition when run() has no event pending). This problem does not occur when registering in the scheduling thread.
     */
    void schedule_next();

    /**
     * @brief Lock-holding call: When the endpoint is idle (not being processed) and the queue is not empty, the queue head is calculated according to the self-sustaining chain
     * service time and promote chain benchmarks
     *
     * service time = max (now, last planned service time) After: isochronous + data duration;
     * bulk/interrupt align to frame boundaries. Fixed at submission; subsequent on_urb_done does not recalculate
     * (processing time-consuming/trigger delay does not enter the rhythm)
     */
    void arm_next(std::uint8_t ep_address, EndpointState &state);

    /**
     * @brief Frame boundary alignment at service time (1ms grid, vudc frame timer semantics)
     */
    static std::chrono::steady_clock::time_point align_to_frame(std::chrono::steady_clock::time_point t);

    void on_timer(const asio::error_code &ec);

    std::mutex mutex;
    std::unordered_map<std::uint8_t, EndpointState> endpoints;
    // device bus speed: endpoint transaction interval is derived according to it (high speed 125µs×2^(bInterval-1), etc.)
    UsbSpeed speed;
    asio::io_context io_context;
    // run() keep alive: io_context When there is no pending operation, run() will return immediately (only later
    // arriving post/timer registrations are left unprocessed, making scheduling wait indefinitely). work_guard keeps
    // run() is resident until stop() is explicitly reset. Hold with unique_ptr:
    // work_guard contains reference members and cannot be reassigned
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard;
    asio::steady_timer timer{io_context};
    std::thread thread;
    bool started = false;
    // Whether the timer has been scheduled (RUNNING): New URBs are only queued and not rescheduled during the scheduling period.
    // is rearranged according to the queue after expiration processing (on_timer); the queue is idle IDLE (false)
    bool timer_pending = false;
};

} // namespace usbipdcpp