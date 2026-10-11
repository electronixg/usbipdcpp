set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(GTest REQUIRED)
include(GoogleTest)

function(add_test_file name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE usbipdcpp GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # PRE_TEST: move test discovery to ctest runtime to prevent multiple test executables in parallel builds
    # from starting concurrently during the build (occasional dynamic-library loading failures caused JSON parsing errors, observed in CI)
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

function(add_test_dir name)
    file(GLOB_RECURSE src ${name}/*.cpp)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE usbipdcpp GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

add_test_dir(test_protocol)

add_test_file(test_network)
# Connection/stop/race tests (requiring frequent repetition to expose problems), separate from pure data tests:
# For frequent repetition, use ctest --repeat until-fail:N -R "TestNetworkConnection" directly
add_test_file(test_network_connection)
add_test_file(test_stringpool)
add_test_file(test_usb_device)
add_test_file(test_setup_packet)
add_test_file(test_usbip_response)
add_test_file(test_object_pool)
add_test_file(test_small_vector)
add_test_file(test_transfer_handle)
add_test_file(test_generic_transfer)
add_test_file(test_transfer_direction)
add_test_file(test_reuse_map)
add_test_file(test_reuse_set)
add_test_file(test_descriptors)
add_test_file(test_ring_buffer)

# Audio sources are in the virtual-device library (FourierSource/SineWaveSource, no third-party dependencies;
# AudioFileSource moved with its implementation to examples/mock_audio; its tests are added by mock_audio's CMakeLists)
if (TARGET usbipdcpp_virtual_device)
    add_test_file(test_audio_sources)
    target_link_libraries(test_audio_sources PRIVATE usbipdcpp_virtual_device)

    # Pure logic tests for virtual-device handlers, one file per device type
    add_test_file(test_hid_handler)
    target_link_libraries(test_hid_handler PRIVATE usbipdcpp_virtual_device)

    add_test_file(test_cdc_acm_handler)
    target_link_libraries(test_cdc_acm_handler PRIVATE usbipdcpp_virtual_device)

    # Virtual-device tests using the network (stop after import, etc.)
    add_test_file(test_network_vdev)
    target_link_libraries(test_network_vdev PRIVATE usbipdcpp_virtual_device)

    # Dedicated release-semantics tests after physical device removal (is_device_removed()): discard semantics,
    # disconnection/stop/contention between multiple clients, and similar scenarios
    add_test_file(test_device_removal)
    target_link_libraries(test_device_removal PRIVATE usbipdcpp_virtual_device)

    # Dedicated ServerObserver notification tests: event timing and arguments, multiple observers, idempotent registration,
    # unregistration, callback exception isolation, querying Server in callbacks (notifications outside the lock), count limit
    add_test_file(test_server_observer)
    target_link_libraries(test_server_observer PRIVATE usbipdcpp_virtual_device)

    # Dedicated connection-filter (source admission control) tests: allow/reject semantics; rejected connections establish no session and send no
    # notifications; source (source-port) filtering; exceptions treated as rejection; persistence across start/stop; clearing
    add_test_file(test_connection_filter)
    target_link_libraries(test_connection_filter PRIVATE usbipdcpp_virtual_device)

    # End-to-end tests for the generic pipe virtual device (PipeDeviceHandler)
    add_test_file(test_pipe_device_handler)
    target_link_libraries(test_pipe_device_handler PRIVATE usbipdcpp_virtual_device)

    # MSC Bulk-Only (USB flash drive) BOT state-machine data-plane tests
    add_test_file(test_msc_handler)
    target_link_libraries(test_msc_handler PRIVATE usbipdcpp_virtual_device)

    # ECM network-adapter data-plane tests (bulk frame reception/transmission)
    add_test_file(test_ecm_handler)
    target_link_libraries(test_ecm_handler PRIVATE usbipdcpp_virtual_device)

    # RNDIS network-adapter tests (encapsulated-command handshake, OID table, data encapsulation/decapsulation)
    add_test_file(test_rndis_handler)
    target_link_libraries(test_rndis_handler PRIVATE usbipdcpp_virtual_device)

    # Microsoft OS 1.0 descriptor tests (0xEE signature string, Compatible ID feature descriptor)
    add_test_file(test_ms_os_handler)
    target_link_libraries(test_ms_os_handler PRIVATE usbipdcpp_virtual_device)

    # UVC camera iso IN frame retrieval/frame-clock tests
    add_test_file(test_uvc_handler)
    target_link_libraries(test_uvc_handler PRIVATE usbipdcpp_virtual_device)

    # UAC speaker (OUT stream reception) iso data-plane tests (delayed scheduler consumption + stream-state switching)
    add_test_file(test_uac_speaker_stream)
    target_link_libraries(test_uac_speaker_stream PRIVATE usbipdcpp_virtual_device)

    # Transfer scheduler (equivalent to vudc frame scheduling) tests
    add_test_file(test_transfer_scheduler)
    target_link_libraries(test_transfer_scheduler PRIVATE usbipdcpp_virtual_device)

    # IN data-channel component (pending-response pattern) tests
    add_test_file(test_in_endpoint_channel)
    target_link_libraries(test_in_endpoint_channel PRIVATE usbipdcpp_virtual_device)

    # OUT data-channel component (NAK backpressure pattern) tests
    add_test_file(test_out_endpoint_channel)
    target_link_libraries(test_out_endpoint_channel PRIVATE usbipdcpp_virtual_device)

    # Audio sink (UAC speaker direction) AC/AS descriptor topology tests
    add_test_file(test_audio_sinks)
    target_link_libraries(test_audio_sinks PRIVATE usbipdcpp_virtual_device)

    # WavFileSink (audio sink in the library) WAV file-writing tests
    add_test_file(test_wav_file_sink)
    target_link_libraries(test_wav_file_sink PRIVATE usbipdcpp_virtual_device)
endif ()
