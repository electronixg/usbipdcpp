// usbipdcpp Windows service example
// installation: sc create UsbipdcppServer binPath= "path\to\libusb_windows_service.exe [port]"
// Start: sc start UsbipdcppServer
// Stop: sc stop UsbipdcppServer
// Delete: sc delete UsbipdcppServer


#include "usbipdcpp/LibusbHandler/LibusbServer.h"

#include <windows.h>

#include <libusb-1.0/libusb.h>
#include <spdlog/spdlog.h>

#include <semaphore>
#include <iostream>



using namespace usbipdcpp;

static constexpr wchar_t SERVICE_NAME[] = L"UsbipdcppServer";

static SERVICE_STATUS g_status = {};
static SERVICE_STATUS_HANDLE g_status_handle = nullptr;
static uint16_t g_port = 53240;
static std::binary_semaphore g_stop_sem{0};

void WINAPI service_ctrl_handler(DWORD ctrl) {
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_status.dwCurrentState = SERVICE_STOP_PENDING;
            SetServiceStatus(g_status_handle, &g_status);
            g_stop_sem.release();
            return;
        default:
            SetServiceStatus(g_status_handle, &g_status);
            return;
    }
}

void WINAPI service_main(DWORD argc, LPWSTR *argv) {
    g_status_handle = RegisterServiceCtrlHandlerW(SERVICE_NAME, service_ctrl_handler);
    if (!g_status_handle) {
        return;
    }

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_status_handle, &g_status);

    // initialize libusb
    if (libusb_init(nullptr) != 0) {
        g_status.dwCurrentState = SERVICE_STOPPED;
        g_status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        SetServiceStatus(g_status_handle, &g_status);
        return;
    }

    // Create server and start
    LibusbServerConfig config{};
    config.auto_bind_hotplug = true;
    LibusbServer server(config);

    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), g_port);
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("Server startup failed: {}", ec.message());
        g_status.dwCurrentState = SERVICE_STOPPED;
        g_status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        SetServiceStatus(g_status_handle, &g_status);
        return;
    }
    server.bind_existing_devices();

    g_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_status_handle, &g_status);

    // Blocked waiting for SCM stop signal
    g_stop_sem.acquire();

    // Cleanup
    server.stop();
    libusb_exit(nullptr);

    g_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_status_handle, &g_status);
}

int main(int argc, char **argv) {
    // supports command line parameters to specify the port, such as libusb_windows_service.exe 3240
    if (argc > 1) {
        g_port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(SERVICE_NAME), service_main}, {nullptr, nullptr}};

    if (!StartServiceCtrlDispatcherW(table)) {
        std::cerr << "This program must be run as a Windows Service." << std::endl;
        std::cerr << "To install: sc create UsbipdcppServer binPath= \"" << argv[0] << "\"" << std::endl;
        return 1;
    }
    return 0;
}
