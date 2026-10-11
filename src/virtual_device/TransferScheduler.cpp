#include "usbipdcpp/virtual_device/TransferScheduler.h"

#include <algorithm>
#include <vector>

#include "usbipdcpp/Session.h"

namespace usbipdcpp {

namespace {

/// Wait duration to align the service start time to the next frame boundary (1ms grid):
/// vudc's frame timer advances in 1ms ticks; frame boundaries = 1ms grid.
/// Note: precision is limited by the platform timer resolution (Windows defaults to ~15.6ms granularity,
/// and asio timers cannot accurately trigger submillisecond deadlines); actual throttling granularity is
/// 1ms~15.6ms. With batched expirations, multiple URBs on the same endpoint may complete in one batch; average throughput remains limited
std::chrono::microseconds frame_alignment_delay(std::chrono::steady_clock::time_point service_start) {
    return std::chrono::microseconds(1000 - static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(service_start.time_since_epoch()).count() % 1000));
}

} // namespace

void TransferScheduler::start() {
    std::lock_guard lock(mutex);
    if (started)
        return;
    started = true;
    io_context.restart();
    work_guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io_context.get_executor());
    thread = std::thread([this] { run(); });
}

void TransferScheduler::stop() {
    {
        std::lock_guard lock(mutex);
        if (!started)
            return;
        started = false;
        // Connection disconnected: discard unfinished URBs (no response, matching vudc stop_activity's queue nuke)
        endpoints.clear();
        // Cancel scheduling: the aborted callback returns immediately because started=false
        timer_pending = false;
        timer.cancel();
    }
    // Release work_guard (allow run() to return with no work) + stop io_context
    // Make run() return immediately (an executing on_timer finishes first); join waits
    work_guard.reset();
    io_context.stop();
    if (thread.joinable())
        thread.join();
}

void TransferScheduler::submit(TransferResponder &responder, const UsbEndpoint &ep, EndpointAttributes type,
                               std::chrono::microseconds data_duration, std::uint32_t seqnum,
                               TransferHandle transfer, UrbProcessCallback processor) {
    // Safeguard: control requests do not use the processor variant (handler has no such call scenario); notify for processing immediately
    if (type == EndpointAttributes::Control) {
        processor(responder, ep, seqnum, std::move(transfer));
        return;
    }

    std::lock_guard lock(mutex);
    // URB arriving after stop: connection disconnected; discard directly
    if (!started)
        return;

    auto &state = endpoints[ep.address];
    bool queue_was_empty = state.queue.empty();
    state.queue.push_back(PendingUrb{{}, std::move(processor), {}, ep, type, seqnum, data_duration,
                                     std::move(transfer), &responder});
    // Fix service time on becoming queue head (self-sustaining chain); during processing, calculate only without scheduling
    //(schedule through on_urb_done's kick after release)
    if (queue_was_empty) {
        arm_next(ep.address, state);
        if (!state.processing)
            kick();
    }
}

void TransferScheduler::submit(TransferResponder &responder, const UsbEndpoint &ep, EndpointAttributes type,
                               int num_iso_packets, UsbIpResponse::UsbIpRetSubmit &&submit,
                               std::chrono::microseconds data_duration) {
    // Endpoint variant: derive interval from bInterval and device speed (matching the USB specification).
    // Parameter name submit shadows the member function; use this-> for explicit resolution
    this->submit(responder, ep.address, type, endpoint_interval(ep, speed), num_iso_packets, std::move(submit),
                 data_duration);
}

void TransferScheduler::submit(TransferResponder &responder, std::uint8_t ep_address, EndpointAttributes type,
                               std::chrono::microseconds interval, int num_iso_packets,
                               UsbIpResponse::UsbIpRetSubmit &&submit,
                               std::chrono::microseconds data_duration) {
    // Control requests and iso URBs without isochronous packets do not occupy a scheduling window; respond immediately
    //(control endpoints must be fast, matching vudc's delay-free ep0 handling)
    if (type == EndpointAttributes::Control || (type == EndpointAttributes::Isochronous && num_iso_packets <= 0)) {
        on_urb_completed(responder, std::move(submit));
        return;
    }

    std::lock_guard lock(mutex);
    // URB arriving after stop: connection disconnected; discard directly
    if (!started)
        return;

    auto &state = endpoints[ep_address];
    // Isochronous delay: explicit data duration takes priority (follows the host's data amount; negative values allow an early response,
    // used for closed-loop receive-rate control); otherwise packet count x interval.
    // bulk/interrupt: align service time to frame boundaries (done in arm_next; delay unused)
    auto delay = (type == EndpointAttributes::Isochronous)
                         ? (data_duration != std::chrono::microseconds::zero()
                                    ? data_duration
                                    : interval * num_iso_packets)
                         : std::chrono::microseconds::zero();
    auto seqnum = submit.header.seqnum;
    bool queue_was_empty = state.queue.empty();
    state.queue.push_back(PendingUrb{{}, {}, std::move(submit), UsbEndpoint{}, type, seqnum, delay, {}, &responder});
    // Fix service time on becoming queue head (self-sustaining chain); during processing, calculate only without scheduling
    //(schedule through on_urb_done's kick after release)
    if (queue_was_empty) {
        arm_next(ep_address, state);
        if (!state.processing)
            kick();
    }
}

void TransferScheduler::on_urb_done(std::uint8_t ep_address, std::uint32_t seqnum) {
    std::lock_guard lock(mutex);
    // Reports arriving after stop (queue already cleared): ignore
    if (!started)
        return;
    auto it = endpoints.find(ep_address);
    if (it == endpoints.end())
        return;
    auto &state = it->second;
    // Validate seqnum: must match the URB being processed (duplicate reports after processor cleanup are ignored)
    if (!state.processing || state.processing_seqnum != seqnum)
        return;
    // Only release, without recalculating: the next URB's service time was fixed on submission (self-sustaining chain),
    // so processing time/timer-trigger delay do not enter the cadence. If processing is slow (chain time has passed),
    // schedule_next schedules the already-past service_time, triggering service immediately
    state.processing = false;
    state.processing_seqnum = 0;
    if (!state.queue.empty())
        kick();
}

bool TransferScheduler::cancel(std::uint32_t seqnum) {
    std::lock_guard lock(mutex);
    for (auto &[ep, state] : endpoints) {
        // URBs already handed to the processor (being processed) cannot be cancelled
        if (state.processing && state.processing_seqnum == seqnum)
            return false;
        for (auto it = state.queue.begin(); it != state.queue.end(); ++it) {
            if (it->seqnum == seqnum) {
                auto is_front = (it == state.queue.begin());
                auto canceled_service_time = it->service_time;
                state.queue.erase(it);
                // If the cancelled URB is queue head, its service window is not reclaimed (matching a real bus: a time slot
                // passes even if empty); advance the chain baseline to its service time, and successor URBs still complete
                // at the original cadence, without advancing early
                if (is_front)
                    state.last_scheduled = std::max(state.last_scheduled, canceled_service_time);
                // Pending URBs remain and the endpoint is idle: recalculate the new queue head's service time
                if (!state.processing && !state.queue.empty())
                    arm_next(ep, state);
                return true;
            }
        }
    }
    return false;
}

std::chrono::microseconds TransferScheduler::endpoint_interval(const UsbEndpoint &ep, UsbSpeed speed) {
    auto interval = ep.interval;
    if (interval == 0)
        interval = 1; // Guard against invalid bInterval=0
    switch (speed) {
        case UsbSpeed::High:
            // High speed: 2^(bInterval-1) microframes, each 125us (bInterval maximum 16)
            interval = std::min(interval, static_cast<std::uint8_t>(16));
            return std::chrono::microseconds(125) * (std::uint64_t{1} << (interval - 1));
        case UsbSpeed::Super:
        case UsbSpeed::SuperPlus:
            // SuperSpeed:bInterval × 125µs
            return std::chrono::microseconds(125) * interval;
        default:
            // Full/low speed: bInterval frames, each 1ms
            return std::chrono::milliseconds(1) * interval;
    }
}

void TransferScheduler::run() {
    io_context.run();
}

void TransferScheduler::on_urb_completed(TransferResponder &current_session, UsbIpResponse::UsbIpRetSubmit &&submit) {
    current_session.submit_ret_submit(std::move(submit));
}

void TransferScheduler::kick() {
    // Timer already scheduled (RUNNING): new URBs only enter the queue and will be found during expiration handling
    //(serial execution on the same endpoint guarantees increasing service times, without delay by the existing schedule)
    if (timer_pending)
        return;
    // Leave scheduling to the scheduler thread (post wakeup is a reliable core io_context mechanism):
    // direct cross-thread timer registration occasionally fails to wake on Windows; see the schedule_next comment
    asio::post(io_context, [this] { schedule_on_thread(); });
}

void TransferScheduler::schedule_on_thread() {
    std::lock_guard lock(mutex);
    if (!started)
        return;
    // The other path (on_timer's schedule_next) may already have scheduled it
    if (timer_pending)
        return;
    schedule_next();
}

void TransferScheduler::schedule_next() {
    auto next = std::chrono::steady_clock::time_point::max();
    for (auto &[ep, state] : endpoints) {
        // Being processed (waiting for on_urb_done) or queue empty: do not participate in scheduling
        if (state.processing || state.queue.empty())
            continue;
        next = std::min(next, state.queue.front().service_time);
    }
    if (next == std::chrono::steady_clock::time_point::max())
        return; // Queue empty: timer becomes IDLE (matching vudc's IDLE state)

    timer_pending = true;
    timer.expires_at(next);
    timer.async_wait([this](const asio::error_code &ec) { on_timer(ec); });
}

void TransferScheduler::arm_next(std::uint8_t ep_address, EndpointState &state) {
    if (state.queue.empty())
        return;
    auto &front = state.queue.front();
    auto now = std::chrono::steady_clock::now();
    // First schedule (chain baseline uninitialized = epoch): start from submission time; otherwise
    // epoch + delay is far earlier than now, so max takes now and the first URB responds immediately
    if (state.last_scheduled == std::chrono::steady_clock::time_point{})
        state.last_scheduled = now;
    // Self-sustaining chain: service time = max(now, last scheduled service time + delay). Fix the chain when becoming
    // queue head (submission/taking the previous URB/cancelling the head): when the host submits ahead (pool mechanism),
    // service time follows the chain; processing time, timer-trigger delay, and host resubmission delay never
    // enter the cadence (starting from actual completion or submission would add these overheads to the interval, beyond the receive-rate
    // loop's compensation limits -> receive rate below consumption rate -> underrun noise). If the host submits too slowly
    //(chain time already passed), use now and serve immediately (host controls the cadence)
    // Isochronous: completion cadence = host data rate (data duration, possibly negative for early service);
    // bulk/interrupt: align to frame boundaries (wait 0-1ms, average 0.5ms)
    front.service_time = (front.type == EndpointAttributes::Isochronous)
                                 ? std::max(now, state.last_scheduled + front.delay)
                                 : align_to_frame(std::max(now, state.last_scheduled));
    state.last_scheduled = front.service_time;
}

std::chrono::steady_clock::time_point TransferScheduler::align_to_frame(std::chrono::steady_clock::time_point t) {
    return t + frame_alignment_delay(t);
}

void TransferScheduler::on_timer(const asio::error_code &ec) {
    struct DueUrb {
        TransferResponder *responder;
        PendingUrb urb;
        std::uint8_t ep_address;
    };
    std::vector<DueUrb> due;
    {
        std::lock_guard lock(mutex);
        timer_pending = false;
        // aborted: only stop() cancellation of scheduling; connection disconnected, so do not process
        if (ec || !started)
            return;

        auto now = std::chrono::steady_clock::now();
        // Serve all due queue-head URBs (at most one per endpoint: skip endpoints being processed;
        // different endpoints may become due simultaneously). Take the head and mark it processing; the next URB on the same
        // endpoint must wait for handler's on_urb_done report before scheduling
        for (auto &[addr, state] : endpoints) {
            if (state.processing || state.queue.empty())
                continue;
            auto &front = state.queue.front();
            if (front.service_time <= now) {
                due.push_back({front.responder, std::move(front), addr});
                state.queue.pop_front();
                state.processing = true;
                state.processing_seqnum = due.back().urb.seqnum;
                // The next URB becomes head: immediately fix service time according to the self-sustaining chain (fixed when taking the previous URB,
                // so processing time/trigger delay do not enter cadence; on_urb_done only releases without recalculating)
                arm_next(addr, state);
            }
        }
        // The queue may still have URBs not yet due: schedule the next expiration
        schedule_next();
    }
    // Process outside the lock (processors can safely reenter submit()/on_urb_done(), without holding locks or blocking):
    // URB requiring processing -> notify processor (handler sends itself + reports on_urb_done);
    // URB with data ready -> send directly; sending completes it, immediately advancing the serialization point
    for (auto &d : due) {
        if (d.urb.processor) {
            d.urb.processor(*d.responder, d.urb.ep, d.urb.seqnum, std::move(d.urb.transfer));
        }
        else {
            on_urb_completed(*d.responder, std::move(d.urb.submit));
            on_urb_done(d.ep_address, d.urb.seqnum);
        }
    }
}

TransferScheduler::~TransferScheduler() {
    stop();
}

} // namespace usbipdcpp
