#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <thread>
#include <vector>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/utils/RingBuffer.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"

using namespace usbipdcpp;

namespace {

// Test channel: records sent responses, simulates message-mode buffering (consume entire message).
// push/push_blocking/push_locked implementations match real MessageInChannel line for line
// (members have identical names pending/max_pending_; only "submit response through session" in try_send_one/reply_empty
// is replaced by recording to sent); debug_front is a test-only observation point (no such API in the real channel)
class RecordingChannel : public InEndpointChannelBase<RecordingChannel> {
public:
    struct Sent {
        std::uint32_t seqnum;
        std::uint32_t length;
        data_type data;
    };

    std::vector<Sent> sent;
    std::function<data_type(std::uint32_t)> pull; // Optional pull, simulating a derived-class override
    int pull_calls = 0;

    // Same API as real MessageInChannel: set_max_pending_messages / size (tests use only the public interface)
    void set_max_pending_messages(std::size_t max_pending) {
        std::lock_guard lock(this->channel_mutex);
        max_pending_ = max_pending;
    }

    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return pending.size();
    }

    /// For testing only: inspect the oldest buffered message (no such API in the real channel)
    data_type debug_front() const {
        std::lock_guard lock(this->channel_mutex);
        return pending.empty() ? data_type{} : pending.front();
    }

    // ===== The following matches the real MessageInChannel implementation line for line (member names need no changes,
    // as they are already identical) =====

    bool push(data_type data, bool drop_oldest = true) {
        std::lock(this->channel_mutex, this->requests_mutex);
        std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
        std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
        // Stop receiving after disconnection: return false to tell the caller this send failed (data would be cleared on the next
        // connection, accumulating uselessly; matches write_nb returning 0 on disconnection)
        if (this->disconnected) {
            return false;
        }
        // Full and caller does not drop old messages: do not enqueue; tell caller it is full
        if (!drop_oldest && max_pending_ != 0 && pending.size() >= max_pending_) {
            return false;
        }
        push_locked(std::move(data));
        this->try_send_pending_locked();
        return true;
    }

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
            return false; // Still full after wait timeout
        }
        // Space available: additionally lock requests, enqueue, and advance pending requests (same lock order as push)
        std::lock_guard lock2(this->requests_mutex);
        push_locked(std::move(data));
        this->try_send_pending_locked();
        return true;
    }

    void push_locked(data_type data) {
        // Buffer over limit: drop oldest (keep-latest-message semantics)
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            pending.pop_front();
        }
        pending.emplace_back(std::move(data));
    }

    bool buffer_empty() const {
        return pending.empty();
    }

    void buffer_clear() {
        pending.clear();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // Take the oldest message, truncated to length; part matching real implementation (real code here sets transfer data via set_transfer_data +
        // submits the response through session; stub records sent instead)
        auto &front = pending.front();
        auto send_len = std::min(front.size(), static_cast<std::size_t>(length));
        sent.push_back({seqnum, static_cast<std::uint32_t>(send_len),
                        data_type(front.begin(), front.begin() + send_len)});
        pending.pop_front();
        this->space_cv.notify_one(); // Free a slot, waking blocked push_blocking
    }

    void reply_empty(std::uint32_t seqnum, TransferHandle /*transfer*/) {
        // Evicted pending request: real code here submits an empty response (0 bytes) through session; stub records it instead
        sent.push_back({seqnum, 0, {}});
    }

    data_type try_pull_data(std::uint32_t length) {
        pull_calls++;
        return pull ? pull(length) : data_type{};
    }

    void send_pulled_locked(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer,
                            data_type pulled) {
        // Matches MessageInChannel: buffer the entire message, then consume it through this request
        push_locked(std::move(pulled));
        try_send_one(ep, seqnum, length, std::move(transfer));
    }

private:
    std::deque<data_type> pending;
    std::size_t max_pending_ = 0;
};

// Byte-stream fragmentation variant: RingBuffer buffering, consumed in request-length fragments (simulates ByteStreamInChannel)
class ShardChannel : public InEndpointChannelBase<ShardChannel> {
public:
    struct Sent {
        std::uint32_t seqnum;
        std::uint32_t length;
    };

    std::vector<Sent> sent;
    RingBuffer buffer{64 * 1024};
    std::function<data_type(std::uint32_t)> pull; // Optional pull, simulating try_pull_data
    int pull_calls = 0;

    // Same API as real ByteStreamInChannel: size (number of buffered bytes awaiting transmission)
    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.size();
    }

    void write(const std::uint8_t *data, std::size_t size) {
        std::lock(channel_mutex, requests_mutex);
        std::lock_guard lock1(channel_mutex, std::adopt_lock);
        std::lock_guard lock2(requests_mutex, std::adopt_lock);
        buffer.write(data, size);
        try_send_pending_locked();
    }

    bool buffer_empty() const {
        return buffer.empty();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // Extract a request-length fragment, leaving the remainder buffered
        std::size_t send_len = std::min(buffer.size(), static_cast<std::size_t>(length));
        data_type data(send_len);
        buffer.read(data.data(), send_len);
        sent.push_back({seqnum, static_cast<std::uint32_t>(send_len)});
        space_cv.notify_one(); // Free space, waking blocked writers
    }

    void reply_empty(std::uint32_t seqnum, TransferHandle /*transfer*/) {
        // Evicted pending request: record empty completion (0 bytes); transfer destruction releases it
        sent.push_back({seqnum, 0});
    }

    data_type try_pull_data(std::uint32_t length) {
        pull_calls++;
        return pull ? pull(length) : data_type{};
    }

    void send_pulled_locked(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer,
                            data_type pulled) {
        // Matches ByteStreamInChannel: this request first sends the min portion, buffering the remainder
        std::size_t send_len = std::min(pulled.size(), static_cast<std::size_t>(length));
        sent.push_back({seqnum, static_cast<std::uint32_t>(send_len)});
        if (pulled.size() > send_len) {
            buffer.write(pulled.data() + send_len, pulled.size() - send_len);
        }
    }

    void push_locked(data_type data) {
        buffer.write(data.data(), data.size());
    }

    void buffer_clear() {
        buffer.clear();
    }
};

// Construct a transfer handle (op is a process-wide singleton: handles of requests left in the queue may be destroyed only at test end,
// when channel is destroyed; static op guarantees validity independently of local-variable lifetime)
struct TransferMaker {
    static inline GenericTransferOperator op;  // Stateless; concurrent free from multiple threads is safe
    TransferHandle make() {
        return TransferHandle(new GenericTransfer{}, &op);
    }
};

} // namespace

TEST(TestInEndpointChannel, RequestPendsThenPushAnswers) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // No data: request pending
    channel.on_in_request(0x81, 10, 8, maker.make());
    EXPECT_TRUE(channel.sent.empty());

    // Data arrives: match pending request and respond
    channel.push({1, 2, 3, 4, 5});
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 10u);
    EXPECT_EQ(channel.sent[0].length, 5u);
    EXPECT_EQ(channel.sent[0].data, (data_type{1, 2, 3, 4, 5}));
}

TEST(TestInEndpointChannel, BufferedDataServedImmediately) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.push({0xAA, 0xBB});
    channel.on_in_request(0x81, 20, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 20u);
    EXPECT_EQ(channel.sent[0].data, (data_type{0xAA, 0xBB}));
}

TEST(TestInEndpointChannel, MessageTruncatedToRequestLength) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.push({1, 2, 3, 4, 5});
    channel.on_in_request(0x81, 30, 3, maker.make()); // Request needs only 3 bytes

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 3u);
    EXPECT_EQ(channel.sent[0].data, (data_type{1, 2, 3}));
    EXPECT_EQ(channel.size(), 0u); // Consume entire message, discarding remainder (message semantics)
}

TEST(TestInEndpointChannel, SameEndpointFifoOrder) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Two requests on the same endpoint become pending in order; data arrives message by message: respond to the earlier request first
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.push({0x01});
    channel.push({0x02});

    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
}

TEST(TestInEndpointChannel, PendingRequestBlocksLaterBufferedRequest) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // When an endpoint already has pending requests, a new request must queue even if data is buffered (FIFO)
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.push({0x01});       // Respond to request 1
    channel.push({0x02});       // No more pending requests; buffer it
    channel.on_in_request(0x81, 2, 8, maker.make()); // Buffered data → respond immediately

    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[1].data, (data_type{0x02}));
}

TEST(TestInEndpointChannel, MaxPendingDropsOldest) {
    RecordingChannel channel;
    channel.on_new_connection();
    channel.set_max_pending_messages(2);
    TransferMaker maker;

    // Push 3 messages without requests: message 1 dropped (keep-latest-message semantics)
    channel.push({0x01});
    channel.push({0x02});
    channel.push({0x03});

    channel.on_in_request(0x81, 40, 8, maker.make());
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].data, (data_type{0x02}));
}

TEST(TestInEndpointChannel, PullProducesDataOnRequest) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Pull model: when a request arrives without buffered data, the callback generates data on the spot
    channel.pull = [](std::uint32_t length) { return data_type{0x42}; };
    channel.on_in_request(0x81, 50, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 50u);
    EXPECT_EQ(channel.sent[0].data, (data_type{0x42}));
    EXPECT_EQ(channel.sent[0].length, 1u);
    EXPECT_EQ(channel.pull_calls, 1);
}

TEST(TestInEndpointChannel, PullDataTruncatedToRequestLength) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // An entire packet generated by pull passes through the buffer: request takes only the first 4 bytes, consuming the entire message
    channel.pull = [](std::uint32_t length) { return data_type{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}; };
    channel.on_in_request(0x81, 51, 4, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 4u);
    EXPECT_EQ(channel.sent[0].data, (data_type{1, 2, 3, 4}));
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, PullNotCalledWhenSameEndpointHasPending) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // pull returns empty (no data available to generate on the spot)
    channel.pull = [](std::uint32_t length) { return data_type{}; };

    // Request A: pull invoked but no data → request pending
    channel.on_in_request(0x81, 1, 8, maker.make());
    EXPECT_EQ(channel.pull_calls, 1);
    // Same-endpoint request B arrives: already pending on that endpoint; should not invoke pull again
    channel.on_in_request(0x81, 2, 8, maker.make());
    EXPECT_EQ(channel.pull_calls, 1);
    // Different-endpoint request C: may invoke pull
    channel.on_in_request(0x82, 3, 8, maker.make());
    EXPECT_EQ(channel.pull_calls, 2);
    // A remains pending (empty pull data did not consume the request)
    EXPECT_TRUE(channel.sent.empty());
}

TEST(TestInEndpointChannel, ByteStreamPullPrefersCurrentRequest) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // pull generates 10 bytes; request needs only 4: send all 4 for this request, leave 6 buffered
    channel.pull = [](std::uint32_t length) { return data_type{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}; };
    channel.on_in_request(0x81, 70, 4, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 70u);
    EXPECT_EQ(channel.sent[0].length, 4u);
    EXPECT_EQ(channel.size(), 6u);

    // Subsequent request consumes the remainder
    channel.on_in_request(0x81, 71, 8, maker.make());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].length, 6u);
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, ByteStreamPullAllSentWhenFits) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // pull data ≤ request length: send all, leaving nothing buffered
    channel.pull = [](std::uint32_t length) { return data_type{1, 2, 3}; };
    channel.on_in_request(0x81, 72, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 3u);
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, ByteStreamChannelLifecycle) {
    // Real byte-stream channel: instantiation + connection lifecycle + write/read, without triggering session use
    // (full pending-response path covered by end-to-end tests)
    ByteStreamInChannel channel;

    channel.set_capacity(1024);
    EXPECT_EQ(channel.capacity(), 1024u);
    EXPECT_EQ(channel.size(), 0u);

    // Initially disconnected: write_nb rejects directly
    const std::uint8_t data[] = {1, 2, 3};
    EXPECT_EQ(channel.write_nb(data, 3), 0u);

    // Writable after connection; queries correct
    channel.on_new_connection();
    EXPECT_EQ(channel.write_nb(data, 3), 3u);
    EXPECT_EQ(channel.size(), 3u);
    EXPECT_EQ(channel.available(), 1021u);

    // Disconnection clears buffer
    channel.on_disconnection();
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, EmptyMessagePushed) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Empty message: respond with 0 bytes (consistent with host-request semantics; request consumed)
    channel.push({});
    channel.on_in_request(0x81, 60, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 60u);
    EXPECT_EQ(channel.sent[0].length, 0u);
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, MultipleEndpointsIndependentFifo) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Two requests for 0x81, one for 0x82: independent FIFO on each endpoint
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x82, 2, 8, maker.make());
    channel.on_in_request(0x81, 3, 8, maker.make());
    channel.push({0x01});
    channel.push({0x02});
    channel.push({0x03});

    ASSERT_EQ(channel.sent.size(), 3u);
    // The two 0x81 requests must follow order 1 → 3 (0x82 request may interleave, but they do not reorder relative to each other)
    std::vector<std::uint32_t> order;
    for (auto &s: channel.sent) {
        if (s.seqnum == 1 || s.seqnum == 3) {
            order.push_back(s.seqnum);
        }
    }
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1u);
    EXPECT_EQ(order[1], 3u);
    // The 0x82 request also receives a response
    bool ep82_served = false;
    for (auto &s: channel.sent) {
        ep82_served |= (s.seqnum == 2);
    }
    EXPECT_TRUE(ep82_served);
}

TEST(TestInEndpointChannel, ManyPendingRequestsServedInOrder) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // All 10 requests pending, then push messages individually: all must receive responses in enqueue order
    for (std::uint32_t i = 1; i <= 10; i++) {
        channel.on_in_request(0x81, i, 8, maker.make());
    }
    for (int i = 1; i <= 10; i++) {
        channel.push(data_type{static_cast<std::uint8_t>(i)});
    }

    ASSERT_EQ(channel.sent.size(), 10u);
    for (std::uint32_t i = 0; i < 10; i++) {
        EXPECT_EQ(channel.sent[i].seqnum, i + 1);
        EXPECT_EQ(channel.sent[i].data, (data_type{static_cast<std::uint8_t>(i + 1)}));
    }
}

TEST(TestInEndpointChannel, ByteStreamShardingAcrossRequests) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Write 100 bytes, request 64 → respond 64, leave 36; request again → respond 36 (fragmentation semantics)
    data_type payload(100, 0xAB);
    channel.write(payload.data(), payload.size());
    channel.on_in_request(0x81, 1, 64, maker.make());
    channel.on_in_request(0x81, 2, 64, maker.make());

    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[0].length, 64u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[1].length, 36u);
}

TEST(TestInEndpointChannel, LengthZeroRequestDoesNotConsumeStreamData) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Byte-stream mode: length=0 request receives 0 bytes, retaining buffered data for subsequent requests
    data_type payload(10, 0xAB);
    channel.write(payload.data(), payload.size());
    channel.on_in_request(0x81, 1, 0, maker.make());
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 0u);
    EXPECT_NE(channel.size(), 0u); // Buffered data not consumed

    channel.on_in_request(0x81, 2, 64, maker.make());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].length, 10u);
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, LengthZeroRequestConsumesOneMessage) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Message mode: length=0 request receives 0 bytes but still consumes one message (message semantics)
    channel.push({1, 2, 3, 4, 5});
    channel.on_in_request(0x81, 1, 0, maker.make());
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 0u);
    EXPECT_EQ(channel.size(), 0u); // Message consumed

    channel.push({0x07});
    channel.on_in_request(0x81, 2, 8, maker.make());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].data, (data_type{0x07}));
}

TEST(TestInEndpointChannel, RequestsOutnumberMessages) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 3 requests pending, only 1 message arrives: respond to 1, leave the others pending
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.on_in_request(0x81, 3, 8, maker.make());
    channel.push({0x01});

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    // Remaining requests can receive responses from subsequent data
    channel.push({0x02});
    channel.push({0x03});
    ASSERT_EQ(channel.sent.size(), 3u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[2].seqnum, 3u);
}

TEST(TestInEndpointChannel, PullNotCalledWhenBufferHasData) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Buffered data produces a direct response without invoking pull (pull is a fallback only when the buffer is empty)
    channel.pull = [](std::uint32_t length) { return data_type{0x42}; };
    channel.push({0x01});
    channel.on_in_request(0x81, 1, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].data, (data_type{0x01}));
    EXPECT_EQ(channel.pull_calls, 0);
}

TEST(TestInEndpointChannel, DoubleDisconnectionIdempotent) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_disconnection();
    channel.on_disconnection(); // Repeated disconnection is idempotent; no crash or incorrect clearing

    EXPECT_FALSE(channel.cancel_pending(1));
    EXPECT_EQ(channel.size(), 0u);
    channel.on_new_connection();
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.push({0x01});
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 2u);
}

TEST(TestInEndpointChannel, ByteStreamHangingRequestServedByWrite) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Request pending first, data arrives later: write matches the pending request
    channel.on_in_request(0x81, 1, 64, maker.make());
    data_type payload(10, 0xCD);
    channel.write(payload.data(), payload.size());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[0].length, 10u);
}

TEST(TestInEndpointChannel, CancelPendingRequest) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_in_request(0x81, 60, 8, maker.make());
    channel.on_in_request(0x82, 61, 8, maker.make());

    EXPECT_TRUE(channel.cancel_pending(60));
    EXPECT_FALSE(channel.cancel_pending(60)); // Already canceled
    EXPECT_FALSE(channel.cancel_pending(999));

    // One remaining request can still receive a response
    channel.push({0x01});
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 61u);
}

TEST(TestInEndpointChannel, DisconnectionClearsEverything) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_in_request(0x81, 70, 8, maker.make());
    channel.push({0x01});
    ASSERT_EQ(channel.sent.size(), 1u);

    channel.on_disconnection();
    // Queue and buffer cleared
    EXPECT_FALSE(channel.cancel_pending(70));
    EXPECT_EQ(channel.size(), 0u);
    // After disconnection, push no longer matches requests (none pending; data buffered but not sent)
    channel.push({0x02});
    EXPECT_EQ(channel.sent.size(), 1u);

    // Reconnect: buffer cleared, operation restored (pending requests can receive responses normally)
    channel.on_new_connection();
    EXPECT_EQ(channel.size(), 0u);
    channel.on_in_request(0x81, 71, 8, maker.make());
    channel.push({0x03});
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].seqnum, 71u);
    EXPECT_EQ(channel.sent[1].data, (data_type{0x03}));
}

// ===== Concurrency: request side (receiver thread) competes with data side (application thread) =====

TEST(TestInEndpointChannel, ConcurrentPushAndRequestsNoLostRequests) {
    constexpr int kRequests = 500;

    // maker must be declared before channel (destroyed later): pending transfers may remain queued,
    // and channel destruction still needs it to release them; op is stateless and delete is thread-safe, so it may be shared across threads
    TransferMaker maker;
    RecordingChannel channel;
    channel.on_new_connection();

    // Request thread: send IN requests continuously (simulate session receiver thread)
    std::thread requester([&]() {
        for (int i = 0; i < kRequests; i++) {
            channel.on_in_request(0x81, static_cast<std::uint32_t>(i), 8, maker.make());
        }
    });
    // Data thread: push messages continuously (simulate application thread)
    std::thread pusher([&]() {
        for (int i = 0; i < kRequests; i++) {
            channel.push(data_type{static_cast<std::uint8_t>(i)});
        }
    });
    requester.join();
    pusher.join();

    // Thread exit order is unspecified: request thread may end after data thread; the final batch of requests may stay pending
    // awaiting data. Supply data for pending requests, verifying "with continuous data supply, requests are neither lost nor duplicated"
    std::vector<int> counts(kRequests, 0);
    for (auto &s: channel.sent) {
        ASSERT_LT(s.seqnum, kRequests);
        counts[s.seqnum]++;
    }
    for (int i = 0; i < kRequests; i++) {
        if (counts[i] == 0) {
            channel.push(data_type{static_cast<std::uint8_t>(i)});
            counts[i] = 1;
        }
    }

    // Exactly one response per request (component must neither lose requests nor respond twice)
    ASSERT_EQ(channel.sent.size(), kRequests);
    for (int i = 0; i < kRequests; i++) {
        EXPECT_EQ(counts[i], 1) << "seqnum " << i;
    }
}

TEST(TestInEndpointChannel, ConcurrentByteStreamWriteAndRequests) {
    constexpr int kRequests = 200;
    constexpr std::size_t kBytesPerWrite = 10;

    // maker must be declared before channel (destroyed later), for the same reason as ConcurrentPushAndRequestsNoLostRequests
    TransferMaker maker;
    ShardChannel channel;
    channel.on_new_connection();

    std::thread requester([&]() {
        for (int i = 0; i < kRequests; i++) {
            channel.on_in_request(0x81, static_cast<std::uint32_t>(i), 64, maker.make());
        }
    });
    std::thread writer([&]() {
        for (int i = 0; i < kRequests; i++) {
            data_type payload(kBytesPerWrite, static_cast<std::uint8_t>(i));
            channel.write(payload.data(), payload.size());
        }
    });
    requester.join();
    writer.join();

    // Thread exit order is unspecified: final requests may await data. Write supplementary data and verify:
    // no lost requests and conservation of total byte-stream volume (all 2000 written bytes consumed, buffer cleared)
    std::vector<int> counts(kRequests, 0);
    std::size_t total_sent = 0;
    for (auto &s: channel.sent) {
        ASSERT_LT(s.seqnum, kRequests);
        counts[s.seqnum]++;
        total_sent += s.length;
    }
    // After supplementary writes, each request receives exactly one response
    std::size_t topped_up = 0;
    for (int i = 0; i < kRequests; i++) {
        if (counts[i] == 0) {
            data_type payload(kBytesPerWrite, static_cast<std::uint8_t>(i));
            channel.write(payload.data(), payload.size());
            counts[i] = 1;
            topped_up++;
        }
    }
    ASSERT_EQ(channel.sent.size(), kRequests);
    for (int i = 0; i < kRequests; i++) {
        EXPECT_EQ(counts[i], 1) << "seqnum " << i;
    }
    // Data conservation: total sent bytes + buffered remainder == total written (original + supplementary).
    // Note that some supplementary data may remain buffered (queue already empty when written); cannot assert an empty buffer
    std::size_t total_sent_after = 0;
    for (auto &s: channel.sent) {
        total_sent_after += s.length;
    }
    std::size_t total_written = kRequests * kBytesPerWrite + topped_up * kBytesPerWrite;
    EXPECT_EQ(total_sent_after + channel.size(), total_written);
}

TEST(TestInEndpointChannel, ConcurrentDisconnectAndPushNoCrash) {
    RecordingChannel channel;
    channel.on_new_connection();

    std::atomic<bool> running{true};
    std::thread pusher([&]() {
        while (running) {
            channel.push({0x01});
        }
    });
    std::thread disconnecter([&]() {
        for (int i = 0; i < 200; i++) {
            channel.on_disconnection();
            channel.on_new_connection();
        }
    });
    disconnecter.join();
    running = false;
    pusher.join();
    // No crash is sufficient; requests receive no response after disconnection (data only enters buffer)
    EXPECT_LE(channel.sent.size(), 1u);
}

TEST(TestInEndpointChannel, ConcurrentRequestsAndDisconnect) {
    // maker must be declared before channel (destroyed later), for the same reason as ConcurrentPushAndRequestsNoLostRequests
    TransferMaker maker;
    RecordingChannel channel;
    channel.on_new_connection();

    std::atomic<bool> running{true};
    std::thread requester([&]() {
        std::uint32_t i = 0;
        while (running) {
            channel.on_in_request(0x81, i++, 8, maker.make());
        }
    });
    std::thread disconnecter([&]() {
        for (int i = 0; i < 200; i++) {
            channel.on_disconnection();
            channel.on_new_connection();
        }
    });
    disconnecter.join();
    running = false;
    requester.join();
    // No crash is sufficient
}

// ===== Real ByteStreamInChannel blocking-write path (write in two phases: wait for space → write → respond) =====
// These tests do not trigger try_send_one (produce no responses), so no real Session is needed:
// "Blocking write awakened when host IN requests retrieve data" depends on session->submit_ret_submit and is covered by end-to-end
// (Pipe → ByteStreamInChannel) tests.

TEST(TestInEndpointChannel, RealByteStreamBlockingWriteSucceedsWhenSpaceAvailable) {
    ByteStreamInChannel channel;
    channel.set_capacity(16);
    channel.on_new_connection();

    // Buffer empty: blocking write fills it directly without blocking
    std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto n = channel.write(payload, 8);
    EXPECT_EQ(n, 8u);
    EXPECT_EQ(channel.size(), 8u);
    EXPECT_EQ(channel.available(), 8u);
}

TEST(TestInEndpointChannel, RealByteStreamBlockingWriteUnblockedByDisconnect) {
    ByteStreamInChannel channel;
    channel.set_capacity(16);
    channel.on_new_connection();

    std::uint8_t payload[32] = {};
    // Occupy 8 bytes first: write(32) can buffer its first 8 bytes; remaining 24 block awaiting space
    EXPECT_EQ(channel.write_nb(payload, 8), 8u);

    std::uint32_t written = 0xFFFFFFFF;
    std::thread writer([&]() {
        written = channel.write(payload, 32);
    });
    // Wait for writer to enter waiting state (8 written, remaining 24 blocked)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Disconnect: space_cv wakes blocked writer, returning amount already written (8), without continuing
    channel.on_disconnection();
    writer.join();
    EXPECT_EQ(written, 8u);
}

TEST(TestInEndpointChannel, RealByteStreamBlockingWriteTimesOut) {
    ByteStreamInChannel channel;
    channel.set_capacity(16);
    channel.on_new_connection();

    std::uint8_t payload[32] = {};
    // Occupy all 16 bytes, no host retrieval: blocking timeout returns 0
    EXPECT_EQ(channel.write_nb(payload, 16), 16u);
    EXPECT_EQ(channel.available(), 0u);

    auto t0 = std::chrono::steady_clock::now();
    auto n = channel.write(payload, 32, 30);
    auto waited = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(n, 0u);
    // Actually blocked for about 30ms (rather than returning immediately)
    EXPECT_GE(waited, std::chrono::milliseconds(20));
    EXPECT_EQ(channel.size(), 16u);  // Writer did not modify existing data
}

TEST(TestInEndpointChannel, PendingLimitEvictsOldestWithEmptyReply) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending_requests(2);

    // 3 requests pending without data: when request 3 arrives, evict request 1 and respond with empty completion (0 bytes)
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.on_in_request(0x81, 3, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[0].length, 0u);

    // Data arrives: respond to remaining requests 2, 3 (FIFO order)
    channel.push({0xAA, 0xBB});
    channel.push({0xCC, 0xDD});
    ASSERT_EQ(channel.sent.size(), 3u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[2].seqnum, 3u);
    EXPECT_EQ(channel.sent[1].data, (data_type{0xAA, 0xBB}));
    EXPECT_EQ(channel.sent[2].data, (data_type{0xCC, 0xDD}));
}

TEST(TestInEndpointChannel, PushRejectsWhenFullIfNotDropping) {
    RecordingChannel channel;
    channel.on_new_connection();
    channel.set_max_pending_messages(1);

    EXPECT_TRUE(channel.push({0x01}));          // Space available; enqueue succeeds
    // Full and old messages must be retained: return false, buffer unchanged
    EXPECT_FALSE(channel.push({0x02}, false));
    ASSERT_EQ(channel.size(), 1u);
    EXPECT_EQ(channel.debug_front(), (data_type{0x01}));
    // Drop-oldest mode: enqueue succeeds, old message evicted
    EXPECT_TRUE(channel.push({0x03}));
    ASSERT_EQ(channel.size(), 1u);
    EXPECT_EQ(channel.debug_front(), (data_type{0x03}));
}

TEST(TestInEndpointChannel, PushBlockingWaitsForSpace) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending_messages(1);
    channel.push({0x01}); // Occupy the only slot

    bool pushed = false;
    std::thread writer([&]() { pushed = channel.push_blocking({0x02}); });

    // Wait for writer to block (buffer still full); host request retrieves message, freeing a slot
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    channel.on_in_request(0x81, 9, 8, maker.make()); // Buffered data → respond immediately, retrieving it

    writer.join();
    EXPECT_TRUE(pushed);
    ASSERT_EQ(channel.size(), 1u);
    EXPECT_EQ(channel.debug_front(), (data_type{0x02}));
}

TEST(TestInEndpointChannel, PushBlockingInterruptedByDisconnection) {
    // Blocking push into a full buffer: disconnection must interrupt the wait and return false (space_cv wakeup + disconnected check)
    RecordingChannel channel;
    channel.on_new_connection();
    channel.set_max_pending_messages(1);
    channel.push({0x01}); // Occupy the only slot

    bool pushed = true;
    std::thread writer([&]() { pushed = channel.push_blocking({0x02}); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Ensure it is blocked
    channel.on_disconnection();
    writer.join();
    EXPECT_FALSE(pushed);
}

TEST(TestInEndpointChannel, ShardPendingLimitEvictsOldestWithEmptyReply) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending_requests(2);

    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.on_in_request(0x81, 3, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[0].length, 0u);

    const data_type bytes = {1, 2, 3, 4};
    channel.write(bytes.data(), bytes.size());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[1].length, 4u);
}

TEST(TestInEndpointChannel, ConcurrentPushAndRequestsMatchAll) {
    // Multiple threads push data concurrently + multiple threads issue requests concurrently: every request eventually receives a response, whose data
    // matches the corresponding pushed message (two locks prevent loss/mismatches; TSan verifies absence of races)
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    constexpr int PUSHERS = 4;
    constexpr int REQUESTERS = 4;
    constexpr int PER_THREAD = 20;

    // First leave 16 requests pending (64 bytes each): device-side 16 messages respond individually
    std::atomic<int> next_seq{1};
    std::vector<std::thread> requesters;
    for (int t = 0; t < REQUESTERS; t++) {
        requesters.emplace_back([&]() {
            for (int i = 0; i < PER_THREAD; i++) {
                channel.on_in_request(0x81, static_cast<std::uint32_t>(next_seq.fetch_add(1)), 64, maker.make());
            }
        });
    }
    // Concurrently push 16 messages individually (each tagged with a sequence number); t must be captured by value
    //([&] reference capture races with the main thread's loop increment, observed under TSan; i is local to the lambda)
    std::vector<std::thread> pushers;
    for (int t = 0; t < PUSHERS; t++) {
        pushers.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; i++) {
                channel.push(data_type{static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(i)});
            }
        });
    }
    for (auto &th: requesters) {
        th.join();
    }
    for (auto &th: pushers) {
        th.join();
    }

    // All 80 messages received responses (one per request; messages truncated to request length, length ≤ 2 bytes)
    ASSERT_EQ(channel.sent.size(), REQUESTERS * PER_THREAD);
    // Each response seqnum covers 1..80 (no loss or duplication)
    std::vector<int> seqs;
    for (auto &s: channel.sent) {
        seqs.push_back(static_cast<int>(s.seqnum));
    }
    std::sort(seqs.begin(), seqs.end());
    for (int i = 0; i < REQUESTERS * PER_THREAD; i++) {
        EXPECT_EQ(seqs[static_cast<std::size_t>(i)], i + 1);
    }
    EXPECT_EQ(channel.size(), 0u); // All consumed, no leftovers
}

TEST(TestInEndpointChannel, ConcurrentPushBlockingWriters) {
    // Limit 1: 4 writers concurrently push blocking; host retrieves individually to free slots; all enqueue successfully and their order
    // matches response order (TSan verifies wait/wakeup without races)
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending_messages(1);

    constexpr int WRITERS = 4;
    std::atomic<bool> ok{true};
    std::vector<std::thread> writers;
    for (int t = 0; t < WRITERS; t++) {
        writers.emplace_back([&, t]() {
            if (!channel.push_blocking(data_type{static_cast<std::uint8_t>(t)})) {
                ok.store(false);
            }
        });
    }
    // Host retrieves individually (4 messages require 4 requests)
    for (int i = 0; i < WRITERS; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        channel.on_in_request(0x81, static_cast<std::uint32_t>(i + 1), 8, maker.make());
    }
    for (auto &th: writers) {
        th.join();
    }

    EXPECT_TRUE(ok.load());
    ASSERT_EQ(channel.sent.size(), WRITERS);
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, ConcurrentPendingLimitKeepsQueueBounded) {
    // Limit 2: multiple threads issue requests concurrently; queue always ≤2; all evicted requests receive empty completion (0 bytes)
    // responses (TSan verifies concurrency safety of dropping oldest and enqueueing)
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending_requests(2);

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 25;
    std::atomic<int> next_seq{1};
    std::vector<std::thread> requesters;
    for (int t = 0; t < THREADS; t++) {
        requesters.emplace_back([&]() {
            for (int i = 0; i < PER_THREAD; i++) {
                channel.on_in_request(0x81, static_cast<std::uint32_t>(next_seq.fetch_add(1)), 8, maker.make());
            }
        });
    }
    for (auto &th: requesters) {
        th.join();
    }

    // 100 requests: 98 evicted (0-length responses), 2 remain pending
    ASSERT_EQ(channel.sent.size(), THREADS * PER_THREAD - 2);
    for (auto &s: channel.sent) {
        EXPECT_EQ(s.length, 0u);
    }
    // Pending queue contains exactly the last two seqnums
    channel.push({0x01, 0x02});
    channel.push({0x03, 0x04});
    ASSERT_EQ(channel.sent.size(), THREADS * PER_THREAD);
    EXPECT_EQ(channel.sent[THREADS * PER_THREAD - 2].length, 2u);
    EXPECT_EQ(channel.sent[THREADS * PER_THREAD - 1].length, 2u);
}

TEST(TestInEndpointChannel, ConcurrentPushAndDisconnection) {
    // push concurrent with disconnection: either push enqueues before disconnection (then cleared by disconnection), or
    // the disconnection check returns false—no leftovers, no crash (verified by TSan)
    RecordingChannel channel;
    channel.on_new_connection();

    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::thread pusher([&]() {
        int ok = 0;
        for (int i = 0; i < 500; i++) {
            if (channel.push(data_type{static_cast<std::uint8_t>(i)})) {
                ok++;
                if (!started.load()) {
                    started.store(true);
                }
            }
        }
        stop.store(true);
    });
    // Disconnect after pusher starts enqueueing (concurrent competition with push)
    while (!started.load() && !stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    channel.on_disconnection();
    pusher.join();

    // No leftovers after disconnection (earlier enqueues cleared; later pushes return false without enqueueing)
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, ConcurrentUnlinkAndRequests) {
    // cancel_pending (host UNLINK) concurrent with on_in_request: canceled requests are removed from the queue
    // (no further response); uncanceled requests remain pending awaiting data—no duplicate responses, no crash (verified by TSan)
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    std::atomic<int> next_seq{1};
    std::atomic<bool> stop{false};
    std::thread requester([&]() {
        while (!stop.load()) {
            channel.on_in_request(0x81, static_cast<std::uint32_t>(next_seq.fetch_add(1)), 8, maker.make());
        }
    });
    int cancelled = 0;
    for (int i = 0; i < 200; i++) {
        if (channel.cancel_pending(static_cast<std::uint32_t>(i + 1))) {
            cancelled++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    requester.join();

    // Canceled request count does not exceed issued count; remaining requests receive responses when data arrives
    EXPECT_LE(cancelled, next_seq.load() - 1);
    channel.push({0x01});
    EXPECT_FALSE(channel.sent.empty());
    // Every response seqnum is unique (no duplicate responses)
    std::vector<int> seqs;
    for (auto &s: channel.sent) {
        seqs.push_back(static_cast<int>(s.seqnum));
    }
    std::sort(seqs.begin(), seqs.end());
    EXPECT_EQ(std::adjacent_find(seqs.begin(), seqs.end()), seqs.end());
}

TEST(TestInEndpointChannel, ConcurrentRequestAndDisconnection) {
    // Request enqueueing concurrent with disconnection: no loss/crash while disconnection clears queue; later requests leave no leftovers
    // (protected by requests_mutex; TSan verifies absence of races)
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    std::atomic<bool> stop{false};
    std::thread requester([&]() {
        std::uint32_t seq = 1;
        while (!stop.load()) {
            channel.on_in_request(0x81, seq++, 8, maker.make());
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    channel.on_disconnection();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    channel.on_disconnection();
    stop.store(true);
    requester.join();

    // After disconnection, queue and buffer cleared; further push returns false (not receiving after disconnection), no leftovers
    EXPECT_FALSE(channel.push({0x01}));
    EXPECT_TRUE(channel.sent.empty());
    EXPECT_EQ(channel.size(), 0u);
}
