# AudioFileSource (--audio option) depends on miniaudio (single header file library, optional), detection logic
# copies mock_audio (AudioFileSource source code is in the mock_audio directory, cross-directory reference):
# Find miniaudio in two ways: first search for miniaudio.h directly, then fall back to pkgconf (for environments such as Termux
# that provide only pkg-config). Treat it as missing only if both searches fail; then print DISABLED and skip
# AudioFileSource (sine/fourier sources are unaffected), following the same approach as mock_uvc_ffmpeg.
# Layout differences of  stb_vorbis.c (vcpkg include root/Termux include/stb/ subdirectory)
# are handled adaptively by __has_include in AudioFileSource.cpp; no detection is needed here.
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if (NOT MINIAUDIO_INCLUDE_DIR)
    # Homebrew 0.11.25+ installs the header file in the include/miniaudio/ subdirectory, and its miniaudio.pc
    # Cflags also points to the include root (the pkgconf branch cannot be found either), so the subdirectory must be found directly.
    # PATH_SUFFIXES causes find_path to return <prefix>/include/miniaudio, which is the directory required by -I
    find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h PATH_SUFFIXES miniaudio)
endif ()
if (MINIAUDIO_INCLUDE_DIR)
    set(MINIAUDIO_INCLUDE_DIRS ${MINIAUDIO_INCLUDE_DIR})
else ()
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(MINIAUDIO_PKGCONF QUIET miniaudio)
        if (MINIAUDIO_PKGCONF_FOUND)
            set(MINIAUDIO_INCLUDE_DIRS ${MINIAUDIO_PKGCONF_INCLUDE_DIRS})
        endif ()
    endif ()
endif ()

# AudioVideoFileSource (--video audio and video synchronization source) depends on FFmpeg (including libswresample audio
# resampling); detect it as in mock_uvc_ffmpeg. If unavailable, do not register the --video option
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(ffmpeg IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
endif ()

file(GLOB_RECURSE src *.cpp *.c)
# The  test file is compiled separately into the test target and does not participate in the main program (same as mock_audio)
list(FILTER src EXCLUDE REGEX "test_.*\\.cpp")
# AudioVideoFileSource is only compiled when FFmpeg is available (non-FFmpeg builds without libav* headers)
list(FILTER src EXCLUDE REGEX "AudioVideoFileSource")
if (MINIAUDIO_INCLUDE_DIRS)
    message(STATUS "miniaudio: ENABLED (include: ${MINIAUDIO_INCLUDE_DIRS})")
    list(APPEND src ../mock_audio/AudioFileSource.cpp)
else ()
    message(STATUS "miniaudio: DISABLED (--audio option unavailable)")
endif ()
if (PkgConfig_FOUND AND ffmpeg_FOUND)
    list(APPEND src AudioVideoFileSource.cpp)
    message(STATUS "FFmpeg: ENABLED (building A/V-synchronized --video source)")
else ()
    message(STATUS "FFmpeg: DISABLED (--video option unavailable)")
endif ()
add_executable(mock_webcam ${src})
target_link_libraries(mock_webcam PRIVATE usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_webcam PRIVATE . ../mock_audio)
if (MINIAUDIO_INCLUDE_DIRS)
    target_include_directories(mock_webcam PRIVATE ${MINIAUDIO_INCLUDE_DIRS})
    target_compile_definitions(mock_webcam PRIVATE USBIPDCPP_USE_MINIAUDIO)
endif ()
if (PkgConfig_FOUND AND ffmpeg_FOUND)
    target_link_libraries(mock_webcam PRIVATE PkgConfig::ffmpeg)
    target_compile_definitions(mock_webcam PRIVATE USBIPDCPP_USE_FFMPEG)
endif ()

# mock_webcam assembly unit tests (implementation accompanies the example: add when building tests and GTest is available,
# as in mock_audio; only the assembled layout is tested, and SineWaveSource/ColorBarSource have no third-party dependencies,
# does not require FFmpeg/miniaudio)
if (USBIPDCPP_BUILD_TESTS)
    find_package(GTest QUIET)
    if (GTest_FOUND)
        include(GoogleTest)
        enable_testing()
        add_executable(test_mock_webcam test_webcam.cpp)
        target_link_libraries(test_mock_webcam PRIVATE usbipdcpp_virtual_device GTest::gtest GTest::gtest_main)
        target_include_directories(test_mock_webcam PRIVATE .)
        # PRE_TEST: move test discovery to the build stage (same approach as tests/CMakeLists.txt, avoiding parallel-build races)
        gtest_discover_tests(test_mock_webcam DISCOVERY_MODE PRE_TEST)
    endif ()
endif ()
