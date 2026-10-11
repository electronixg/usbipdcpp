#include "mock_cdc_acm.h"

using namespace usbipdcpp;

// ==================== MockCdcAcmCommunicationInterfaceHandler ====================

MockCdcAcmCommunicationInterfaceHandler::MockCdcAcmCommunicationInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    CdcAcmCommunicationInterfaceHandler(handle_interface, string_pool) {
}

void MockCdcAcmCommunicationInterfaceHandler::on_set_line_coding(const LineCoding &coding) {
    SPDLOG_INFO("Line coding set: baud={}, data_bits={}, stop_bits={}, parity={}",
                coding.dwDTERate, coding.bDataBits, coding.bCharFormat, coding.bParityType);
}

void MockCdcAcmCommunicationInterfaceHandler::on_set_control_line_state(const ControlSignalState &state) {
    SPDLOG_INFO("Control line state: DTR={}, RTS={}", state.dtr, state.rts);

    // Send DCD and DSR signals when DTR goes high
    if (state.dtr && get_data_handler()) {
        // Send status notification: DCD and DSR valid
        send_serial_state_notification(static_cast<std::uint16_t>(CdcAcmSerialState::DCD) |
                                       static_cast<std::uint16_t>(CdcAcmSerialState::DSR));
    }
}

// ==================== MockCdcAcmDataInterfaceHandler ====================

MockCdcAcmDataInterfaceHandler::MockCdcAcmDataInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    CdcAcmDataInterfaceHandler(handle_interface, string_pool) {
}

void MockCdcAcmDataInterfaceHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    CdcAcmDataInterfaceHandler::on_new_connection(current_session, ec);
    should_immediately_stop = false;
}

void MockCdcAcmDataInterfaceHandler::on_disconnection(error_code &ec) {
    should_immediately_stop = true;
    CdcAcmDataInterfaceHandler::on_disconnection(ec);
}

bool MockCdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    if (should_immediately_stop) {
        // Do not process in the stopped state: return false without moving data (the contract); the request remains pending and is cleared on disconnection.
        return false;
    }

    // echo: Send the host data to the IN direction as it is, blocking the transmission to ensure that it is not lost (the generated data is
    // buffered in in_channel, waiting for the host's IN request to consume it)
    SPDLOG_DEBUG("Echo {} bytes", data.size());
    send_data_blocking(std::move(data));
    return true;
}
