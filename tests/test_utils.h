#pragma once

#include <asio.hpp>
#include <gtest/gtest.h>

#include "crash_handler.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/TransferResponder.h"

namespace usbipdcpp {
namespace test {

// Test-only comparison function
inline void expect_header_equal(const UsbIpHeaderBasic &actual, const UsbIpHeaderBasic &expected) {
    EXPECT_EQ(actual.command, expected.command);
    EXPECT_EQ(actual.seqnum, expected.seqnum);
    EXPECT_EQ(actual.devid, expected.devid);
    EXPECT_EQ(actual.direction, expected.direction);
    EXPECT_EQ(actual.ep, expected.ep);
}

inline void expect_cmd_unlink_equal(const UsbIpCommand::UsbIpCmdUnlink &actual,
                                     const UsbIpCommand::UsbIpCmdUnlink &expected) {
    expect_header_equal(actual.header, expected.header);
    EXPECT_EQ(actual.unlink_seqnum, expected.unlink_seqnum);
}

    template<typename T>
    concept with_header = requires(T &&t)
    {
        std::forward<T>(t).header;
    };

// ---- Shared network-test utilities ----

// Probe a free port. Fixed port needed to verify repeated start listening does not conflict
inline std::uint16_t probe_free_port(asio::io_context &io) {
    asio::ip::tcp::acceptor probe(io);
    probe.open(asio::ip::tcp::v4());
    probe.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    return probe.local_endpoint().port();
}

// Connect to server, polling until listening ready. Recreate socket and retry on failure; return connection success
inline bool connect_with_retry(asio::ip::tcp::socket &client, const asio::ip::tcp::endpoint &ep) {
    for (int i = 0; i < 200; i++) {
        std::error_code ec;
        client.connect(ep, ec);
        if (!ec) {
            return true;
        }
        client.close();
        client = asio::ip::tcp::socket(client.get_executor());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// Disconnect with RST (linger=0 sends no FIN): simulate abnormal client disconnection;
// server reads connection-reset error rather than EOF, taking a different error branch
inline void rst_disconnect(asio::ip::tcp::socket &client) {
    std::error_code ec;
    client.set_option(asio::socket_base::linger(true, 0), ec);
    client.close();
}

// Poll until server cleans up all disconnected connections: after client disconnection, session thread cleans up
// and removes itself (get_session_count returns live sessions); zero means cleanup complete. If connection still
// in accept queue (session not created), count also 0; early return harmless—
// subsequent stop() active-session count wait (including all created sessions) ensures full cleanup
inline bool wait_sessions_gone(Server &server, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.get_session_count() == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return server.get_session_count() == 0;
}

// Send import request and read reply; return reply status; any error returns maximum value
inline std::uint32_t import_device(asio::ip::tcp::socket &client, const std::string &busid) {
    UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
    std::copy(busid.begin(), busid.end(), req.busid.begin());
    usbipdcpp::error_code send_ec;
    req.to_socket(client, send_ec);
    if (send_ec) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    std::uint16_t version = 0;
    std::uint16_t command = 0;
    std::uint32_t status = 0;
    data_read_from_socket(client, version, command, status);
    if (command != OP_REP_IMPORT) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return status;
}

    template<usbipdcpp::Serializable T>
    T reread_from_socket_with_command(const T &origin, std::uint16_t cmd) {
        asio::io_context io_context;
        asio::ip::tcp::acceptor acceptor(io_context);
        auto server_endpoint = asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0);
        acceptor.open(server_endpoint.protocol());

        acceptor.bind(server_endpoint);
        acceptor.listen();

        auto server_port = acceptor.local_endpoint().port();

        std::thread sender([&]() {
            auto sock = acceptor.accept();
            usbipdcpp::data_type buffer;
            // Send version number + command code
            usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint16_t>(USBIP_VERSION));
            usbipdcpp::vector_append_to_net(buffer, (std::uint16_t) cmd);
            auto data = origin.to_bytes();
            sock.send(asio::buffer(data));
        });

        T received{};
        asio::ip::tcp::socket server_socket(io_context);
        asio::error_code ec;
        server_socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server_port), ec);

        [[maybe_unused]] auto version = usbipdcpp::read_u16(server_socket);
        auto op_command = usbipdcpp::read_u16(server_socket);
        received.from_socket(server_socket);
        // SPDLOG_INFO("Received header from server");
        if constexpr (with_header<T>) {
            received.header.command = op_command;
        }
        else {
            received.command = op_command;
        }
        server_socket.close();

        sender.join();

        return received;
    }

// ---- Shared virtual-device data-plane test utilities ----

// Recording TransferResponder stub: capture all submitted responses; tests assert content directly.
// Replacing real Session permits network-independent data-plane tests of handlers/channels/scheduler
//
// Lifetime: RetSubmit entries in submits hold TransferHandle (bound to make_cmd_submit
// op); destroying this stub destroys handles, calling op to release them. Thus op must outlive stub:
// declare op before stub in tests, or make it a fixture member (fixture declared before stub,
// destroyed later in reverse order)—otherwise clang ASan reports stack-use-after-scope
class CaptureResponder : public TransferResponder {
public:
    void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) override {
        submits.push_back(std::move(submit));
    }
    void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) override {
        unlinks.push_back(std::move(unlink));
    }
    void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) override {
        enqueued_submits.push_back(std::move(submit));
    }
    void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) override {
        enqueued_unlinks.push_back(std::move(unlink));
    }
    void wakeup_sender() override {
        ++wakeup_count;
    }
    void stop_transfer() override {
        stopped = true;
    }

    std::vector<UsbIpResponse::UsbIpRetSubmit> submits;
    std::vector<UsbIpResponse::UsbIpRetUnlink> unlinks;
    std::vector<UsbIpResponse::UsbIpRetSubmit> enqueued_submits;
    std::vector<UsbIpResponse::UsbIpRetUnlink> enqueued_unlinks;
    int wakeup_count = 0;
    bool stopped = false;
};

// Extract data phase from RET_SUBMIT (empty when transfer not taken over, e.g. OUT response)
inline data_type ret_submit_data(const UsbIpResponse::UsbIpRetSubmit &ret) {
    if (auto *trx = GenericTransfer::from_handle(ret.transfer.get())) {
        return trx->data;
    }
    return {};
}

// Read little-endian integer at byte-stream offset (for wire-format data-field assertions)
inline std::uint32_t read_le32_at(const data_type &d, std::size_t offset) {
    std::uint32_t v{};
    std::memcpy(&v, d.data() + offset, sizeof(v));
    return v;
}

inline std::uint16_t read_le16_at(const data_type &d, std::size_t offset) {
    std::uint16_t v{};
    std::memcpy(&v, d.data() + offset, sizeof(v));
    return v;
}

// Construct CMD_SUBMIT (matching CmdSubmit::from_socket allocation: both IN/OUT allocate
// transfer; IN requests read no data phase; OUT requests carry data). Caller owns op,
// which must outlive submit (TransferHandle destruction uses it to release)
inline UsbIpCommand::UsbIpCmdSubmit make_cmd_submit(GenericTransferOperator &op, std::uint32_t seqnum,
                                                    std::uint8_t ep_num, std::uint32_t direction,
                                                    std::uint32_t transfer_buffer_length,
                                                    const SetupPacket &setup = {}, const data_type &out_data = {},
                                                    int num_iso = 0) {
    UsbIpCommand::UsbIpCmdSubmit submit{};
    submit.header.command = USBIP_CMD_SUBMIT;
    submit.header.seqnum = seqnum;
    submit.header.devid = 1;
    submit.header.direction = direction;
    submit.header.ep = ep_num; // Wire-format endpoint number (without direction bit; direction supplied by direction field)
    submit.transfer_flags = 0;
    submit.transfer_buffer_length = transfer_buffer_length;
    submit.start_frame = 0;
    submit.number_of_packets = num_iso;
    submit.interval = 0;
    submit.setup = setup;
    UsbIpHeaderBasic header{};
    header.direction = direction;
    auto *trx = GenericTransfer::from_handle(op.alloc_transfer_handle(transfer_buffer_length, num_iso, header, setup));
    if (direction == UsbIpDirection::Out && !out_data.empty()) {
        trx->data = out_data;
    }
    TransferHandle handle(trx, &op);
    submit.transfer = std::move(handle);
    return submit;
}

}
}
