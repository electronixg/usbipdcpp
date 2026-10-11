#include <pybind11/pybind11.h>

namespace py = pybind11;

// Declare the binding functions for each module
void bind_enums(py::module_ &m);
void bind_endpoint(py::module_ &m);
void bind_string_pool(py::module_ &m);
void bind_interface(py::module_ &m);
void bind_device(py::module_ &m);
void bind_server(py::module_ &m);
void bind_virtual_device(py::module_ &m);
void bind_absolute_mouse(py::module_ &m);
void bind_session(py::module_ &m);

PYBIND11_MODULE(usbipdcpp, m) {
    m.doc() = "usbipdcpp - Python bindings for USB/IP virtual device library";

    // Bind in dependency order
    bind_enums(m);
    bind_endpoint(m);
    bind_string_pool(m);
    bind_interface(m);
    bind_device(m);
    bind_server(m);
    bind_session(m);
    bind_virtual_device(m);
    bind_absolute_mouse(m);
}
