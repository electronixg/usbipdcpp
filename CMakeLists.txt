cmake_minimum_required(VERSION 3.22)
project(usbipdcpp VERSION 1.0.9)

set(CMAKE_CXX_STANDARD 23)

# Make find_package consistently prefer config mode, with Find modules as the fallback (CMake 3.15+
# CMAKE_FIND_PACKAGE_PREFER_CONFIG): vcpkg/newer dependencies exporting *Config.cmake use
# config mode; only environments with legacy layouts (no config, only built-in CMake Find modules such as FindGTest)
# automatically fall back to module mode. Therefore, all find_package calls in the project omit the CONFIG/MODULE keywords
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)

include(GNUInstallDirs)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IS_TOP_LEVEL ON)
else ()
    set(IS_TOP_LEVEL OFF)
endif ()
option(USBIPDCPP_BUILD_EXAMPLES "Build example applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_LIBUSB_SERVER "Build example libusb server applications" ${IS_TOP_LEVEL})

if ((UNIX AND NOT APPLE) OR ANDROID)
    set(CAN_THIS_SYSTEM_SUPPORT_EVDEV TRUE)
else ()
    set(CAN_THIS_SYSTEM_SUPPORT_EVDEV FALSE)
endif ()

if (IS_TOP_LEVEL AND CAN_THIS_SYSTEM_SUPPORT_EVDEV)
    set(USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE_DEFAULT ON)
else ()
    set(USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE_DEFAULT OFF)
endif ()
option(
        USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE
        "Build example libevdev mouse applications"
        ${USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE_DEFAULT}
)

if (IS_TOP_LEVEL AND ANDROID)
    set(USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER_DEFAULT ON)
else ()
    set(USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER_DEFAULT OFF)
endif ()
option(
        USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER
        "Build example libusb applications that can be run in termux by termux-usb"
        ${USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER_DEFAULT}
)

option(USBIPDCPP_BUILD_EXAMPLE_MOCK_MOUSE "Build example mock mouse applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_KEYBOARD "Build example mock keyboard applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_GAMEPAD "Build example mock gamepad applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_CDC_ACM "Build example mock CDC ACM (virtual serial port) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_CDC_THROTTLE "Build example throttled CDC ACM (OUT NAK) virtual serial applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_MSC "Build example mock MSC (virtual USB flash drive) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_UVC "Build example mock UVC (virtual USB camera) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_AUDIO "Build example mock UAC (virtual USB microphone) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_SPEAKER "Build example mock UAC (virtual USB speaker) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MULTI_DEVICES "Build example multi devices applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_ABSOLUTE_MOUSE "Build example absolute mouse applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_UVC_FFMPEG "Build example mock UVC applications with FFmpeg" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MULTI_INTERFACE_HID "Build example multi-interface HID (mouse + keyboard) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_PIPE "Build example mock generic pipe applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_PIPE_HID "Build example mock HID keyboard via generic pipe applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_ECM "Build example mock CDC ECM (virtual ethernet adapter) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_RNDIS "Build example mock CDC RNDIS (virtual ethernet adapter, driverless on Windows) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_WEBCAM "Build example mock UVC+UAC webcam (virtual camera + microphone) applications" ${IS_TOP_LEVEL})

if (IS_TOP_LEVEL AND WIN32)
    set(USBIPDCPP_BUILD_EXAMPLE_LIBUSB_WINDOWS_SERVICE_DEFAULT ON)
else ()
    set(USBIPDCPP_BUILD_EXAMPLE_LIBUSB_WINDOWS_SERVICE_DEFAULT OFF)
endif ()
option(
        USBIPDCPP_BUILD_EXAMPLE_LIBUSB_WINDOWS_SERVICE
        "Build example libusb Windows Service applications"
        ${USBIPDCPP_BUILD_EXAMPLE_LIBUSB_WINDOWS_SERVICE_DEFAULT}
)

option(USBIPDCPP_BUILD_VIRTUAL_DEVICE "Build virtual device component" ON)
option(USBIPDCPP_BUILD_LIBUSB_COMPONENTS "Build libusb component" ON)
option(USBIPDCPP_BUILD_TESTS "Build tests" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_PYTHON_BINDINGS "Build Python bindings" OFF)
option(USBIPDCPP_BUILD_SHARED_LIBS "Build as shared library (recommended for LGPL compliance)" ON)
option(USBIPDCPP_INSTALL_EXAMPLES "Install example executables" OFF)

option(USBIPDCPP_FORCE_TRACK_PACKAGE "Force to track package even in Release build" OFF)
option(USBIPDCPP_USE_PKGCONF_ASIO "Use pkgconf to find asio instead of find_package" OFF)

if (USBIPDCPP_USE_PKGCONF_ASIO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(asio REQUIRED IMPORTED_TARGET asio)
else ()
    find_package(asio REQUIRED)
endif ()
find_package(spdlog REQUIRED)

if (MSVC)
    add_compile_options("/Zc:preprocessor")
endif ()


if (CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_definitions(
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
            NDEBUG
    )
elseif (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_definitions(
            #            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
            NDEBUG
    )
else ()
    add_compile_definitions(
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
            TRANSFER_DELAY_RECORD
    )
endif ()

file(GLOB BASE_SRC_FILES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB UTILS_SRC_FILES CONFIGURE_DEPENDS src/utils/*.cpp)
file(GLOB DEVICE_HANDLER_SRC_FILES CONFIGURE_DEPENDS src/DeviceHandler/*.cpp)
file(GLOB INTERFACE_HANDLER_SRC_FILES CONFIGURE_DEPENDS src/InterfaceHandler/*.cpp)
file(GLOB VIRTUAL_DEVICE_SRC_FILES CONFIGURE_DEPENDS src/virtual_device/*.cpp)
file(GLOB VIRTUAL_DEVICE_DEVICES_SRC_FILES CONFIGURE_DEPENDS src/virtual_device/devices/*.cpp)
file(GLOB VIRTUAL_DEVICE_STORAGE_BACKENDS_SRC_FILES CONFIGURE_DEPENDS src/virtual_device/storage_backends/*.cpp)
file(GLOB VIRTUAL_DEVICE_VIDEO_SOURCES_SRC_FILES CONFIGURE_DEPENDS src/virtual_device/video_sources/*.cpp)
file(GLOB VIRTUAL_DEVICE_AUDIO_SOURCES_SRC_FILES CONFIGURE_DEPENDS src/virtual_device/audio_sources/*.cpp)
file(GLOB VIRTUAL_DEVICE_AUDIO_SINKS_SRC_FILES CONFIGURE_DEPENDS src/virtual_device/audio_sinks/*.cpp)
file(GLOB VIRTUAL_DEVICE_NETWORK_BACKENDS_SRC_FILES CONFIGURE_DEPENDS src/virtual_device/network_backends/*.cpp)
# TunBackend relies on Linux’s /dev/net/tun + TUNSETIFF ioctl (Android/termux kernel also
# provides these), but macOS lacks this interface (utun is a layer-3 device with no public TAP API), so do not compile it there
list(FILTER VIRTUAL_DEVICE_NETWORK_BACKENDS_SRC_FILES EXCLUDE REGEX ".*TunBackend\\.cpp$")
if (UNIX AND NOT APPLE)
    list(APPEND VIRTUAL_DEVICE_NETWORK_BACKENDS_SRC_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/virtual_device/network_backends/TunBackend.cpp)
endif ()
file(GLOB LIBUSB_SRC_FILES CONFIGURE_DEPENDS src/LibusbHandler/*.cpp)

# controls whether to compile a dynamic library or a static library
set(LIB_TYPE STATIC)
if (USBIPDCPP_BUILD_SHARED_LIBS)
    set(LIB_TYPE SHARED)
    # indicates that this is a dynamic library, and the macros defined by add_compile_definitions will not be propagated to users.
    add_compile_definitions(USBIPDCPP_EXPORTS)

    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    if (APPLE)
        set(CMAKE_BUILD_RPATH "@loader_path;${CMAKE_INSTALL_FULL_LIBDIR}")
        set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/../${CMAKE_INSTALL_LIBDIR};${CMAKE_INSTALL_FULL_LIBDIR}")
    else ()
        set(CMAKE_BUILD_RPATH "$ORIGIN;${CMAKE_INSTALL_FULL_LIBDIR}")
        set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../${CMAKE_INSTALL_LIBDIR};${CMAKE_INSTALL_FULL_LIBDIR}")
    endif ()

    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(CMAKE_CXX_VISIBILITY_PRESET hidden)
        set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
    endif ()
endif ()

# core library (excluding virtual devices)
add_library(${PROJECT_NAME} ${LIB_TYPE} ${BASE_SRC_FILES} ${UTILS_SRC_FILES} ${DEVICE_HANDLER_SRC_FILES} ${INTERFACE_HANDLER_SRC_FILES})
# link dependent library
if (USBIPDCPP_USE_PKGCONF_ASIO)
    target_link_libraries(${PROJECT_NAME} PUBLIC PkgConfig::asio)
else ()
    target_link_libraries(${PROJECT_NAME} PUBLIC asio::asio)
endif ()
target_link_libraries(${PROJECT_NAME} PUBLIC spdlog::spdlog)
target_include_directories(${PROJECT_NAME} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# Set whether to force delayed tracking
if (USBIPDCPP_FORCE_TRACK_PACKAGE)
    message(STATUS "USBIPDCPP: Force tracking package")
    target_compile_definitions(${PROJECT_NAME} PUBLIC USBIPDCPP_FORCE_TRACK_PACKAGE)
endif ()
# Define the USBIPDCPP_STATIC macro after creating the target, and set it to PUBLIC to propagate it to users.
if (NOT USBIPDCPP_BUILD_SHARED_LIBS)
    target_compile_definitions(${PROJECT_NAME} PUBLIC USBIPDCPP_STATIC)
endif()
# On some embedded platforms (such as ESP-IDF), libc declares std::signal without implementing it, causing link failures.
# Disabled by default; build systems that do not use this CMakeLists (such as esp32 IDF) can directly define
# the USBIPDCPP_DISABLE_SIGNAL macro for the same effect as this option
option(USBIPDCPP_DISABLE_SIGNAL "Disable std::signal usage (platforms whose libc declares it but has no implementation)" OFF)
if (USBIPDCPP_DISABLE_SIGNAL)
    target_compile_definitions(${PROJECT_NAME} PUBLIC USBIPDCPP_DISABLE_SIGNAL)
endif ()

# virtual device library
if (USBIPDCPP_BUILD_VIRTUAL_DEVICE)
    add_library(${PROJECT_NAME}_virtual_device ${LIB_TYPE} ${VIRTUAL_DEVICE_SRC_FILES} ${VIRTUAL_DEVICE_DEVICES_SRC_FILES} ${VIRTUAL_DEVICE_STORAGE_BACKENDS_SRC_FILES} ${VIRTUAL_DEVICE_VIDEO_SOURCES_SRC_FILES} ${VIRTUAL_DEVICE_AUDIO_SOURCES_SRC_FILES} ${VIRTUAL_DEVICE_AUDIO_SINKS_SRC_FILES} ${VIRTUAL_DEVICE_NETWORK_BACKENDS_SRC_FILES})
    set_target_properties(${PROJECT_NAME}_virtual_device PROPERTIES EXPORT_NAME virtual_device)
    target_link_libraries(${PROJECT_NAME}_virtual_device PUBLIC ${PROJECT_NAME})
    target_include_directories(${PROJECT_NAME}_virtual_device PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endif ()

if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)

    add_library(${PROJECT_NAME}_libusb ${LIB_TYPE} ${LIBUSB_SRC_FILES})
    set_target_properties(${PROJECT_NAME}_libusb PROPERTIES EXPORT_NAME libusb)
    target_link_libraries(${PROJECT_NAME}_libusb PUBLIC PkgConfig::libusb)
    target_link_libraries(${PROJECT_NAME}_libusb PUBLIC ${PROJECT_NAME})
endif ()

if (USBIPDCPP_BUILD_TESTS)
    # Must be called before add_subdirectory; otherwise, subtrees such as examples do not generate
    # CTestTestfile.cmake, and ctest does not include their gtest_discover_tests tests
    enable_testing()
endif ()

if (USBIPDCPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

if (USBIPDCPP_BUILD_TESTS)
    add_subdirectory(tests)
endif ()

# Python bindings
if (USBIPDCPP_BUILD_PYTHON_BINDINGS AND USBIPDCPP_BUILD_VIRTUAL_DEVICE)
    add_subdirectory(bindings)
endif ()

if (IS_TOP_LEVEL)
    set(EXPORT_TARGETS ${PROJECT_NAME})
    if (TARGET ${PROJECT_NAME}_virtual_device)
        list(APPEND EXPORT_TARGETS ${PROJECT_NAME}_virtual_device)
    endif ()
    if (TARGET ${PROJECT_NAME}_libusb)
        list(APPEND EXPORT_TARGETS ${PROJECT_NAME}_libusb)
    endif ()

    export(TARGETS ${EXPORT_TARGETS}
            NAMESPACE ${PROJECT_NAME}::
            FILE ${PROJECT_NAME}Targets.cmake
    )

    # installation header file
    if (USBIPDCPP_BUILD_VIRTUAL_DEVICE)
        if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS)
            install(DIRECTORY include/${PROJECT_NAME}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME})
        else ()
            install(DIRECTORY include/${PROJECT_NAME}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
                    PATTERN "LibusbHandler" EXCLUDE)
        endif ()
    else ()
        if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS)
            install(DIRECTORY include/${PROJECT_NAME}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
                    PATTERN "virtual_device" EXCLUDE)
        else ()
            install(DIRECTORY include/${PROJECT_NAME}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
                    PATTERN "virtual_device" EXCLUDE
                    PATTERN "LibusbHandler" EXCLUDE)
        endif ()
    endif ()


    install(EXPORT ${PROJECT_NAME}Targets
            FILE ${PROJECT_NAME}Targets.cmake
            NAMESPACE ${PROJECT_NAME}::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    )

    install(TARGETS ${PROJECT_NAME}
            EXPORT ${PROJECT_NAME}Targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    if (USBIPDCPP_BUILD_VIRTUAL_DEVICE)
        install(TARGETS ${PROJECT_NAME}_virtual_device
                EXPORT ${PROJECT_NAME}Targets
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif ()
    if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS)
        install(TARGETS ${PROJECT_NAME}_libusb
                EXPORT ${PROJECT_NAME}Targets
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif ()
    #install(TARGETS mock_mouse empty_server libusb_server
    #        RUNTIME DESTINATION example
    #)


    include(CMakePackageConfigHelpers)

    configure_package_config_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PROJECT_NAME}Config.cmake.in"
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
            INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    )


    write_basic_package_version_file(
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
    )

    install(FILES
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    )

    if (WIN32)
        # Install vcpkg dependent DLL in the build directory to the bin directory
        # CMAKE_INSTALL_CONFIG_NAME will be set in install(CODE) script mode,
        # but not CMAKE_CONFIGURATION_TYPES. At the same time, glob root directory and configuration subdirectory,
        # is compatible with single configuration generator (Ninja, DLL in build/) and multi-configuration generator (VS, DLL in build/<Config>/)
        install(CODE "
            file(GLOB dlls
                \"${CMAKE_BINARY_DIR}/*.dll\"
                \"${CMAKE_BINARY_DIR}/\${CMAKE_INSTALL_CONFIG_NAME}/*.dll\")
            if(dlls)
                list(FILTER dlls EXCLUDE REGEX \".*[/\\\\\\\\]usbipdcpp[^/\\\\\\\\]*\\\\.dll$\")
                if(dlls)
                    file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}\" FILES \${dlls})
                endif()
            endif()
        ")
    endif ()
endif ()