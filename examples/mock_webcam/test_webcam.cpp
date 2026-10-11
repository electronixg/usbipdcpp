// mock_webcam (UVC camera + UAC microphone composite device) assembly layer test: two function helpers
// Layout correctness of sequential assembly to the same UsbDevice (interface number, IAD ownership, endpoint uniqueness, handler
// type). The behavior of each handler has been covered by test_uvc_handler / test_uac_speaker_stream

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <vector>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/UvcVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"
#include "usbipdcpp/virtual_device/video_sources/ColorBarSource.h"

using namespace usbipdcpp;

namespace {

// has the same assembly process as mock_webcam_main: UVC(VC/VS) interface 0-1 + UAC microphone (AC/AS) interface 2-3
struct WebcamTestEnv {
    StringPool pool;
    std::shared_ptr<UsbDevice> device;

    WebcamTestEnv() {
        // endpoint layout copied mock_webcam_main: UVC VS uses 0x81, UAC AS uses 0x83
        // (0x81 is already used by UVC VS; endpoint numbers are unique within the device, matching kernel g_webcam
        // uvc ep1 + uac ep2/3 distribution)
        std::vector<UsbInterface> interfaces = {
                UsbInterface{.interface_class = 0x0E, .interface_subclass = 0x01, .interface_protocol = 0x01,
                             .endpoints = {{UsbEndpoint{.address = 0x87, .attributes = 0x03,
                                                        .max_packet_size = 16, .interval = 8}}}},
                UsbInterface{.interface_class = 0x0E, .interface_subclass = 0x02, .interface_protocol = 0x01,
                             .endpoints = {{}, {UsbEndpoint{.address = 0x81, .attributes = 0x01,
                                                            .max_packet_size = 512, .interval = 1}}}},
                UsbInterface{.interface_class = 0x01, .interface_subclass = 0x01, .interface_protocol = 0x00,
                             .endpoints = {{UsbEndpoint{.address = 0x82, .attributes = 0x03,
                                                        .max_packet_size = 2, .interval = 4}}}},
                UsbInterface{.interface_class = 0x01, .interface_subclass = 0x02, .interface_protocol = 0x00,
                             .endpoints = {{}, {UsbEndpoint{.address = 0x83, .attributes = 0x01,
                                                            .max_packet_size = 192, .interval = 4}}}},
        };

        device = std::make_shared<UsbDevice>(UsbDevice{
                .path = "/usbipdcpp/mock_webcam",
                .busid = "1-1",
                .bus_num = 1,
                .dev_num = 1,
                .speed = static_cast<std::uint32_t>(UsbSpeed::High),
                .vendor_id = 0x1234,
                .product_id = 0x5685,
                .device_bcd = 0x0100,
                .device_class = 0xEF, // Miscellaneous (IAD): required for a composite device
                .device_subclass = 0x02,
                .device_protocol = 0x01,
                .configuration_value = 1,
                .num_configurations = 1,
                .interfaces = interfaces,
                .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
                .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
        });
        device->assign_interface_numbers();
        // The assembly sequence of  is the same as that of mock_webcam_main: UVC first and then UAC (UAC opens the device-level scheduler,
        // UVC isochronous URB does not go through the scheduler and does not affect each other, see UvcDeviceHelper::setup comment).
        // ASSERT cannot be used in the constructor (non-void return), and assembly failure is reported with EXPECT
        auto ec_uvc = UvcDeviceHelper::setup(device, 0, pool, std::make_unique<ColorBarSource>(320, 240, 15));
        EXPECT_EQ(ec_uvc, std::error_code());
        auto ec_uac = UacDeviceHelper::setup_microphone(device, 2, pool,
                                                        std::make_unique<SineWaveSource>(440, std::vector<std::uint32_t>{48000}, 1, 0.5));
        EXPECT_EQ(ec_uac, std::error_code());
    }
};

} // namespace

TEST(WebcamDevice, InterfacesNumberedInOrder) {
    WebcamTestEnv env;
    for (std::size_t i = 0; i < env.device->interfaces.size(); i++) {
        EXPECT_EQ(env.device->interfaces[i].interface_number, i);
    }
}

TEST(WebcamDevice, FunctionGroupIadsDeclared) {
    WebcamTestEnv env;
    // The first interface in each function group declares its IAD: UVC group = VC (interface 0, attached by UvcDeviceHelper),
    // UAC group = AC (interface 2, attached by UacDeviceHelper). For a composite device (0xEF), usbccgp
    // The interface group is merged only by IAD. When the UAC group does not have IAD, the AC/AS is disassembled. usbaudio.sys
    // fails to start (CM_PROB_FAILED_START observed on Windows), so both helpers must declare IADs symmetrically
    ASSERT_TRUE(env.device->interfaces[0].interface_association_descriptor.has_value());
    auto uvc_iad = *env.device->interfaces[0].interface_association_descriptor;
    EXPECT_EQ(uvc_iad.bInterfaceCount, 2);
    EXPECT_EQ(uvc_iad.bFunctionClass, 0x0E); // CC_VIDEO
    EXPECT_EQ(uvc_iad.bFunctionSubClass, 0x03); // SC_VIDEO_INTERFACE_COLLECTION

    ASSERT_TRUE(env.device->interfaces[2].interface_association_descriptor.has_value());
    auto uac_iad = *env.device->interfaces[2].interface_association_descriptor;
    EXPECT_EQ(uac_iad.bInterfaceCount, 2);
    EXPECT_EQ(uac_iad.bFunctionClass, 0x01); // CC_AUDIO
    EXPECT_EQ(uac_iad.bFunctionSubClass, 0); // Audio has no collection subclass concept
    EXPECT_EQ(uac_iad.iFunction,
              env.device->interfaces[2].handler->get_string_interface_value());
    // The non-first interface in the group does not declare IAD
    EXPECT_FALSE(env.device->interfaces[1].interface_association_descriptor.has_value());
    EXPECT_FALSE(env.device->interfaces[3].interface_association_descriptor.has_value());
}

TEST(WebcamDevice, EndpointAddressesUnique) {
    WebcamTestEnv env;
    // Endpoint numbers must be unique within the device: UAC AS uses 0x83 to avoid UVC VS 0x81 (a classic composite-device
    // error: individual devices work correctly, but their endpoints conflict when combined)
    std::set<std::uint8_t> seen;
    for (auto &intf: env.device->interfaces) {
        for (auto &alt_eps: intf.endpoints) {
            for (auto &ep: alt_eps) {
                EXPECT_TRUE(seen.insert(ep.address).second) << "Duplicate endpoint address 0x" << std::hex
                                                            << static_cast<int>(ep.address);
            }
        }
    }
}

TEST(WebcamDevice, HandlerTypesCorrect) {
    WebcamTestEnv env;
    EXPECT_NE(std::dynamic_pointer_cast<UvcVideoControlHandler>(env.device->interfaces[0].handler), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<UvcVideoStreamingHandler>(env.device->interfaces[1].handler), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<UacAudioControlHandler>(env.device->interfaces[2].handler), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<UacAudioStreamingSourceHandler>(env.device->interfaces[3].handler), nullptr);
}

TEST(WebcamDevice, DeviceClassIsIadProtocol) {
    WebcamTestEnv env;
    // bDeviceClass=0xEF/0x02/0x01 (IAD protocol): Windows usbccgp split by IAD
    // function group, UVC/UAC respective binding class driver (same as mock_uvc)
    EXPECT_EQ(env.device->device_class, 0xEF);
    EXPECT_EQ(env.device->device_subclass, 0x02);
    EXPECT_EQ(env.device->device_protocol, 0x01);
}
