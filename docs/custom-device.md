# Implementing a Custom Virtual USB Device

This article explains how to implement a custom virtual USB device in usbipdcpp (without physical hardware), using either of two methods:

| Method | Required USB knowledge | Suitable use cases |
|---|---|---|
| [Method 1: PipeDeviceHandler](#method-1-pipedevicehandler-generic-pipes) | Almost none | A simple data-stream device: the host writes and the device reads; the device writes and the host reads |
| [Method 2: Derive from VirtualInterfaceHandler](#method-2-derive-from-virtualinterfacehandler-full-customization) | Endpoints, transfer types, and control requests | Implementing a specific USB class protocol (HID/UAC/UVC/MSC), or handling control requests specially |

Both methods share the same framework (define the device model → bind handlers → start the server); only the handler implementation differs.

## Background: The Three Layers of a Virtual Device

Virtual devices follow the three-level structure of USB devices:

```
UsbDevice (device descriptor + interface list)
 ├── UsbInterface[0] ──── VirtualInterfaceHandler (interface-level handler)
 ├── UsbInterface[1] ──── VirtualInterfaceHandler
 └── ...
 + device-level handler: VirtualDeviceHandler / SimpleVirtualDeviceHandler
```

**Data flow** (after the client runs `usbip attach`):

```
The host sends CMD_SUBMIT (one USB transfer)
  → The Session::receiver thread reads it
  → The device-level handler's receive_urb()
  → Dispatch according to endpoint type:
     Control transfer → device-level handle_control_urb (standard requests handled internally; non-standard requests forwarded to the interface handler)
     Bulk/Interrupt/Iso → the corresponding interface's handler
  → The handler submits a response through session->submit_ret_submit()
  → The Session::sender thread sends RET_SUBMIT back to the host
```

**Descriptors are generated automatically**: the device-level handler assembles device, configuration, and string descriptors from your `UsbDevice` / `UsbInterface` / `UsbEndpoint` structures. The host uses them directly during enumeration; you do not need to construct byte streams manually.

## Step 1: Define the Device Model

> **Do not hand-write interfaces for built-in library handlers**: every built-in InterfaceHandler provides a static factory with the same name,
> `make_interface(<one address argument per endpoint>...)`, which returns a
> `UsbInterface` matching that handler's descriptors (e.g. `KeyboardHandler::make_interface(0x81)`). Interface handlers
> may make hard-coded assumptions about the interface structure (e.g. recording endpoint addresses in the constructor or assuming endpoint counts/attributes).
> A hand-written interface that violates those assumptions causes enumeration or data-transfer failures, so **always
> prefer factory-created UsbInterface objects** (custom handlers should provide their own factories too; see [make_interface Factory](#make_interface-factory)).
> Factories **return only descriptor templates; they do not bind handlers**: handler constructors hold interface references, so the interface object must first be placed in
> the device (its address is stable after `UsbDevice::make` completes), then bound (see "Shared Steps").
> `UsbDevice` has no such constraint: you can construct it manually for more flexible configuration (all members are mutable).
> The hand-written `UsbInterface`/`UsbEndpoint` below only illustrates the underlying structure.

Every device starts with descriptor structures:

```cpp
#include "usbipdcpp/Server.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"

using namespace usbipdcpp;

StringPool string_pool;

// Interface definition: class/subclass/protocol identify the USB class (0xFF = vendor-specific class)
std::vector<UsbInterface> interfaces = {
        UsbInterface{
                .interface_class = 0xFF,        // vendor specific
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{                 // Outer list = alternate settings; inner list = endpoints for that alt
                        UsbEndpoint{
                                .address = 0x81,          // 0x80 | endpoint number 1 = IN (device → host)
                                .attributes = 0x02,       // Bulk(0x03 = Interrupt,0x01 = Isochronous)
                                .max_packet_size = 64,    // Full-speed bulk maximum
                                .interval = 0,
                        },
                        UsbEndpoint{
                                .address = 0x02,          // OUT (host → device)
                                .attributes = 0x02,
                                .max_packet_size = 64,
                                .interval = 0,
                        },
                }},
        },
};

// Device definition: VID/PID, speed, interface list, EP0
auto device = std::make_shared<UsbDevice>(UsbDevice{
        .path = "/usbipdcpp/my_device",   // Device path (shown by usbip list)
        .busid = "1-1",                   // busid (identifier used by usbip attach -b)
        .bus_num = 1,
        .dev_num = 1,
        .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
        .vendor_id = 0x1234,
        .product_id = 0x5678,
        .device_bcd = 0x0100,
        .device_class = 0x00,             // 0 = class defined at the interface level
        .device_subclass = 0x00,
        .device_protocol = 0x00,
        .configuration_value = 1,
        .num_configurations = 1,
        .interfaces = interfaces,
        .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
        .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
});
```

Key points:

- **Endpoint address**: a set high bit means IN (device→host); the low 7 bits contain the endpoint number. EP0 is the control endpoint and need not be defined manually (the `ep0_in`/`ep0_out` fields above)
- **`attributes`**: the low 2 bits specify transfer type: `0x00` control, `0x01` isochronous, `0x02` Bulk, `0x03` interrupt. HID uses interrupt endpoints; storage/MSC uses Bulk
- **`endpoints` is `vector<vector<UsbEndpoint>>`**: the outer index is the alternate setting (alt 0 must exist)
- **`speed`** determines EP0 size (Full=64, Low=8), bMaxPacketSize0 in the descriptor, and appropriate values for `max_packet_size`

## Shared Steps: Bind Handlers and Start the Server

Both methods use this framework (replace the `with_handler` line for Method 1 or 2):

```cpp
// 1. Bind the device-level handler (essential step)
auto handler = device->with_handler<XXXDeviceHandler>(string_pool);
handler->setup_interface_handlers();   // Register interface-handler endpoints in the device-level routing table

// 2. Register with the server and start it
Server server;
server.add_device(std::move(device));

asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 53240};
if (auto ec = server.start(endpoint); ec) {
    SPDLOG_ERROR("Server startup failed: {}", ec.message());
    return 1;
}
// Connect: usbip attach -r <host> -b 1-1
```

---

## Method 1: PipeDeviceHandler (Generic Pipes)

`PipeDeviceHandler` implements the device as a set of FIFOs: **no USB protocol details need to be implemented**. All endpoints become pipes automatically, and standard control requests are handled automatically. Application code uses only two blocking functions:

```cpp
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

struct PipeXfer {
    std::uint8_t ep;                  // Endpoint address (including direction bit); 0 for control requests
    std::optional<SetupPacket> setup_req; // Valid only for control requests (ep==0)
    data_type data;                   // OUT data or control-request data
};

bool read(PipeXfer &xfer, std::uint32_t timeout_ms = 0);       // Wait for an OUT transfer or non-standard control request
std::size_t write(const PipeXfer &xfer, std::uint32_t timeout_ms = 0); // Send data to an IN endpoint (blocks if the FIFO is full)
```

- `read()`: blocks waiting for OUT data from the host (or a class/vendor control request, in which case `setup_req` has a value). `timeout_ms = 0` waits indefinitely; returns false on disconnection
- `write()`: writes data to the specified IN endpoint's FIFO. **Blocks when the FIFO is full** (until the host reads the data). `timeout_ms = 0` waits indefinitely; returns 0 on disconnection
- Exit semantics: on disconnection, `read` returns false and `write` returns 0; `read` drains queued OUT data before returning false

### Complete Example (Echo Device)

```cpp
#include <atomic>
#include <thread>

#include "usbipdcpp/Server.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

using namespace usbipdcpp;

int main() {
    StringPool string_pool;

    // (The interfaces definition from Step 1: bulk IN 0x81 + bulk OUT 0x02, vendor-specific class)

    auto device = std::make_shared<UsbDevice>(UsbDevice{ /* Device definition from Step 1 */ });
    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
    pipe->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 53240};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    std::atomic<bool> running{true};
    // Application thread: echo received OUT data unchanged through the IN endpoint
    std::thread echo_thread([&]() {
        PipeXfer xfer;
        while (running) {
            if (pipe->read(xfer, 200)) {               // Poll with a 200ms timeout to allow exit
                pipe->write(PipeXfer{.ep = 0x81, .data = std::move(xfer.data)}, 200);
            }
        }
    });

    std::cin.get();
    running = false;      // Stop the application thread first
    echo_thread.join();
    server.stop();        // Then stop the server
    return 0;
}
```

See `examples/mock_pipe/mock_pipe_main.cpp` for a complete compilable version (with threads for echoing and periodic messages).

**Shutdown order matters**: the application thread must **exit first** (stop calling `read`/`write`), before
`server.stop()`; otherwise, when the handler is destroyed with the session, the application thread remains blocked in read/write,
causing use-after-free (the example uses a `running` flag plus polling with a short timeout).

### When a USB Class Identifier Is Needed

A pipe device uses the vendor-specific class by default (interface class=0xFF), so **no host driver is loaded**. To let the host recognize a specific device class (e.g. a HID mouse), take two steps:

1. Change the interface's `interface_class` to the corresponding class (e.g. `ClassCode::HID`) and use the endpoint types required by that class (HID uses interrupt endpoints)
2. Supply class descriptors: `PipeDeviceHandler` does not generate them; set them manually before connection:

```cpp
// HID device: append the HID descriptor (type 0x21) after the interface descriptor
pipe->set_class_specific_descriptor({
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3E, 0x00,  // HID descriptor (9 bytes)
});
// HID report descriptor (type 0x22): returned by type for the host's GET_DESCRIPTOR request
pipe->set_custom_descriptor(0x22, report_descriptor_bytes);
```

See the complete example in `examples/mock_pipe_hid/`.

### Configurable Standard-Request Behavior

Default interface-level standard-request behavior for pipe devices: accept alternate settings present in the interface definition (SET_INTERFACE,
reject other settings), and accept other requests with a success response. For special behavior, set the callback structure before connection:
`standard_request_handler`: 8 callbacks correspond to 8 interface/endpoint-level standard requests. Unset callbacks
retain their defaults. Callbacks run on the session receiver thread; their first argument is a reference to the handler itself:

```cpp
// Demonstration: reject interface-level SET_FEATURE (this device supports no interface-level features)
PipeStandardRequestHandler handler;
handler.set_feature = [](PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
};
pipe->set_standard_request_handler(std::move(handler));
```

| Callback member | Corresponding request | Callback arguments (excluding the pipe reference) |
|---|---|---|
| `clear_feature` / `set_feature` | Interface-level CLEAR/SET_FEATURE | feature_selector, p_status |
| `endpoint_clear_feature` / `endpoint_set_feature` | Endpoint-level CLEAR/SET_FEATURE | feature_selector, ep_address, p_status |
| `get_interface` | GET_INTERFACE | p_status (returns the current alt) |
| `set_interface` | SET_INTERFACE | alternate_setting, p_status |
| `get_status` / `endpoint_get_status` | Interface/endpoint-level GET_STATUS | (ep_address), p_status (returns the status value) |

`p_status` is the response status (0 = success); nonzero values cause the host to receive the corresponding USB error.

### Applicability and Limitations

- The data path provides only "read OUT stream + write IN stream": all endpoints share the same data stream, with no endpoint-specific semantics
- Standard control requests (enumeration) are handled automatically; **non-standard** control requests (class/vendor setup) appear in the `PipeXfer` returned by `read()` (`ep==0`, `setup_req` has a value). Respond with `write({.ep = 0, .data = ...})`
- Unsuitable for stateful query/configuration protocols requiring responses to the host (e.g. audio sample-rate negotiation or UVC stream-parameter negotiation): these control requests are numerous and varied, and the pipe model handles them awkwardly

---

## Method 2: Derive from VirtualInterfaceHandler (Full Customization)

**This is the recommended way to implement a custom device.** Derive from the interface-level handler to implement a class-specific protocol. First, examine the inheritance hierarchy:

```
AbstInterfaceHandler
 └── VirtualInterfaceHandler      ← Interface-level base class for virtual devices
      ├── HidVirtualInterfaceHandler   (HID class already implemented)
      ├── CdcAcmVirtualInterfaceHandler (CDC ACM serial-port class already implemented)
      └── Your own handler             ← Derive from this class
```

Use `SimpleVirtualDeviceHandler` for the device-level handler (its default standard-request implementations all return OK). Interface-handler standard-request callbacks also have default implementations.

### Standard Requests: Handled Internally, with Sufficient Defaults

**Parsing and responses for standard control requests (enumeration/status queries) are handled entirely inside the library** (device-level
`VirtualDeviceHandler::handle_control_urb`):

```
The host sends a standard control request (low 5 bits of bmRequestType = 0, e.g. GET_DESCRIPTOR / SET_ADDRESS /
SET_CONFIGURATION / GET_STATUS / GET_INTERFACE / SET_INTERFACE / SET_FEATURE ...)
  └→ Device-level handle_control_urb parses and responds
       ├─ recipient = device   → handled internally at device level (descriptors, address, configuration... all automatic)
       └─ recipient = interface/endpoint → calls this handler's request_* callbacks (8 standard-request callbacks)
```

Deriving from `VirtualInterfaceHandler` **does not require implementing any functions**; the base class already provides the simplest
correct behavior. Override only for devices with special requirements:

| Callback | Default behavior |
|---|---|
| 8 standard-request callbacks (`request_*`) | Accept and return success; `request_set_interface` accepts only alternate settings present in the device definition (others return EPIPE); `request_get_interface` returns the current alt |
| `get_class_specific_descriptor` | Returns empty (no class descriptors) |
| 5 data-path callbacks (`handle_bulk_transfer` / `handle_interrupt_transfer` / `handle_isochronous_transfer` / `handle_non_standard_request_type_control_urb` (and to_endpoint)) | Return EPIPE—**must be overridden**, otherwise all transfers sent by the host fail |

Override example (reject all interface-level SET_FEATURE requests):

```cpp
void request_set_feature(std::uint16_t, std::uint32_t *p_status) override {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}
```

**Only non-standard requests (CLASS/VENDOR types) require your own parsing**; their callback is
`handle_non_standard_request_type_control_urb` (returns EPIPE by default). For example, HID's
GET_REPORT/SET_REPORT are class requests parsed by `HidVirtualInterfaceHandler` in this callback.

### Core Mechanism: Pending Requests and Responses for IN Data

USB device protocols are **asynchronous**: a device may have no data when the host requests IN data, and the host may not be requesting when data becomes available. The library's `InEndpointChannel` component (`include/usbipdcpp/virtual_device/InEndpointChannel.h`) handles this timing mismatch:

```
The host sends an IN request (handle_bulk_transfer, ep is IN)
  ├─ Data buffer nonempty → populate transfer immediately and return RET_SUBMIT
  └─ No data → leave the request pending (queued by endpoint), waiting for data

The application thread produces data (push / write)
  ├─ Pending requests exist → remove one from the queue and respond
  └─ No pending requests → buffer the data, waiting for the host's next request
```

The component handles request queuing/responses, two locks (blocking for space on the data side does not hold the request-queue lock), and disconnection
cleanup and wakeup. It populates data through `TransferOperator::set_transfer_data` of the operator held by `TransferHandle`
(the component makes no assumptions about transfer internals; any implementation, such as GenericTransfer / libusb, is supported).
Two modes are provided according to consumption semantics:

| Mode | Class | Use cases | Data production entry point |
|---|---|---|---|
| Message mode | `MessageInChannel` | Each data item is one complete message: HID input reports, UVC status notifications | `push(data)` is nonblocking; exceeding the buffer limit drops the oldest item |
| Byte-stream mode | `ByteStreamInChannel` | Continuous bytes split according to request lengths: CDC data interfaces, generic pipes | `write(data, timeout_ms)` blocks, waiting for the host to consume data when the buffer is full (matching kernel FIFO behavior) |

**Complete example: an echo interface handler** (returns through IN whatever it receives through OUT, and also responds to a vendor control request):

```cpp
#include "usbipdcpp/Session.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
// These includes are required by the interface-handler class; the complete program also needs the includes from Step 1
// (StringPool / Device / Server)

class EchoInterfaceHandler : public VirtualInterfaceHandler {
public:
    EchoInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
        VirtualInterfaceHandler(handle_interface, string_pool) {
        // Buffer limit: drop oldest data and retain newest if the host stops reading, preventing unbounded memory growth
        echo_channel.set_max_pending_messages(1024);
    }

    /**
     * @brief Create an echo interface (descriptor template, without a bound handler). Built-in handlers all provide
     * a factory with this name; custom handlers may optionally do so (see the "make_interface Factory" conventions below):
     *
     * Parameters = one address per endpoint type in this interface (names identify their roles; callers may explicitly supply any addresses).
     * The factory supplies appropriate default endpoint attributes/directions; only addresses are configurable.
     * Do not bind the handler in the factory: its constructor holds an interface reference. Wait until the interface is in the device (stable address),
     * then let the caller bind it with device->interfaces[i].with_handler<EchoInterfaceHandler>(string_pool).
     * @param in_ep Bulk IN endpoint address (device→host)
     * @param out_ep Bulk OUT endpoint address (host→device)
     */
    static UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::VendorSpecific),
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 64,
                                           .interval = 0},
                               UsbEndpoint{.address = out_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 64,
                                           .interval = 0}}},
        };
        return i;
    }

    // ===== Lifecycle: session binding and disconnection cleanup (channel state must be maintained in these callbacks) =====
    void on_new_connection(Session &current_session, error_code &ec) override {
        // Base class sets session first (needed for channel responses); then bind the channel and reset disconnection state
        VirtualInterfaceHandler::on_new_connection(current_session, ec);
        echo_channel.on_new_connection(&current_session);
    }

    void on_disconnection(error_code &ec) override {
        // Clear the channel first (buffer + pending requests, automatically released by TransferHandle destruction); then let the base class clear session
        echo_channel.on_disconnection();
        VirtualInterfaceHandler::on_disconnection(ec);
    }

    // ===== Data path =====
    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              error_code &ec) override {
        if (ep.is_in()) {
            // ===== IN: delegate host data requests entirely to the channel (respond immediately if data exists; otherwise wait for push) =====
            echo_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
        }
        else {
            // ===== OUT: retrieve data from the host =====
            auto *trx = GenericTransfer::from_handle(transfer.get());
            // The host's OUT request supplies data that the device must produce, so return it through an IN request
            // (the channel responds directly when a request is pending)
            echo_channel.push(data_type(trx->data.begin(), trx->data.end()));
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, 0));
        }
    }

    // ===== Non-standard control requests (class/vendor setup): respond to vendor request 0xAA here =====
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup, TransferHandle transfer,
                                                      error_code &ec) override {
        if (setup.request == 0xAA && !setup.is_out()) {
            auto *trx = GenericTransfer::from_handle(transfer.get());
            trx->data = {0x42, 0x00};  // Response data
            trx->actual_length = 2;
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    seqnum, 2, std::move(transfer)));
        }
        else {
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        }
    }

    // ===== UNLINK: the host cancels a pending transfer =====
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {
        bool cancelled = echo_channel.cancel_pending(unlink_seqnum);
        session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
    }

private:
    // Echo channel (message mode): returns OUT data through IN; one OUT data item = one message
    MessageInChannel echo_channel;
};
```

> Note:
> - **Lifecycle callbacks are required**: bind the session and reset disconnection state in `on_new_connection`;
>   clear buffered data and pending requests in `on_disconnection` (omitting this leaves a dangling session pointer or residual
>   state from the previous session)
> - Multiple endpoints: one channel manages one endpoint (pass its address to `on_in_request`; requests for that endpoint
>   retain FIFO order). See `PipeDeviceHandler::get_in_channel` for lazily creating channels by endpoint
> - Byte-stream mode: replace `MessageInChannel` above with `ByteStreamInChannel`, and replace `push`
>   with `write` (optionally with `timeout_ms` to block until the host consumes data). Byte streams also support a pull
>   model: `set_pull_callback` produces data on demand when an IN request arrives and both buffer and queue are empty
>   (this is the purpose of the CDC data interface's `on_data_requested`). The callback runs under the lock;
>   do not call this channel's write methods inside it (that would deadlock)
> - Complete references: `src/virtual_device/HidVirtualInterfaceHandler.cpp` (message mode +
>   push), `src/virtual_device/CdcAcmVirtualInterfaceHandler.cpp` (byte-stream mode +
>   blocking writes + pull), `src/virtual_device/PipeDeviceHandler.cpp` (lazy per-endpoint channels)

### Complete File: Binding and Usage

The two binding entry points differ as follows:

- `device->with_handler<T>(...)`: binds the **device-level** handler (one per virtual device)
- `interfaces[i].with_handler<T>(...)`: binds an interface-level handler to **one interface**. Call only after
  device creation completes (the constructor holds an interface reference; the interface address becomes stable only after it enters
  `device->interfaces`; `make_interface` returns only a descriptor template, without binding a handler).
  `setup_interface_handlers()` registers endpoints only for interfaces with bound handlers; **the host cannot access
  interfaces without handlers** (the device-level handler does not automatically create interface handlers; Method 1's PipeDeviceHandler
  automatically binds pipe interface handlers to every interface during setup_interface_handlers, so no manual binding is needed for Method 1)

### make_interface Factory

Built-in handlers (`KeyboardHandler`/`RelativeMouseHandler`/`AbsoluteMouseHandler`/
`GamepadHandler`/`DigitizerHandler`/`MscBulkOnlyHandler`/`CdcAcmCommunicationInterfaceHandler`/
`CdcAcmDataInterfaceHandler`) all provide a static factory with the same name:

```cpp
// Parameters = one address per endpoint in this interface (names identify roles); callers may explicitly specify any addresses;
// the factory supplies appropriate default attributes (attributes/max_packet_size/interval/direction),
// and returns a descriptor template without a bound handler (the caller binds it after device creation)
static UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep);
```

Conventions:

- **Factories describe interfaces without binding handlers**: handler constructors hold interface references; addresses become stable only after interfaces enter the device
  (`UsbDevice::make` completes). At that point, call
  `device->interfaces[i].with_handler<T>(string_pool, ...)` to bind
- **Endpoint addresses must be explicit** (cannot be omitted): callers must control endpoint numbers; parameter names identify endpoint roles
- **Handler constructors receive application parameters** (e.g. AbsoluteMouseHandler screen dimensions, DigitizerHandler
  coordinate limits): pass these to with_handler when binding; factories do not accept them
- **Multiple endpoints in the same direction need distinct roles** (e.g. two IN endpoints in one interface have different purposes): factory address arguments
  alone cannot express this meaning, so the handler's **constructor** additionally accepts the endpoint address for its role; the caller
  passes it through when binding (with_handler). The handler stores addresses by role and dispatches by ep; the framework does not guess mappings
- Custom handlers may optionally provide a factory with the same name (EchoInterfaceHandler above is an example). Omitting it has no
  effect: you may still hand-write an aggregate `UsbInterface` and bind with `device->interfaces[i].with_handler<T>(...)`
- **UsbDevice may be constructed manually**: for configuration more flexible than make parameters (e.g. dynamically varying interface counts),
  use `UsbDevice{...}` aggregate initialization directly (all members are mutable). **Always prefer factories for UsbInterface**:
  interface handlers may hard-code endpoint structures (recording endpoint addresses in the constructor,
  assuming endpoint counts/attributes); an incompatible hand-written interface causes enumeration or data-transfer problems
- The device-level equivalent is `UsbDevice::make(busid, vid, pid, interfaces, ...)`, which creates the device in one step:
  default parameters are ordered by decreasing change frequency (`bus_num`/`dev_num`/`device_class`/`path`/`speed` first,
  followed by `device_bcd`/`device_subclass`/`device_protocol`/`configuration_value`/
  `num_configurations`). EP0 is generated automatically from speed; you may still change any field after return before binding the device-level handler

Putting the sections above together gives this complete custom-device file (define the interface using
`EchoInterfaceHandler::make_interface`; the data path is inside the interface handler, so the device needs no application thread):

```cpp
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"

using namespace usbipdcpp;

// Insert the EchoInterfaceHandler class defined above here

int main() {
    StringPool string_pool;

    // EchoInterfaceHandler::make_interface returns a descriptor template (vendor-specific class, bulk IN
    // 0x81 + bulk OUT 0x02). Bind the interface handler after creating the device (the handler holds an interface
    // reference; the interface address becomes stable only after it enters the device)
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678,
                                  {EchoInterfaceHandler::make_interface(0x81, 0x02)});
    device->interfaces[0].with_handler<EchoInterfaceHandler>(string_pool);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();   // Required: register endpoint routing + interface handlers

    Server server;
    server.add_device(std::move(device));
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 53240};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        return 1;
    }

    std::cin.get();
    server.stop();
    return 0;
}
```

### More Realistic Reference Implementations

| File | Features |
|---|---|
| `include/usbipdcpp/virtual_device/devices/KeyboardHandler.h` + implementation | Complete device derived from `HidVirtualInterfaceHandler`: report descriptor, report data path, LED state, `wait_for_client` |
| `examples/mock_keyboard/mock_keyboard_main.cpp` | Complete usage of that handler (including a `press_key` application thread) |
| `src/virtual_device/HidVirtualInterfaceHandler.cpp` | Pending requests/responses for interrupt endpoints (`MessageInChannel`) + data pushed by `send_input_report` + UNLINK; the clearest reference |
| `src/virtual_device/CdcAcmVirtualInterfaceHandler.cpp` | Multiple endpoints + blocking writes (`ByteStreamInChannel`, waiting for host consumption when full) + pull callback (`on_data_requested`); an example of interfaces with alternate settings |
| `src/virtual_device/PipeDeviceHandler.cpp` | Example of a device-level handler aggregating multiple interface handlers (also demonstrates forwarding from interface handlers to the device level) |

## Comparing the Two Methods

| | PipeDeviceHandler | Derive from VirtualInterfaceHandler |
|---|---|---|
| Learning effort | Low: two functions, read/write | High: understand endpoints/transfer types/control requests |
| Data path | Unified stream (shared by all endpoints) | Independent semantics per endpoint/interface |
| Control requests | Standard requests handled automatically; non-standard requests exposed through read() | Implemented yourself (precise responses to each class/vendor request) |
| Typical uses | Generic pipes, serial forwarding, simple echo, rapid HID prototypes | Actual USB classes such as HID/UAC/UVC/MSC; devices with stateful negotiation |
| Host driver | Default vendor-specific class has no driver; manually set_* when class descriptors are needed | Correct interface class + complete class descriptors allow the host driver to load directly |

**Choosing a method: Method 2 (derive from `VirtualInterfaceHandler`) is recommended for custom devices**.
Method 1 suits devices that are entirely a byte pipe (e.g. reading/writing a virtual device like a file).
Once you need endpoint-specific semantics or a specific USB class protocol (HID reports, CDC control-line state,
UVC stream negotiation, etc.), the pipe model is insufficient. Method 2's callbacks give precise control over each endpoint's
behavior, while the library still handles standard requests. All class devices in this project (HID / CDC ACM / UVC) use
Method 2; `PipeDeviceHandler` itself uses the same callbacks through its internal interface handlers.

## Validation

1. After server startup, `usbip list -r <server>` on the host should show the device
2. Connect with `usbip attach -r <server> -b <busid>` (this project's usbip tool usage: `usbip -t <port> attach -r ...`)
3. Perform operations appropriate to your device type (write/read data or inspect enumeration logs with `dmesg`)
4. Windows client: after `usbipd` / attach in wsl, check Device Manager for enumeration of the corresponding driver

Debugging tip: `spdlog::set_level(spdlog::level::trace)` enables logs tracing each CMD_SUBMIT/RET_SUBMIT. Defining `USBIPDCPP_STRACE` at the top of `src/virtual_device/VirtualDeviceHandler.cpp` also shows all control-request details, useful for diagnosing stuck enumeration.

## Coordinating Multiple Interfaces (Function/Interface Group + IAD)

Many USB classes require more than one interface: RNDIS/ECM coordinate two interfaces,
"communication + data"; UAC uses "AC control + AS streaming", and UVC uses "VC + VS". This section describes the correct
workflow for such devices: populating interfaces validly in `UsbDevice`, declaring IADs, and using
assembly helpers.

### interface_number: Interface Number Semantics and Assignment

`UsbInterface::interface_number` is the USB specification's bInterfaceNumber (interface number).
Control requests (SET_INTERFACE, etc.) address interfaces by it, and bInterfaceNumber in the configuration descriptor is generated strictly
from it. **It is distinct from the array index**:

- Descriptor-template factories such as `make_interface` **do not assign interface numbers** (default 0)
- For devices numbered consecutively from 0 (most virtual devices), call
  `device->assign_interface_numbers()` once after creation to assign 0, 1, 2, ... in index order
- For specially numbered devices (gaps or interleaving with other functions), manually populate each interface's
  `interface_number`

```cpp
auto device = UsbDevice::make("1-1", 0x1234, 0x56E2,
                              {CommHandler::make_interface(0x83), DataHandler::make_interface(0x81, 0x02)});
device->assign_interface_numbers(); // Assign interface_number for interfaces 0 and 1 in order
```

### IAD: Interface Association Descriptor

A multi-interface function requires an IAD (Interface Association Descriptor) to be recognized as a **composite device** on Windows,
matching each kernel gadget function's `*_iad_descriptor`. Without it, Windows neither enumerates subordinate interfaces
in function groups nor probes MS OS descriptors. In testing, an RNDIS device without an IAD was
incorrectly bound by usbser as a serial port (COM port), rather than a network adapter.

An IAD is a **declarative property of a function (interface group)**, attached to the function's first interface:

```cpp
// Fields: bInterfaceCount, bFunctionClass, bFunctionSubClass, bFunctionProtocol, iFunction
// bLength/bDescriptorType are automatic; bFirstInterface is filled from the interface number during generation, so do not pass it
intf.interface_association_descriptor = IadDesc::make(
        2, static_cast<std::uint8_t>(ClassCode::CDC), 0x06 /* USB_CDC_SUBCLASS_ETHERNET */);
```

- RNDIS: `IadDesc::make(2, ClassCode::CDC, 0x06)` (communication + data interfaces, matching the kernel's
  rndis_iad_descriptor in f_rndis.c)
- UVC:`IadDesc::make(2, CC_VIDEO, SC_VIDEO_INTERFACE_COLLECTION, 0, iFunction)`,
  iFunction must equal the VC interface's iInterface (UVC 1.5 Table 3-1)
- A device may have multiple IADs (each function attaches one to its first interface). During configuration descriptor generation, insert them
  in interface order; **each IAD immediately precedes its associated interface descriptor**

Declare the IAD in **the function's own make_interface or assembly helper** (it belongs to
the function definition, so callers need not handle it). `RndisCommunicationInterfaceHandler::make_interface`
does this: assembling an RNDIS device automatically includes its IAD.

### Assembly Workflow (Helper Pattern)

Multi-interface functions usually have cooperating handlers holding references to one another (RNDIS data handlers hold communication-handler
pointers; UVC VC/VS refer to each other). The library provides a common assembly pattern (`UvcDeviceHelper` and
`UacDeviceHelper` follow it):

1. **Locate interfaces by interface_number**, independently of array indices (function interfaces in composite devices
   may interleave with other functions): `device->find_interfaces_by_number<N>(first_number)`
   returns `std::array<UsbInterface*, N>`, with pointers at number-relative offsets and nullptr for missing interfaces
2. **Bind handlers using `with_handler`** (valid only after interfaces enter the device and have stable addresses);
   do not modify `interfaces[i].handler` directly
3. **Set mutual references and declare the IAD inside the helper**; the caller needs one line
4. **Return `std::error_code`**; report missing interfaces and other errors through the return value, without exceptions

```cpp
std::error_code MyFeatureHelper::setup(std::shared_ptr<UsbDevice> device,
                                       std::uint8_t first_interface_number,
                                       StringPool &string_pool, ...) {
    // 1. Locate two adjacent interfaces by number
    auto found = device->find_interfaces_by_number<2>(first_interface_number);
    if (!found[0] || !found[1]) {
        return make_error_code(ErrorType::INVALID_ARG);
    }
    auto *intf0 = found[0];
    auto *intf1 = found[1];

    // 2. Bind with with_handler (non-destructive: only changes these two interfaces' handlers)
    auto h0 = intf0->with_handler<FeatureControlHandler>(string_pool);
    auto h1 = intf1->with_handler<FeatureDataHandler>(string_pool, h0.get()); // 3. Mutual references

    // 3. Declare the IAD (on the function's first interface)
    intf0->interface_association_descriptor = IadDesc::make(
            2, static_cast<std::uint8_t>(ClassCode::CDC), 0x06);

    auto dh = device->handler ? std::dynamic_pointer_cast<VirtualDeviceHandler>(device->handler)
                              : device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    dh->setup_interface_handlers(); // Required: register endpoint routing + interface handlers
    return {};
}

// Caller:
if (auto ec = MyFeatureHelper::setup(device, 0, string_pool, ...); ec) {
    SPDLOG_ERROR("Function assembly failed: {}", ec.message());
    return 1;
}
```

### Complete Example (Two-Interface RNDIS, examples/mock_rndis Framework)

```cpp
#include "usbipdcpp/virtual_device/MSOSSimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/RndisVirtualInterfaceHandler.h"
// ...

std::vector<UsbInterface> interfaces = {
        RndisCommunicationInterfaceHandler::make_interface(0x83), // Includes its own IAD (CDC/0x06/count=2)
        RndisDataInterfaceHandler::make_interface(0x81, 0x02),
};
auto device = UsbDevice::make(busid, 0x1234, 0x56E3, std::move(interfaces),
                              1, 1, static_cast<std::uint8_t>(ClassCode::CDC), "/usbipdcpp/mock_rndis");
device->assign_interface_numbers(); // Assign interface_number values 0 and 1

// Assembly: data handler holds a pointer to the communication handler (which owns the state machine)
auto comm = device->interfaces[0].with_handler<RndisCommunicationInterfaceHandler>(string_pool, mac, UsbSpeed::Full);
device->interfaces[1].with_handler<RndisDataInterfaceHandler>(string_pool, backend.get(), comm.get());

// MS OS 1.0 Compatible ID "RNDIS": Windows uses it to load the built-in rndismp network adapter driver
// (IAD is also required to trigger probing; without it, usbser incorrectly binds it as a serial port)
auto dev_handler = device->with_handler<MSOSSimpleVirtualDeviceHandler>(string_pool);
dev_handler->set_ms_os_compatible_id("RNDIS");
dev_handler->setup_interface_handlers();
```
