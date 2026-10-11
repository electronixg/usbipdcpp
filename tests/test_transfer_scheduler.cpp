#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/TransferResponder.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/virtual_device/TransferScheduler.h"

using namespace usbipdcpp;
using namespace std::chrono_literals;

namespace {

/// Response stub: replaces real Session (all submissions discarded/recorded; responses intercepted and asserted by TestTransferScheduler
/// on_urb_completed, without network access)
class StubResponder : public TransferResponder {
public:
    void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&) override {}
    void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&) override {}
    void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&) override {}
    void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&) override {}
    void wakeup_sender() override {}
    void stop_transfer() override {}
};

/// Scheduler test subclass collecting completion responses: override on_urb_completed to intercept responses and record
/// completion times for delay/cadence/content assertions
class TestTransferScheduler : public TransferScheduler {
public:
    explicit TestTransferScheduler(UsbSpeed speed) : TransferScheduler(speed) {}

    /// Wait for at least n collected responses; return false on timeout
    bool wait_for_response_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return responses.size() >= n; });
    }

    std::size_t response_count() {
        std::lock_guard lock(mutex);
        return responses.size();
    }

    /// Retrieve all responses (clear collection; correspond one-to-one with completion_times)
    std::vector<UsbIpResponse::UsbIpRetSubmit> take_responses() {
        std::lock_guard lock(mutex);
        return std::move(responses);
    }

    /// Completion times of responses (one-to-one correspondence)
    std::vector<std::chrono::steady_clock::time_point> completion_times() {
        std::lock_guard lock(mutex);
        return times;
    }

protected:
    void on_urb_completed(TransferResponder &, UsbIpResponse::UsbIpRetSubmit &&submit) override {
        {
            std::lock_guard lock(mutex);
            responses.push_back(std::move(submit));
            times.push_back(std::chrono::steady_clock::now());
        }
        cv.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<UsbIpResponse::UsbIpRetSubmit> responses;
    std::vector<std::chrono::steady_clock::time_point> times;
};

/// Test scaffold: StubResponder replaces real Session (subclass intercepts responses; no network access).
/// Destruction order (later declarations destroyed first): scheduler (internal thread stops first) → responder
struct TestFixture {
    StubResponder responder;
    TestTransferScheduler scheduler{UsbSpeed::High};

    void start() {
        scheduler.start();
    }
};

/// Convenience wrapper for isochronous submission (response OK + 10 bytes)
void submit_iso(TestTransferScheduler &scheduler, TransferResponder &responder, std::uint8_t ep,
                std::chrono::microseconds interval, int num_packets, std::uint32_t seqnum) {
    scheduler.submit(responder, ep, EndpointAttributes::Isochronous, interval, num_packets,
                     UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, 10));
}

} // namespace

// ==================== Endpoint-interval conversion (pure function) ====================

TEST(TransferSchedulerTest, EndpointIntervalHighSpeed) {
    // High speed: 2^(bInterval-1) × 125µs
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 1}, UsbSpeed::High), 125us);
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 2}, UsbSpeed::High), 250us);
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 3}, UsbSpeed::High), 500us);
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 4}, UsbSpeed::High), 1000us);
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 5}, UsbSpeed::High), 2000us);
}

TEST(TransferSchedulerTest, EndpointIntervalOtherSpeeds) {
    // Full/low speed: bInterval × 1ms
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 4}, UsbSpeed::Full), 4ms);
    // Super:bInterval × 125µs
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 4}, UsbSpeed::Super), 500us);
    // Defensive bInterval=0 handling: treat as 1
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 0}, UsbSpeed::High), 125us);
}

// ==================== Delay semantics ====================

TEST(TransferSchedulerTest, DelayedResponse) {
    // 1 packet × 20ms: response waits at least a full interval (verify only lower bound on coarse-timer platforms)
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, fx.responder,0x81, 20ms, 1, 7);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    auto responses = fx.scheduler.take_responses();
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0].header.seqnum, 7);
    EXPECT_EQ(responses[0].actual_length, 10);
    EXPECT_GE(elapsed, 15ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, MultiplePacketsExtendDelay) {
    // 5 packets × 10ms: delay 50ms (multiple packets distributed across multiple intervals)
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, fx.responder,0x81, 10ms, 5, 1);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 40ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, DataDurationOverridesInterval) {
    // Explicit data duration takes precedence over packet count×interval: 3 packets × 10ms = 30ms; data duration 60ms → delay 60ms
    // (simulates adaptive OUT following host data volume: response interval = audio duration corresponding to data)
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.responder, 0x81, EndpointAttributes::Isochronous, 10ms, 3,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(9, 10), 60ms);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 3s));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 50ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, DataDurationShorterThanIntervalProduct) {
    // Use data duration even when shorter than packet count×interval: 2 packets × 20ms = 40ms; data duration 10ms → about 10ms
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.responder, 0x81, EndpointAttributes::Isochronous, 20ms, 2,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(11, 10), 10ms);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, 8ms);
    EXPECT_LE(elapsed, 150ms); // Upper bound only needs to exclude a 40ms delay (ample even on slow-timer machines)
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, NegativeDataDurationRespondsImmediately) {
    // Negative data duration = early response (for buffer-level feedback correcting host submission overhead): deadline no earlier than
    // current time; complete when due without waiting packet count×interval
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.responder, 0x81, EndpointAttributes::Isochronous, 20ms, 3,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(12, 10), -500us);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_LE(std::chrono::steady_clock::now() - t0, 200ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 12);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, BulkDelayedOneFrame) {
    // bulk matches vudc frame-driven semantics: URB completes on frame boundary (no immediate response).
    // Relax upper-bound assertion to 100ms: Windows default timer granularity ~15.6ms; submillisecond
    // deadlines cannot trigger precisely (see TransferScheduler.cpp frame-alignment comment)
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.responder, 0x01, EndpointAttributes::Bulk, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(13, 10));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GT(elapsed, 0us);
    EXPECT_LT(elapsed, 100ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 13);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, InterruptDelayedOneFrame) {
    // interrupt like bulk: completes on frame boundary (no immediate response); same relaxed upper bound as bulk
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.responder, 0x82, EndpointAttributes::Interrupt, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(14, 10));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GT(elapsed, 0us);
    EXPECT_LT(elapsed, 100ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 14);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, BulkSerialCompletionSameEndpoint) {
    // 2 bulk URBs on same endpoint: serial completion (no overlap on same endpoint). Relax interval assertion:
    // Windows timer granularity may make them expire in same batch (average throughput still limited; see frame-alignment comment)
    TestFixture fx;
    fx.start();
    fx.scheduler.submit(fx.responder, 0x01, EndpointAttributes::Bulk, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(16, 10));
    fx.scheduler.submit(fx.responder, 0x01, EndpointAttributes::Bulk, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(17, 10));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(2, 2s));
    auto responses = fx.scheduler.take_responses();
    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0].header.seqnum, 16);
    EXPECT_EQ(responses[1].header.seqnum, 17);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ControlRespondsImmediately) {
    // Control requests bypass frame scheduling: immediate response (matching vudc ep0 without delay)
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.responder, 0x00, EndpointAttributes::Control, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(15, 10));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_LE(std::chrono::steady_clock::now() - t0, 200ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 15);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ZeroPacketsRespondImmediately) {
    // No isochronous packets: occupies no scheduling window; immediate response
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, fx.responder,0x81, 20ms, 0, 3);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_LE(std::chrono::steady_clock::now() - t0, 200ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 3);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, NegativePacketsRespondImmediately) {
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, fx.responder,0x81, 20ms, -1, 4);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 4);
    fx.scheduler.stop();
}

// ==================== Same-endpoint serialization ====================

TEST(TransferSchedulerTest, SerialCompletionSameEndpoint) {
    // 2 URBs on same endpoint, 1 packet each: later completion no earlier than submission time + one interval (late allowed, early forbidden),
    // with order preserved. Do not assert adjacent intervals: scheduling delay can bring adjacent URB completion times closer
    // (delivery can only occur after frame boundary, never before); interval lower-bound assertion invalid in this scenario
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, fx.responder,0x81, 60ms, 1, 1);
    auto submit2_at = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, fx.responder,0x81, 60ms, 1, 2);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(2, 3s));
    auto times = fx.scheduler.completion_times();
    ASSERT_EQ(times.size(), 2);
    EXPECT_GE(times[1] - submit2_at, 55ms) << "Second URB completed early";
    EXPECT_GT(times[1], times[0]);
    auto responses = fx.scheduler.take_responses();
    EXPECT_EQ(responses[0].header.seqnum, 1);
    EXPECT_EQ(responses[1].header.seqnum, 2);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, PacingAcrossManyUrbs) {
    // 4 URBs submitted consecutively on same endpoint: each waits at least one full interval (submission time + 40ms)
    // before completion, with strictly increasing completion times. Do not assert adjacent intervals; see SerialCompletionSameEndpoint
    TestFixture fx;
    fx.start();
    std::vector<std::chrono::steady_clock::time_point> submitted_at;
    for (std::uint32_t i = 1; i <= 4; ++i) {
        submitted_at.push_back(std::chrono::steady_clock::now());
        submit_iso(fx.scheduler, fx.responder,0x81, 40ms, 1, i);
    }
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(4, 4s));
    auto times = fx.scheduler.completion_times();
    ASSERT_EQ(times.size(), 4);
    for (std::size_t i = 0; i < times.size(); ++i) {
        // Timer can only trigger late, never early (5ms tolerance matches DelayedResponse style)
        EXPECT_GE(times[i] - submitted_at[i], 35ms) << "URB " << i + 1 << " Completed early";
    }
    for (std::size_t i = 1; i < times.size(); ++i) {
        EXPECT_GT(times[i], times[i - 1]) << "URB " << i << " Should be later than previous URB";
    }
    fx.scheduler.stop();
}

// ==================== Independent endpoints ====================

TEST(TransferSchedulerTest, DifferentEndpointsRunIndependently) {
    // Endpoint A every 40ms, B every 80ms: independent cadences without mutual delay
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, fx.responder,0x81, 40ms, 1, 1);
    submit_iso(fx.scheduler, fx.responder,0x82, 80ms, 1, 2);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(2, 3s));
    auto responses = fx.scheduler.take_responses();
    ASSERT_EQ(responses.size(), 2);
    // Faster A (seqnum 1) should complete first
    EXPECT_EQ(responses[0].header.seqnum, 1);
    EXPECT_EQ(responses[1].header.seqnum, 2);
    fx.scheduler.stop();
}

// ==================== Cancellation (UNLINK semantics) ====================

TEST(TransferSchedulerTest, CancelPendingUrb) {
    // Successful cancellation: return true and send no further RET_SUBMIT
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, fx.responder,0x81, 20ms, 1, 9);
    EXPECT_TRUE(fx.scheduler.cancel(9));
    // Wait beyond original deadline, confirming no response
    EXPECT_FALSE(fx.scheduler.wait_for_response_count(1, 200ms));
    EXPECT_EQ(fx.scheduler.response_count(), 0);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, CancelMissingUrbReturnsFalse) {
    TestFixture fx;
    fx.start();
    EXPECT_FALSE(fx.scheduler.cancel(123));
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, CanceledUrbKeepsPacingOfFollowers) {
    // Bus time occupied by canceled URB not reclaimed: subsequent URB still completes at original deadline, not early
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, fx.responder,0x81, 60ms, 1, 1); // deadline ≈ t0+60
    submit_iso(fx.scheduler, fx.responder,0x81, 60ms, 1, 2); // deadline ≈ t0+120 (serial)
    ASSERT_TRUE(fx.scheduler.cancel(1));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 2);
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 90ms);
    fx.scheduler.stop();
}

// ==================== Connection lifecycle ====================

TEST(TransferSchedulerTest, StopDropsPendingUrbs) {
    // Disconnect: discard incomplete URBs (no response); discard URBs submitted after stop too
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, fx.responder,0x81, 20ms, 1, 1);
    fx.scheduler.stop();
    EXPECT_EQ(fx.scheduler.response_count(), 0);
    submit_iso(fx.scheduler, fx.responder,0x81, 20ms, 1, 2);
    EXPECT_EQ(fx.scheduler.response_count(), 0);
}

TEST(TransferSchedulerTest, RestartAfterStopWorks) {
    // Reconnect: start after stop works again, with clean state
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, fx.responder,0x81, 20ms, 1, 1);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    fx.scheduler.take_responses(); // Clear first connection's responses
    fx.scheduler.stop();

    fx.start();
    submit_iso(fx.scheduler, fx.responder,0x81, 20ms, 1, 2);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 2);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, StopIsIdempotent) {
    TestFixture fx;
    fx.start();
    fx.scheduler.stop();
    fx.scheduler.stop(); // Second stop should not crash
}

// ==================== Scale and concurrency ====================

TEST(TransferSchedulerTest, ManyUrbsAllRespondInOrder) {
    // 30 consecutive URBs on same endpoint: all receive responses in submission order (increasing seqnum)
    TestFixture fx;
    fx.start();
    constexpr int kCount = 30;
    for (int i = 1; i <= kCount; ++i)
        submit_iso(fx.scheduler, fx.responder,0x81, 5ms, 1, static_cast<std::uint32_t>(i));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(kCount, 5s));
    auto responses = fx.scheduler.take_responses();
    ASSERT_EQ(responses.size(), kCount);
    for (int i = 0; i < kCount; ++i)
        EXPECT_EQ(responses[i].header.seqnum, static_cast<std::uint32_t>(i + 1));
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ConcurrentSubmitFromMultipleThreads) {
    // Multithreaded concurrent submission (simulate handler/scheduler thread concurrency): all respond without loss
    TestFixture fx;
    fx.start();
    constexpr int kThreads = 2;
    constexpr int kPerThread = 5;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                // Use distinct endpoints per thread, avoiding cumulative test duration
                submit_iso(fx.scheduler, fx.responder,static_cast<std::uint8_t>(0x81 + t), 30ms, 1,
                           static_cast<std::uint32_t>(t * kPerThread + i + 1));
            }
        });
    }
    for (auto &th: threads)
        th.join();
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(kThreads * kPerThread, 5s));
    EXPECT_EQ(fx.scheduler.response_count(), kThreads * kPerThread);
    fx.scheduler.stop();
}

// ==================== Processor variant (notification semantics: self-send + on_urb_done) ====================

TEST(TransferSchedulerTest, IsoProcessorCalledAfterDataDuration) {
    // iso processor variant: after data duration (30ms), scheduler thread notifies processor to start processing; processor
    // sends itself (collected in test) + reports via on_urb_done. Rate limiting uses "delayed service time"
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<UsbIpResponse::UsbIpRetSubmit> sent;
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        30ms, 21, {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                sent.push_back(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(
                                        seqnum, 10));
                            }
                            // Report completion after processing: advance serialization point, service next URB
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return !sent.empty(); }));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 25ms);
    EXPECT_EQ(sent[0].header.seqnum, 21);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, BulkProcessorCalledAtFrameBoundary) {
    // bulk processor variant: notify processor after frame alignment. Upper bound relaxed as in BulkDelayedOneFrame
    // (Windows timer granularity)
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    bool notified = false;
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x01, .interval = 0}, EndpointAttributes::Bulk, 0us, 22,
                        {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum, TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified = true;
                            }
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                            cv.notify_all();
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 1s, [&] { return notified; }));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 100ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ProcessorUrbsSerializeOnCompletion) {
    // Two iso processor URBs on same endpoint: service-time chain fixed at submission (40ms interval);
    // processing time/trigger delay excluded from cadence; but if processing is slow (exceeds chain interval), second URB
    // must wait for first on_urb_done to allow it (serialization safeguard)—processor 1 sleeps 60ms,
    // exceeding 40ms chain interval; second notification no earlier than first completion report
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::chrono::steady_clock::time_point> notify_at;
    // Initialize to max(): if processor 2 notified before processor 1 reports, assertion always fails (detectable)
    auto first_done_at = std::chrono::steady_clock::time_point::max();
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        40ms, 23, {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notify_at.push_back(std::chrono::steady_clock::now());
                                cv.notify_all();
                            }
                            // Simulate slow processing: block 60ms before reporting (exceeds 40ms chain interval;
                            // second must await report, not proceed early at already-passed chain time)
                            std::this_thread::sleep_for(60ms);
                            first_done_at = std::chrono::steady_clock::now();
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        40ms, 24, {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notify_at.push_back(std::chrono::steady_clock::now());
                                cv.notify_all();
                            }
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 3s, [&] { return notify_at.size() >= 2; }));
    ASSERT_EQ(notify_at.size(), 2);
    // Serialization safeguard: second notification no earlier than first completion report (when slow processing exceeds chain interval)
    EXPECT_GE(notify_at[1], first_done_at);
    EXPECT_GT(notify_at[1], notify_at[0]);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, CancelPendingProcessorUrb) {
    // Processor URB carries seqnum: cancellable before service (return true, processor not invoked);
    // cannot cancel after notifying processor (processing; return false)
    TestFixture fx;
    fx.start();
    bool notified = false;
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        30ms, 25, {}, [&](TransferResponder &, const UsbEndpoint &, std::uint32_t, TransferHandle &&) {
                            notified = true;
                        });
    EXPECT_TRUE(fx.scheduler.cancel(25));
    // Wait beyond original service time, confirming processor not invoked
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(notified);

    // Already notified (processing) URB cannot be canceled
    bool done = false;
    std::mutex m;
    std::condition_variable cv;
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        30ms, 26, {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                done = true;
                            }
                            cv.notify_all();
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return done; }));
    EXPECT_FALSE(fx.scheduler.cancel(26));
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, OnUrbDoneWrongSeqnumIgnored) {
    // on_urb_done seqnum must match processing URB: ignore reports with wrong seqnum,
    // without advancing endpoint (next URB not serviced)—erroneous report after processor finishes cleanup will not
    // disrupt cadence
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint32_t> notified_seqnums;
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        20ms, 31, {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified_seqnums.push_back(seqnum);
                            }
                            cv.notify_all();
                            if (seqnum == 31) {
                                // First report wrong seqnum (must have no effect), then report correctly
                                fx.scheduler.on_urb_done(ep.address, 999);
                                fx.scheduler.on_urb_done(ep.address, seqnum);
                            } else {
                                fx.scheduler.on_urb_done(ep.address, seqnum);
                            }
                        });
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        20ms, 32, {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified_seqnums.push_back(seqnum);
                            }
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                            cv.notify_all();
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 3s, [&] { return notified_seqnums.size() >= 2; }));
    // Order preserved: 31 before 32 (wrong report neither advanced nor skipped 32)
    EXPECT_EQ(notified_seqnums[0], 31);
    EXPECT_EQ(notified_seqnums[1], 32);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, OnUrbDoneFromOtherThreadConcurrentSubmit) {
    // Truly asynchronous processing: processor callback delegates delayed on_urb_done report to another thread
    //(simulating slow asynchronous device completion), while main thread continues concurrent submit—all complete,
    // order preserved, no deadlock. Counter ensures all detached threads finish before stop
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint32_t> notified_seqnums;
    int pending_done_threads = 0;
    std::atomic<bool> keep_submitting{true};
    // Network thread: continuous submission (on_urb_done called from processor callback on scheduler thread, a different thread)
    std::thread submitter([&] {
        std::uint32_t seq = 100;
        while (keep_submitting) {
            fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4},
                                EndpointAttributes::Isochronous, 10ms, seq, {},
                                [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                    TransferHandle &&) {
                                    {
                                        std::lock_guard lock(m);
                                        notified_seqnums.push_back(seqnum);
                                        pending_done_threads++;
                                    }
                                    cv.notify_all();
                                    // Asynchronous report: new thread calls on_urb_done after 5ms delay.
                                    // join waits for thread exit (on_urb_done indeed called from another
                                    // thread), avoiding detached-thread lifetime
                                    // race (thread still notifying while main thread destroys cv)
                                    std::thread t([&, ep, seqnum] {
                                        std::this_thread::sleep_for(5ms);
                                        fx.scheduler.on_urb_done(ep.address, seqnum);
                                        {
                                            std::lock_guard lock(m);
                                            pending_done_threads--;
                                        }
                                        cv.notify_all();
                                    });
                                    t.join();
                                });
            ++seq;
            std::this_thread::sleep_for(1ms);
        }
    });
    std::unique_lock lock(m);
    // Wait for all 10 URBs notified and all asynchronous-report threads finished
    ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return notified_seqnums.size() >= 10 && pending_done_threads == 0; }));
    keep_submitting = false;
    // Must unlock before join: submitter may submit another URB before exiting; its processor
    //(scheduler thread) needs m lock—main thread joining while holding lock deadlocks (with coarse Windows timer
    // granularity, final URBs scheduled in same batch before stop; Linux 1ms granularity yields different timing
    // without triggering it—timing-dependent deadlock; always unlock first in tests)
    lock.unlock();
    submitter.join();
    lock.lock();
    // Same-endpoint notifications serialized (submission order); asynchronous reporting preserves order
    for (std::size_t i = 1; i < notified_seqnums.size(); ++i)
        EXPECT_GT(notified_seqnums[i], notified_seqnums[i - 1]);
    lock.unlock();
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ProcessingEndpointDoesNotBlockOthers) {
    // Independent endpoints: A (60ms) submitted first and enters processing; B (20ms) URB notified normally at its own
    // cadence—A's processing state does not block B's schedule (processor follows interrupt
    // semantics, returning quickly; see UrbProcessCallback contract; here verify independent scheduling)
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint32_t> notified; // (seqnum)
    // A: notified after 60ms, reports immediately afterward (fast processing)
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        60ms, 51, {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified.push_back(seqnum);
                            }
                            cv.notify_all();
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    auto t0 = std::chrono::steady_clock::now();
    // B: notified after 20ms
    fx.scheduler.submit(fx.responder, UsbEndpoint{.address = 0x82, .interval = 4}, EndpointAttributes::Isochronous,
                        20ms, 52, {}, [&](TransferResponder &responder, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified.push_back(seqnum);
                            }
                            cv.notify_all();
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    std::unique_lock lock(m);
    // B arrives first (20ms): A's 60ms schedule does not delay B
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return notified.size() >= 1; }));
    EXPECT_EQ(notified[0], 52);
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 15ms);
    // A arrives afterward
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return notified.size() >= 2; }));
    EXPECT_EQ(notified[1], 51);
    fx.scheduler.stop();
}
