#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"

using namespace usbipdcpp;

namespace {

// Test channel recording responses: derived class implements CRTP reply interface to record (without actual socket transmission)
class RecordingOutChannel : public OutEndpointChannelBase<RecordingOutChannel> {
public:
    struct Replied {
        std::uint32_t seqnum;
        std::uint32_t length;
        std::uint32_t status;
    };
    std::vector<Replied> replied;

    // CRTP interface: respond to one request (base class calls it on retrieval/rejection at limit)
    void reply(std::uint32_t seqnum, std::uint32_t length, std::uint32_t status = 0) {
        replied.push_back({seqnum, length, status});
    }
};

// Construct transfer handle containing data (op is a process-wide singleton: handles of requests remaining queued may be destroyed only at test end,
// when channel is destroyed; static op guarantees validity independently of local-variable lifetime)
struct TransferMaker {
    static inline GenericTransferOperator op;  // Stateless; concurrent free from multiple threads is safe
    TransferHandle make(const data_type &data) {
        auto *trx = new GenericTransfer{};
        trx->data = data;
        return TransferHandle(trx, &op);
    }
};

} // namespace

TEST(TestOutEndpointChannel, RequestPendsUntilTaken) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Request pending: no response (NAK backpressure: host URB remains pending until application retrieves it)
    channel.on_out_request(0x02, 10, maker.make({1, 2, 3}));
    EXPECT_TRUE(channel.replied.empty());
    ASSERT_EQ(channel.size(), 1u);

    // Application retrieves it: read data and respond
    auto p = channel.try_take();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->ep, 0x02);
    EXPECT_EQ(p->data, (data_type{1, 2, 3}));
    ASSERT_EQ(channel.replied.size(), 1u);
    EXPECT_EQ(channel.replied[0].seqnum, 10u);
    EXPECT_EQ(channel.replied[0].length, 3u);
    EXPECT_EQ(channel.replied[0].status, 0u);
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestOutEndpointChannel, FifoOrderPreserved) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Retrieve multiple requests in enqueue order, with matching response order
    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_out_request(0x02, 2, maker.make({2, 2}));
    channel.on_out_request(0x02, 3, maker.make({3, 3, 3}));

    auto p1 = channel.try_take();
    auto p2 = channel.try_take();
    auto p3 = channel.try_take();
    ASSERT_TRUE(p1 && p2 && p3);
    EXPECT_EQ(p1->data, (data_type{1}));
    EXPECT_EQ(p2->data, (data_type{2, 2}));
    EXPECT_EQ(p3->data, (data_type{3, 3, 3}));
    ASSERT_EQ(channel.replied.size(), 3u);
    EXPECT_EQ(channel.replied[0].seqnum, 1u);
    EXPECT_EQ(channel.replied[1].seqnum, 2u);
    EXPECT_EQ(channel.replied[2].seqnum, 3u);
}

TEST(TestOutEndpointChannel, TakeBlocksUntilData) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // First block retrieving; another thread enqueues later: take awakened
    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        channel.on_out_request(0x02, 7, maker.make({9}));
    });
    auto p = channel.take();
    producer.join();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->data, (data_type{9}));
}

TEST(TestOutEndpointChannel, TakeTimeoutReturnsNullopt) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    // No data and not disconnected: timeout returns nullopt
    auto p = channel.take(30);
    EXPECT_FALSE(p.has_value());
}

TEST(TestOutEndpointChannel, InitialDisconnectedTakeReturnsNull) {
    RecordingOutChannel channel;
    // on_new_connection not called (initially disconnected): take immediately returns nullopt
    auto p = channel.take(0);
    EXPECT_FALSE(p.has_value());
    // When disconnected, on_out_request releases request directly without enqueueing
    TransferMaker maker;
    channel.on_out_request(0x02, 1, maker.make({1}));
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestOutEndpointChannel, EmptyDataRequestAnsweredWithZero) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Empty-data request: 0-length response; request consumed
    channel.on_out_request(0x02, 60, maker.make({}));
    auto p = channel.try_take();
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->data.empty());
    ASSERT_EQ(channel.replied.size(), 1u);
    EXPECT_EQ(channel.replied[0].seqnum, 60u);
    EXPECT_EQ(channel.replied[0].length, 0u);
}

TEST(TestOutEndpointChannel, NotificationWithoutTransfer) {
    RecordingOutChannel channel;
    channel.on_new_connection();

    // Notification only (exposed control IN setup): transfer empty; take returns only setup, without responding
    SetupPacket setup;
    setup.request = 0xAA;
    channel.on_out_request(0, 5, TransferHandle{}, setup);

    auto p = channel.try_take();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->ep, 0);
    ASSERT_TRUE(p->setup_req.has_value());
    EXPECT_EQ(p->setup_req->request, 0xAA);
    EXPECT_TRUE(p->data.empty());
    EXPECT_TRUE(channel.replied.empty());
}

TEST(TestOutEndpointChannel, NotificationAndDataMixedOrder) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Notifications interleaved with data requests: consume in enqueue order
    SetupPacket setup;
    setup.request = 0xAA;
    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_out_request(0, 2, TransferHandle{}, setup);
    channel.on_out_request(0x02, 3, maker.make({3}));

    auto p1 = channel.try_take();
    auto p2 = channel.try_take();
    auto p3 = channel.try_take();
    ASSERT_TRUE(p1 && p2 && p3);
    EXPECT_EQ(p1->data, (data_type{1}));
    EXPECT_TRUE(p2->setup_req.has_value());
    EXPECT_TRUE(p2->data.empty());
    EXPECT_EQ(p3->data, (data_type{3}));
    // Only data requests receive responses (seqnum 1, 3); notifications receive none
    ASSERT_EQ(channel.replied.size(), 2u);
    EXPECT_EQ(channel.replied[0].seqnum, 1u);
    EXPECT_EQ(channel.replied[1].seqnum, 3u);
}

TEST(TestOutEndpointChannel, MultipleEndpointsEpPreserved) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // Requests on different endpoints: take returns respective ep (queue globally FIFO; ep follows request)
    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_out_request(0x83, 2, maker.make({2}));
    channel.on_out_request(0x04, 3, maker.make({3}));

    auto p1 = channel.try_take();
    auto p2 = channel.try_take();
    auto p3 = channel.try_take();
    ASSERT_TRUE(p1 && p2 && p3);
    EXPECT_EQ(p1->ep, 0x02);
    EXPECT_EQ(p2->ep, 0x83);
    EXPECT_EQ(p3->ep, 0x04);
}

TEST(TestOutEndpointChannel, ManyPendingRequestsTakenInOrder) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // All 10 requests pending, then take individually: consume + respond in enqueue order
    for (std::uint32_t i = 1; i <= 10; i++) {
        channel.on_out_request(0x02, i, maker.make({static_cast<std::uint8_t>(i)}));
    }
    ASSERT_EQ(channel.size(), 10u);

    for (std::uint32_t i = 1; i <= 10; i++) {
        auto p = channel.try_take();
        ASSERT_TRUE(p.has_value());
        EXPECT_EQ(p->data, (data_type{static_cast<std::uint8_t>(i)}));
    }
    ASSERT_EQ(channel.replied.size(), 10u);
    for (std::uint32_t i = 0; i < 10; i++) {
        EXPECT_EQ(channel.replied[i].seqnum, i + 1);
        EXPECT_EQ(channel.replied[i].length, 1u);
    }
}

TEST(TestOutEndpointChannel, CancelPending) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_out_request(0x02, 10, maker.make({1}));
    channel.on_out_request(0x02, 11, maker.make({2}));

    // Found: request removed (transfer destruction releases it); no further response
    EXPECT_TRUE(channel.cancel_pending(10));
    // Not found
    EXPECT_FALSE(channel.cancel_pending(99));

    auto p = channel.try_take();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->data, (data_type{2}));
    ASSERT_EQ(channel.replied.size(), 1u);
    EXPECT_EQ(channel.replied[0].seqnum, 11u);
}

TEST(TestOutEndpointChannel, DisconnectClearsAndWakes) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_disconnection();

    // Queue cleared: take immediately returns nullopt (disconnected)
    auto p = channel.take(10);
    EXPECT_FALSE(p.has_value());
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestOutEndpointChannel, DisconnectWakesBlockedTaker) {
    RecordingOutChannel channel;
    channel.on_new_connection();

    // Disconnect while take blocked: awaken and return nullopt
    std::thread disconnector([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        channel.on_disconnection();
    });
    auto p = channel.take();
    disconnector.join();
    EXPECT_FALSE(p.has_value());
}

TEST(TestOutEndpointChannel, MaxPendingRejectsWithBusy) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    channel.set_max_pending_requests(2);
    TransferMaker maker;

    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_out_request(0x02, 2, maker.make({2}));
    // Over limit: reject with EPIPE, do not enqueue
    channel.on_out_request(0x02, 3, maker.make({3}));

    ASSERT_EQ(channel.replied.size(), 1u);
    EXPECT_EQ(channel.replied[0].seqnum, 3u);
    EXPECT_EQ(channel.replied[0].status,
              static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(channel.size(), 2u);

    // Retrieving one frees a slot; new request may enqueue
    EXPECT_TRUE(channel.try_take().has_value());
    channel.on_out_request(0x02, 4, maker.make({4}));
    EXPECT_EQ(channel.size(), 2u);
}

TEST(TestOutEndpointChannel, GetTransferDataAppends) {
    // op interface semantics: append data to end of out, return number of appended bytes
    TransferMaker maker;
    auto handle = maker.make({1, 2, 3});

    bool supported = false;
    data_type out{0xAA};
    auto n = maker.op.get_transfer_data(handle.get(), out, supported);
    EXPECT_TRUE(supported);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out, (data_type{0xAA, 1, 2, 3}));

    // Read again (handle data unchanged): continue appending
    out.clear();
    n = maker.op.get_transfer_data(handle.get(), out, supported);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out, (data_type{1, 2, 3}));

    // Default implementation: unsupported (supported=false, return 0)
    class DefaultOp : public TransferOperator {
    public:
        void *alloc_transfer_handle(std::size_t, int, const UsbIpHeaderBasic &, const SetupPacket &) override {
            return nullptr;
        }
        void free_transfer_handle(void *handle) override {}
        std::size_t get_actual_length(void *handle) override { return 0; }
        bool transfer_is_in(void *handle) override { return true; }
        UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) override { return {}; }
        void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) override {}
        void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                std::error_code &ec) override {}
        void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                std::error_code &ec) override {}
    };
    DefaultOp def_op;
    data_type buf;
    bool supported2 = true;
    EXPECT_EQ(def_op.get_transfer_data(nullptr, buf, supported2), 0u);
    EXPECT_FALSE(supported2);
}

TEST(TestOutEndpointChannel, ConcurrentPushAndTakeNoLoss) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    constexpr int N = 1000;

    std::thread producer([&]() {
        for (int i = 0; i < N; i++) {
            channel.on_out_request(0x02, i, maker.make({static_cast<std::uint8_t>(i)}));
        }
    });
    std::thread consumer([&]() {
        for (int i = 0; i < N; i++) {
            while (!channel.try_take().has_value()) {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();

    EXPECT_EQ(channel.size(), 0u);
    ASSERT_EQ(channel.replied.size(), N);
    // Response seqnums cover 0..N-1 without duplicates (FIFO without loss)
    std::vector<bool> seen(N, false);
    for (auto &r: channel.replied) {
        ASSERT_LT(r.seqnum, N);
        seen[r.seqnum] = true;
    }
    for (int i = 0; i < N; i++) {
        EXPECT_TRUE(seen[i]);
    }
}

TEST(TestOutEndpointChannel, ConcurrentBlockingTakeAndPush) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    constexpr int N = 500;

    // Multiple threads repeatedly block in take (each consumes one), one thread continuously enqueues
    std::atomic<int> consumed{0};
    std::vector<std::thread> takers;
    for (int t = 0; t < 4; t++) {
        takers.emplace_back([&]() {
            for (int i = 0; i < N / 4; i++) {
                auto p = channel.take();
                if (p) {
                    consumed++;
                }
            }
        });
    }
    std::thread producer([&]() {
        for (int i = 0; i < N; i++) {
            channel.on_out_request(0x02, i, maker.make({}));
        }
    });
    for (auto &t: takers) {
        t.join();
    }
    producer.join();

    EXPECT_EQ(consumed, N);
    EXPECT_EQ(channel.size(), 0u);
    EXPECT_EQ(channel.replied.size(), N);
}

TEST(TestOutEndpointChannel, ConcurrentDisconnectAndPushNoCrash) {
    // Disconnection concurrent with enqueueing: no crash, consistent final state (requests no longer enqueue after disconnection)
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    std::thread disconnector([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        channel.on_disconnection();
    });
    std::thread pusher([&]() {
        for (int i = 0; i < 500; i++) {
            channel.on_out_request(0x02, i, maker.make({}));
        }
    });
    disconnector.join();
    pusher.join();

    // Once disconnection flag takes effect, release requests directly: queue ends empty
    EXPECT_EQ(channel.size(), 0u);
    auto p = channel.take(10);
    EXPECT_FALSE(p.has_value());
}

TEST(TestOutEndpointChannel, ConcurrentSubmitAndUnlink) {
    // UNLINK (cancel_pending) concurrent with request enqueueing (on_out_request):
    // Each request ends in exactly one outcome (responded or canceled), without loss/duplication.
    // Race occurs within two separate lock scopes (enqueue holds mutex_; cancel holds same lock); valid outcome unique.
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    constexpr int N = 2000;

    std::thread submitter([&]() {
        for (int i = 0; i < N; i++) {
            channel.on_out_request(0x02, i, maker.make({static_cast<std::uint8_t>(i)}));
        }
    });
    std::atomic<int> cancelled{0};
    std::thread unlinker([&]() {
        for (int i = 0; i < N; i++) {
            if (channel.cancel_pending(static_cast<std::uint32_t>(i))) {
                cancelled++;
            }
        }
    });
    submitter.join();
    unlinker.join();

    // Uncanceled requests remain queued; retrieve and respond to all (Pending has no seqnum; verification uses replied)
    while (channel.try_take()) {
    }

    // Both outcomes mutually exclusive and exhaustive: responses + cancellations == N (race inside lock; no duplicates/loss)
    EXPECT_EQ(channel.replied.size() + static_cast<std::size_t>(cancelled),
              static_cast<std::size_t>(N));

    // Response seqnums unique and in range
    std::vector<bool> replied_seen(N, false);
    for (auto &r: channel.replied) {
        ASSERT_LT(r.seqnum, static_cast<std::uint32_t>(N));
        ASSERT_FALSE(replied_seen[r.seqnum]);
        replied_seen[r.seqnum] = true;
    }
    // Every unanswered request must have been canceled: cancellations == unanswered requests
    std::size_t not_replied = N - channel.replied.size();
    EXPECT_EQ(static_cast<std::size_t>(cancelled), not_replied);
}
