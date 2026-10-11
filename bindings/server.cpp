#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <asio/ip/tcp.hpp>
#include "usbipdcpp/Server.h"
#include "usbipdcpp/Device.h"

namespace py = pybind11;

// Trampoline class: allows Python to derive from ServerObserver and override event callbacks.
// Callbacks run on the server's session/network/backend threads; pybind11's override macros automatically
// acquire the GIL. The main thread releases the GIL in blocking calls such as start/stop that wait for callbacks
// (see the bindings below), so the two sides do not wait on each other's locks
class PyServerObserver : public usbipdcpp::ServerObserver {
public:
    using usbipdcpp::ServerObserver::ServerObserver;

    void on_session_started(std::uint64_t session_id, const std::string &peer) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_session_started, session_id, peer);
    }

    void on_session_ended(std::uint64_t session_id) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_session_ended, session_id);
    }

    void on_device_added(const std::string &busid) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_device_added, busid);
    }

    void on_device_attached(const std::string &busid) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_device_attached, busid);
    }

    void on_device_released(const std::string &busid, usbipdcpp::DeviceReleaseReason reason) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_device_released, busid, reason);
    }
};

void bind_server(py::module_ &m) {
    // ServerNetworkConfig
    py::class_<usbipdcpp::ServerNetworkConfig>(m, "ServerNetworkConfig")
        .def(py::init<>())
        .def_readwrite("socket_recv_buffer_size", &usbipdcpp::ServerNetworkConfig::socket_recv_buffer_size)
        .def_readwrite("socket_send_buffer_size", &usbipdcpp::ServerNetworkConfig::socket_send_buffer_size)
        .def_readwrite("tcp_no_delay", &usbipdcpp::ServerNetworkConfig::tcp_no_delay);

    // DeviceReleaseReason (parameter to on_device_released)
    py::enum_<usbipdcpp::DeviceReleaseReason>(m, "DeviceReleaseReason")
        .value("ClientDisconnected", usbipdcpp::DeviceReleaseReason::ClientDisconnected)
        .value("DeviceRemoved", usbipdcpp::DeviceReleaseReason::DeviceRemoved)
        .value("ServerStopped", usbipdcpp::DeviceReleaseReason::ServerStopped)
        .export_values();

    // AddDeviceStatus (return value of add_device)
    py::enum_<usbipdcpp::AddDeviceStatus>(m, "AddDeviceStatus")
        .value("Added", usbipdcpp::AddDeviceStatus::Added)
        .value("AlreadyBound", usbipdcpp::AddDeviceStatus::AlreadyBound)
        .value("InUse", usbipdcpp::AddDeviceStatus::InUse)
        .export_values();

    // ServerObserver: derive from it and override the event methods of interest as needed
    py::class_<usbipdcpp::ServerObserver, PyServerObserver>(
            m, "ServerObserver",
            "Server state observer: derive from this class and override the event methods of interest (methods that are not overridden do nothing)."
            "Callbacks run on the server's session/network/backend threads (not the Python main thread) and should return promptly;"
            "exceptions raised by callbacks are caught and logged by the library and do not propagate to the server")
        .def(py::init<>())
        .def("on_session_started", &usbipdcpp::ServerObserver::on_session_started,
             py::arg("session_id"), py::arg("peer"))
        .def("on_session_ended", &usbipdcpp::ServerObserver::on_session_ended,
             py::arg("session_id"))
        .def("on_device_added", &usbipdcpp::ServerObserver::on_device_added,
             py::arg("busid"))
        .def("on_device_attached", &usbipdcpp::ServerObserver::on_device_attached,
             py::arg("busid"))
        .def("on_device_released", &usbipdcpp::ServerObserver::on_device_released,
             py::arg("busid"), py::arg("reason"));

    // Server
    py::class_<usbipdcpp::Server>(m, "Server")
        .def(py::init<>())
        .def(py::init<const usbipdcpp::ServerNetworkConfig &>(), py::arg("network_config"))
        .def("set_connection_filter", [](usbipdcpp::Server &self, py::object filter) {
            if (filter.is_none()) {
                self.set_connection_filter(nullptr);
                return;
            }
            self.set_connection_filter(
                    [filter = py::function(filter)](const asio::ip::tcp::endpoint &ep) -> bool {
                        // The filter is called on the server's network thread (the accept path), not the Python
                        // main thread, so acquire the GIL explicitly; the main thread releases the GIL in start/stop
                        py::gil_scoped_acquire acquire;
                        try {
                            return filter(ep.address().to_string() + ":" + std::to_string(ep.port()))
                                    .cast<bool>();
                        } catch (py::error_already_set &e) {
                            // If the Python callback raises an exception, reject the connection (fail-closed, matching the C++
                            // semantics). The exception must not escape to the C++ layer: destruction of error_already_set
                            // would leave the Python error state for the next pybind call
                            e.discard_as_unraisable("connection_filter");
                            return false;
                        } catch (const py::cast_error &) {
                            // If the callback returns a value that cannot be converted to bool (such as None), also reject the connection
                            return false;
                        }
                    });
        }, py::arg("filter"),
             "Set the connection-source admission filter: filter(peer) returns True to accept / False to reject"
             "(close the connection without creating a session or sending notifications); peer has the form \"ip:port\". The callback runs on"
             "the server's network thread and should return promptly; exceptions cause rejection. Pass None to clear"
             "the filter (all connections are accepted after clearing it). Must be set before start")
        .def("start", [](usbipdcpp::Server &self, const std::string &address, unsigned short port) {
            asio::ip::tcp::endpoint ep(asio::ip::make_address(address), port);
            py::gil_scoped_release release;
            // start does not throw exceptions; errors are reported through its return value (for embedded platforms without exception support).
            // The language binding converts them back to exceptions here and throws them to the caller
            if (auto ec = self.start(ep); ec) {
                throw std::runtime_error("server start failed: " + ec.message());
            }
        }, py::arg("address"), py::arg("port"))
        .def("stop", [](usbipdcpp::Server &self) {
            py::gil_scoped_release release;
            self.stop();
        })
        .def("add_device", [](usbipdcpp::Server &self, std::shared_ptr<usbipdcpp::UsbDevice> device) {
            return self.add_device(std::move(device));
        }, py::arg("device"),
             "Add a device (checking for duplicate busid): returns AddDeviceStatus.Added / AlreadyBound / InUse;"
             "emits on_device_added on success")
        .def("detach_available_device", &usbipdcpp::Server::detach_available_device, py::arg("busid"),
             "Remove a device from the available list and emit on_device_released(busid, DeviceRemoved):"
             "returns the removed device, or None if that busid is absent from the available list (or the device is in use)")
        .def("has_bound_device", &usbipdcpp::Server::has_bound_device, py::arg("busid"))
        .def("get_session_count", &usbipdcpp::Server::get_session_count)
        .def("print_bound_devices", &usbipdcpp::Server::print_bound_devices)
        .def("add_observer", &usbipdcpp::Server::add_observer, py::arg("observer"),
             py::keep_alive<1, 2>(),
             "Register a state observer (idempotent). Server stores only a raw pointer and does not take ownership;"
             "keep_alive ties the observer object's lifetime to this Server;"
             "call remove_observer to unregister it earlier")
        .def("remove_observer", &usbipdcpp::Server::remove_observer, py::arg("observer"));
}
